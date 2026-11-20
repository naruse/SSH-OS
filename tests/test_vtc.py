"""Tests for kernel/vt.c, the terminal SSH sessions draw on.

It is compiled for this computer (tests/vt_host.c) and fed what programs
send. Some cases state what must come out; the rest compare it, order by
order, with the other terminal in this project (tools/model/vt.py), which
was written separately.

    python3 -m unittest tests.test_vtc
"""
import os
import random
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools.model.vt import Screen                            # noqa: E402

HOST = os.path.join(ROOT, os.environ.get("B", "build"), "vt_host")
COLS, ROWS = 85, 32
INVERSE, BOLD, UNDERLINE = 1, 2, 4


class Result:
    def __init__(self, data, frames=False):
        env = dict(os.environ, VT_FRAMES="1") if frames else None
        out = subprocess.run([HOST], input=data, stdout=subprocess.PIPE, check=True, env=env).stdout
        lines = out.decode().split("\n")
        self.codes = [[int(l[i:i + 2], 16) for i in range(0, COLS * 3, 3)] for l in lines[:ROWS]]
        self.attrs = [[int(l[i + 2], 16) for i in range(0, COLS * 3, 3)] for l in lines[:ROWS]]
        self.cursor = tuple(int(v) for v in lines[ROWS].split()[1:])
        self.reply = bytes.fromhex(lines[ROWS + 1][6:])
        self.drawn, self.scrolls = (int(v) for v in lines[ROWS + 2].split()[1::2])

    def row(self, r):
        return bytes(self.codes[r]).decode("latin-1").rstrip()


def run(text):
    return Result(text if isinstance(text, bytes) else text.encode())


class Stated(unittest.TestCase):
    def test_text_and_newlines(self):
        r = run("one\r\ntwo\r\n  three")
        self.assertEqual([r.row(0), r.row(1), r.row(2)], ["one", "two", "  three"])
        self.assertEqual(r.cursor, (7, 2))

    def test_go_and_erase(self):
        r = run("abcdef\x1b[1;3H\x1b[K\x1b[5;10Hx\x1b[2;1Hrow two\x1b[1K")
        self.assertEqual(r.row(0), "ab")
        self.assertEqual(r.row(4), " " * 9 + "x")
        self.assertEqual(r.row(1), "")

    def test_wrap_waits_for_the_next_character(self):
        r = run("x" * COLS)
        self.assertEqual(r.cursor, (COLS - 1, 0))       # still on the first row
        r = run("x" * COLS + "y")
        self.assertEqual((r.row(1), r.cursor), ("y", (1, 1)))
        r = run("x" * COLS + "\r\ny")                   # no blank row in between
        self.assertEqual(r.row(1), "y")

    def test_scrolling_off_the_bottom(self):
        r = run("".join("line %d\r\n" % i for i in range(40)))
        self.assertEqual(r.row(0), "line 9")
        self.assertEqual(r.row(ROWS - 2), "line 39")

    def test_a_screen_painted_one_row_up_is_scrolled(self):
        # As Claude Code draws: no scrolling, every row written again with
        # the text of the row below, a status line staying at the bottom.
        text = ["line %d of the story, with words enough to fill it" % i for i in range(80)]

        def frame(first):
            out = "\x1b[?25l\x1b[H"
            for r in range(ROWS - 2):
                out += text[first + r] + "\x1b[K\r\x1b[1B"
            return out + "\x1b[%d;1Hstatus %d\x1b[K\x1b[?25h" % (ROWS, first)

        for step in (1, 2, 3):
            frames = [frame(i) for i in range(0, 12 * step + 1, step)]
            r = Result("".join(frames).encode(), frames=True)
            for row in range(ROWS - 2):
                self.assertEqual(r.row(row), text[12 * step + row])
            self.assertEqual(r.row(ROWS - 1), "status %d" % (12 * step))
            self.assertEqual(r.scrolls, 12)
            first = Result(frames[0].encode(), frames=True).drawn
            self.assertLess(r.drawn - first, 12 * step * 2 * COLS)   # not a screenful each time

    def test_a_screen_painted_one_row_down_is_scrolled(self):
        text = ["line %d" % i for i in range(60)]
        frames = ["\x1b[?25l\x1b[H" + "".join(text[f + r] + "\x1b[K\r\n" for r in range(ROWS - 1)) + "\x1b[?25h"
                  for f in range(10, -1, -1)]
        r = Result("".join(frames).encode(), frames=True)
        self.assertEqual([r.row(i) for i in range(3)], text[:3])
        self.assertEqual(r.scrolls, 10)

    def test_region(self):
        r = run("top\x1b[2;4r\x1b[2;1Ha\r\nb\r\nc\r\nd\x1b[r")
        self.assertEqual([r.row(i) for i in range(4)], ["top", "b", "c", "d"])

    def test_insert_and_delete(self):
        r = run("abcdef\x1b[1;3H\x1b[2@")
        self.assertEqual(r.row(0), "ab  cdef")
        r = run("abcdef\x1b[1;3H\x1b[2P")
        self.assertEqual(r.row(0), "abef")
        r = run("1\r\n2\r\n3\x1b[2;1H\x1b[L")
        self.assertEqual([r.row(i) for i in range(4)], ["1", "", "2", "3"])
        r = run("1\r\n2\r\n3\x1b[2;1H\x1b[M")
        self.assertEqual([r.row(i) for i in range(3)], ["1", "3", ""])

    def test_looks(self):
        r = run("\x1b[1mB\x1b[0;4mU\x1b[0;7mI\x1b[0;41mR\x1b[0;38;5;200;48;2;250;250;250mW\x1b[0mn")
        self.assertEqual(r.attrs[0][:6], [BOLD, UNDERLINE, INVERSE, INVERSE, 0, 0])

    def test_coloured_background_is_erased_to(self):
        r = run("\x1b[44m\x1b[K\x1b[0m")
        self.assertEqual(set(r.attrs[0]), {INVERSE})

    def test_second_screen_and_back(self):
        r = run("shell$ \x1b[?1049h\x1b[2J\x1b[Hfull screen\x1b[?1049l")
        self.assertEqual((r.row(0), r.cursor), ("shell$", (7, 0)))
        r = run("shell$ \x1b[?1049h\x1b[2J\x1b[Hfull screen")
        self.assertEqual(r.row(0), "full screen")

    def test_answers(self):
        self.assertEqual(run("\x1b[5;7H\x1b[6n").reply, b"\x1b[5;7R")
        self.assertEqual(run("\x1b[c").reply, b"\x1b[?1;2c")
        self.assertEqual(run("\x1b[?u\x1b[c").reply, b"\x1b[?1;2c")

    def test_things_to_ignore(self):
        r = run("a\x1b]0;a title\x07b\x1b]11;?\x1b\\c\x1b[?2004h\x1b[>4;2m\x1b[2 qd\x1b(Be\x1bPq#0\x1b\\f")
        self.assertEqual(r.row(0), "abcdef")

    def test_hidden_cursor(self):
        self.assertEqual(run("ab\x1b[?25l").cursor, (-1, -1))

    def test_utf8_and_line_drawing(self):
        r = run("niño ─│ \U0001F600| \x1b(0lqk\x1b(B")
        self.assertEqual(r.codes[0][:16],
                         [ord("n"), ord("i"), 0xF1, ord("o"), 32, 0x80, 0x81, 32, ord("?"), 32,
                          ord("|"), 32, 0x82, 0x80, 0x83, 32])

    def test_repeat(self):
        self.assertEqual(run("-\x1b[4b").row(0), "-----")


def orders(rng, n):
    """A stream of plain text and the orders both terminals know."""
    out = []
    for _ in range(n):
        k = rng.randrange(22)
        a, b = rng.randrange(1, 12), rng.randrange(1, 40)
        if k < 6:
            out.append("".join(rng.choice("abcdefghij XYZ.") for _ in range(rng.randrange(1, 60))))
        elif k == 6:
            out.append("\r\n" * rng.randrange(1, 4))
        elif k == 7:
            out.append("\x1b[%d;%dH" % (rng.randrange(1, ROWS + 1), rng.randrange(1, COLS + 1)))
        elif k == 8:
            out.append("\x1b[%d%s" % (a, rng.choice("ABCD")))
        elif k == 9:
            out.append("\x1b[%dJ" % rng.randrange(3))
        elif k == 10:
            out.append("\x1b[%dK" % rng.randrange(3))
        elif k == 11:
            out.append("\x1b[%d%s" % (a, rng.choice("LM")))
        elif k == 12:
            out.append("\x1b[%d%s" % (a, rng.choice("@PX")))
        elif k == 13:
            top = rng.randrange(1, ROWS - 2)
            out.append("\x1b[%d;%dr" % (top, rng.randrange(top + 1, ROWS + 1)))
        elif k == 14:
            out.append("\x1b[r")
        elif k == 15:
            out.append(rng.choice(["\x1bM", "\x1bD", "\x1bE", "\r", "\b", "\t"]))
        elif k == 16:
            out.append("\x1b[%d%s" % (a, rng.choice("ST")))
        elif k == 17:
            out.append("\x1b[%dG" % b + "\x1b[%dd" % a)
        elif k == 18:
            out.append(rng.choice(["\x1b7", "\x1b8"]))
        elif k == 19:
            out.append("x" * (COLS - rng.randrange(3)))
        elif k == 20:
            out.append(rng.choice(["\x1b[?1049h", "\x1b[?1049l"]))
        else:
            out.append("\x1b[%dm" % rng.choice([0, 1, 4, 7, 22, 24, 27]))
    return "".join(out)


class Compared(unittest.TestCase):
    def test_against_the_other_terminal(self):
        rng = random.Random(1987)
        for case in range(300):
            data = orders(rng, rng.randrange(5, 120)).encode()
            ours = Result(data)
            theirs = Screen(COLS, ROWS)
            theirs.feed(data)
            for r in range(ROWS):
                want = [c for c, _ in theirs.cells[r]]
                self.assertEqual(ours.codes[r], want,
                                 "case %d row %d\n%r\nours   %r\ntheirs %r" % (
                                     case, r, data, ours.row(r),
                                     bytes(want).decode("latin-1").rstrip()))


if __name__ == "__main__":
    unittest.main()
