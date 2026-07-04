# blenders+boiler — hot chocolate dispenser firmware

Firmware for a 4-blender + boiler hot chocolate dispenser on a Seeed Studio
XIAO ESP32-S3. The blenders' front-panel buttons are driven ("fake fingers")
through two MCP23017 I/O expanders; the boiler is a simple relay. A RemoteXY
app over the ESP32's own WiFi access point is the user interface.

**Power constraint:** only ONE device (a blender heating/blending, or the
boiler) draws power at any time. A round-robin scheduler shares the power
between the enabled devices.

## Hardware

Full wiring details and diagram: [docs/wiring.md](docs/wiring.md).

| Function | XIAO ESP32-S3 pin |
|---|---|
| I2C SDA (both MCP23017) | GPIO 5 (D4) |
| I2C SCL (both MCP23017) | GPIO 6 (D5) |
| Boiler relay (HIGH = on) | GPIO 44 (D7) |
| Blender 1 physical button | GPIO 1 (D0) |
| Blender 2 physical button | GPIO 2 (D1) |
| Blender 3 physical button | GPIO 3 (D2) |
| Blender 4 physical button | GPIO 4 (D3) |

Physical buttons wire to **GND** (internal pull-ups, active low).

### MCP23017 mapping (Waveshare boards, address jumpers A2..A0)

| Chip | I2C addr | Jumpers A2 A1 A0 | Port A (pins 0–4) | Port B (pins 11–15) |
|---|---|---|---|---|
| 0 | 0x27 | 1 1 1 (Waveshare default) | Blender 1 | Blender 2 |
| 1 | 0x26 | 1 1 0 (A0 low) | Blender 3 | Blender 4 |

Pin offset within port A: `4=Minus 3=Blend 2=Start/Stop 1=Heat 0=Plus`.
Pin offset within port B: `3=Minus 4=Blend 5=Start/Stop 6=Heat 7=Plus`.
A simulated press drives the pin HIGH for `BUTTON_PRESS_MS` (default 500 ms),
then LOW, then waits `BUTTON_PRESS_GAP_MS` (default 200 ms). Both are
adjustable live from the UI.

## Software architecture

| Task | Core | Role |
|---|---|---|
| `wifiTask` | 0 | RemoteXY engine, publishes status LEDs, flips the snapshot toggle |
| `mainTask` | 1 | scheduler, physical buttons, program 1/2 orchestration |
| `Blender1..4` | 1 | one task per blender; executes button sequences from a job queue |

- **I2C** is serialised with a FreeRTOS mutex; press/gap delays happen outside
  the mutex so all four blenders can sequence concurrently.
- **RemoteXY variables** are copied into a snapshot under a mutex (toggle
  handshake), so core-1 code never reads the live struct.
- **Jobs** sent to a blender task carry all their parameters — blender tasks
  never touch shared settings. `STOP` cancels any sequence mid-wait.

## Behaviour

### Round-robin loop
Each enabled device gets its allocated slot (seconds, from the UI) in turn:
Blender 1 → 2 → 3 → 4 → Boiler → … A blender slot runs the full heating
sequence (Heat → temperature ± steps → Start/Stop → timer ± steps →
Start/Stop); the machine's internal heat timer is set to the slot length
rounded **up** (5–90 min), and the slot ends with the clear-state sequence
(3× Start/Stop). The boiler slot simply closes the relay. Disabling a device
mid-slot ends its slot immediately.

### Physical buttons
- **Short press** — "blender is back on its base": if that blender currently
  owns the loop slot, its heating sequence is restarted immediately.
- **Double press** (2 short presses within 600 ms) — cancels the program the
  blender is running: program 1 stops and the loop resumes; for program 2 only
  that blender drops out, the others continue.
- **Long press** (≥ 500 ms, settable in the UI "Buttons settings" page) —
  pauses everything and runs **program 1** on that blender.

### Program 1 — initial blending task
Loop paused (active device stopped, boiler off), then on the one blender:
clear state → blend phase 1 → melting pause → blend phase 2 → done, loop
resumes (the interrupted slot restarts with a fresh timer). Also triggerable
from the UI (`P1` buttons). Requests arriving while another program runs are
queued and executed afterwards.

### Program 2 — homogeneity blending
A global interval timer (seconds; 0 disables) raises a flag; the flag is acted
on **at the next slot switch** (the UI `Run now` button pre-empts the current
slot instead). The loop pauses, all enabled blenders are configured
*concurrently* (clear state, Blend, speed, Start/Stop, duration — stopping
short of the final start press), then they blend *sequentially*: Start/Stop on
blender k, wait for its machine timer to expire, move to blender k+1. The
interval timer then restarts and the loop resumes where it left off.

Every orchestration step has a timeout (prep 60 s, run = duration + 30 s,
program 1 = total + 90 s); on timeout the affected blender gets a stop
sequence and the system moves on — the loop can never dead-lock on a stuck
sequence.

### Blender machine model (from section 3 of the spec)
- Temperature: default 75 °C, step 5, range 40–100.
- Heat timer: default 10 min, step 5, range 5–90.
- Blend speed: default 5, step 1, range 1–10.
- Blend timer: default 120 s, step 5, range 5 s–180 s.
- Clear state: 3× Start/Stop.
Values from the UI are rounded to the machine step and clamped before the
press counts are computed.

## UI

See [docs/RemoteXY_UI.md](docs/RemoteXY_UI.md) for the full variable
reference, defaults, and how to regenerate the conf array in the RemoteXY
editor. Connect to the WiFi AP configured in `src/secrets.h` (copy
`src/secrets.h.example`; without it, placeholder credentials from
`src/main.cpp` apply), port 6377.

All numeric settings persist in flash (EEPROM). ON/OFF switches intentionally
do **not** persist: after a power cycle everything is off until re-enabled.

## Build & flash

```sh
pio run                 # build
pio run -t upload       # flash
pio device monitor      # 115200 baud, timestamped debug log
```

## Troubleshooting

**App error `ECONNREFUSED` / "connection refused" on port 6377, even though
the phone joined the firmware's WiFi network and got an IP** — this means I2C
hardware isn't fully wired up yet. `wifiTask` (which opens the RemoteXY TCP
listener) is started *before* the MCP23017 bring-up in `setup()` precisely so
the app and serial log stay reachable while you're still wiring; watch the
serial monitor for:

```
MCP23017 chip 0 not found at 0x27 — check wiring/jumpers; retrying...
  I2C scan: 0x26
```

That means the named chip isn't responding at its expected address. The scan
line lists every address that *does* answer on the bus: devices at wrong
addresses point to the jumpers (see [docs/wiring.md](docs/wiring.md)); an
empty scan points to SDA/SCL wiring or power. The firmware retries once a second and
continues automatically once the chip answers; blender tasks (and their MCP
outputs) aren't created until every chip is detected, so blenders 1-4 stay
unavailable until then, but the app connection and UI work the whole time.
