#include "Blender.h"

// ── Globals defined here, declared extern in Blender.h ────────────────────────
Adafruit_MCP23X17 mcpChips[NUM_MCP_CHIPS];
SemaphoreHandle_t i2cMutex = nullptr;

// ── Debug macro ───────────────────────────────────────────────────────────────
#define BDBG(fmt, ...) do { \
    uint32_t _t = millis(); \
    Serial.printf("[%02lu:%02lu:%02lu:%04lu][B%d] " fmt "\n", \
        _t / 3600000UL, (_t % 3600000UL) / 60000UL, \
        (_t % 60000UL) / 1000UL, _t % 1000UL, \
        _index + 1, ##__VA_ARGS__); \
    Serial.flush(); \
} while(0)

// ── Value helpers ─────────────────────────────────────────────────────────────
static int16_t roundStep(int16_t v, int16_t step) {
    return (int16_t)(((v + step / 2) / step) * step);
}

int16_t clampTempC(int16_t c) {
    return constrain(roundStep(c, TEMP_STEP_C), TEMP_MIN_C, TEMP_MAX_C);
}

int16_t clampSpeed(int16_t s) {
    return constrain(s, SPEED_MIN, SPEED_MAX);
}

// Machine blend timer to program: the smallest settable value >= the desired
// duration, never below the machine default. Whenever it exceeds the desired
// duration the firmware presses STOP itself at the exact time, so leaving the
// timer at the default skips the MINUS presses and the machine timer only
// remains as a backstop (machine still stops itself if the firmware dies).
static int16_t machineBlendTimerS(int16_t durS) {
    if (durS <= BLEND_TIMER_DEFAULT_S) return BLEND_TIMER_DEFAULT_S;
    int16_t r = (int16_t)(((durS + BLEND_TIMER_STEP_S - 1) / BLEND_TIMER_STEP_S)
                          * BLEND_TIMER_STEP_S);
    return min(r, (int16_t)BLEND_TIMER_MAX_S);
}

int16_t clampHeatTimerM(int16_t m) {
    // Round UP so the machine never shuts off before the requested time.
    int16_t r = (int16_t)(((m + HEAT_TIMER_STEP_M - 1) / HEAT_TIMER_STEP_M) * HEAT_TIMER_STEP_M);
    return constrain(r, HEAT_TIMER_MIN_M, HEAT_TIMER_MAX_M);
}

// ── Constructor / task setup ──────────────────────────────────────────────────
Blender::Blender(int index, const BlenderPins& pins)
    : _index(index), _pins(pins) {}

void Blender::begin(UBaseType_t priority, BaseType_t core) {
    _jobQueue = xQueueCreate(8, sizeof(BlenderJob));
    char name[12];
    snprintf(name, sizeof(name), "Blender%d", _index + 1);
    xTaskCreatePinnedToCore(taskEntry, name, 4096, this, priority, &_taskHandle, core);
}

void Blender::sendJob(const BlenderJob& job) {
    if (xQueueSend(_jobQueue, &job, 0) != pdTRUE) {
        BDBG("job queue full — job %d dropped", (int)job.type);
    }
}

void Blender::sendStop() {
    BlenderJob job;
    job.type = JobType::STOP;
    sendJob(job);
}

void Blender::sendManualPress(uint8_t btn) {
    BlenderJob job;
    job.type      = JobType::PRESS;
    job.manualBtn = btn;
    sendJob(job);
}

void Blender::taskEntry(void* arg) {
    static_cast<Blender*>(arg)->run();
}

void Blender::run() {
    BlenderJob job;
    for (;;) {
        xQueueReceive(_jobQueue, &job, portMAX_DELAY);
        switch (job.type) {
            case JobType::STOP:
                if (_state != State::IDLE) {
                    BDBG("STOP");
                    runStopSequence();
                }
                _state = State::IDLE;
                break;
            case JobType::HEAT:        doHeat(job);       break;
            case JobType::INITIAL:     doInitial(job);    break;
            case JobType::PREP_HOMOG:  doPrepHomog(job);  break;
            case JobType::START_HOMOG:
                if (_state == State::HOMOG_READY) doStartHomog();
                else BDBG("START_HOMOG ignored (state=%d)", (int)_state);
                break;
            case JobType::PRESS:       doManualPress(job.manualBtn); break;
        }
    }
}

// One manual press from UI page 2. Runs only between sequences (PRESS jobs
// arriving mid-sequence are discarded by waitWithCancel). Deliberately does
// not touch _state: the user is driving the machine by hand, and the firmware
// cannot know what state that puts it in.
void Blender::doManualPress(uint8_t btn) {
    static const char* NAMES[MB_COUNT] = { "MINUS", "PLUS", "BLEND", "START_STOP", "HEAT" };
    if (btn >= MB_COUNT) return;
    const uint8_t PINS[MB_COUNT] = { _pins.minus, _pins.plus, _pins.blend,
                                     _pins.startStop, _pins.heat };
    BDBG("manual press %s", NAMES[btn]);
    pressBtn(PINS[btn]);
}

// ── Heating sequence ──────────────────────────────────────────────────────────
// HEAT, +/- to temperature, START_STOP, +/- to timer, START_STOP.
// The machine then heats on its own; the scheduler sends STOP at slot end.
void Blender::doHeat(const BlenderJob& job) {
    int16_t tempC   = clampTempC(job.tempC);
    int16_t timerM  = clampHeatTimerM(job.heatMinutes);
    BDBG("HEAT temp=%dC timer=%dmin", tempC, timerM);

    runStopSequence();                              // known clean state first

    pressBtn(_pins.heat);
    applySteps((tempC - TEMP_DEFAULT_C) / TEMP_STEP_C);
    pressBtn(_pins.startStop);                      // switch to timer setting
    applySteps((timerM - HEAT_TIMER_DEFAULT_M) / HEAT_TIMER_STEP_M);
    pressBtn(_pins.startStop);                      // start heating

    _state = State::HEATING;
}

// ── Program 1: initial blending task ──────────────────────────────────────────
// Stop & reset, blend phase 1, melting pause, blend phase 2, done.
void Blender::doInitial(const BlenderJob& job) {
    BDBG("INITIAL blend1=%ds pause=%ds blend2=%ds speed=%d",
         job.blend1S, job.pauseS, job.blend2S, job.speed);
    _state = State::RUNNING_INITIAL;

    runStopSequence();

    if (!blendCycle(job.speed, job.blend1S)) return;    // cancelled

    BDBG("INITIAL melting pause %ds", job.pauseS);
    if (!waitWithCancel((uint32_t)job.pauseS * 1000UL)) return;

    if (!blendCycle(job.speed, job.blend2S)) return;

    BDBG("INITIAL complete");
    _state = State::IDLE;
}

// ── Program 2: homogeneity blending ───────────────────────────────────────────
// PREP configures everything except the final start press, so all blenders can
// be configured concurrently and then started one at a time by the scheduler.
void Blender::doPrepHomog(const BlenderJob& job) {
    int16_t durS = constrain(job.durationS, BLEND_DURATION_MIN_S, BLEND_TIMER_MAX_S);
    BDBG("PREP_HOMOG speed=%d duration=%ds machineTimer=%ds",
         job.speed, durS, machineBlendTimerS(durS));

    runStopSequence();
    configureBlend(job.speed, machineBlendTimerS(durS));

    _homogDurationS = durS;
    _state = State::HOMOG_READY;
}

void Blender::doStartHomog() {
    int16_t machineS = machineBlendTimerS(_homogDurationS);
    BDBG("START_HOMOG %ds (machine timer %ds)", _homogDurationS, machineS);
    pressBtn(_pins.startStop);                      // start blending
    _state = State::HOMOG_BLENDING;

    if (machineS > _homogDurationS) {
        // Machine timer is only the backstop — stop at the exact time ourselves.
        if (!waitWithCancel((uint32_t)_homogDurationS * 1000UL)) return;
        runStopSequence();
    } else {
        // The machine stops itself when its blend timer expires.
        if (!waitWithCancel((uint32_t)_homogDurationS * 1000UL + SEQ_MARGIN_MS)) return;
    }

    BDBG("HOMOG blend done");
    _state = State::IDLE;
}

// ── Blend helpers ─────────────────────────────────────────────────────────────
void Blender::configureBlend(int16_t speed, int16_t machineTimerS) {
    pressBtn(_pins.blend);
    applySteps(clampSpeed(speed) - SPEED_DEFAULT);
    pressBtn(_pins.startStop);                      // switch to timer setting
    applySteps((machineTimerS - BLEND_TIMER_DEFAULT_S) / BLEND_TIMER_STEP_S);
}

bool Blender::blendCycle(int16_t speed, int16_t durationS) {
    int16_t durS     = constrain(durationS, BLEND_DURATION_MIN_S, BLEND_TIMER_MAX_S);
    int16_t machineS = machineBlendTimerS(durS);
    BDBG("blendCycle speed=%d duration=%ds machineTimer=%ds",
         clampSpeed(speed), durS, machineS);

    configureBlend(speed, machineS);
    pressBtn(_pins.startStop);                      // start blending

    if (machineS > durS) {
        // Machine timer is only the backstop — stop at the exact time ourselves.
        if (!waitWithCancel((uint32_t)durS * 1000UL)) return false;
        runStopSequence();
        return true;
    }
    // The machine stops itself when its blend timer expires.
    return waitWithCancel((uint32_t)durS * 1000UL + SEQ_MARGIN_MS);
}

void Blender::applySteps(int steps) {
    uint8_t pin   = (steps >= 0) ? _pins.plus : _pins.minus;
    int     count = (steps >= 0) ? steps : -steps;
    for (int i = 0; i < count; i++) pressBtn(pin);
}

// ── Stop / cancel ─────────────────────────────────────────────────────────────
void Blender::runStopSequence() {
    for (int i = 0; i < STOP_PRESS_COUNT; i++) pressBtn(_pins.startStop);
    vTaskDelay(pdMS_TO_TICKS(buttonPressMs()));     // settling delay
}

bool Blender::waitWithCancel(uint32_t ms) {
    uint32_t deadline = millis() + ms;
    while ((int32_t)(deadline - millis()) > 0) {
        uint32_t remaining = deadline - millis();
        BlenderJob job;
        if (xQueueReceive(_jobQueue, &job,
                          pdMS_TO_TICKS(min(remaining, (uint32_t)50))) == pdTRUE) {
            if (job.type == JobType::STOP) {
                BDBG("cancelled by STOP");
                runStopSequence();
                _state = State::IDLE;
                return false;
            }
            BDBG("job %d discarded (busy)", (int)job.type);
        }
    }
    return true;
}

// ── Simulated finger ──────────────────────────────────────────────────────────
// The hold and gap delays are OUTSIDE the mutex so the other blender tasks can
// perform their own I2C transactions concurrently.
void Blender::pressBtn(uint8_t pin) {
    xSemaphoreTake(i2cMutex, portMAX_DELAY);
    mcpChips[_pins.mcpIdx].digitalWrite(pin, HIGH);
    xSemaphoreGive(i2cMutex);

    vTaskDelay(pdMS_TO_TICKS(buttonPressMs()));

    xSemaphoreTake(i2cMutex, portMAX_DELAY);
    mcpChips[_pins.mcpIdx].digitalWrite(pin, LOW);
    xSemaphoreGive(i2cMutex);

    vTaskDelay(pdMS_TO_TICKS(buttonPressGapMs()));
}
