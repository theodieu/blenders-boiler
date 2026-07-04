#!/usr/bin/env python3
"""Generate the RemoteXY V21 conf array for the 4-blender + boiler UI.

Byte templates are copied verbatim from two known-working editor-generated
confs (blenders_fake_fingers and AC_Dimmer, both editorVersion 21):
  switch   : 2,x,y,w,h, 0,2,26,31,31, 'ON',0,'OFF',0
  edit i16 : 7,x,y,w,h, 85,64,2,26
  button   : 1,x,y,w,h, 0,2,31, text,0
  led      : 70,x,y,w,h, 16,26,37,0
  label    : 129,x,y,w,h, 64,STYLE, text,0     (STYLE 17=title, 242=small)
  navbtn   : 131,x,y,w,h, SHAPE,17,2,31, text,0, ACTIONS   (page switch, NOT a
             variable; SHAPE 2=circle 1=rounded rect; ACTIONS = per-page
             visibility mask, 2 bits per page at bits 2*(page-1):
             0=leave as is, 1=show, 2=hide. Show the target and hide all other
             pages, else pages stack. Byte-verified against 3-page exports.)
Pages, portrait 106x200 (byte-verified against 1/2/3-page editor exports):
  blob = VER,0,0,0, name,0, 31, 1,106,200, pageCount,
         pageCount flag bytes (1 = main page, 0 for the others),
         then per page: count u16, elements
  VER: old exports 21, current editor 19 — element templates identical.
Input/output variables bind to input/output elements in order of appearance
across ALL pages (navbtns and labels don't count).
Header (device-side, parsed by the library, opaque to the app):
  254, IL u16, OL u16, 0,0 (complex vars), EE u16, EE*(off,size,hi), CL u16, blob
"""

elems = []          # (is_input_var, is_output_var, bytes) or PAGE marker
PAGE = "PAGE"

def next_page():
    elems.append(PAGE)

def label(x, y, w, text, title=False):
    h = 5 if title else 4
    st = 17 if title else 242
    elems.append((0, 0, [129, x, y, w, h, 64, st] + [ord(c) for c in text] + [0]))

def switch(x, y):
    elems.append((1, 0, [2, x, y, 20, 11, 0, 2, 26, 31, 31,
                         ord('O'), ord('N'), 0, ord('O'), ord('F'), ord('F'), 0]))

def edit(x, y, w=20):
    elems.append((2, 0, [7, x, y, w, 6, 85, 64, 2, 26]))

def button(x, y, w, h, text):
    elems.append((1, 0, [1, x, y, w, h, 0, 2, 31] + [ord(c) for c in text] + [0]))

def led(x, y):
    elems.append((0, 1, [70, x, y, 7, 7, 16, 26, 37, 0]))

NUM_PAGES = 3

def nav_actions(target):
    # 2 bits per page: 1 = show, 2 = hide. Show only the target page.
    b = 0
    for p in range(1, NUM_PAGES + 1):
        b |= (1 if p == target else 2) << (2 * (p - 1))
    return b

def navbtn(x, y, w, h, text, target, shape=2):
    # target = 1-based page number to open; shape 2=circle, 1=rounded rect
    elems.append((0, 0, [131, x, y, w, h, shape, 17, 2, 31]
                        + [ord(c) for c in text] + [0, nav_actions(target)]))

COL = [4, 29, 54, 79]   # 4 blender columns, elements w=20

# ── Page 1: settings + programs ──────────────────────────────────────────────
# Blender grid: name, active-device LED right under it, then the ON/OFF switch.
for i, x in enumerate(COL):
    label(x, 2, 22, f"Blender {i+1}", title=True)
# outputs 1-4: blender active LEDs
for x in COL:
    led(x + 6, 8)
# inputs 1-4: switches (order matters: variable elements must match struct order)
for x in COL:
    switch(x, 16)
# input 5: boiler switch (visually in boiler section below)
label(4, 86, 30, "Boiler", title=True)
switch(4, 92)
# inputs 6-9: loop slot edits
label(19, 29, 68, "Loop slot time (s, 0=300)")
for x in COL:
    edit(x, 34)
# input 10: boiler loop slot edit
label(41, 92, 62, "Loop slot time (s, 0=300)")
edit(45, 97, 30)
# inputs 11-14: heat temperature
label(8, 42, 90, "Heat temperature (0=off, 40-100 C)")
for x in COL:
    edit(x, 47)
# inputs 15-18: blend speed
label(22, 55, 62, "Blend speed (1-10, 0=5)")
for x in COL:
    edit(x, 60)
# inputs 19-21: homogeneity blending globals
label(4, 107, 98, "Homogeneity blending - program 2", title=True)
label(4, 114, 62, "Interval (s, 0=off)");      edit(70, 113, 30)
label(4, 122, 62, "Blend duration (s, 0=30)"); edit(70, 121, 30)
label(4, 130, 62, "Blend speed (1-10, 0=5)");  edit(70, 129, 30)
# inputs 22-24: initial blending task globals
label(4, 149, 98, "Initial blend - program 1", title=True)
label(4, 156, 62, "Phase 1 blend (s, 0=5)");   edit(70, 155, 30)
label(4, 164, 62, "Melting pause (s, 0=7)");   edit(70, 163, 30)
label(4, 172, 62, "Phase 2 blend (s, 0=7)");   edit(70, 171, 30)
# inputs 25-28: program-1 trigger buttons, one per blender
label(25, 68, 56, "Run program 1 (initial blend)")
for x in COL:
    button(x, 73, 20, 9, "P1")
# input 29: run homogeneity cycle now
button(70, 137, 30, 9, "Run now")
# output 5: boiler LED, beside its switch
led(28, 94)
# page links (not variables)
navbtn( 4, 189, 47, 9, "Fake fingers test", 2)
navbtn(55, 189, 47, 9, "Buttons settings",  3)

# ── Page 2: manual machine buttons (fake fingers test) ───────────────────────
# 5 momentary buttons per blender in quincunx: minus/plus on top, centred over
# the gaps of the blend / start-stop / heat row below. Inputs 30-49.
next_page()
navbtn(4, 3, 40, 8, "< Back", 1)
label(46, 5, 58, "Manual machine buttons")
for i in range(4):
    y0 = 14 + i * 46
    label(4, y0, 40, f"Blender {i+1}", title=True)
    button(20, y0 + 6, 32, 12, "-")
    button(54, y0 + 6, 32, 12, "+")
    button( 4, y0 + 19, 32, 12, "Blend")
    button(37, y0 + 19, 32, 12, "Start/Stop")
    button(70, y0 + 19, 32, 12, "Heat")

# ── Page 3: buttons settings ─────────────────────────────────────────────────
# inputs 50-52
next_page()
navbtn(4, 3, 40, 8, "< Back", 1)
label(4, 18, 98, "Blenders fake finger timing", title=True)
label(4, 26, 62, "Press (ms, 0=500)");         edit(70, 25, 30)
label(4, 34, 62, "Gap (ms, 0=200)");           edit(70, 33, 30)
label(4, 46, 98, "Press buttons timing", title=True)
label(4, 54, 62, "Long press (ms, 0=500)");    edit(70, 53, 30)

# ── Assemble ─────────────────────────────────────────────────────────────────
NAME = "blenders+boiler"

pages = [[]]
for e in elems:
    if e == PAGE: pages.append([])
    else:         pages[-1].append(e)

IN_SIZES  = []   # per input element, bytes in struct (across all pages, in order)
OUT_SIZES = []
page_bodies = []
for p in pages:
    body = []
    for kind_in, kind_out, b in p:
        body += b
        if kind_in:  IN_SIZES.append(kind_in)  # 1=uint8, 2=int16
        if kind_out: OUT_SIZES.append(kind_out)
    page_bodies.append((len(p), body))

IL = sum(IN_SIZES)
OL = sum(OUT_SIZES)

# EEPROM persistence: every int16 edit (not switches/buttons — safety after
# power loss: devices come back disabled).
ee = []
off = 0
for s in IN_SIZES:
    if s == 2:
        ee.append((off, 2))
    off += s

EDITOR_VERSION = 19    # matches the current editor's exports

blob = [EDITOR_VERSION, 0, 0, 0] + [ord(c) for c in NAME] + [0] + [31, 1, 106, 200]
blob += [len(pages)] + [1] + [0] * (len(pages) - 1)   # per-page flag, 1 = main
for cnt, body in page_bodies:
    blob += [cnt & 0xFF, cnt >> 8] + body
CL = len(blob)

conf = [254, IL & 0xFF, IL >> 8, OL & 0xFF, OL >> 8, 0, 0,
        len(ee) & 0xFF, len(ee) >> 8]
for o, s in ee:
    conf += [o & 0xFF, s & 0xFF, ((o >> 8) & 0x3F) | (((s >> 2) & 0xC0))]
conf += [CL & 0xFF, CL >> 8] + blob

print(f"// IL={IL} OL={OL} eeprom={len(ee)} elements={len(elems)} total={len(conf)} bytes")
lines = []
for i in range(0, len(conf), 20):
    lines.append("  " + ",".join(str(b) for b in conf[i:i+20]))
arr = ",\n".join(lines)
out = (f"uint8_t const PROGMEM RemoteXY_CONF_PROGMEM[] =   // {len(conf)} bytes, "
       f"generated, blob V{EDITOR_VERSION}\n  {{ {arr.lstrip()} }};\n")
import os
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "conf_array.txt")
open(out_path, "w").write(out)
print(out[:400])
print(f"input elements: {len(IN_SIZES)}  output elements: {len(OUT_SIZES)}")
print(f"eeprom offsets: {ee}")
