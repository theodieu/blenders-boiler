#!/usr/bin/env python3
"""Decode RemoteXY V21 conf blobs with corrected grammar; validate against both samples."""
import re, sys

def load(path):
    src = open(path).read()
    m = re.search(r"RemoteXY_CONF_PROGMEM\[\]\s*=.*?\{(.*?)\}", src, re.S)
    return [int(x) for x in re.findall(r"\d+", m.group(1))]

def decode(data, name):
    print(f"\n===== {name}: {len(data)} bytes =====")
    i = 0
    def rd(n=1):
        nonlocal i
        v = data[i:i+n]
        i += n
        return v if n > 1 else v[0]
    def u16():
        lo, hi = rd(2)
        return lo | (hi << 8)
    def zstr():
        s = []
        while True:
            c = rd()
            if c == 0: break
            s.append(chr(c))
        return ''.join(s)

    ver = rd(); IL = u16(); OL = u16()
    if ver == 0xFF:                       # new-style header: no CV/EE fields
        print(f"ver=0x{ver:02x} IL={IL} OL={OL}")
    else:
        CV = u16(); EE = u16()
        print(f"ver=0x{ver:02x} IL={IL} OL={OL} CV={CV} EE={EE}")
        for _ in range(EE):
            v, s, b = rd(3)
            print(f"  eeprom off={v | ((b & 0x3f) << 8)} size={s | ((b & 0xc0) << 2)}")
    CL = u16()
    print(f"CL={CL} remaining={len(data)-i}")
    print(f"editorVersion={rd()} zeros={rd(3)} name={zstr()!r} pre31={data[i]}")
    rd()  # the 31
    scr = rd(3)
    pagecount = rd()
    print(f"screen={scr} pageCount={pagecount}")

    TYPES = {1,2,4,7,67,70,129,130,131}
    page = 0
    elems_on_page = 0
    counts = []

    # one flag byte per page (1 = main page, 0 = others), then per page:
    # count u16 + elements
    flags = rd(pagecount) if pagecount > 1 else [rd()]
    print(f"page flags={flags}")
    declared = []

    def page_header(first):
        nonlocal page, elems_on_page
        if page > 0:
            counts.append(elems_on_page)
        page += 1
        elems_on_page = 0
        cnt = u16()
        print(f"--- page {page} hdr: declared_count={cnt} ---")
        declared.append(cnt)

    page_header(True)
    while i < len(data):
        t = data[i]
        pos = i
        if t not in TYPES:
            print(f"@{pos}: !!! unknown byte {t}: {data[pos:pos+10]}")
            rd()
            continue
        rd()
        x, y, w, h = rd(4)
        desc = ""
        if t == 130: st = rd(2)
        elif t == 129:
            st = rd(2); desc = repr(zstr())
        elif t == 2:
            st = rd(5); desc = f"{zstr()!r}/{zstr()!r}"
        elif t == 1:
            st = rd(3); desc = repr(zstr())
        elif t == 4: st = rd(3)
        elif t == 7:
            st = rd(4)
            if st[0] == 78: st = st + [rd()]
        elif t == 67:
            st = rd(2)
            if st[0] in (121, 105): st = st + [rd()]
            if st[0] == 105: st = st + [rd()]
        elif t == 70: st = rd(4)
        elif t == 131:
            st = rd(4); desc = repr(zstr())   # st[0] = shape (2=circle, 1=round rect)
            acts, tb, p = [], rd(), 1
            raw_acts = tb
            while tb:
                acts.append(f"p{p}:{['no','show','hide','?'][tb & 3]}")
                tb >>= 2; p += 1
            desc += f" actions={raw_acts} ({','.join(acts)})"
        elems_on_page += 1
        print(f"@{pos:4d} p{page} #{elems_on_page:2d} t{t:<3d} x={x:3d} y={y:3d} w={w:3d} h={h:3d} st={st} {desc}")
        if elems_on_page == declared[-1] and page < pagecount:
            page_header(False)
    counts.append(elems_on_page)
    print(f"page element counts: {counts} (declared {declared}); consumed {i}/{len(data)}")

if __name__ == "__main__":
    import sys
    decode(load(sys.argv[1]), sys.argv[1])
