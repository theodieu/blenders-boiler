# Wiring — blenders+boiler

Components:

- 1× Seeed Studio **XIAO ESP32-S3**
- 2× Waveshare **MCP23017 IO Expansion Board** (shared I2C bus)
- 1× relay module for the **boiler** (HIGH = on)
- 4× **physical push buttons** (one per blender, to GND)
- 4× blenders, each with 5 front-panel buttons driven as "fake fingers"

## Overview

```
                                        ┌──────────────────────┐
                                        │    XIAO ESP32-S3     │
                                        │                      │
GND ──┬──o o── Blender 1 button ── D0 ──┤ GPIO1          GPIO44├── D7 ── relay IN ──► Boiler
      ├──o o── Blender 2 button ── D1 ──┤ GPIO2                │
      ├──o o── Blender 3 button ── D2 ──┤ GPIO3           3V3  ├──┬────────────────┐
      └──o o── Blender 4 button ── D3 ──┤ GPIO4           GND  ├──┼──┬─────────────┼──┐
 (buttons' common                       │                      │  │  │             │  │
  ground, same net             ┌── D4 ──┤ GPIO5 (SDA)          │  │  │             │  │
  as XIAO GND)                 │ ┌─ D5 ─┤ GPIO6 (SCL)          │  │  │             │  │
                               │ │      └──────────────────────┘  │  │             │  │
                               │ │                                │  │             │  │
                               │ │   ┌────────────────────────┐   │  │             │  │
                               ├─┼───┤SDA   MCP23017 #0    VCC├───┤  │             │  │
                               │ ├───┤SCL   addr 0x27     GND ├──────┤             │  │
                               │ │   │  PA0..PA4 ──► Blender 1│   │  │             │  │
                               │ │   │  PB3..PB7 ──► Blender 2│   │  │             │  │
                               │ │   └────────────────────────┘   │  │             │  │
                               │ │   ┌────────────────────────┐   │  │             │  │
                               ├─┼───┤SDA   MCP23017 #1    VCC├───┘  │             │  │
                               │ └───┤SCL   addr 0x26     GND ├──────┘             │  │
                               │     │  PA0..PA4 ──► Blender 3│                    │  │
                               │     │  PB3..PB7 ──► Blender 4│    (3V3 & GND also │  │
                               │     └────────────────────────┘     feed the relay─┘  │
                               │                                    module, or 5V     │
                               └─ both boards share the same         if it needs it) ─┘
                                  SDA/SCL wires (I2C bus)
```

## XIAO ESP32-S3 pin usage

| XIAO pin | GPIO | Connects to | Notes |
|---|---|---|---|
| D0 | GPIO 1 | Blender 1 physical button → GND | `INPUT_PULLUP`, active low |
| D1 | GPIO 2 | Blender 2 physical button → GND | `INPUT_PULLUP`, active low |
| D2 | GPIO 3 | Blender 3 physical button → GND | `INPUT_PULLUP`, active low |
| D3 | GPIO 4 | Blender 4 physical button → GND | `INPUT_PULLUP`, active low |
| D4 | GPIO 5 | SDA — both MCP23017 boards | I2C data |
| D5 | GPIO 6 | SCL — both MCP23017 boards | I2C clock |
| D7 | GPIO 44 | Boiler relay module IN | HIGH = boiler on |
| 3V3 | — | MCP23017 VCC (both boards) | keeps I2C at 3.3 V logic |
| GND | — | common ground for everything | buttons, MCP boards, relay |
| 5V | — | relay module VCC (if it is a 5 V module) | see relay note below |

Free for future use: D6 (GPIO 43), D8 (GPIO 7), D9 (GPIO 8), D10 (GPIO 9).

## MCP23017 boards (Waveshare)

Both boards sit on the same I2C bus (SDA, SCL, VCC=3V3, GND daisy-chained).
Only the address jumpers differ:

| Board | A2 A1 A0 jumpers | I2C address | Drives |
|---|---|---|---|
| #0 | 1 1 1 (all high — Waveshare default) | **0x27** | Blenders 1 & 2 |
| #1 | 1 1 0 (A0 low) | **0x26** | Blenders 3 & 4 |

The Waveshare boards already include I2C pull-up resistors — no external
pull-ups needed. Power them from **3V3** so the bus never sees 5 V.

### Expander → blender button mapping

Each blender uses 5 consecutive pins of one port. Firmware pin numbers 0–7 =
PA0–PA7, 8–15 = PB0–PB7.

| Signal | Blender 1 (board #0) | Blender 2 (board #0) | Blender 3 (board #1) | Blender 4 (board #1) |
|---|---|---|---|---|
| Minus | PA4 | PB3 | PA4 | PB3 |
| Blend | PA3 | PB4 | PA3 | PB4 |
| Start/Stop | PA2 | PB5 | PA2 | PB5 |
| Heat | PA1 | PB6 | PA1 | PB6 |
| Plus | PA0 | PB7 | PA0 | PB7 |

Unused: PA5–PA7 and PB0–PB2 on both boards.

### "Fake finger" interface

The expander outputs are plain 3.3 V logic: **HIGH during a simulated press**
(default 500 ms), LOW otherwise, with a 200 ms gap between presses. Use the
same button interface as the working 2-blender build (`blenders_fake_fingers`)
— i.e. whatever couples each output to the blender's front-panel button pads
(typically an optocoupler or transistor across the button contacts, since the
blender's button matrix is not ground-referenced to the ESP32). One coupler
per button, 20 in total. Do **not** wire the MCP outputs directly to the
blender electronics unless you have verified they share a common ground and
the button contacts are 3.3 V-compatible.

## Boiler relay

| Relay module pin | Connects to |
|---|---|
| IN / SIG | XIAO D7 (GPIO 44) |
| VCC | XIAO 5V (typical 5 V relay module) or 3V3 if it is a 3.3 V module |
| GND | XIAO GND |

- Firmware drives GPIO 44 **HIGH to turn the boiler on** and holds it LOW at
  boot before anything else is initialised.
- If you use a 5 V relay module, make sure its IN pin triggers reliably from a
  3.3 V signal (most opto-isolated modules do; jumper it for "high level
  trigger"). Otherwise add a small NPN/MOSFET driver.
- The relay contacts switch the boiler's heating circuit — size the relay for
  the boiler's mains load and keep mains wiring physically separated from the
  logic side.

## Physical blender buttons

One momentary push button per blender, between the GPIO and **GND** (internal
pull-ups are enabled, no external resistor needed):

```
GPIO 1..4 ────o o──── GND
```

- **Short press** — "blender is back on its base": restarts heating if that
  blender currently owns the loop slot.
- **Double press** (2 short presses within 600 ms) — cancels the program that
  blender is running (program 1, or its part of program 2).
- **Hold ≥ 500 ms** (threshold settable in the UI) — pauses the whole system
  and runs program 1 (initial blend) on that blender.

Note: GPIO 3 is an ESP32-S3 strapping pin (JTAG source select). With the
internal pull-up and a button to GND this is harmless — just avoid holding the
Blender 3 button down while the board powers up.

## Power

- The XIAO is powered over USB-C (or 5 V on the 5V pin).
- Everything shares a **common ground**: XIAO, both MCP boards, relay module,
  physical buttons.
- The MCP boards and the ESP32 draw only logic-level current; the only real
  load on the 5V rail is the relay coil (~70–100 mA for a typical module).
- Blenders and boiler have their own mains power — the controller only
  "presses buttons" and closes the relay; it never powers the appliances.
