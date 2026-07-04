#pragma once
#include <Arduino.h>
#include <Adafruit_MCP23X17.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ── Hardware layout ────────────────────────────────────────────────────────────
// 2 MCP23017 chips on the same I2C bus, 2 blenders per chip (one per port).
// Port A offsets: 4=MINUS  3=BLEND  2=START_STOP  1=HEAT  0=PLUS
// Port B offsets: 3=MINUS  4=BLEND  5=START_STOP  6=HEAT  7=PLUS
//
//  Chip 0 (0x27): blender 0 (port A, pins 0–4) and blender 1 (port B, pins 11–15)
//  Chip 1 (0x26): blender 2 (port A, pins 0–4) and blender 3 (port B, pins 11–15)
//
// Waveshare boards default to A2/A1/A0 all high (0x27); chip 1 has A0 low.

static const int     NUM_BLENDERS  = 4;
static const int     NUM_MCP_CHIPS = 2;
static const uint8_t MCP_ADDRS[NUM_MCP_CHIPS] = { 0x27, 0x26 };

// Shared across all Blender tasks — must be created before begin() is called.
extern Adafruit_MCP23X17 mcpChips[NUM_MCP_CHIPS];
extern SemaphoreHandle_t i2cMutex;

// ── Machine constants (blender front-panel behaviour) ─────────────────────────
#define BUTTON_PRESS_MS       500   // default; overridable from the UI
#define BUTTON_PRESS_GAP_MS   200   // default; overridable from the UI
#define STOP_PRESS_COUNT      3     // START_STOP presses that clear any program

#define TEMP_DEFAULT_C        75    // machine default when entering heat mode
#define TEMP_STEP_C           5
#define TEMP_MIN_C            40
#define TEMP_MAX_C            100

#define HEAT_TIMER_DEFAULT_M  10    // machine default heat timer (minutes)
#define HEAT_TIMER_STEP_M     5
#define HEAT_TIMER_MIN_M      5
#define HEAT_TIMER_MAX_M      90

#define SPEED_DEFAULT         5     // machine default when entering blend mode
#define SPEED_MIN             1
#define SPEED_MAX             10

#define BLEND_TIMER_DEFAULT_S 120   // machine default blend timer (seconds)
#define BLEND_TIMER_STEP_S    5
#define BLEND_TIMER_MIN_S     5     // machine timer minimum (machine constraint)
#define BLEND_TIMER_MAX_S     180

// Firmware floor for requested blend durations: the machine timer stays at its
// default while the firmware self-stops, so blends may be shorter than the
// machine's own 5 s timer minimum.
#define BLEND_DURATION_MIN_S  1

#define SEQ_MARGIN_MS         1500  // extra wait after a machine-timed phase

// Resolved at runtime from the RemoteXY snapshot (defined in main.cpp).
uint32_t buttonPressMs();
uint32_t buttonPressGapMs();

// ── BlenderPins ────────────────────────────────────────────────────────────────
struct BlenderPins {
    uint8_t mcpIdx;                 // index into mcpChips[]
    uint8_t minus, blend, startStop, heat, plus;
};

// Port A uses absolute pins 0–7, port B uses 8–15.
static const BlenderPins BLENDER_PINS[NUM_BLENDERS] = {
    { 0,  4,  3,  2,  1,  0 },  // blender 1: chip 0 port A
    { 0, 11, 12, 13, 14, 15 },  // blender 2: chip 0 port B
    { 1,  4,  3,  2,  1,  0 },  // blender 3: chip 1 port A
    { 1, 11, 12, 13, 14, 15 },  // blender 4: chip 1 port B
};

// ── Jobs ───────────────────────────────────────────────────────────────────────
// A job is a complete, self-contained instruction: it carries every parameter
// the sequence needs, so the blender task never reads shared settings.
enum class JobType : uint8_t {
    HEAT,           // full heating sequence, then machine heats on its own
    STOP,           // clear-state sequence (3x START_STOP), return to IDLE
    INITIAL,        // program 1: stop, blend phase 1, pause, blend phase 2
    PREP_HOMOG,     // program 2 setup: stop, blend + speed + timer (no final start)
    START_HOMOG,    // program 2 run: final START_STOP, wait for the machine timer
    PRESS,          // one manual button press from the UI (manualBtn selects it)
};

// PRESS job button indexes (order matches the UI page 2 / btn_manual layout).
enum ManualBtn : uint8_t {
    MB_MINUS = 0, MB_PLUS, MB_BLEND, MB_START_STOP, MB_HEAT, MB_COUNT
};

struct BlenderJob {
    JobType type;
    int16_t tempC       = TEMP_DEFAULT_C;   // HEAT
    int16_t heatMinutes = HEAT_TIMER_DEFAULT_M; // HEAT
    int16_t speed       = SPEED_DEFAULT;    // INITIAL, PREP_HOMOG
    int16_t durationS   = 0;                // PREP_HOMOG blend duration
    int16_t blend1S     = 0;                // INITIAL phase 1
    int16_t pauseS      = 0;                // INITIAL melting pause
    int16_t blend2S     = 0;                // INITIAL phase 2
    uint8_t manualBtn   = 0;                // PRESS: which button (ManualBtn)
};

// ── Blender ────────────────────────────────────────────────────────────────────
// One FreeRTOS task per blender. Jobs are executed to completion (blocking
// inside the task); STOP is honoured mid-sequence via waitWithCancel().
// The scheduler on core 1 polls state() to orchestrate programs 1 and 2.
class Blender {
public:
    enum class State : uint8_t {
        IDLE,             // machine assumed stopped
        HEATING,          // heat sequence done, machine heating on its own timer
        RUNNING_INITIAL,  // program 1 in progress
        HOMOG_READY,      // program 2 configured, waiting for START_HOMOG
        HOMOG_BLENDING,   // program 2 blend running
    };

    Blender(int index, const BlenderPins& pins);

    // Creates the FreeRTOS task. Call after i2cMutex and mcpChips are initialised.
    void begin(UBaseType_t priority = 1, BaseType_t core = 1);

    // Thread-safe: enqueues a job for the blender task (drops if the queue is full).
    void sendJob(const BlenderJob& job);
    void sendStop();
    void sendManualPress(uint8_t btn);      // one PRESS job (ManualBtn index)

    State state() const { return _state; }
    int   index() const { return _index; }

    // True while the machine is actually consuming power (heating or blending).
    bool isActive() const {
        State s = _state;
        return s == State::HEATING || s == State::RUNNING_INITIAL ||
               s == State::HOMOG_BLENDING;
    }

private:
    static void taskEntry(void* arg);
    void run();

    // ── Sequences (run inside the task) ────────────────────────────────────────
    void doHeat(const BlenderJob& job);
    void doInitial(const BlenderJob& job);
    void doPrepHomog(const BlenderJob& job);
    void doStartHomog();
    void doManualPress(uint8_t btn);

    // Clear State Sequence: STOP_PRESS_COUNT x START_STOP.
    void runStopSequence();

    // Configures and starts one blend, then waits durationS. The machine timer
    // is only stepped UP (never below its default): when it exceeds durationS
    // the firmware stops the machine itself at the exact time, saving the
    // MINUS presses; the machine timer remains as a crash backstop.
    // Returns false if cancelled by a STOP job (state already set to IDLE).
    bool blendCycle(int16_t speed, int16_t durationS);

    // Presses BLEND + speed steps + START_STOP + timer steps (no final start).
    // machineTimerS must be a settable machine value (see machineBlendTimerS).
    void configureBlend(int16_t speed, int16_t machineTimerS);

    // Presses PLUS (steps > 0) or MINUS (steps < 0) |steps| times.
    void applySteps(int steps);

    // Delays ms, polling the job queue every 50 ms. Returns false and stops the
    // machine (state = IDLE) if a STOP job arrives; other jobs are discarded.
    bool waitWithCancel(uint32_t ms);

    // Simulated finger: pin HIGH for buttonPressMs(), LOW, then gap delay.
    // I2C writes are serialised through i2cMutex; delays happen outside it.
    void pressBtn(uint8_t pin);

    // ── Members ────────────────────────────────────────────────────────────────
    int            _index;
    BlenderPins    _pins;
    volatile State _state = State::IDLE;
    int16_t        _homogDurationS = 0;   // remembered between PREP and START

    QueueHandle_t  _jobQueue   = nullptr;
    TaskHandle_t   _taskHandle = nullptr;
};

// ── Value helpers (shared with main.cpp) ──────────────────────────────────────
// Round to the nearest machine step and clamp to the machine range.
int16_t clampTempC(int16_t c);
int16_t clampSpeed(int16_t s);
int16_t clampHeatTimerM(int16_t m);
