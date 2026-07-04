# RemoteXY UI Reference — blenders+boiler

Three pages (portrait, 106×200 canvas): page 1 holds the device settings and
program triggers; page 2 ("Fake fingers test") exposes the five machine buttons
of each blender for manual presses; page 3 ("Buttons settings") holds the
button timing settings. The phone app connects to the ESP32's own access point:

| | |
|---|---|
| WiFi AP / password | set in the untracked `src/secrets.h` (copy `secrets.h.example`) |
| App access password | set in `src/secrets.h` |
| Server port | `6377` |

Without a `secrets.h`, the firmware builds with the public placeholder
credentials defined in `src/main.cpp` (AP `blenders`).

## Page 1 layout (top to bottom)

1. **Blender grid** — four columns (Blender 1–4): name, active-device LED
   (lit while heating/blending), ON/OFF switch, loop slot time, heat
   temperature, blend speed, and a `P1` button per blender.
2. **Boiler** — ON/OFF switch (boiler active LED beside it), loop slot time.
3. **Homogeneity blending (program 2)** — interval, duration, speed, `Run now`.
4. **Initial blend (program 1)** — phase 1 / melting pause / phase 2 durations.
5. **Page links** (bottom) — `Fake fingers test` (page 2), `Buttons settings`
   (page 3).

## Page 2 layout — "Fake fingers test" (manual machine buttons)

`< Back` returns to page 1. Below it, one block per blender with its five
machine buttons in quincunx: `-` / `+` on top, `Blend` / `Start/Stop` / `Heat`
underneath. Each tap queues exactly one simulated press on that blender (press
duration/gap from the page 3 timing settings). Presses are executed only when
the blender's task is idle — during a running program or heating sequence they
are discarded (visible in the serial log). The firmware does not track what
state manual presses leave the machine in; the programs always start with the
3× Start/Stop clear-state sequence, which recovers from anything.

## Page 3 layout — "Buttons settings"

`< Back` returns to page 1. Two sections:

- **Blenders fake finger timing** — simulated press duration and gap between
  presses (used for every MCP23017-driven button press).
- **Press buttons timing** — long-press threshold of the physical blender
  buttons (hold longer than this to trigger program 1).

## Input variables

`0` in any numeric field means "use the firmware default" (all fields are 0 on
first boot). Values outside the valid range are clamped by the firmware.
Exceptions: for `homog_interval_s` and `temp_blender` a 0 means **off**.

| Struct field | Element | Range (clamped) | Default when 0 | Saved to flash | Meaning |
|---|---|---|---|---|---|
| `sw_blender[4]` | switch ×4 | 0/1 | — | no | include blender in the round-robin loop |
| `sw_boiler` | switch | 0/1 | — | no | include boiler in the loop |
| `loop_s_blender[4]` | edit ×4 | 5–3600 s | 300 s | yes | allocated active slot per blender |
| `loop_s_boiler` | edit | 5–3600 s | 300 s | yes | allocated active slot for the boiler |
| `temp_blender[4]` | edit ×4 | 40–100 °C (step 5), **0 = heating off** | 0 (heating off) | yes | target heating temperature; at 0 the blender gets no loop slot but still joins the blend programs |
| `speed_blender[4]` | edit ×4 | 1–10 | 5 | yes | blend speed used by program 1 |
| `homog_interval_s` | edit | 1–32000 s, **0 = off** | 0 (off) | yes | program 2 interval (measured from the end of one cycle) |
| `homog_blend_s` | edit | 1–180 s | 30 s | yes | program 2 blend duration |
| `homog_speed` | edit | 1–10 | 5 | yes | program 2 blend speed |
| `init_blend1_s` | edit | 1–180 s | 5 s | yes | program 1 phase 1 blend |
| `melt_pause_s` | edit | 1–600 s | 7 s | yes | program 1 melting pause |
| `init_blend2_s` | edit | 1–180 s | 7 s | yes | program 1 phase 2 blend |
| `btn_prog1_blender[4]` | button `P1` ×4 | momentary | — | no | run program 1 on that blender (same as a physical long press) |
| `btn_homog_now` | button `Run now` | momentary | — | no | run program 2 immediately (pre-empts the current slot) |
| `btn_manual[20]` | button ×20 (page 2) | momentary | — | no | one simulated press; index = blender×5 + {0=minus, 1=plus, 2=blend, 3=start/stop, 4=heat} |
| `btn_press_ms` | edit (page 3) | > 0 ms | 500 ms | yes | simulated button hold time |
| `btn_press_gap_ms` | edit (page 3) | > 0 ms | 200 ms | yes | gap between simulated presses |
| `btn_long_press_ms` | edit (page 3) | > 0 ms | 500 ms | yes | physical button long-press threshold (program 1 trigger) |

Switches are deliberately **not** persisted: after a power cycle every device
comes back disabled, so heating never restarts unattended.

## Output variables

| Struct field | Element | Meaning |
|---|---|---|
| `led_blender[4]` | LED ×4 (under each blender name) | lit while that blender is powered (heating or blending) |
| `led_boiler` | LED (beside the boiler switch) | lit while the boiler relay is on |

## About the conf array

The full reverse-engineered byte format (headers, page framing, element
templates, nav-button action masks, failure modes) is documented in
[help/remoteXY_encoding_guide.md](../help/remoteXY_encoding_guide.md).

`src/remotexy_ui.h` contains the `RemoteXY_CONF_PROGMEM` byte array. It was
**not** produced by the RemoteXY editor — it was generated programmatically
using byte templates extracted from two known-working editor (V21) exports, and
validated with a round-trip decoder. Verify the rendering once in the app; if
anything looks off, or the layout needs to change, rebuild the page at
<https://remotexy.com> with the element list above.

Rules when regenerating in the editor:

- Keep the **order of input elements** identical to the input-variable order in
  the table above, across both pages — page 1 elements first, then page 2
  (RemoteXY binds variables by element order, not by name).
- Keep the **order of output elements** identical to the output table.
- Page-switch buttons (`Fake fingers test`, `Buttons settings`, `< Back`) are
  *page* elements — they bind no variable.
- Element types: switches → *switch*, numeric settings → *edit field, integer
  (int16)*, `P1`/`Run now` → *button*, indicators → *LED*.
- Enable "save to flash/EEPROM" on all edit fields (and only on them) to match
  the firmware's `EEPROM.begin(RemoteXYEngine.getEepromSize())` sizing.
- Paste the editor's struct field names back into `remotexy_ui.h`, or keep the
  existing names and just replace the array (sizes must match: 72 input bytes,
  5 output bytes).
