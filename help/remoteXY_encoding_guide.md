# RemoteXY conf encoding — reverse-engineered reference

Everything in this guide was derived by byte-diffing working RemoteXY editor
exports (editor at <https://remotexy.com>), validated with the round-trip
decoder in `tools/decode_remotexy_conf.py`, and confirmed on a real phone app
against the firmware in this repo. It documents the `RemoteXY_CONF_PROGMEM[]`
byte array: what each byte means, who parses it, and the traps that produce
"GUI has unknown elements" or visually broken pages.

Evidence base (all decode with zero errors under this grammar):

| Sample | Pages | Header | Blob version |
|---|---|---|---|
| `AC_Dimmer` export | 1 | `254` | 21 |
| `blenders_fake_fingers` export | 2 | `254` | 21 |
| Fresh 3-page test export (2026-07) | 3 | `255` | 19 |
| This repo's generated conf | 3 | `254` | 19 |

## 1. Two independent layers

The conf array contains **two things stacked**, parsed by **different
consumers**:

```
[ device header ][ GUI blob ]
```

- The **device header** is read only by the RemoteXY *Arduino library* on the
  microcontroller. It sizes the input/output buffers and the EEPROM
  persistence area. The library treats the GUI blob as opaque bytes to ship to
  the phone.
- The **GUI blob** is read only by the *phone app*, which receives it over the
  wire. The app never sees the device header.

Because the layers are independent, they can be mixed: this repo ships a
`254`-style header (which carries the EEPROM table the firmware needs) with a
blob declaring version 19 (matching the current editor/app). That combination
is verified working.

## 2. Device header

Two formats, selected by the first byte:

### `254` (older exports, used by this repo)

```
254, IL u16, OL u16, CV u16, EE u16,
EE × ( offLo u8, sizeLo u8, packed u8 ),
CL u16
```

- `IL` — total bytes of **input** variables (app → device).
- `OL` — total bytes of **output** variables (device → app).
- `CV` — complex-variable count (always 0 in everything seen so far).
- `EE` — number of EEPROM-persisted variables, then one 3-byte entry each:
  - offset = `offLo | ((packed & 0x3F) << 8)`
  - size = `sizeLo | ((packed & 0xC0) << 2)`
  - The *offset* is the variable's byte offset inside the input area, so
    reordering elements changes offsets — see §7.
- `CL` — length of the GUI blob that follows.

### `255` (current editor exports)

```
255, IL u16, OL u16, CL u16
```

No CV/EE fields. How EEPROM persistence is encoded in this format is **not yet
known** (the only 255 sample had no persisted fields) — one reason this repo
keeps using the 254 header.

All u16 values are little-endian throughout.

## 3. GUI blob

```
VER, 0, 0, 0,
project name as z-string (may be empty: just the 0),
31,                        ← constant, meaning unknown
1, W, H,                   ← screen block: 1 = portrait?, W×H canvas units
pageCount u8,
pageCount × flag u8,       ← 1 for the main page, 0 for every other page
then per page:
    elementCount u16,
    the page's elements back to back
```

- `VER`: 21 in older exports, 19 in current ones. The element byte templates
  are **identical** between the two — only pick one and be consistent. The
  phone app accepts both.
- Canvas: all known exports use 106×200 (portrait phone). Element x/y/w/h are
  in these canvas units.
- **The per-page flag array is the classic trap**: it is exactly `pageCount`
  bytes (`[1]`, `[1,0]`, `[1,0,0]`, …). Emitting the wrong number of bytes
  shifts everything after it and the app reports **"GUI has unknown
  elements"**. (It looks deceptively like a "page id + count" header — it is
  not; pages have no ids in the conf.)
- Pages after the first have **no header of their own** beyond their
  `count u16`.

## 4. Variable binding

There are no names or ids anywhere — binding is purely positional:

- Input variables bind to input elements **in order of appearance across all
  pages** (page 1's elements first, then page 2's, …).
- Output variables bind to output elements the same way, as an independent
  sequence.
- Labels, rectangles/panels, and page-nav buttons bind **nothing** (verified
  by IL/OL byte accounting on real exports).

The C struct next to the conf must list fields in exactly that order, packed
(`#pragma pack(push,1)`): inputs first (`IL` bytes), then outputs (`OL`
bytes). Get the order wrong and values silently land in the wrong fields — no
error anywhere.

## 5. Element templates (byte-verified)

Common shape: `type u8, x, y, w, h, style bytes…, [text z-string], [extras]`.

| Element | Bytes | Variable |
|---|---|---|
| Button | `1, x,y,w,h, 0,2,31, text,0` | input u8 (1 while pressed) |
| Switch | `2, x,y,w,h, 0,2,26,31,31, 'ON',0,'OFF',0` | input u8 |
| Slider | `4, x,y,w,h, 160,2,26` | input u8 |
| Edit, int16 | `7, x,y,w,h, 85,64,2,26` | input i16 |
| Edit, float | `7, x,y,w,h, 78,64,2,26, decimals` | input float (4 B) |
| Text out, int8 | `67, x,y,w,h, 121,242,26` | output i8 |
| Text out, float | `67, x,y,w,h, 105,242,26, decimals` | output float |
| LED (green) | `70, x,y,w,h, 16,26,37,0` | output u8 (0/1) |
| Label | `129, x,y,w,h, 64, STYLE, text,0` | none |
| Panel/rect | `130, x,y,w,h, 9,17` | none |
| Page button | `131, x,y,w,h, SHAPE,17,2,31, text,0, ACTIONS` | none |

Notes:

- Label `STYLE`: `17` = title (use h≈5), `242` = small text (h≈4). Text width
  ≈ 2.6 canvas units per character at the small size — make the box wide
  enough or the app squeezes it.
- The style bytes are color/theme constants copied verbatim from editor
  exports (e.g. `2,26` dark theme, `31` white text, LED `16,26,37` green). If
  you want other colors, export a sample from the editor and diff.
- Text is plain ASCII, z-terminated, embedded directly in the element.

## 6. Page-nav buttons (element 131) — the tricky one

```
131, x, y, w, h, SHAPE, 17, 2, 31, caption, 0, ACTIONS
```

- `SHAPE`: 2 = circle, 1 = rounded rectangle (matches the editor's "shape"
  property).
- `ACTIONS` — the byte that took four samples to crack. It is **not a target
  page id**. It is a per-page visibility mask, 2 bits per page, page *N* at
  bits `2·(N−1)`:

  | 2-bit value | Editor option | Meaning |
  |---|---|---|
  | 0 | "no" | leave that page as it is |
  | 1 | "show" | show that page |
  | 2 | "hide" | hide that page |

  Worked examples (3-page project):
  - open page 2 exclusively: hide p1, show p2, hide p3 → `2 + (1<<2) + (2<<4)` = **38**
  - open page 3 exclusively: `2 + (2<<2) + (1<<4)` = **26**
  - back to page 1: `1 + (2<<2) + (2<<4)` = **41**
  - 2-page project: "Settings" = 6 (hide p1, show p2), "Exit" = 9 (show p1,
    hide p2).

- **Always pair `show` with `hide` for every other page.** Using only the
  show bit (editor option "no" for the rest) leaves the previous page's
  elements on screen and the new page renders **on top of it** — the pages
  visually stack. This was observed on a real device.
- Capacity limit: 2 bits × pages must fit the byte, so this encoding covers
  at most 4 pages. What a ≥5-page export looks like is unknown — get an editor
  sample before attempting one.

## 7. EEPROM persistence gotchas

- Enable persistence only on the fields that should survive power-loss. This
  project deliberately does **not** persist ON/OFF switches (devices must not
  restart heating unattended after an outage) but does persist numeric edits.
- The EEPROM table addresses variables **by byte offset in the input area**,
  so inserting/moving/removing any input element changes the offsets of
  everything after it. After such a change, previously stored values may map
  onto different fields or reset to 0 — re-check settings in the app after
  reflashing. Appending new inputs at the end is the safe pattern.
- Device side, size the EEPROM with
  `EEPROM.begin(RemoteXYEngine.getEepromSize())` — the library derives it from
  the header's EE table.

## 8. Field-tested failure modes

| Symptom | Cause seen |
|---|---|
| App: "GUI has unknown elements" | page-flag array has wrong byte count (§3); or an invalid byte inside an element (e.g. shape=3) desyncs the parser |
| Pages render stacked on each other | nav ACTIONS uses show without hiding the other pages (§6) |
| Values land in wrong struct fields | struct order ≠ element order (§4) |
| Stored settings scrambled after reflash | input elements reordered, EEPROM offsets moved (§7) |
| Phone connects to WiFi but app gets connection refused | firmware issue, not conf: the TCP listener only starts once `RemoteXYEngine.handler()` is being pumped — start that task before any blocking hardware bring-up |

## 9. Tooling in this repo

- `tools/gen_remotexy_conf.py` — builds this project's conf from the element
  helpers documented above (edit the layout there, never the array by hand);
  writes `tools/conf_array.txt` and prints IL/OL/EEPROM stats. Paste/splice
  the result into `src/remotexy_ui.h` and keep the struct in sync.
- `tools/decode_remotexy_conf.py <file>` — decodes any file containing a
  `RemoteXY_CONF…[] = { … }` array (handles both header formats) and prints
  every element with position, style, text and decoded nav-button action
  masks. Two invariants to check after generating: **zero "unknown byte"
  lines** and **"consumed N/N"** at the end, with element counts matching the
  declared per-page counts.

## 10. Method, for the next unknown byte

The whole format fell to one technique: **make the editor produce two exports
that differ in exactly one property, and diff the bytes**. Shape circle vs
rounded rectangle → found the shape byte. Two page buttons identical except
target → found the actions byte. "no" vs "hide" for other pages → decoded the
mask values. When a new element or a ≥5-page layout is needed, build a minimal
sample in the editor, export, run the decoder, and extend the templates —
don't guess: two of the three bugs this project hit came from extrapolating
past the verified samples.
