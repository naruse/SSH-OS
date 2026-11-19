"""The SSH OS character set: one byte per character.

0x20-0x7E  ASCII
0x80-0x9F  line drawing, blocks, arrows and a few symbols (EXTRA below)
0xA0-0xFF  the same as Unicode U+00A0-U+00FF (Latin-1): accented letters
           for Western European languages

The console font (fonts/juan6x10.txt, through tools/mkfont.py) and the
tests both take the table from here.
"""

EXTRA = ("─│┌┐└┘├┤┬┴┼╭╮╰╯█"
         "▀▄▌▐░▒▓←↑→↓•…●—✓")
assert len(EXTRA) == 32

CODE = {ch: 0x80 + i for i, ch in enumerate(EXTRA)}     # character -> byte
CODE.update((chr(c), c) for c in range(0xA0, 0x100))
CODE.update((chr(c), c) for c in range(0x20, 0x7F))


def char(code):
    """The Unicode character shown for a byte, or None."""
    if 0x80 <= code < 0xA0:
        return EXTRA[code - 0x80]
    if 0x20 <= code < 0x7F or 0xA0 <= code < 0x100:
        return chr(code)
    return None
