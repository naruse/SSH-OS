#!/usr/bin/env python3
"""Render the boot-block text into 1-bit bitmaps for boot/boot.S.

Usage: mkbanner.py OUTPUT.inc [OUTPUT.c]

Each glyph is 8x8, drawn here as ASCII art (X = black pixel). The output is
an assembler include with one label per piece of text. Every row of a bitmap
is a whole number of bytes, so the boot block can copy it straight into video
memory with no shifting.
"""
import os
import sys

GLYPH_ART = """
A
..XXX...
.XX.XX..
XX...XX.
XX...XX.
XXXXXXX.
XX...XX.
XX...XX.
........
B
XXXXXX..
XX...XX.
XX...XX.
XXXXXX..
XX...XX.
XX...XX.
XXXXXX..
........
C
..XXXX..
.XX..XX.
XX......
XX......
XX......
.XX..XX.
..XXXX..
........
D
XXXXX...
XX..XX..
XX...XX.
XX...XX.
XX...XX.
XX..XX..
XXXXX...
........
E
XXXXXXX.
XX......
XX......
XXXXX...
XX......
XX......
XXXXXXX.
........
F
XXXXXXX.
XX......
XX......
XXXXX...
XX......
XX......
XX......
........
G
..XXXX..
.XX..XX.
XX......
XX..XXX.
XX...XX.
.XX..XX.
..XXXXX.
........
H
XX...XX.
XX...XX.
XX...XX.
XXXXXXX.
XX...XX.
XX...XX.
XX...XX.
........
I
.XXXXXX.
...XX...
...XX...
...XX...
...XX...
...XX...
.XXXXXX.
........
J
....XXX.
.....XX.
.....XX.
.....XX.
XX...XX.
XX...XX.
.XXXXX..
........
K
XX...XX.
XX..XX..
XX.XX...
XXXX....
XX.XX...
XX..XX..
XX...XX.
........
L
XX......
XX......
XX......
XX......
XX......
XX......
XXXXXXX.
........
M
XX...XX.
XXX.XXX.
XXXXXXX.
XX.X.XX.
XX...XX.
XX...XX.
XX...XX.
........
N
XX...XX.
XXX..XX.
XXXX.XX.
XX.XXXX.
XX..XXX.
XX...XX.
XX...XX.
........
O
.XXXXX..
XX...XX.
XX...XX.
XX...XX.
XX...XX.
XX...XX.
.XXXXX..
........
R
XXXXXX..
XX...XX.
XX...XX.
XXXXXX..
XX.XX...
XX..XX..
XX...XX.
........
S
.XXXXX..
XX...XX.
XX......
.XXXXX..
.....XX.
XX...XX.
.XXXXX..
........
T
XXXXXXX.
...XX...
...XX...
...XX...
...XX...
...XX...
...XX...
........
a
........
........
.XXXXX..
.....XX.
.XXXXXX.
XX...XX.
.XXXXXX.
........
n
........
........
XX.XXX..
XXX..XX.
XX...XX.
XX...XX.
XX...XX.
........
u
........
........
XX...XX.
XX...XX.
XX...XX.
XX..XXX.
.XXX.XX.
........
"""


def load_glyphs():
    glyphs = {" ": ["........"] * 8}
    lines = [l for l in GLYPH_ART.splitlines() if l]
    for i in range(0, len(lines), 9):
        name, rows = lines[i], lines[i + 1:i + 9]
        assert len(name) == 1, f"bad glyph name {name!r}"
        assert len(rows) == 8 and all(len(r) == 8 for r in rows), f"bad glyph {name!r}"
        glyphs[name] = rows
    return glyphs


def render(glyphs, text, scale):
    """Return 8 rows of bytes; each glyph pixel is `scale` pixels wide."""
    rows = []
    for r in range(8):
        bits = "".join(("1" if c == "X" else "0") * scale
                       for ch in text for c in glyphs[ch][r])
        assert len(bits) % 8 == 0
        rows.append(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)))
    return rows


def emit(out, label, rows):
    out.append(f"        .equ    {label}_wb, {len(rows[0])}    | bytes per row")
    out.append(f"{label}:")
    for row in rows:
        out.append("        .byte   " + ",".join(f"0x{b:02x}" for b in row))


NAME = "SSH OS"


def load_logo():
    """The 96 x 96 "logo" picture of fonts/icons.txt, as rows of 12 bytes."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts", "icons.txt")
    rows, inside = [], False
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("icon "):
            inside = line.split()[1] == "logo"
            continue
        if inside and line and line[0] in ".X":
            bits = line.ljust(96, ".")
            rows.append(bytes(sum(0x80 >> k for k in range(8) if bits[i + k] == "X")
                              for i in range(0, 96, 8)))
    if len(rows) != 96:
        raise SystemExit("mkbanner: the logo should have 96 rows, found %d" % len(rows))
    return rows


HERE = os.path.dirname(os.path.abspath(__file__))


def setting(name):
    """A quoted #define from kernel/kernel.h."""
    for line in open(os.path.join(HERE, "..", "kernel", "kernel.h")):
        f = line.split(None, 2)
        if len(f) == 3 and f[0] == "#define" and f[1] == name:
            return f[2].strip().strip('"')
    raise SystemExit("mkbanner: no %s in kernel.h" % name)


def small_text(text):
    """Text in the 6 x 10 screen font, centred on the 512-pixel screen as
    gfx_text() would put it: (first byte across, 10 rows of whole bytes)."""
    sys.path.insert(0, HERE)
    import mkfont
    glyphs = mkfont.parse(os.path.join(HERE, "..", "fonts", "juan6x10.txt"))
    x = (512 - 6 * len(text)) // 2
    xb, lead = x // 8, x % 8
    width = -(-(lead + 6 * len(text)) // 8)
    rows = []
    for r in range(10):
        bits = "." * lead
        for ch in text:
            g = glyphs[ch]
            if len(g[0]) == 6:
                bits += g[r]
            else:
                bits += (g[r - 1] if r else ".....") + "."
        bits = bits.ljust(width * 8, ".")
        rows.append(bytes(sum(0x80 >> k for k in range(8) if bits[i + k] == "X")
                          for i in range(0, width * 8, 8)))
    return xb, rows


def main():
    glyphs = load_glyphs()
    out = ["| Generated by tools/mkbanner.py - do not edit."]
    emit(out, "txt_big", render(glyphs, NAME, 4))
    emit(out, "txt_error", render(glyphs, "  DISK ERROR  ", 1))
    # The mark, for the loader that has room for it (the one on a hard disk).
    out.append("#ifdef SCSI_BOOT")
    out.append("logo:")
    out += ["        .byte " + ",".join("0x%02x" % b for b in row) for row in load_logo()]
    # The small print of the start-up picture, as the kernel would draw it
    # (kernel/splash.c): same font, same places.
    for label, text in (("txt_by", "by:"), ("txt_who", setting("SSHOS_AUTHOR")),
                        ("txt_where", setting("SSHOS_SOURCE"))):
        xb, rows = small_text(text)
        out.append(f"        .equ    {label}_xb, {xb}")
        emit(out, label, rows)
    out.append("#endif")
    with open(sys.argv[1], "w") as f:
        f.write("\n".join(out) + "\n")
    # The same name for the kernel's start-up screen, so that it is drawn
    # with exactly the pixels the loader put there.
    if len(sys.argv) > 2:
        big = render(glyphs, NAME, 4)
        c = ["/* Generated by tools/mkbanner.py - do not edit. */",
             "const unsigned char banner_name_bytes = %d;" % len(big[0]),
             "const unsigned char banner_name[8][%d] = {" % len(big[0])]
        c += ["    {" + ",".join("0x%02x" % b for b in row) + "}," for row in big]
        c.append("};")
        with open(sys.argv[2], "w") as f:
            f.write("\n".join(c) + "\n")


if __name__ == "__main__":
    main()
