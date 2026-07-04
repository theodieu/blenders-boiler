//
// Hot chocolate dispenser — 4 blenders + boiler, XIAO ESP32-S3.
//
// Power constraint: only ONE device (a blender heating/blending or the boiler)
// draws power at any time. A round-robin scheduler grants each enabled device
// an active slot; program 1 (initial blend) and program 2 (homogeneity blend)
// pause the loop and run exclusively.
//
//  Core 0: wifiTask   — RemoteXY engine + UI snapshot toggle
//  Core 1: mainTask   — scheduler, physical buttons, program orchestration
//          Blender1-4 — one task per blender, executes button sequences
//
#include <Arduino.h>
#include <Wire.h>
#include "Blender.h"

// ── RemoteXY ──────────────────────────────────────────────────────────────────
#define REMOTEXY_MODE__WIFI_POINT
#include <WiFi.h>
#include <EEPROM.h>

// Credentials live in the untracked src/secrets.h (copy secrets.h.example).
// Without it the firmware still builds, with these public placeholders:
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define REMOTEXY_WIFI_SSID        "blenders"
#define REMOTEXY_WIFI_PASSWORD    "Xq4Zr8Kv2N"
#define REMOTEXY_ACCESS_PASSWORD  "Tm6Wp3Jd9S"
#endif
#define REMOTEXY_SERVER_PORT      6377

#include <RemoteXY.h>
#include "remotexy_ui.h"

// ── Pin map (XIAO ESP32-S3) ───────────────────────────────────────────────────
#define PIN_I2C_SDA        5                    // D4
#define PIN_I2C_SCL        6                    // D5
#define PIN_BOILER_RELAY   44                   // D7 — simple relay, HIGH = on
static const uint8_t BLENDER_BUTTON_PINS[NUM_BLENDERS] = { 1, 2, 3, 4 };  // D0-D3
                                                // physical buttons to GND, INPUT_PULLUP

#define BUTTON_DEBOUNCE_MS     30
#define BUTTON_LONG_PRESS_MS   500   // default; overridable from UI page 3
#define BUTTON_DOUBLE_PRESS_MS 600   // max gap between the 2 releases of a double press

// ── Debug ─────────────────────────────────────────────────────────────────────
static const bool DEBUG = true;

#define DBG(fmt, ...) do { if (DEBUG) { \
    uint32_t _t = millis(); \
    Serial.printf("[%02lu:%02lu:%02lu:%04lu] " fmt "\n", \
        _t / 3600000UL, (_t % 3600000UL) / 60000UL, \
        (_t % 60000UL) / 1000UL, _t % 1000UL, \
        ##__VA_ARGS__); \
    Serial.flush(); \
} } while(0)

static const char* slotName(int s) {
    if (s >= 0 && s < NUM_BLENDERS) {
        static char buf[12];
        snprintf(buf, sizeof(buf), "Blender%d", s + 1);
        return buf;
    }
    return (s == NUM_BLENDERS) ? "Boiler" : "none";
}

// ── Globals ───────────────────────────────────────────────────────────────────
static Blender*          blenders[NUM_BLENDERS];
static SemaphoreHandle_t remotexyMutex;
static SemaphoreHandle_t wifiReadySem;
static bool              remotexyToggle = false;
static decltype(RemoteXY) snap = {};             // thread-safe UI snapshot

static volatile bool boilerActive = false;

// Requests set from wifiTask (RemoteXY events) and mainTask (physical buttons),
// consumed by mainTask. Single-writer-per-transition bools — no lock needed.
static volatile bool pendingInitial[NUM_BLENDERS] = {};
static volatile bool homogNowRequest = false;

// ── Parameters derived from the UI snapshot ───────────────────────────────────
struct DeviceParams {
    bool    enabled = false;
    int16_t loopS   = 300;
    int16_t tempC   = TEMP_DEFAULT_C;
    int16_t speed   = SPEED_DEFAULT;
};

struct GlobalParams {
    int16_t homogIntervalS   = 0;    // 0 = homogeneity blending disabled
    int16_t homogBlendS      = 30;
    int16_t homogSpeed       = SPEED_DEFAULT;
    int16_t initBlend1S      = 30;
    int16_t meltPauseS       = 60;
    int16_t initBlend2S      = 30;
};

static DeviceParams blenderParams[NUM_BLENDERS];
static DeviceParams boilerParams;
static GlobalParams globalParams;

// Timing helpers used by Blender.cpp (0 in the UI = firmware default).
uint32_t buttonPressMs()    { return snap.btn_press_ms     > 0 ? (uint32_t)snap.btn_press_ms     : BUTTON_PRESS_MS; }
uint32_t buttonPressGapMs() { return snap.btn_press_gap_ms > 0 ? (uint32_t)snap.btn_press_gap_ms : BUTTON_PRESS_GAP_MS; }
static uint32_t buttonLongPressMs() {
    return snap.btn_long_press_ms > 0 ? (uint32_t)snap.btn_long_press_ms : BUTTON_LONG_PRESS_MS;
}

// 0 (unset, e.g. first boot before any UI edit) falls back to def.
static int16_t valOr(int16_t v, int16_t def, int16_t lo, int16_t hi) {
    return (v <= 0) ? def : constrain(v, lo, hi);
}

// ── readRemoteXY ──────────────────────────────────────────────────────────────
static void readRemoteXY() {
    static bool lastToggle = false;

    if (xSemaphoreTake(remotexyMutex, 0) == pdTRUE) {
        if (remotexyToggle != lastToggle) {
            lastToggle = remotexyToggle;
            memcpy(&snap, (const void*)&RemoteXY, sizeof(RemoteXY));
        }
        xSemaphoreGive(remotexyMutex);
    }

    for (int i = 0; i < NUM_BLENDERS; i++) {
        blenderParams[i].enabled = (snap.sw_blender[i] != 0);
        blenderParams[i].loopS   = valOr(snap.loop_s_blender[i], 300, 5, 3600);
        // Unlike the other fields, 0 is a valid value here: heating stays off
        // (the blender still takes part in the blend programs).
        blenderParams[i].tempC   = (snap.temp_blender[i] <= 0)
                                   ? 0 : constrain(snap.temp_blender[i],
                                                   (int16_t)TEMP_MIN_C, (int16_t)TEMP_MAX_C);
        blenderParams[i].speed   = valOr(snap.speed_blender[i], SPEED_DEFAULT, SPEED_MIN, SPEED_MAX);
    }
    boilerParams.enabled = (snap.sw_boiler != 0);
    boilerParams.loopS   = valOr(snap.loop_s_boiler, 300, 5, 3600);

    globalParams.homogIntervalS = (snap.homog_interval_s <= 0)
                                  ? 0 : constrain(snap.homog_interval_s, (int16_t)1, (int16_t)32000);
    globalParams.homogBlendS = valOr(snap.homog_blend_s, 30, BLEND_DURATION_MIN_S, BLEND_TIMER_MAX_S);
    globalParams.homogSpeed  = valOr(snap.homog_speed, SPEED_DEFAULT, SPEED_MIN, SPEED_MAX);
    globalParams.initBlend1S = valOr(snap.init_blend1_s, 5, BLEND_DURATION_MIN_S, BLEND_TIMER_MAX_S);
    globalParams.meltPauseS  = valOr(snap.melt_pause_s, 7, 1, 600);
    globalParams.initBlend2S = valOr(snap.init_blend2_s, 7, BLEND_DURATION_MIN_S, BLEND_TIMER_MAX_S);
}

// ── Job builders ──────────────────────────────────────────────────────────────
static BlenderJob heatJob(int i) {
    BlenderJob job;
    job.type        = JobType::HEAT;
    job.tempC       = blenderParams[i].tempC;
    // Machine heat timer must outlast the slot; the scheduler stops it anyway,
    // so never step it below the machine default (saves MINUS presses).
    job.heatMinutes = max(clampHeatTimerM((int16_t)((blenderParams[i].loopS + 59) / 60)),
                          (int16_t)HEAT_TIMER_DEFAULT_M);
    return job;
}

static BlenderJob initialJob(int i) {
    BlenderJob job;
    job.type    = JobType::INITIAL;
    job.speed   = blenderParams[i].speed;
    job.blend1S = globalParams.initBlend1S;
    job.pauseS  = globalParams.meltPauseS;
    job.blend2S = globalParams.initBlend2S;
    return job;
}

static BlenderJob prepHomogJob() {
    BlenderJob job;
    job.type      = JobType::PREP_HOMOG;
    job.speed     = globalParams.homogSpeed;
    job.durationS = globalParams.homogBlendS;
    return job;
}

// ── Boiler ────────────────────────────────────────────────────────────────────
static void setBoiler(bool on) {
    if (boilerActive == on) return;
    boilerActive = on;
    digitalWrite(PIN_BOILER_RELAY, on ? HIGH : LOW);
    DBG("boiler relay %s", on ? "ON" : "OFF");
}

// ── Round-robin scheduler ─────────────────────────────────────────────────────
// Slots 0..NUM_BLENDERS-1 = blenders, slot NUM_BLENDERS = boiler.
static const int SLOT_BOILER = NUM_BLENDERS;
static const int NUM_SLOTS   = NUM_BLENDERS + 1;

static int      activeSlot  = -1;
static uint32_t slotStartMs = 0;

// A power slot only serves to heat, so a blender with temp 0 (heating off)
// never gets one — it still joins the blend programs via `enabled`.
static bool slotEnabled(int slot) {
    if (slot < NUM_BLENDERS)
        return blenderParams[slot].enabled && blenderParams[slot].tempC > 0;
    return boilerParams.enabled;
}

static int nextSlot(int current) {
    for (int i = 1; i <= NUM_SLOTS; i++) {
        int candidate = (current + i) % NUM_SLOTS;
        if (slotEnabled(candidate)) return candidate;
    }
    return -1;
}

static uint32_t slotDurationMs(int slot) {
    int16_t s = (slot < NUM_BLENDERS) ? blenderParams[slot].loopS : boilerParams.loopS;
    return (uint32_t)max((int16_t)1, s) * 1000UL;
}

static void startSlot(int slot) {
    DBG("startSlot(%s)", slotName(slot));
    slotStartMs = millis();
    if (slot < NUM_BLENDERS) blenders[slot]->sendJob(heatJob(slot));
    else                     setBoiler(true);
}

static void endSlot(int slot) {
    if (slot < 0) return;
    DBG("endSlot(%s)", slotName(slot));
    if (slot < NUM_BLENDERS) blenders[slot]->sendStop();
    else                     setBoiler(false);
}

// ── Homogeneity interval timer ────────────────────────────────────────────────
static bool     homogPending  = false;  // run program 2 at the next slot switch
static bool     homogForce    = false;  // manual request: switch slots right away
static bool     homogTimerOn  = false;
static uint32_t homogLastMs   = 0;

static void handleHomogTimer() {
    if (homogNowRequest) {              // manual "Run now" from UI
        homogNowRequest = false;
        homogPending    = true;
        homogForce      = true;
        DBG("homogeneity cycle requested manually");
    }

    if (globalParams.homogIntervalS <= 0) {
        homogTimerOn = false;
        return;
    }
    uint32_t intervalMs = (uint32_t)globalParams.homogIntervalS * 1000UL;
    if (!homogTimerOn) {
        homogTimerOn = true;
        homogLastMs  = millis();
        return;
    }
    if (millis() - homogLastMs >= intervalMs) {
        homogLastMs  = millis();
        homogPending = true;
        DBG("homogeneity interval elapsed (%d s)", globalParams.homogIntervalS);
    }
}

static bool anyEnabledBlender() {
    for (int i = 0; i < NUM_BLENDERS; i++)
        if (blenderParams[i].enabled) return true;
    return false;
}

// ── System mode (program orchestration) ───────────────────────────────────────
enum class SysMode : uint8_t { LOOP, INITIAL, HOMOG_PREP, HOMOG_RUN };

static SysMode  sysMode        = SysMode::LOOP;
static uint32_t modeStartMs    = 0;
static uint32_t modeDeadlineMs = 0;
static int      initialBlender = -1;
static bool     homogTarget[NUM_BLENDERS] = {};
static int      homogIdx       = -1;
static int      resumeSlot     = -1;   // slot to (re)start when a program ends

// Grace period before trusting a blender's IDLE state after sending a job
// (the job may not have been dequeued yet).
#define JOB_PICKUP_GRACE_MS 1500

static void resumeLoop() {
    sysMode = SysMode::LOOP;
    if (resumeSlot != -1 && slotEnabled(resumeSlot)) {
        activeSlot = resumeSlot;
        startSlot(activeSlot);
    } else {
        activeSlot = -1;                 // scheduler picks the next enabled slot
    }
    resumeSlot = -1;
}

// Pause the loop (stop whatever is powered) and remember where to resume.
static void pauseLoop() {
    resumeSlot = activeSlot;
    endSlot(activeSlot);
    activeSlot = -1;
}

// ── Program 1 orchestration ───────────────────────────────────────────────────
static void startInitialTask(int i) {
    DBG("program 1 start on Blender%d — loop paused", i + 1);
    pauseLoop();
    blenders[i]->sendJob(initialJob(i));
    initialBlender = i;
    modeStartMs    = millis();
    modeDeadlineMs = modeStartMs
        + ((uint32_t)globalParams.initBlend1S + globalParams.meltPauseS
           + globalParams.initBlend2S) * 1000UL + 90000UL;
    sysMode = SysMode::INITIAL;
}

static void tickInitialTask() {
    bool done    = (millis() - modeStartMs > JOB_PICKUP_GRACE_MS)
                   && (blenders[initialBlender]->state() == Blender::State::IDLE);
    bool timeout = (int32_t)(millis() - modeDeadlineMs) > 0;
    if (!done && !timeout) return;

    if (timeout) {
        DBG("program 1 TIMEOUT on Blender%d — forcing stop", initialBlender + 1);
        blenders[initialBlender]->sendStop();
    } else {
        DBG("program 1 complete on Blender%d — loop resumed", initialBlender + 1);
    }
    initialBlender = -1;
    resumeLoop();
}

// ── Program 2 orchestration ───────────────────────────────────────────────────
static void startHomogTask() {
    DBG("program 2 start — loop paused, configuring all enabled blenders");
    for (int i = 0; i < NUM_BLENDERS; i++) {
        homogTarget[i] = blenderParams[i].enabled;
        if (homogTarget[i]) blenders[i]->sendJob(prepHomogJob());
    }
    modeStartMs    = millis();
    modeDeadlineMs = modeStartMs + 60000UL;
    sysMode = SysMode::HOMOG_PREP;
}

static void abortHomogTask(const char* why) {
    DBG("program 2 ABORT (%s)", why);
    for (int i = 0; i < NUM_BLENDERS; i++)
        if (homogTarget[i]) blenders[i]->sendStop();
    homogPending = false;
    homogLastMs  = millis();
    resumeLoop();
}

static void startNextHomogBlender() {
    while (++homogIdx < NUM_BLENDERS && !homogTarget[homogIdx]) {}
    if (homogIdx >= NUM_BLENDERS) {
        DBG("program 2 complete — loop resumed");
        homogPending = false;
        homogLastMs  = millis();
        resumeLoop();
        return;
    }
    DBG("program 2: starting blend on Blender%d", homogIdx + 1);
    BlenderJob job;
    job.type = JobType::START_HOMOG;
    blenders[homogIdx]->sendJob(job);
    modeStartMs    = millis();
    modeDeadlineMs = modeStartMs + (uint32_t)globalParams.homogBlendS * 1000UL + 30000UL;
}

static void tickHomogPrep() {
    bool allReady = true;
    for (int i = 0; i < NUM_BLENDERS; i++)
        if (homogTarget[i] && blenders[i]->state() != Blender::State::HOMOG_READY)
            allReady = false;

    if (allReady) {
        DBG("program 2: all blenders configured — sequential blending");
        homogIdx = -1;
        sysMode  = SysMode::HOMOG_RUN;
        startNextHomogBlender();
        return;
    }
    if ((int32_t)(millis() - modeDeadlineMs) > 0) abortHomogTask("prep timeout");
}

static void tickHomogRun() {
    bool done    = (millis() - modeStartMs > JOB_PICKUP_GRACE_MS)
                   && (blenders[homogIdx]->state() == Blender::State::IDLE);
    bool timeout = (int32_t)(millis() - modeDeadlineMs) > 0;
    if (!done && !timeout) return;

    if (timeout) {
        DBG("program 2 TIMEOUT on Blender%d — forcing stop", homogIdx + 1);
        blenders[homogIdx]->sendStop();
    }
    startNextHomogBlender();
}

// ── Scheduler tick (LOOP mode only) ───────────────────────────────────────────
static void runScheduler() {
    // Program 1 request (physical long press or UI button) pre-empts the loop.
    for (int i = 0; i < NUM_BLENDERS; i++) {
        if (pendingInitial[i]) {
            pendingInitial[i] = false;
            startInitialTask(i);
            return;
        }
    }

    if (activeSlot == -1) {
        int first = nextSlot(NUM_SLOTS - 1);
        // Slot-switch moment: honour a pending homogeneity request first.
        // Checked before the no-slot bail-out: blend-only blenders (temp 0)
        // never own a slot but must still be homogenised.
        if (homogPending) {
            homogForce = false;
            if (anyEnabledBlender()) { resumeSlot = first; startHomogTask(); return; }
            homogPending = false;                // no blender to mix
        }
        if (first == -1) return;                 // nothing to power
        activeSlot = first;
        startSlot(activeSlot);
        return;
    }

    // The interval flag waits for the natural slot switch; only the manual
    // "Run now" request pre-empts the current slot.
    bool expired = (millis() - slotStartMs >= slotDurationMs(activeSlot));
    bool force   = homogForce;
    if (!expired && !force && slotEnabled(activeSlot)) return;

    DBG("scheduler: %s %s", slotName(activeSlot),
        expired ? "slot expired" : (slotEnabled(activeSlot) ? "pre-empted" : "disabled"));
    endSlot(activeSlot);
    int next = nextSlot(activeSlot);
    activeSlot = -1;
    if (next == -1) { DBG("scheduler: all devices disabled"); return; }

    homogForce = false;
    if (homogPending && anyEnabledBlender()) {
        resumeSlot = next;                       // continue the loop here afterwards
        startHomogTask();
        return;
    }
    homogPending = homogPending && anyEnabledBlender();
    activeSlot = next;
    startSlot(activeSlot);
}

// ── Physical buttons ──────────────────────────────────────────────────────────
// Active LOW (INPUT_PULLUP, button to GND).
// Short press  : blender back on its base — restart heating if it owns the slot.
// Double press : 2 short presses — cancel the program the blender is running.
// Long press   : >= buttonLongPressMs() — pause everything, run program 1.

// Cancel whatever program blender i is currently part of. Program 1 resumes
// the loop; for program 2 only this blender drops out, the others continue.
static bool cancelProgramOnBlender(int i) {
    if (sysMode == SysMode::INITIAL && initialBlender == i) {
        DBG("program 1 CANCELLED on Blender%d", i + 1);
        blenders[i]->sendStop();
        initialBlender = -1;
        resumeLoop();
        return true;
    }
    if ((sysMode == SysMode::HOMOG_PREP || sysMode == SysMode::HOMOG_RUN)
        && homogTarget[i]) {
        DBG("program 2 CANCELLED on Blender%d (others continue)", i + 1);
        homogTarget[i] = false;                  // skipped by the run sequencer
        blenders[i]->sendStop();                 // stops it if blending right now
        return true;
    }
    return false;
}

static void handleButtons() {
    static bool     stable[NUM_BLENDERS]      = {};
    static bool     lastRaw[NUM_BLENDERS]     = {};
    static uint32_t lastEdgeMs[NUM_BLENDERS]  = {};
    static uint32_t pressStartMs[NUM_BLENDERS]= {};
    static bool     longFired[NUM_BLENDERS]   = {};
    static uint32_t lastShortMs[NUM_BLENDERS] = {};

    uint32_t now = millis();
    for (int i = 0; i < NUM_BLENDERS; i++) {
        bool raw = (digitalRead(BLENDER_BUTTON_PINS[i]) == LOW);
        if (raw != lastRaw[i]) { lastRaw[i] = raw; lastEdgeMs[i] = now; }
        if (now - lastEdgeMs[i] < BUTTON_DEBOUNCE_MS) continue;

        if (raw && !stable[i]) {                 // pressed
            stable[i]       = true;
            pressStartMs[i] = now;
            longFired[i]    = false;
        } else if (raw && stable[i] && !longFired[i]
                   && now - pressStartMs[i] >= buttonLongPressMs()) {
            longFired[i] = true;                 // long press: program 1
            DBG("button B%d LONG press -> program 1", i + 1);
            pendingInitial[i] = true;
        } else if (!raw && stable[i]) {          // released
            stable[i] = false;
            if (!longFired[i]) {                 // short press
                bool isDouble = (now - lastShortMs[i] <= BUTTON_DOUBLE_PRESS_MS);
                lastShortMs[i] = now;
                if (isDouble && cancelProgramOnBlender(i)) {
                    DBG("button B%d DOUBLE press -> cancel", i + 1);
                    lastShortMs[i] = 0;          // consumed — 3rd press is a new single
                } else {                         // single: back on base
                    DBG("button B%d short press", i + 1);
                    if (sysMode == SysMode::LOOP && activeSlot == i
                        && blenderParams[i].tempC > 0) {
                        DBG("Blender%d owns the slot -> restart heating", i + 1);
                        blenders[i]->sendJob(heatJob(i));
                    }
                }
            }
        }
    }
}

// ── Manual machine buttons (UI page 2) ────────────────────────────────────────
// A rising edge on any of the 20 momentary buttons queues one simulated press
// on that blender. If the blender is mid-sequence the press is discarded by
// its task (logged) — the page is meant for driving an otherwise idle machine.
static void handleManualButtons() {
    static uint8_t last[NUM_BLENDERS * MB_COUNT] = {};
    for (int k = 0; k < NUM_BLENDERS * MB_COUNT; k++) {
        uint8_t v = snap.btn_manual[k];
        if (v && !last[k]) blenders[k / MB_COUNT]->sendManualPress(k % MB_COUNT);
        last[k] = v;
    }
}

// ── Status LEDs (written by wifiTask under the mutex) ─────────────────────────
static volatile uint8_t ledState[NUM_SLOTS] = {};

static void updateLeds() {
    for (int i = 0; i < NUM_BLENDERS; i++)
        ledState[i] = blenders[i]->isActive() ? 1 : 0;
    ledState[SLOT_BOILER] = boilerActive ? 1 : 0;
}

// ── CORE 0 – WiFi + RemoteXY ──────────────────────────────────────────────────
void wifiTask(void*) {
    for (;;) {
        xSemaphoreTake(remotexyMutex, portMAX_DELAY);
        for (int i = 0; i < NUM_BLENDERS; i++) RemoteXY.led_blender[i] = ledState[i];
        RemoteXY.led_boiler = ledState[SLOT_BOILER];
        remotexyToggle = !remotexyToggle;
        xSemaphoreGive(remotexyMutex);

        RemoteXYEngine.handler();

        static bool firstRun = true;
        if (firstRun) {
            DBG("wifiTask: first run complete");
            xSemaphoreGive(wifiReadySem);
            firstRun = false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── CORE 1 – Main (buttons + scheduler + program orchestration) ───────────────
void mainTask(void*) {
    xSemaphoreTake(wifiReadySem, portMAX_DELAY);
    DBG("mainTask: running");

    for (;;) {
        readRemoteXY();
        handleButtons();
        handleManualButtons();
        handleHomogTimer();

        switch (sysMode) {
            case SysMode::LOOP:       runScheduler();   break;
            case SysMode::INITIAL:    tickInitialTask();break;
            case SysMode::HOMOG_PREP: tickHomogPrep();  break;
            case SysMode::HOMOG_RUN:  tickHomogRun();   break;
        }

        updateLeds();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Scan the whole bus and report every ACKing address — distinguishes "chip
// alive but wrong address jumpers" from "nothing on the bus" at a glance.
static void i2cScanReport() {
    int found = 0;
    Serial.print("  I2C scan:");
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf(" 0x%02X", a);
            found++;
        }
    }
    if (found == 0)
        Serial.print(" nothing ACKs — check 3V3/GND, SDA=D4/GPIO5, SCL=D5/GPIO6,"
                     " RESET pin tied to 3V3");
    Serial.println();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    RemoteXY_Init();

    // UI buttons → requests consumed by mainTask (events fire in wifiTask context).
    RemoteXYEngine.addVariableEvent(RemoteXY.btn_prog1_blender[0], []{ if (RemoteXY.btn_prog1_blender[0]) pendingInitial[0] = true; });
    RemoteXYEngine.addVariableEvent(RemoteXY.btn_prog1_blender[1], []{ if (RemoteXY.btn_prog1_blender[1]) pendingInitial[1] = true; });
    RemoteXYEngine.addVariableEvent(RemoteXY.btn_prog1_blender[2], []{ if (RemoteXY.btn_prog1_blender[2]) pendingInitial[2] = true; });
    RemoteXYEngine.addVariableEvent(RemoteXY.btn_prog1_blender[3], []{ if (RemoteXY.btn_prog1_blender[3]) pendingInitial[3] = true; });
    RemoteXYEngine.addVariableEvent(RemoteXY.btn_homog_now,        []{ if (RemoteXY.btn_homog_now)        homogNowRequest   = true; });

    EEPROM.begin(RemoteXYEngine.getEepromSize());

    Serial.begin(115200);
    DBG("setup: Serial ready");

    // Start WiFi/RemoteXY first, ahead of I2C/MCP hardware bring-up. The
    // RemoteXY TCP server only starts listening once wifiTask begins pumping
    // RemoteXYEngine.handler() — if that were delayed behind the MCP23017
    // detection loop below (which halts on a missing chip), the AP would come
    // up and hand out DHCP leases while the app got connection-refused on the
    // RemoteXY port, with no way to see why. Starting it here means the app
    // (and this serial log) stay reachable even while I2C wiring is unfinished.
    remotexyMutex = xSemaphoreCreateMutex();
    wifiReadySem  = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wifiTask, "WiFi", 8192, nullptr, 2, nullptr, 0);
    DBG("setup: WiFi/RemoteXY task created");

    // Boiler relay — off before anything else.
    pinMode(PIN_BOILER_RELAY, OUTPUT);
    digitalWrite(PIN_BOILER_RELAY, LOW);

    // Physical blender buttons.
    for (int i = 0; i < NUM_BLENDERS; i++) pinMode(BLENDER_BUTTON_PINS[i], INPUT_PULLUP);

    // I2C bus — shared by both MCP chips, serialised with i2cMutex.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    i2cMutex = xSemaphoreCreateMutex();

    for (int chip = 0; chip < NUM_MCP_CHIPS; chip++) {
        while (!mcpChips[chip].begin_I2C(MCP_ADDRS[chip])) {
            Serial.printf("MCP23017 chip %d not found at 0x%02X — check wiring/jumpers; retrying...\n",
                          chip, MCP_ADDRS[chip]);
            i2cScanReport();
            delay(1000);
        }
        DBG("setup: MCP chip %d ready at 0x%02X", chip, MCP_ADDRS[chip]);
    }

    // All blender control pins: outputs, idle LOW.
    for (int b = 0; b < NUM_BLENDERS; b++) {
        const BlenderPins& p = BLENDER_PINS[b];
        uint8_t pins[] = { p.minus, p.blend, p.startStop, p.heat, p.plus };
        for (uint8_t pin : pins) {
            mcpChips[p.mcpIdx].pinMode(pin, OUTPUT);
            mcpChips[p.mcpIdx].digitalWrite(pin, LOW);
        }
    }
    DBG("setup: all MCP outputs configured");

    // Blender tasks (i2cMutex must exist first).
    for (int i = 0; i < NUM_BLENDERS; i++) {
        blenders[i] = new Blender(i, BLENDER_PINS[i]);
        blenders[i]->begin(/*priority=*/1, /*core=*/1);
        DBG("setup: Blender %d task created", i + 1);
    }

    xTaskCreatePinnedToCore(mainTask, "Main", 4096, nullptr, 1, nullptr, 1);
    DBG("setup: mainTask created");
}

void loop() {
    // Empty — FreeRTOS tasks own execution on both cores.
}
