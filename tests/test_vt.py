"""Tests for tools/juanterm/vt.py (the terminal screen model).

Run from the project root with either of:

    python3 -m unittest tests.test_vt
    python3 tests/test_vt.py
"""

import os
import random
import sys
import unittest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from tools.juanterm import vt, charset                          # noqa: E402
from tools.juanterm.vt import (Screen, ATTR_INVERSE, ATTR_BOLD,  # noqa: E402
                               ATTR_UNDERLINE)

BLANK = (32, 0)
ESC = b'\x1b'
CSI = b'\x1b['


def make(cols=20, rows=6):
    s = Screen(cols, rows)
    s._strict = True        # let internal errors surface in tests
    return s


def text(s, row):
    """The row as Unicode text: each cell's SSH OS character code decoded."""
    return ''.join(charset.char(ch) for ch, _ in s.cells[row])


def line(s, row):
    return text(s, row).rstrip()


def lines(s):
    return [line(s, r) for r in range(s.rows)]


def attrs(s, row):
    return [a for _, a in s.cells[row]]


def cur(s):
    return (s.cursor_row, s.cursor_col)


def snapshot(s):
    return ([list(row) for row in s.cells], s.cursor_row, s.cursor_col,
            s.cursor_visible, s.app_cursor_keys, s.app_keypad,
            s.bracketed_paste, s.bell_count, s.title)


class Base(unittest.TestCase):

    def check(self, s):
        """Invariants that must hold after any input whatsoever."""
        cols = s.cols
        self.assertIsInstance(s.cells, list)
        self.assertEqual(len(s.cells), s.rows)
        for row in s.cells:
            self.assertIsInstance(row, list)
            if len(row) != cols:
                self.fail('row has %d cells, expected %d' % (len(row), cols))
            for cell in row:
                ch, attr = cell
                if not (type(cell) is tuple and type(ch) is int
                        and type(attr) is int
                        and charset.char(ch) is not None and 0 <= attr <= 7):
                    self.fail('bad cell %r' % (cell,))
        self.assertIs(type(s.cursor_row), int)
        self.assertIs(type(s.cursor_col), int)
        self.assertTrue(0 <= s.cursor_row < s.rows, s.cursor_row)
        self.assertTrue(0 <= s.cursor_col < s.cols, s.cursor_col)
        self.assertIs(type(s.cursor_visible), bool)
        self.assertIs(type(s.app_cursor_keys), bool)
        self.assertIs(type(s.app_keypad), bool)
        self.assertIs(type(s.bracketed_paste), bool)
        self.assertIs(type(s.bell_count), int)
        self.assertIs(type(s.title), str)
        for ch in s.title:
            if ch not in charset.CODE:
                self.fail('bad title %r' % (s.title,))


class TestBasics(Base):

    def test_defaults(self):
        s = Screen()
        self.assertEqual((s.cols, s.rows), (85, 34))
        self.assertEqual(len(s.cells), 34)
        self.assertTrue(all(len(r) == 85 for r in s.cells))
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))
        self.assertEqual(cur(s), (0, 0))
        self.assertIs(s.cursor_visible, True)
        self.assertIs(s.app_cursor_keys, False)
        self.assertIs(s.app_keypad, False)
        self.assertIs(s.bracketed_paste, False)
        self.assertEqual(s.bell_count, 0)
        self.assertEqual(s.title, '')
        self.assertEqual(s.feed(b''), b'')
        self.check(s)

    def test_constants(self):
        self.assertEqual((ATTR_INVERSE, ATTR_BOLD, ATTR_UNDERLINE), (1, 2, 4))
        self.assertEqual(vt.ATTR_INVERSE, 1)

    def test_custom_size(self):
        s = make(10, 3)
        self.assertEqual((s.cols, s.rows), (10, 3))
        self.assertEqual(len(s.cells), 3)
        self.assertEqual(len(s.cells[0]), 10)

    def test_rows_are_independent(self):
        s = make(5, 3)
        s.feed(b'a')
        self.assertEqual(s.cells[0][0], (97, 0))
        self.assertEqual(s.cells[1][0], BLANK)
        self.assertEqual(s.cells[2][0], BLANK)

    def test_print_text(self):
        s = make()
        self.assertEqual(s.feed(b'Hello'), b'')
        self.assertEqual(line(s, 0), 'Hello')
        self.assertEqual(s.cells[0][0], (ord('H'), 0))
        self.assertEqual(cur(s), (0, 5))

    def test_feed_accepts_bytearray_and_memoryview(self):
        s = make()
        s.feed(bytearray(b'ab'))
        s.feed(memoryview(b'cd'))
        self.assertEqual(line(s, 0), 'abcd')


class TestControls(Base):

    def test_cr_lf(self):
        s = make()
        s.feed(b'ab\r\ncd')
        self.assertEqual(lines(s)[:2], ['ab', 'cd'])
        self.assertEqual(cur(s), (1, 2))

    def test_lf_keeps_column(self):
        s = make()
        s.feed(b'ab\ncd')
        self.assertEqual(lines(s)[:2], ['ab', '  cd'])

    def test_vt_ff_act_as_lf(self):
        s = make()
        s.feed(b'a\x0bb\x0cc')
        self.assertEqual(lines(s)[:3], ['a', ' b', '  c'])
        self.assertEqual(cur(s), (2, 3))

    def test_cr_overwrites(self):
        s = make()
        s.feed(b'hello\rHE')
        self.assertEqual(line(s, 0), 'HEllo')

    def test_bs(self):
        s = make()
        s.feed(b'abc\x08\x08X')
        self.assertEqual(line(s, 0), 'aXc')
        s.feed(b'\r\x08\x08')
        self.assertEqual(cur(s), (0, 0))

    def test_tab_stops_every_8(self):
        s = make(20, 3)
        s.feed(b'\t')
        self.assertEqual(cur(s), (0, 8))
        s.feed(b'x\t')
        self.assertEqual(cur(s), (0, 16))
        s.feed(b'\t')
        self.assertEqual(cur(s), (0, 19))
        s.feed(b'\t\t')
        self.assertEqual(cur(s), (0, 19))
        s.feed(b'\ra\tb')
        self.assertEqual(line(s, 0)[:9], 'a       b')

    def test_bel(self):
        s = make()
        s.feed(b'a\x07b\x07\x07')
        self.assertEqual(s.bell_count, 3)
        self.assertEqual(line(s, 0), 'ab')

    def test_lf_scrolls_at_bottom(self):
        s = make(10, 3)
        s.feed(b'1\r\n2\r\n3\r\n4')
        self.assertEqual(lines(s), ['2', '3', '4'])
        self.assertEqual(cur(s), (2, 1))

    def test_other_controls_ignored(self):
        s = make()
        s.feed(b'a\x00\x01\x02\x7fb')
        self.assertEqual(line(s, 0), 'ab')
        self.assertEqual(cur(s), (0, 2))


class TestWrap(Base):

    def test_deferred_wrap(self):
        s = make(5, 3)
        s.feed(b'abcde')
        self.assertEqual(line(s, 0), 'abcde')
        self.assertEqual(cur(s), (0, 4))        # pending wrap: reported as cols-1
        s.feed(b'f')
        self.assertEqual(lines(s)[:2], ['abcde', 'f'])
        self.assertEqual(cur(s), (1, 1))

    def test_wrap_pending_cancelled_by_cr(self):
        s = make(5, 3)
        s.feed(b'abcde\rX')
        self.assertEqual(lines(s), ['Xbcde', '', ''])
        self.assertEqual(cur(s), (0, 1))

    def test_full_line_then_crlf_is_one_line_break(self):
        s = make(5, 4)
        s.feed(b'abcde\r\nfghij\r\nk')
        self.assertEqual(lines(s), ['abcde', 'fghij', 'k', ''])

    def test_wrap_pending_cancelled_by_cursor_move(self):
        s = make(5, 3)
        s.feed(b'abcde' + CSI + b'DX')
        self.assertEqual(line(s, 0), 'abcXe')
        s = make(5, 3)
        s.feed(b'abcde\x08X')
        self.assertEqual(line(s, 0), 'abcXe')

    def test_wrap_report_position(self):
        s = make(5, 3)
        self.assertEqual(s.feed(b'abcde' + CSI + b'6n'), b'\x1b[1;5R')

    def test_long_text_wraps(self):
        s = make(5, 4)
        s.feed(b'abcdefghijklm')
        self.assertEqual(lines(s), ['abcde', 'fghij', 'klm', ''])
        self.assertEqual(cur(s), (2, 3))

    def test_wrap_scrolls_at_bottom(self):
        s = make(3, 2)
        s.feed(b'abcdef')
        self.assertEqual(lines(s), ['abc', 'def'])
        self.assertEqual(cur(s), (1, 2))
        s.feed(b'g')
        self.assertEqual(lines(s), ['def', 'g'])
        self.assertEqual(cur(s), (1, 1))

    def test_autowrap_off(self):
        s = make(5, 3)
        s.feed(CSI + b'?7l' + b'abcdefgh')
        self.assertEqual(lines(s), ['abcdh', '', ''])
        self.assertEqual(cur(s), (0, 4))
        s.feed(b'Z')
        self.assertEqual(line(s, 0), 'abcdZ')
        s.feed(CSI + b'?7h' + b'\rabcdefgh')
        self.assertEqual(lines(s), ['abcde', 'fgh', ''])

    def test_autowrap_off_char_by_char(self):
        s = make(5, 3)
        s.feed(CSI + b'?7l')
        for ch in b'abcdefgh':
            s.feed(bytes([ch]))
        self.assertEqual(lines(s), ['abcdh', '', ''])

    def test_wrap_inside_scroll_region(self):
        s = make(3, 5)
        s.feed(b'top\r\n' + CSI + b'2;3r' + CSI + b'2;1H' + b'abcdefghi')
        self.assertEqual(lines(s), ['top', 'def', 'ghi', '', ''])


class TestEsc(Base):

    def test_save_restore_cursor_and_attributes(self):
        s = make()
        s.feed(CSI + b'3;5H' + CSI + b'1;4m' + ESC + b'7')
        s.feed(CSI + b'0m' + CSI + b'1;1H' + b'x')
        s.feed(ESC + b'8' + b'y')
        self.assertEqual(s.cells[0][0], (ord('x'), 0))
        self.assertEqual(s.cells[2][4], (ord('y'), ATTR_BOLD | ATTR_UNDERLINE))
        self.assertEqual(cur(s), (2, 5))

    def test_save_restore_dark_background(self):
        s = make()
        s.feed(CSI + b'40m' + ESC + b'7' + CSI + b'0m' + ESC + b'8' + b'a')
        self.assertEqual(s.cells[0][0], (97, ATTR_INVERSE))

    def test_restore_without_save_homes(self):
        s = make()
        s.feed(CSI + b'3;3H' + CSI + b'1m' + ESC + b'8' + b'a')
        self.assertEqual(s.cells[0][0], (97, 0))

    def test_csi_s_u(self):
        s = make()
        s.feed(CSI + b'2;3H' + CSI + b's' + CSI + b'5;5H' + CSI + b'u' + b'q')
        self.assertEqual(s.cells[1][2], (ord('q'), 0))

    def test_index(self):
        s = make(10, 3)
        s.feed(b'ab' + ESC + b'D' + b'c')
        self.assertEqual(lines(s), ['ab', '  c', ''])
        s.feed(ESC + b'D' + ESC + b'D' + b'd')
        self.assertEqual(lines(s), ['  c', '', '   d'])

    def test_next_line(self):
        s = make(10, 3)
        s.feed(b'ab' + ESC + b'E' + b'c')
        self.assertEqual(lines(s), ['ab', 'c', ''])
        self.assertEqual(cur(s), (1, 1))

    def test_reverse_index(self):
        s = make(10, 3)
        s.feed(b'1\r\n2\r\n3')
        s.feed(ESC + b'M')
        self.assertEqual(cur(s), (1, 1))
        s.feed(ESC + b'M' + ESC + b'M')
        self.assertEqual(cur(s), (0, 1))
        self.assertEqual(lines(s), ['', '1', '2'])

    def test_full_reset(self):
        s = make(10, 3)
        s.feed(b'abc' + CSI + b'1;7m' + CSI + b'2;3r' + CSI + b'?1;25l'
               + CSI + b'?1h' + CSI + b'?2004h' + ESC + b'=' + ESC + b'(0'
               + CSI + b'?1049h' + b'zz' + CSI + b'?7l' + CSI + b'?6h')
        s.feed(ESC + b'c')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))
        self.assertEqual(cur(s), (0, 0))
        self.assertIs(s.cursor_visible, True)
        self.assertIs(s.app_cursor_keys, False)
        self.assertIs(s.app_keypad, False)
        self.assertIs(s.bracketed_paste, False)
        s.feed(b'q\r\n\n\n\nabcdefghijkl')
        self.assertEqual(lines(s), ['', 'abcdefghij', 'kl'])
        self.assertEqual(s.cells[2][0], (ord('k'), 0))
        s.feed(CSI + b'?47h')               # the alternate screen was wiped too
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))
        s.feed(CSI + b'?47l')
        self.assertEqual(lines(s), ['', 'abcdefghij', 'kl'])

    def test_full_reset_keeps_title_and_bell_count(self):
        s = make()
        s.feed(ESC + b']0;t\x07' + b'\x07' + ESC + b'c')
        self.assertEqual(s.title, 't')
        self.assertEqual(s.bell_count, 1)

    def test_keypad_mode(self):
        s = make()
        s.feed(ESC + b'=')
        self.assertIs(s.app_keypad, True)
        s.feed(ESC + b'>')
        self.assertIs(s.app_keypad, False)
        self.assertEqual(lines(s), [''] * s.rows)

    def test_dec_graphics(self):
        s = make()
        s.feed(ESC + b'(0' + b'lqqk' + b'\r\n' + b'x  x' + b'\r\n' + b'mqqj'
               + b'\r\n' + b'tuvwn' + ESC + b'(B' + b'lqk')
        self.assertEqual(lines(s)[:4], ['┌──┐', '│  │', '└──┘', '├┤┴┬┼lqk'])

    def test_dec_graphics_other_glyphs(self):
        s = make(40, 3)
        s.feed(ESC + b'(0' + bytes(range(0x5f, 0x7f)))
        row = text(s, 0)[:32]
        self.assertEqual(row[0], ' ')                   # 0x5f blank
        self.assertEqual(row[0x61 - 0x5f], '▒')         # checkerboard
        self.assertEqual(row[0x66 - 0x5f], '°')
        self.assertEqual(row[0x79 - 0x5f], '<')
        self.assertEqual(row[0x7a - 0x5f], '>')
        self.assertEqual(row[0x7e - 0x5f], '·')
        s.feed(b'\r\nABCabc012')                        # below 0x5f unchanged
        self.assertEqual(line(s, 1)[:3], 'ABC')
        self.assertEqual(line(s, 1)[6:], '012')

    def test_dec_graphics_g1_shift(self):
        s = make()
        s.feed(ESC + b')0' + b'q' + b'\x0e' + b'qx' + b'\x0f' + b'q')
        self.assertEqual(line(s, 0), 'q─│q')
        s.feed(ESC + b')B' + b'\x0e' + b'q')
        self.assertEqual(line(s, 0), 'q─│qq')

    def test_dec_graphics_char_by_char(self):
        s = make()
        for b in ESC + b'(0' + b'lqk' + ESC + b'(B' + b'q':
            s.feed(bytes([b]))
        self.assertEqual(line(s, 0), '┌─┐q')

    def test_unknown_escapes_ignored(self):
        s = make()
        s.feed(ESC + b'H' + ESC + b'N' + ESC + b'\\' + ESC + b' F'
               + ESC + b'%G' + ESC + b'#3' + ESC + b'*0' + ESC + b'Z' + b'ok')
        self.assertEqual(line(s, 0), 'ok')
        self.assertEqual(cur(s), (0, 2))

    def test_alignment_test(self):
        s = make(4, 2)
        s.feed(ESC + b'#8')
        self.assertEqual(lines(s), ['EEEE', 'EEEE'])
        self.assertEqual(cur(s), (0, 0))


class TestCursor(Base):

    def test_cup_and_hvp(self):
        s = make(20, 6)
        s.feed(CSI + b'3;7H')
        self.assertEqual(cur(s), (2, 6))
        s.feed(CSI + b'5;2f')
        self.assertEqual(cur(s), (4, 1))
        s.feed(CSI + b'H')
        self.assertEqual(cur(s), (0, 0))
        s.feed(CSI + b'4H')
        self.assertEqual(cur(s), (3, 0))
        s.feed(CSI + b';9H')
        self.assertEqual(cur(s), (0, 8))
        s.feed(CSI + b'0;0H')
        self.assertEqual(cur(s), (0, 0))
        s.feed(CSI + b'999;999H')
        self.assertEqual(cur(s), (5, 19))
        s.feed(CSI + b'99999999999999999999;99999999999999999999H')
        self.assertEqual(cur(s), (5, 19))

    def test_relative_moves(self):
        s = make(20, 6)
        s.feed(CSI + b'3;7H' + CSI + b'A')
        self.assertEqual(cur(s), (1, 6))
        s.feed(CSI + b'2B')
        self.assertEqual(cur(s), (3, 6))
        s.feed(CSI + b'5C')
        self.assertEqual(cur(s), (3, 11))
        s.feed(CSI + b'3D')
        self.assertEqual(cur(s), (3, 8))
        s.feed(CSI + b'0A' + CSI + b'0D')           # zero means one
        self.assertEqual(cur(s), (2, 7))

    def test_relative_moves_clamped(self):
        s = make(20, 6)
        s.feed(CSI + b'3;7H' + CSI + b'99A')
        self.assertEqual(cur(s), (0, 6))
        s.feed(CSI + b'99B')
        self.assertEqual(cur(s), (5, 6))
        s.feed(CSI + b'99C')
        self.assertEqual(cur(s), (5, 19))
        s.feed(CSI + b'99D')
        self.assertEqual(cur(s), (5, 0))
        s.feed(CSI + b'65535A' + CSI + b'123456789C')
        self.assertEqual(cur(s), (0, 19))

    def test_cnl_cpl(self):
        s = make(20, 6)
        s.feed(CSI + b'3;7H' + CSI + b'2E')
        self.assertEqual(cur(s), (4, 0))
        s.feed(CSI + b'5C' + CSI + b'3F')
        self.assertEqual(cur(s), (1, 0))
        s.feed(CSI + b'99F')
        self.assertEqual(cur(s), (0, 0))
        s.feed(CSI + b'99E')
        self.assertEqual(cur(s), (5, 0))

    def test_cha_hpa_vpa(self):
        s = make(20, 6)
        s.feed(CSI + b'3;7H' + CSI + b'12G')
        self.assertEqual(cur(s), (2, 11))
        s.feed(CSI + b'G')
        self.assertEqual(cur(s), (2, 0))
        s.feed(CSI + b'5`')
        self.assertEqual(cur(s), (2, 4))
        s.feed(CSI + b'99G')
        self.assertEqual(cur(s), (2, 19))
        s.feed(CSI + b'5d')
        self.assertEqual(cur(s), (4, 19))
        s.feed(CSI + b'd')
        self.assertEqual(cur(s), (0, 19))
        s.feed(CSI + b'99d')
        self.assertEqual(cur(s), (5, 19))

    def test_hpr_vpr(self):
        s = make(20, 6)
        s.feed(CSI + b'2;2H' + CSI + b'3a')
        self.assertEqual(cur(s), (1, 4))
        s.feed(CSI + b'2e')
        self.assertEqual(cur(s), (3, 4))
        s.feed(CSI + b'a' + CSI + b'e')
        self.assertEqual(cur(s), (4, 5))
        s.feed(CSI + b'99a' + CSI + b'99e')
        self.assertEqual(cur(s), (5, 19))

    def test_tab_forward_backward(self):
        s = make(40, 3)
        s.feed(CSI + b'2I')
        self.assertEqual(cur(s), (0, 16))
        s.feed(b'x' + CSI + b'Z')
        self.assertEqual(cur(s), (0, 16))
        s.feed(CSI + b'Z')
        self.assertEqual(cur(s), (0, 8))
        s.feed(CSI + b'9Z')
        self.assertEqual(cur(s), (0, 0))

    def test_moves_do_not_print(self):
        s = make()
        s.feed(CSI + b'3;3H' + CSI + b'A' + CSI + b'B' + CSI + b'C'
               + CSI + b'D' + CSI + b'E' + CSI + b'F' + CSI + b'G'
               + CSI + b'd' + CSI + b'`' + CSI + b'a' + CSI + b'e')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))


class TestErase(Base):

    def fill(self, cols=5, rows=4):
        s = make(cols, rows)
        for r in range(rows):
            s.feed(CSI + ('%d;1H' % (r + 1)).encode() + bytes([65 + r]) * cols)
        return s

    def test_ed0(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'J')
        self.assertEqual(lines(s), ['AAAAA', 'BB', '', ''])
        self.assertEqual(cur(s), (1, 2))

    def test_ed0_explicit(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'0J')
        self.assertEqual(lines(s), ['AAAAA', 'BB', '', ''])

    def test_ed1(self):
        s = self.fill()
        s.feed(CSI + b'3;3H' + CSI + b'1J')
        self.assertEqual([text(s, r) for r in range(4)],
                         ['     ', '     ', '   CC', 'DDDDD'])
        self.assertEqual(cur(s), (2, 2))

    def test_ed2(self):
        s = self.fill()
        s.feed(CSI + b'3;3H' + CSI + b'2J')
        self.assertEqual(lines(s), ['', '', '', ''])
        self.assertEqual(cur(s), (2, 2))

    def test_ed3_keeps_screen(self):
        s = self.fill()
        s.feed(CSI + b'3;3H' + CSI + b'3J')
        self.assertEqual(lines(s), ['AAAAA', 'BBBBB', 'CCCCC', 'DDDDD'])

    def test_el0(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'K')
        self.assertEqual(lines(s), ['AAAAA', 'BB', 'CCCCC', 'DDDDD'])

    def test_el1(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'1K')
        self.assertEqual(text(s, 1), '   BB')
        self.assertEqual(text(s, 0), 'AAAAA')

    def test_el2(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'2K')
        self.assertEqual(lines(s), ['AAAAA', '', 'CCCCC', 'DDDDD'])
        self.assertEqual(cur(s), (1, 2))

    def test_erase_unknown_mode_ignored(self):
        s = self.fill()
        s.feed(CSI + b'2;3H' + CSI + b'7J' + CSI + b'9K')
        self.assertEqual(lines(s), ['AAAAA', 'BBBBB', 'CCCCC', 'DDDDD'])

    def test_erased_cells_are_plain_blank(self):
        s = self.fill()
        s.feed(CSI + b'1;7m' + CSI + b'2J')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))

    def test_ech(self):
        s = self.fill()
        s.feed(CSI + b'1;2H' + CSI + b'2X')
        self.assertEqual(text(s, 0), 'A  AA')
        self.assertEqual(cur(s), (0, 1))
        s.feed(CSI + b'2;4H' + CSI + b'99X')
        self.assertEqual(text(s, 1), 'BBB  ')
        s.feed(CSI + b'3;1H' + CSI + b'X')
        self.assertEqual(text(s, 2), ' CCCC')


class TestInsertDelete(Base):

    def test_ich(self):
        s = make(6, 2)
        s.feed(b'abcdef' + CSI + b'1;2H' + CSI + b'2@')
        self.assertEqual(text(s, 0), 'a  bcd')
        self.assertEqual(cur(s), (0, 1))
        s.feed(CSI + b'@')
        self.assertEqual(text(s, 0), 'a   bc')
        s.feed(CSI + b'999@')
        self.assertEqual(text(s, 0), 'a     ')

    def test_dch(self):
        s = make(6, 2)
        s.feed(b'abcdef' + CSI + b'1;2H' + CSI + b'2P')
        self.assertEqual(text(s, 0), 'adef  ')
        self.assertEqual(cur(s), (0, 1))
        s.feed(CSI + b'P')
        self.assertEqual(text(s, 0), 'aef   ')
        s.feed(CSI + b'999P')
        self.assertEqual(text(s, 0), 'a     ')

    def test_il(self):
        s = make(3, 5)
        s.feed(b'111222333444555' + CSI + b'2;2H' + CSI + b'2L')
        self.assertEqual(lines(s), ['111', '', '', '222', '333'])
        self.assertEqual(cur(s), (1, 0))
        s.feed(CSI + b'99L')
        self.assertEqual(lines(s), ['111', '', '', '', ''])

    def test_dl(self):
        s = make(3, 5)
        s.feed(b'111222333444555' + CSI + b'2;2H' + CSI + b'2M')
        self.assertEqual(lines(s), ['111', '444', '555', '', ''])
        self.assertEqual(cur(s), (1, 0))
        s.feed(CSI + b'M')
        self.assertEqual(lines(s), ['111', '555', '', '', ''])
        s.feed(CSI + b'99M')
        self.assertEqual(lines(s), ['111', '', '', '', ''])

    def test_il_dl_respect_scroll_region(self):
        s = make(3, 5)
        s.feed(b'111222333444555' + CSI + b'2;4r' + CSI + b'2;1H' + CSI + b'L')
        self.assertEqual(lines(s), ['111', '', '222', '333', '555'])
        s.feed(CSI + b'M')
        self.assertEqual(lines(s), ['111', '222', '333', '', '555'])
        # outside the region nothing happens
        s.feed(CSI + b'5;1H' + CSI + b'L' + CSI + b'M')
        self.assertEqual(lines(s), ['111', '222', '333', '', '555'])
        s.feed(CSI + b'1;1H' + CSI + b'L' + CSI + b'M')
        self.assertEqual(lines(s), ['111', '222', '333', '', '555'])

    def test_su_sd(self):
        s = make(3, 5)
        s.feed(b'111222333444555' + CSI + b'3;2H')
        s.feed(CSI + b'2S')
        self.assertEqual(lines(s), ['333', '444', '555', '', ''])
        self.assertEqual(cur(s), (2, 1))
        s.feed(CSI + b'T')
        self.assertEqual(lines(s), ['', '333', '444', '555', ''])
        s.feed(CSI + b'99T')
        self.assertEqual(lines(s), ['', '', '', '', ''])

    def test_su_sd_in_region(self):
        s = make(3, 5)
        s.feed(b'111222333444555' + CSI + b'2;4r')
        s.feed(CSI + b'S')
        self.assertEqual(lines(s), ['111', '333', '444', '', '555'])
        s.feed(CSI + b'2T')
        self.assertEqual(lines(s), ['111', '', '', '333', '555'])
        s.feed(CSI + b'99S')
        self.assertEqual(lines(s), ['111', '', '', '', '555'])

    def test_insert_mode(self):
        s = make(6, 2)
        s.feed(b'abcdef' + CSI + b'1;2H' + CSI + b'4h' + b'XY')
        self.assertEqual(text(s, 0), 'aXYbcd')
        s.feed(CSI + b'4l' + b'Z')
        self.assertEqual(text(s, 0), 'aXYZcd')

    def test_repeat(self):
        s = make(10, 2)
        s.feed(b'ab' + CSI + b'3b')
        self.assertEqual(line(s, 0), 'abbbb')
        s.feed(CSI + b'5b')
        self.assertEqual(lines(s), ['abbbbbbbbb', ''])
        self.assertEqual(cur(s), (0, 9))
        s.feed(CSI + b'2b')
        self.assertEqual(lines(s), ['abbbbbbbbb', 'bb'])
        s.feed(CSI + b'99999b')
        self.check(s)
        self.assertEqual(text(s, 0), 'b' * 10)


class TestScrollRegion(Base):

    def setup_screen(self):
        s = make(3, 6)
        s.feed(b'111222333444555666')
        return s

    def test_set_region_homes_cursor(self):
        s = self.setup_screen()
        s.feed(CSI + b'4;2H' + CSI + b'2;5r')
        self.assertEqual(cur(s), (0, 0))

    def test_lf_scrolls_only_region(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;5r' + CSI + b'5;1H' + b'\n')
        self.assertEqual(lines(s), ['111', '333', '444', '555', '', '666'])
        self.assertEqual(cur(s), (4, 0))
        s.feed(b'new\r\n')
        self.assertEqual(lines(s), ['111', '444', '555', 'new', '', '666'])

    def test_lf_below_region_does_not_scroll(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;4r' + CSI + b'6;1H' + b'\n\n\n')
        self.assertEqual(lines(s), ['111', '222', '333', '444', '555', '666'])
        self.assertEqual(cur(s), (5, 0))

    def test_lf_above_region_moves_into_it(self):
        s = self.setup_screen()
        s.feed(CSI + b'3;4r' + CSI + b'1;1H' + b'\n\n\n\n\n\n')
        self.assertEqual(cur(s), (3, 0))
        self.assertEqual(lines(s), ['111', '222', '', '', '555', '666'])

    def test_ri_scrolls_region_down(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;5r' + CSI + b'2;1H' + ESC + b'M')
        self.assertEqual(lines(s), ['111', '', '222', '333', '444', '666'])
        self.assertEqual(cur(s), (1, 0))

    def test_ri_above_region_stops_at_top(self):
        s = self.setup_screen()
        s.feed(CSI + b'3;5r' + CSI + b'2;1H' + ESC + b'M' + ESC + b'M'
               + ESC + b'M')
        self.assertEqual(cur(s), (0, 0))
        self.assertEqual(lines(s), ['111', '222', '333', '444', '555', '666'])

    def test_cursor_up_down_stop_at_margins(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;5r' + CSI + b'3;1H' + CSI + b'99A')
        self.assertEqual(cur(s), (1, 0))
        s.feed(CSI + b'99B')
        self.assertEqual(cur(s), (4, 0))
        s.feed(CSI + b'6;1H' + CSI + b'99A')
        self.assertEqual(cur(s), (1, 0))
        s.feed(CSI + b'1;1H' + CSI + b'99B')
        self.assertEqual(cur(s), (4, 0))
        s.feed(CSI + b'6;1H' + CSI + b'99B')
        self.assertEqual(cur(s), (5, 0))

    def test_reset_region(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;5r' + CSI + b'r' + CSI + b'6;1H' + b'\n')
        self.assertEqual(lines(s), ['222', '333', '444', '555', '666', ''])

    def test_invalid_region_ignored(self):
        s = self.setup_screen()
        s.feed(CSI + b'2;4r' + CSI + b'3;3H')
        s.feed(CSI + b'5;2r')
        self.assertEqual(cur(s), (2, 2))
        s.feed(CSI + b'3;3r')
        self.assertEqual(cur(s), (2, 2))
        s.feed(CSI + b'99;100r')
        self.assertEqual(cur(s), (2, 2))
        s.feed(CSI + b'4;1H' + b'\n')           # region 2..4 still active
        self.assertEqual(lines(s), ['111', '333', '444', '', '555', '666'])

    def test_region_bottom_clamped(self):
        s = self.setup_screen()
        s.feed(CSI + b'3;99r' + CSI + b'6;1H' + b'\n')
        self.assertEqual(lines(s), ['111', '222', '444', '555', '666', ''])

    def test_origin_mode(self):
        s = self.setup_screen()
        s.feed(CSI + b'3;5r' + CSI + b'?6h')
        self.assertEqual(cur(s), (2, 0))
        s.feed(CSI + b'2;2H')
        self.assertEqual(cur(s), (3, 1))
        self.assertEqual(s.feed(CSI + b'6n'), b'\x1b[2;2R')
        s.feed(CSI + b'99;1H')
        self.assertEqual(cur(s), (4, 0))
        s.feed(CSI + b'1d')
        self.assertEqual(cur(s), (2, 0))
        s.feed(CSI + b'99A')
        self.assertEqual(cur(s), (2, 0))
        s.feed(CSI + b'?6l')
        self.assertEqual(cur(s), (0, 0))
        s.feed(CSI + b'2;2H')
        self.assertEqual(cur(s), (1, 1))

    def test_scrolled_in_line_uses_background(self):
        s = make(3, 3)
        s.feed(b'abc' + CSI + b'44m' + CSI + b'3;1H' + b'\n')
        self.assertEqual(s.cells[2], [(32, ATTR_INVERSE)] * 3)
        self.assertEqual(s.cells[1], [BLANK] * 3)


class TestSGR(Base):

    def attr_after(self, seq):
        s = make()
        s.feed(seq + b'a')
        return s.cells[0][0][1]

    def test_bold_underline_inverse(self):
        self.assertEqual(self.attr_after(CSI + b'1m'), ATTR_BOLD)
        self.assertEqual(self.attr_after(CSI + b'4m'), ATTR_UNDERLINE)
        self.assertEqual(self.attr_after(CSI + b'7m'), ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m'), 7)
        self.assertEqual(self.attr_after(CSI + b'1m' + CSI + b'4m'), 6)

    def test_resets(self):
        self.assertEqual(self.attr_after(CSI + b'1;4;7m' + CSI + b'22m'), 5)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m' + CSI + b'24m'), 3)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m' + CSI + b'27m'), 6)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m' + CSI + b'0m'), 0)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m' + CSI + b'm'), 0)
        self.assertEqual(self.attr_after(CSI + b'1;4;7;0m'), 0)
        self.assertEqual(self.attr_after(CSI + b'1;;4m'), ATTR_UNDERLINE)
        self.assertEqual(self.attr_after(CSI + b'0;1m'), ATTR_BOLD)

    def test_ignored_attributes(self):
        self.assertEqual(self.attr_after(CSI + b'2;3;5;8;9m'), 0)
        self.assertEqual(self.attr_after(CSI + b'1;4;7m'
                                         + CSI + b'23;25;28;29m'), 7)

    def test_attributes_apply_per_cell(self):
        s = make()
        s.feed(b'a' + CSI + b'1m' + b'b' + CSI + b'0;7m' + b'c' + CSI + b'm'
               + b'd')
        self.assertEqual(attrs(s, 0)[:5], [0, ATTR_BOLD, ATTR_INVERSE, 0, 0])
        self.assertEqual(line(s, 0), 'abcd')

    def test_foreground_colours_ignored(self):
        for seq in (b'30', b'31', b'37', b'39', b'90', b'97', b'38;5;1',
                    b'38;5;231', b'38;2;1;4;7', b'38:5:1', b'38:2::1:4:7',
                    b'38:2:1:4:7', b'38;2;255;255;255', b'38;5;0',
                    b'58;5;1', b'58;2;1;4;7', b'58:2::1:4:7'):
            self.assertEqual(self.attr_after(CSI + seq + b'm'), 0, seq)

    def test_extended_colour_consumes_its_parameters(self):
        self.assertEqual(self.attr_after(CSI + b'38;5;1;4m'), ATTR_UNDERLINE)
        self.assertEqual(self.attr_after(CSI + b'38;2;1;1;7;1m'), ATTR_BOLD)
        self.assertEqual(self.attr_after(CSI + b'38:5:1;7m'), ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'38:2::7:7:7;1m'), ATTR_BOLD)
        self.assertEqual(self.attr_after(CSI + b'1;48;5;231;4m'),
                         ATTR_BOLD | ATTR_UNDERLINE)

    def test_basic_backgrounds(self):
        self.assertEqual(self.attr_after(CSI + b'40m'), ATTR_INVERSE)   # black
        self.assertEqual(self.attr_after(CSI + b'41m'), ATTR_INVERSE)   # red
        self.assertEqual(self.attr_after(CSI + b'44m'), ATTR_INVERSE)   # blue
        self.assertEqual(self.attr_after(CSI + b'47m'), 0)              # white
        self.assertEqual(self.attr_after(CSI + b'100m'), ATTR_INVERSE)  # grey
        self.assertEqual(self.attr_after(CSI + b'107m'), 0)
        self.assertEqual(self.attr_after(CSI + b'103m'), 0)     # bright yellow
        self.assertEqual(self.attr_after(CSI + b'40m' + CSI + b'49m'), 0)
        self.assertEqual(self.attr_after(CSI + b'40m' + CSI + b'0m'), 0)
        self.assertEqual(self.attr_after(CSI + b'40m' + CSI + b'47m'), 0)

    def test_indexed_backgrounds(self):
        for idx, dark in ((0, True), (4, True), (7, False), (8, True),
                          (15, False), (16, True), (17, True), (21, True),
                          (231, False), (226, False), (232, True),
                          (243, True), (255, False), (196, True)):
            want = ATTR_INVERSE if dark else 0
            self.assertEqual(
                self.attr_after(CSI + ('48;5;%dm' % idx).encode()), want, idx)
            self.assertEqual(
                self.attr_after(CSI + ('48:5:%dm' % idx).encode()), want, idx)

    def test_rgb_backgrounds(self):
        for rgb, dark in (((0, 0, 0), True), ((255, 255, 255), False),
                          ((30, 30, 30), True), ((250, 250, 210), False),
                          ((0, 0, 255), True), ((255, 255, 0), False),
                          ((128, 0, 0), True), ((240, 240, 240), False)):
            want = ATTR_INVERSE if dark else 0
            r, g, b = rgb
            for fmt in ('48;2;%d;%d;%dm', '48:2::%d:%d:%dm', '48:2:%d:%d:%dm',
                        '48;2:%d:%d:%dm'):
                seq = CSI + (fmt % (r, g, b)).encode()
                self.assertEqual(self.attr_after(seq), want, seq)

    def test_luminance_threshold(self):
        # grey level 187 is just below 0.5 relative luminance, 188 just above
        self.assertEqual(self.attr_after(CSI + b'48;2;187;187;187m'),
                         ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'48;2;188;188;188m'), 0)

    def test_inverse_xor_dark_background(self):
        self.assertEqual(self.attr_after(CSI + b'7;40m'), 0)
        self.assertEqual(self.attr_after(CSI + b'40;7m'), 0)
        self.assertEqual(self.attr_after(CSI + b'7;47m'), ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'7;40m' + CSI + b'27m'),
                         ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'7;40m' + CSI + b'49m'),
                         ATTR_INVERSE)
        self.assertEqual(self.attr_after(CSI + b'1;7;40m'), ATTR_BOLD)

    def test_erase_with_dark_background(self):
        inv = (32, ATTR_INVERSE)
        s = make(4, 3)
        s.feed(b'abcdefghijkl' + CSI + b'40m' + CSI + b'2;2H' + CSI + b'K')
        self.assertEqual(s.cells[1], [(ord('e'), 0), inv, inv, inv])
        s.feed(CSI + b'1;2H' + CSI + b'2X')
        self.assertEqual(s.cells[0], [(97, 0), inv, inv, (100, 0)])
        s.feed(CSI + b'3;1H' + CSI + b'P')
        self.assertEqual(s.cells[2][3], inv)
        s.feed(CSI + b'@')
        self.assertEqual(s.cells[2][0], inv)
        s.feed(CSI + b'2J')
        self.assertTrue(all(c == inv for r in s.cells for c in r))
        s.feed(CSI + b'49m' + CSI + b'2;1H' + CSI + b'L')
        self.assertEqual(s.cells[1], [BLANK] * 4)
        s.feed(CSI + b'48;5;17m' + CSI + b'L')
        self.assertEqual(s.cells[1], [inv] * 4)
        s.feed(CSI + b'47m' + CSI + b'2J')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))

    def test_erase_ignores_sgr_inverse(self):
        s = make(4, 2)
        s.feed(CSI + b'7;40m' + CSI + b'2J')
        self.assertTrue(all(c == (32, ATTR_INVERSE) for r in s.cells for c in r))

    def test_sgr_with_prefix_is_not_sgr(self):
        self.assertEqual(self.attr_after(CSI + b'>4;1m'), 0)
        self.assertEqual(self.attr_after(CSI + b'?7m'), 0)
        self.assertEqual(self.attr_after(CSI + b'>1m'), 0)

    def test_underline_styles(self):
        self.assertEqual(self.attr_after(CSI + b'4:3m'), ATTR_UNDERLINE)
        self.assertEqual(self.attr_after(CSI + b'4m' + CSI + b'4:0m'), 0)

    def test_absurd_sgr(self):
        s = make()
        s.feed(CSI + b';'.join([b'1'] * 5000) + b'm' + b'a')
        s.feed(CSI + b'38;5m' + CSI + b'48;2m' + CSI + b'48;2;1m'
               + CSI + b'48;5;999999m' + CSI + b'48;2;999;999;999m'
               + CSI + b'38;m' + CSI + b'48:m' + CSI + b'48:2m' + CSI + b'48:5m'
               + CSI + b':::::m' + CSI + b'48;;;;m' + CSI + b'999999999999m')
        s.feed(b'b')
        self.check(s)
        self.assertEqual(line(s, 0), 'ab')


class TestModes(Base):

    def test_app_cursor_keys(self):
        s = make()
        s.feed(CSI + b'?1h')
        self.assertIs(s.app_cursor_keys, True)
        s.feed(CSI + b'?1l')
        self.assertIs(s.app_cursor_keys, False)

    def test_cursor_visibility(self):
        s = make()
        s.feed(CSI + b'?25l')
        self.assertIs(s.cursor_visible, False)
        s.feed(CSI + b'?25h')
        self.assertIs(s.cursor_visible, True)

    def test_bracketed_paste(self):
        s = make()
        s.feed(CSI + b'?2004h')
        self.assertIs(s.bracketed_paste, True)
        s.feed(CSI + b'?2004l')
        self.assertIs(s.bracketed_paste, False)

    def test_several_modes_in_one_sequence(self):
        s = make()
        s.feed(CSI + b'?1;2004;25h')
        self.assertEqual((s.app_cursor_keys, s.bracketed_paste,
                          s.cursor_visible), (True, True, True))
        s.feed(CSI + b'?25;1l')
        self.assertEqual((s.app_cursor_keys, s.bracketed_paste,
                          s.cursor_visible), (False, True, False))

    def test_other_modes_ignored(self):
        s = make()
        before = snapshot(s)
        s.feed(CSI + b'?12h' + CSI + b'?1000h' + CSI + b'?1002;1006h'
               + CSI + b'?1004l' + CSI + b'?2026h' + CSI + b'?3l'
               + CSI + b'?9999999h' + CSI + b'20h' + CSI + b'12l'
               + CSI + b'?h' + CSI + b'?;;l')
        self.assertEqual(snapshot(s), before)

    def test_non_private_mode_numbers_do_not_alias(self):
        s = make()
        s.feed(CSI + b'1h' + CSI + b'25l' + CSI + b'2004h' + CSI + b'7l'
               + CSI + b'1049h')
        self.assertIs(s.app_cursor_keys, False)
        self.assertIs(s.cursor_visible, True)
        self.assertIs(s.bracketed_paste, False)

    def test_alt_screen_1049_round_trip(self):
        s = make(10, 4)
        s.feed(b'main1\r\nmain2' + CSI + b'1m')
        before_cells = [list(r) for r in s.cells]
        s.feed(CSI + b'?1049h')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))
        self.assertEqual(len(s.cells), 4)
        s.feed(CSI + b'0m' + CSI + b'H' + b'ALT' + CSI + b'4;4H' + b'x')
        self.assertEqual(line(s, 0), 'ALT')
        s.feed(CSI + b'?1049l')
        self.assertEqual([list(r) for r in s.cells], before_cells)
        self.assertEqual(cur(s), (1, 5))
        s.feed(b'!')
        self.assertEqual(s.cells[1][5], (ord('!'), ATTR_BOLD))
        # entering again shows a cleared alternate screen
        s.feed(CSI + b'?1049h')
        self.assertTrue(all(c == BLANK for r in s.cells for c in r))
        s.feed(CSI + b'?1049l')
        self.assertEqual(line(s, 0), 'main1')
        self.assertEqual(line(s, 1), 'main2!')

    def test_alt_screen_keeps_list_identity(self):
        s = make(10, 4)
        cells = s.cells
        s.feed(b'abc' + CSI + b'?1049h' + b'xyz')
        self.assertIs(s.cells, cells)
        s.feed(CSI + b'?1049l')
        self.assertIs(s.cells, cells)
        self.assertEqual(line(s, 0), 'abc')

    def test_alt_screen_47_and_1047(self):
        for mode in (b'47', b'1047'):
            s = make(10, 4)
            s.feed(b'main' + CSI + b'?' + mode + b'h')
            self.assertTrue(all(c == BLANK for r in s.cells for c in r))
            self.assertEqual(cur(s), (0, 4))            # cursor not moved
            s.feed(CSI + b'3;1H' + b'alt')
            self.assertEqual(lines(s), ['', '', 'alt', ''])
            s.feed(CSI + b'?' + mode + b'l')
            self.assertEqual(lines(s), ['main', '', '', ''])
            self.assertEqual(cur(s), (2, 3))            # no cursor restore
            s.feed(CSI + b'?' + mode + b'h')
            self.assertEqual(lines(s), ['', '', '', ''])

    def test_alt_screen_scrolling_does_not_touch_main(self):
        s = make(5, 3)
        s.feed(b'one\r\ntwo\r\nthree' + CSI + b'?1049h')
        s.feed(b'a\r\nb\r\nc\r\nd\r\ne\r\nf' + CSI + b'2J' + CSI + b'?1049l')
        self.assertEqual(lines(s), ['one', 'two', 'three'])

    def test_leave_alt_when_not_in_alt(self):
        s = make(5, 3)
        s.feed(b'ab' + CSI + b'?47l' + CSI + b'?1047l')
        self.assertEqual(lines(s), ['ab', '', ''])
        self.assertEqual(cur(s), (0, 2))

    def test_1048_save_restore(self):
        s = make()
        s.feed(CSI + b'2;3H' + CSI + b'?1048h' + CSI + b'5;5H'
               + CSI + b'?1048l')
        self.assertEqual(cur(s), (1, 2))

    def test_erase_on_alt_entry_uses_background(self):
        s = make(3, 2)
        s.feed(CSI + b'40m' + CSI + b'?1049h')
        self.assertTrue(all(c == (32, ATTR_INVERSE) for r in s.cells for c in r))


class TestReplies(Base):

    def test_cursor_position_report(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'6n'), b'\x1b[1;1R')
        self.assertEqual(s.feed(CSI + b'3;7H' + CSI + b'6n'), b'\x1b[3;7R')
        self.assertEqual(s.feed(b'ab' + CSI + b'6n' + b'cd'), b'\x1b[3;9R')

    def test_status_report(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'5n'), b'\x1b[0n')

    def test_device_attributes(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'c'), b'\x1b[?6c')
        self.assertEqual(s.feed(CSI + b'0c'), b'\x1b[?6c')
        self.assertEqual(s.feed(CSI + b'>c'), b'\x1b[>0;0;0c')
        self.assertEqual(s.feed(CSI + b'>0c'), b'\x1b[>0;0;0c')

    def test_several_replies_in_order(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'5n' + CSI + b'c' + b'x' + CSI + b'6n'),
                         b'\x1b[0n\x1b[?6c\x1b[1;2R')
        self.assertEqual(s.feed(b'plain'), b'')

    def test_reply_split_across_feeds(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'6'), b'')
        self.assertEqual(s.feed(b'n'), b'\x1b[1;1R')
        self.assertEqual(s.feed(b''), b'')

    def test_no_reply_for_other_queries(self):
        s = make()
        self.assertEqual(s.feed(CSI + b'n' + CSI + b'7n' + CSI + b'?6n'
                                + CSI + b'1c' + CSI + b'=c' + CSI + b'>1c'
                                + CSI + b'?u' + CSI + b'>q' + CSI + b'?1$p'
                                + ESC + b']10;?\x07' + ESC + b'P$qm\x1b\\'),
                         b'')

    def test_unknown_csi_consumed_silently(self):
        seqs = [b'8;24;80t', b'14t', b'22;0;0t', b'>4;2m', b'>1u', b'<1u',
                b'=1u', b'?u', b' q', b'2 q', b'!p', b'?1$p', b'>0q', b'1;2z',
                b'y', b'{', b'|', b'}', b'~', b'5i', b'3g', b'0g', b'1;2;3;4;5T',
                b'?1;2s', b'?1r', b'1"q', b'61;1"p', b'2*x', b"1'}", b'$w',
                b'<35;10;5M', b'<35;10;5m', b'=5c', b'?2J', b'?K', b'o', b'q',
                b'v', b'w', b'x', b'p', b'k', b'j', b'i', b'N', b'O', b'Q',
                b'R', b'U', b'V', b'W', b'Y', b'[', b'\\', b']', b'^', b'_',
                b'1?2h', b'1;?2h', b'12;<3m', b'1 2m', b' 1m', b'1$}']
        for seq in seqs:
            s = make()
            out = s.feed(CSI + seq)
            self.assertEqual(out, b'', seq)
            self.assertTrue(all(c == BLANK for r in s.cells for c in r), seq)
            self.assertEqual(cur(s), (0, 0), seq)
            s.feed(b'ok')
            self.assertEqual(line(s, 0), 'ok', seq)
            self.assertEqual(s.cells[0][0], (ord('o'), 0), seq)

    def test_control_inside_csi_still_acts(self):
        s = make()
        s.feed(b'ab' + CSI + b'1\r;7mX')
        self.assertEqual(s.cells[0][0], (ord('X'), ATTR_BOLD | ATTR_INVERSE))

    def test_esc_restarts_sequence(self):
        s = make()
        s.feed(CSI + b'12;' + CSI + b'1mX')
        self.assertEqual(s.cells[0][0], (ord('X'), ATTR_BOLD))
        self.assertEqual(line(s, 0), 'X')

    def test_can_and_sub_abort(self):
        s = make()
        s.feed(CSI + b'1\x18mA' + CSI + b'7\x1amB' + ESC + b'\x18c')
        self.assertEqual(line(s, 0), 'mAmBc')
        self.assertEqual(attrs(s, 0)[:5], [0] * 5)


class TestStrings(Base):

    def test_osc_title_bel(self):
        s = make()
        self.assertEqual(s.feed(ESC + b']0;hello world\x07' + b'x'), b'')
        self.assertEqual(s.title, 'hello world')
        self.assertEqual(line(s, 0), 'x')
        self.assertEqual(s.bell_count, 0)

    def test_osc_title_st(self):
        s = make()
        s.feed(ESC + b']2;second title' + ESC + b'\\' + b'x')
        self.assertEqual(s.title, 'second title')
        self.assertEqual(line(s, 0), 'x')

    def test_osc_title_with_semicolons(self):
        s = make()
        s.feed(ESC + b']0;a;b;c\x07')
        self.assertEqual(s.title, 'a;b;c')
        s.feed(ESC + b']2;\x07')
        self.assertEqual(s.title, '')

    def test_osc_title_uses_our_characters(self):
        s = make()
        s.feed(ESC + b']0;' + 'caf\u00e9 \u2014 \u4e2d'.encode('utf-8')
               + b'\x07')
        self.assertEqual(s.title, 'café — ?')
        s.feed(ESC + b']0;a\xffb\x07')
        self.assertEqual(s.title, 'a?b')

    def test_other_osc_ignored(self):
        s = make()
        s.feed(ESC + b']0;keep\x07')
        s.feed(ESC + b']1;icon\x07' + ESC + b']7;file://host/tmp\x07'
               + ESC + b']8;;http://example.com' + ESC + b'\\link'
               + ESC + b']8;;' + ESC + b'\\'
               + ESC + b']52;c;aGVsbG8=\x07' + ESC + b']133;A\x07'
               + ESC + b']104\x07' + ESC + b']\x07' + ESC + b']0\x07'
               + ESC + b']22;x\x07' + ESC + b']02;x\x07')
        self.assertEqual(s.title, 'keep')
        self.assertEqual(line(s, 0), 'link')
        self.assertEqual(s.bell_count, 0)

    def test_osc_split_across_feeds(self):
        s = make()
        data = ESC + b']0;split title\x07' + b'A' + ESC + b']2;st' + ESC + b'\\B'
        for b in data:
            s.feed(bytes([b]))
        self.assertEqual(s.title, 'st')
        self.assertEqual(line(s, 0), 'AB')
        s = make()
        s.feed(ESC + b']0;one')
        s.feed(b' two' + ESC)
        self.assertEqual(s.title, '')
        s.feed(b'\\')
        self.assertEqual(s.title, 'one two')

    def test_long_strings(self):
        s = make()
        s.feed(ESC + b']52;c;' + b'A' * 300000 + b'\x07' + b'x')
        s.feed(ESC + b'P' + b'q#0;2;0;0;0' * 50000 + ESC + b'\\' + b'y')
        s.feed(ESC + b'_G' + b'f=100;' * 50000 + ESC + b'\\' + b'z')
        self.assertEqual(line(s, 0), 'xyz')
        self.assertEqual(s.title, '')
        s.feed(ESC + b']0;' + b't' * 300000 + b'\x07')
        self.assertTrue(0 < len(s.title) <= 300000)
        self.assertEqual(set(s.title), {'t'})
        self.check(s)

    def test_long_string_in_small_chunks(self):
        s = make()
        data = ESC + b']8;;' + b'u' * 5000 + ESC + b'\\' + b'ok'
        for i in range(0, len(data), 7):
            s.feed(data[i:i + 7])
        self.assertEqual(line(s, 0), 'ok')

    def test_dcs_apc_pm_sos_ignored(self):
        s = make()
        out = s.feed(ESC + b'P1$r0m' + ESC + b'\\' + b'a'
                     + ESC + b'_app command' + ESC + b'\\' + b'b'
                     + ESC + b'^private' + ESC + b'\\' + b'c'
                     + ESC + b'Xsos \x07 with bell' + ESC + b'\\' + b'd')
        self.assertEqual(out, b'')
        self.assertEqual(line(s, 0), 'abcd')
        self.assertEqual(s.title, '')
        self.assertEqual(cur(s), (0, 4))

    def test_string_contents_are_not_interpreted(self):
        s = make()
        s.feed(b'keep' + ESC + b']0;' + b'\r\n\x08\t' + CSI[1:] + b'2J\x07')
        self.assertEqual(line(s, 0), 'keep')
        self.assertEqual(cur(s), (0, 4))
        s.feed(ESC + b'P\r\n[2J[H' + ESC + b'\\')
        self.assertEqual(line(s, 0), 'keep')
        self.assertEqual(cur(s), (0, 4))

    def test_bel_counting(self):
        s = make()
        s.feed(b'\x07' + ESC + b']0;t\x07' + b'\x07' + ESC + b']2;u\x07')
        self.assertEqual(s.bell_count, 2)
        s.feed(CSI + b'1\x07m')                 # BEL inside a CSI still rings
        self.assertEqual(s.bell_count, 3)

    def test_string_aborted_by_can(self):
        s = make()
        s.feed(ESC + b']0;nope\x18' + b'a' + ESC + b'Pzzz\x1a' + b'b')
        self.assertEqual(s.title, '')
        self.assertEqual(line(s, 0), 'ab')

    def test_string_aborted_by_new_escape(self):
        s = make()
        s.feed(ESC + b']0;nope' + CSI + b'1mA')
        self.assertEqual(s.title, '')
        self.assertEqual(s.cells[0][0], (ord('A'), ATTR_BOLD))


class TestUnicode(Base):

    def put(self, string, cols=20, rows=3):
        s = make(cols, rows)
        s.feed(string.encode('utf-8'))
        return s

    def test_accents(self):
        # Latin-1 letters are kept; others (here s with caron) lose the accent.
        s = self.put('\u00e9\u00f1\u00fc\u00c5\u00e7\u00d6\u0161\u00e0\u00ee')
        self.assertEqual(line(s, 0), 'éñüÅçÖsàî')
        self.assertEqual(cur(s), (0, 9))

    def test_letters_without_decomposition(self):
        s = self.put('\u00df\u00e6\u00f8\u0142\u00d8')
        self.assertEqual(line(s, 0), 'ßæølØ')

    def test_box_drawing(self):
        s = self.put('\u250c\u2500\u252c\u2510\u2502\u251c\u253c\u2524'
                     '\u2514\u2534\u2518\u2550\u2551\u2554\u2557\u255a'
                     '\u255d\u256d\u2570')
        self.assertEqual(line(s, 0), '┌─┬┐│├┼┤' '└┴┘─│┌┐└┘╭╰')
        s = self.put('\u2501\u2503\u2504\u2506\u254c\u254e\u2574\u2575'
                     '\u2576\u2577\u257c\u257d')
        self.assertEqual(line(s, 0), '─│─│─│─│─│─│')

    def test_all_box_drawing_and_blocks(self):
        s = make(128, 3)
        s.feed(''.join(chr(c) for c in range(0x2500, 0x2580)).encode('utf-8'))
        self.assertEqual(cur(s), (0, 127))
        self.assertTrue(set(text(s, 0)) <= set('─│┌┐└┘├┤┬┴┼╭╮╰╯/\\× '), text(s, 0))
        s = make(40, 3)
        s.feed(''.join(chr(c) for c in range(0x2580, 0x25A0)).encode('utf-8'))
        self.assertEqual(line(s, 0), '▀_▄▄▄▄█████▌▌▌▌│▐░▒▓¯│' + '▒' * 10)

    def test_punctuation(self):
        s = self.put('\u2018a\u2019 \u201cb\u201d \u2013 \u2014 \u2026 '
                     '\u2022 x\u00a0y')
        self.assertEqual(line(s, 0), '\'a\' "b" - — … • x y')

    def test_arrows_and_marks(self):
        s = self.put('\u2190\u2192\u2191\u2193 \u2713\u2714\u2717\u2718 '
                     '\u21d2\u21d0 \u276f\u25b6\u25c0\u25b2\u25bc')
        self.assertEqual(line(s, 0), '←→↑↓ ✓✓×× →← >><^v')

    def test_unmapped_becomes_question_mark(self):
        s = self.put('\u03bb\u0416\u05d0\u2603')
        self.assertEqual(line(s, 0), '????')
        self.assertEqual(cur(s), (0, 4))

    def test_wide_characters(self):
        s = self.put('a\u4e2d\u6587b')
        self.assertEqual(text(s, 0)[:6], 'a? ? b')
        self.assertEqual(cur(s), (0, 6))
        s = self.put('\U0001f600x')
        self.assertEqual(text(s, 0)[:3], '? x')
        s = self.put('\uff21\u3042\ud55c')        # fullwidth A, hiragana, hangul
        self.assertEqual(text(s, 0)[:6], '? ? ? ')
        self.assertEqual(cur(s), (0, 6))

    def test_wide_character_attributes(self):
        s = make()
        s.feed(CSI + b'7m' + '\u4e2d'.encode('utf-8'))
        self.assertEqual(s.cells[0][0], (ord('?'), ATTR_INVERSE))
        self.assertEqual(s.cells[0][1], (32, ATTR_INVERSE))
        self.assertEqual(s.cells[0][2], BLANK)

    def test_wide_at_right_edge_wraps_whole(self):
        s = self.put('abcd\u4e2d', cols=5)
        self.assertEqual(text(s, 0), 'abcd ')
        self.assertEqual(text(s, 1), '?    ')
        self.assertEqual(cur(s), (1, 2))

    def test_wide_filling_the_line_exactly(self):
        s = self.put('abc\u4e2d', cols=5)
        self.assertEqual(text(s, 0), 'abc? ')
        self.assertEqual(cur(s), (0, 4))            # pending wrap
        s.feed(b'z')
        self.assertEqual(text(s, 1), 'z    ')
        self.assertEqual(cur(s), (1, 1))

    def test_wide_after_pending_wrap(self):
        s = self.put('abcde\u4e2d', cols=5)
        self.assertEqual(text(s, 0), 'abcde')
        self.assertEqual(text(s, 1), '?    ')
        self.assertEqual(cur(s), (1, 2))

    def test_wide_at_right_edge_scrolls(self):
        s = self.put('abcd\r\nefgh\u4e2d', cols=5, rows=2)
        self.assertEqual(lines(s), ['efgh', '?'])

    def test_wide_at_right_edge_without_autowrap(self):
        s = make(5, 2)
        s.feed(CSI + b'?7l' + 'abcd\u4e2d'.encode('utf-8'))
        self.assertEqual(lines(s), ['abcd?', ''])
        self.assertEqual(cur(s), (0, 4))
        s = make(5, 2)
        s.feed(CSI + b'?7l' + 'abc\u4e2d\u4e2d'.encode('utf-8'))
        self.assertEqual(lines(s), ['abc??', ''])

    def test_wide_on_one_column_screen(self):
        s = self.put('\u4e2d\u4e2da', cols=1, rows=3)
        self.assertEqual(lines(s), ['?', '?', 'a'])
        self.check(s)

    def test_zero_width_dropped(self):
        s = self.put('e\u0301a\u200bb\u200dc\ufe0fd\u20dde\u00adf\ufeffg'
                     '\u200e\u2060h')
        self.assertEqual(line(s, 0), 'eabcdefgh')
        self.assertEqual(cur(s), (0, 9))

    def test_emoji_sequence(self):
        s = self.put('\u2764\ufe0f|\U0001f468\u200d\U0001f469|')
        self.assertEqual(line(s, 0), '?|? ? |')

    def test_wide_check_marks(self):
        s = self.put('\u2705\u274c')
        self.assertEqual(text(s, 0)[:4], '✓ × ')

    def test_invalid_bytes(self):
        s = make()
        s.feed(b'a\xffb\x80c\xc0\xafd\xfee')
        self.assertEqual(line(s, 0), 'a?b?c??d?e')

    def test_truncated_sequences(self):
        s = make()
        s.feed(b'a\xc3b')                   # lead byte without continuation
        self.assertEqual(line(s, 0), 'a?b')
        s = make()
        s.feed(b'a\xe4\xb8b')               # two of three bytes
        self.assertEqual(line(s, 0), 'a?b')
        s = make()
        s.feed(b'a\xe4\xb8' + CSI + b'1mb')  # cut short by an escape
        self.assertEqual(line(s, 0), 'a?b')
        self.assertEqual(s.cells[0][2], (ord('b'), ATTR_BOLD))
        s = make()
        s.feed(b'a\xc3\r\nb')
        self.assertEqual(lines(s)[:2], ['a?', 'b'])

    def test_overlong_and_surrogates_rejected(self):
        s = make(40, 2)
        s.feed(b'\xc0\x80|\xe0\x80\x80|\xed\xa0\x80|\xf4\x90\x80\x80|'
               b'\xf8\x88\x80\x80\x80|')
        row = line(s, 0)
        self.assertEqual(row.replace('?', ''), '|||||')
        self.assertTrue(all(ch in '?|' for ch in row))

    def test_matches_python_decoder_on_garbage(self):
        rng = random.Random(7)
        for _ in range(300):
            raw = bytes(rng.choice(b'ab\x80\xbf\xc2\xc3\xe0\xe4\xed\xf0\xf4'
                                   b'\xa0\x9f\x8f\x90\xff\xb8\xad')
                        for _ in range(rng.randint(1, 12)))
            want = ''
            for ch in raw.decode('utf-8', 'replace'):
                m = vt._map_cp(ord(ch))
                want += ''.join(charset.char(x) for x in m)
            s = make(60, 2)
            s.feed(raw + b'|')
            self.assertEqual(line(s, 0), (want + '|').rstrip(), raw)

    def test_utf8_split_across_feeds(self):
        data = 'a\u00e9\u2500\u4e2d\U0001f600z'.encode('utf-8')
        s = make()
        for b in data:
            self.assertEqual(s.feed(bytes([b])), b'')
        self.assertEqual(text(s, 0)[:8], 'aé─? ? z')

    def test_c1_code_points_ignored(self):
        s = make()
        s.feed('a\u0085b\u009bc\u0090d'.encode('utf-8'))
        self.assertEqual(line(s, 0), 'abcd')

    def test_mapping_stays_in_character_set(self):
        for cp in list(range(0x80, 0x3100)) + list(range(0xFE00, 0x10000)) \
                + list(range(0x1F000, 0x1FB00)) + [0x10FFFF, 0xE0001, 0x20000]:
            m = vt._map_cp(cp)
            self.assertIn(len(m), (0, 1, 2))
            for x in m:
                self.assertIsNotNone(charset.char(x), hex(cp))
            if len(m) == 2:
                self.assertEqual(m[1], 32)


SAMPLE = (
    b'hello \x1b[1;7mworld\x1b[0m\r\n'
    + 'caf\u00e9 \u2500\u2502\u250c \u4e2d\u6587 e\u0301 \u2713 '
      '\U0001f600'.encode('utf-8')
    + b'\x1b]0;my title \xc3\xa9\x07'
    + b'\x1b[5;10H\x1b[48;5;17mX\x1b[K\x1b[49m'
    + b'\x1b[38:2::10:20:30m\x1b[48;2;250;250;250mY\x1b[m'
    + b'\x1b[2;4r\x1b[4;1Ha\nb\nc\x1b[r'
    + b'\x1b[6n\x1b[c\x1b[>c\x1b[5n'
    + b'\x1b(0lqk\x1b(B'
    + b'\x1b]2;second\x1b\\'
    + b'\x1bP+q544e\x1b\\'
    + b'\x1b_ignored\x1b\\'
    + b'\x1b[?1049h alt \x1b[2J\x1b[?1049l'
    + b'\x1b7\x1b[6;3Hzz\x1b8!'
    + b'\x1b[?25l\x1b[?1h\x1b=\x1b[?2004h'
    + b'\x07\xff\xc3\x1b[3;1H'
    + b'0123456789' * 9
    + b'\x1b[4mU\x1b[24m\x1b[3D\x1b[2P\x1b[@\x1b[1;1H\x1b[L\x1b[2;1H\x1b[M'
    + b'\x1b[>4;2m\x1b[2 q\x1b[8;1;1t'
    + '\u2192\u00a0\u2026'.encode('utf-8')
    + b'\xe4\xb8'
)


class TestSplitInput(Base):

    def reference(self, cols, rows):
        s = make(cols, rows)
        out = s.feed(SAMPLE)
        return snapshot(s), out

    def test_sample_is_non_trivial(self):
        s = make(40, 8)
        out = s.feed(SAMPLE)
        self.check(s)
        self.assertEqual(s.title, 'second')
        self.assertEqual(s.bell_count, 1)
        self.assertIs(s.cursor_visible, False)
        self.assertIs(s.app_cursor_keys, True)
        self.assertIs(s.app_keypad, True)
        self.assertIs(s.bracketed_paste, True)
        self.assertTrue(out.endswith(b'\x1b[?6c\x1b[>0;0;0c\x1b[0n'))
        self.assertTrue(out.startswith(b'\x1b['))
        used = set(c for r in s.cells for c in r)
        self.assertTrue(len(used) > 12)
        self.assertTrue(any(a & ATTR_UNDERLINE for _, a in used))

    def test_split_at_every_position(self):
        for cols, rows in ((40, 8), (85, 34), (7, 3)):
            ref, ref_out = self.reference(cols, rows)
            for k in range(len(SAMPLE) + 1):
                s = make(cols, rows)
                out = s.feed(SAMPLE[:k])
                out += s.feed(SAMPLE[k:])
                if snapshot(s) != ref or out != ref_out:
                    self.fail('split at %d differs (%dx%d)' % (k, cols, rows))

    def test_one_byte_at_a_time(self):
        for cols, rows in ((40, 8), (7, 3)):
            ref, ref_out = self.reference(cols, rows)
            s = make(cols, rows)
            out = b''
            for b in SAMPLE:
                out += s.feed(bytes([b]))
            self.assertEqual(snapshot(s), ref)
            self.assertEqual(out, ref_out)

    def test_random_chunking(self):
        rng = random.Random(99)
        ref, ref_out = self.reference(40, 8)
        for _ in range(200):
            s = make(40, 8)
            out = b''
            i = 0
            while i < len(SAMPLE):
                j = i + rng.randint(1, 9)
                out += s.feed(SAMPLE[i:j])
                i = j
            self.assertEqual(snapshot(s), ref)
            self.assertEqual(out, ref_out)

    def test_random_stream_chunking_equivalence(self):
        rng = random.Random(4242)
        for _ in range(40):
            data = b''.join(random_sequence(rng) for _ in range(120))
            a = make(23, 7)
            out_a = a.feed(data)
            b = make(23, 7)
            out_b = b''
            i = 0
            while i < len(data):
                j = i + rng.randint(1, 5)
                out_b += b.feed(data[i:j])
                i = j
            self.assertEqual(snapshot(a), snapshot(b))
            self.assertEqual(out_a, out_b)


UNICODE_POOL = ('\u00e9\u00f1\u2500\u2502\u253c\u2588\u2591\u2018\u2019\u201c'
                '\u2014\u2026\u2022\u2192\u2190\u2713\u2717\u00a0\u4e2d\u6587'
                '\uff21\U0001f600\U0001f468\u200d\ufe0f\u0301\u0416\u03bb'
                '\u0085\u009b\ufffd\U0010ffff\u3000\u2705')
FINALS = b'@ABCDEFGHIJKLMPSTXZ`abcdefghilmnrstu'
ALL_FINALS = bytes(range(0x40, 0x7f))
MODES = [1, 3, 4, 5, 6, 7, 12, 25, 47, 1000, 1004, 1047, 1048, 1049, 2004,
         2026, 0, 99999]
SGR_CODES = [0, 1, 2, 3, 4, 5, 7, 8, 9, 21, 22, 23, 24, 25, 27, 28, 29, 30,
             37, 38, 39, 40, 41, 44, 47, 48, 49, 58, 59, 90, 97, 100, 104,
             107, 2, 5, 255, 17, 231]


def random_number(rng):
    k = rng.random()
    if k < 0.15:
        return b''
    if k < 0.30:
        return b'0'
    if k < 0.75:
        return str(rng.randint(1, 12)).encode()
    if k < 0.90:
        return str(rng.randint(13, 300)).encode()
    if k < 0.97:
        return str(rng.randint(301, 70000)).encode()
    return b'9' * rng.randint(6, 40)


def random_params(rng):
    k = rng.random()
    if k < 0.2:
        return b''
    count = rng.randint(1, 4) if k < 0.95 else rng.randint(5, 400)
    sep = b';' if rng.random() < 0.9 else b':'
    return sep.join(random_number(rng) for _ in range(count))


def random_sequence(rng):
    """One random, mostly valid-looking piece of terminal output."""
    k = rng.random()
    if k < 0.22:
        return bytes(rng.randint(32, 126) for _ in range(rng.randint(1, 30)))
    if k < 0.30:
        return ''.join(rng.choice(UNICODE_POOL)
                       for _ in range(rng.randint(1, 6))).encode('utf-8')
    if k < 0.40:
        return rng.choice([b'\r', b'\n', b'\r\n', b'\x08', b'\t', b'\x07',
                           b'\x0b', b'\x0c', b'\x0e', b'\x0f', b'\x00',
                           b'\x7f', b'\x18', b'\x1a'])
    if k < 0.48:
        return ESC + bytes([rng.choice(b'78DEMc=>HNOZ\\@Fclmno|}~')])
    if k < 0.53:
        return ESC + bytes([rng.choice(b'()*+#% -./')]) \
            + bytes([rng.choice(b'0AB8G@<12')])
    if k < 0.70:
        return CSI + random_params(rng) + bytes([rng.choice(FINALS)])
    if k < 0.76:
        codes = [str(rng.choice(SGR_CODES)).encode()
                 for _ in range(rng.randint(1, 6))]
        return CSI + rng.choice([b';', b';', b':']).join(codes) + b'm'
    if k < 0.83:
        modes = b';'.join(str(rng.choice(MODES)).encode()
                          for _ in range(rng.randint(1, 3)))
        return CSI + b'?' + modes + rng.choice([b'h', b'l'])
    if k < 0.88:
        return CSI + rng.choice([b'', b'?', b'>', b'<', b'=']) \
            + random_params(rng) \
            + rng.choice([b'', b'', b' ', b'$', b'!', b'"', b"'"]) \
            + bytes([rng.choice(ALL_FINALS)])
    if k < 0.94:
        body = bytes(rng.choice(b'abc;0129 \xc3\xa9\r\n?=')
                     for _ in range(rng.randint(0, 40)))
        return ESC + rng.choice([b']', b']0;', b']2;', b']8;;', b'P', b'_',
                                 b'^', b'X']) + body \
            + rng.choice([b'\x07', ESC + b'\\', ESC + b'\\', b'', b'\x18'])
    if k < 0.97:
        # a sequence cut short
        seq = CSI + random_params(rng) + bytes([rng.choice(FINALS)])
        return seq[:rng.randint(1, len(seq))]
    return bytes(rng.randint(0, 255) for _ in range(rng.randint(1, 8)))


class TestFuzz(Base):

    def run_chunks(self, s, rng, data, max_chunk):
        i = 0
        n = len(data)
        while i < n:
            j = i + rng.randint(1, max_chunk)
            out = s.feed(data[i:j])
            self.assertIs(type(out), bytes)
            self.check(s)
            i = j

    def test_random_bytes(self):
        rng = random.Random(20240229)
        data = bytes(rng.getrandbits(8) for _ in range(220000))
        self.assertGreaterEqual(len(data), 200000)
        s = make(85, 34)
        self.run_chunks(s, rng, data, 400)

    def test_random_bytes_small_screens(self):
        rng = random.Random(31337)
        for cols, rows in ((1, 1), (2, 1), (1, 2), (2, 2), (3, 2), (8, 3),
                           (9, 5)):
            data = bytes(rng.getrandbits(8) for _ in range(30000))
            s = make(cols, rows)
            self.run_chunks(s, rng, data, 64)

    def test_random_control_heavy_bytes(self):
        # A narrow alphabet makes it much more likely that escape sequences
        # complete and do something.
        rng = random.Random(555)
        alphabet = (b'\x1b\x1b\x1b[[[]??;;;::0123456789>< !$'
                    b'ABCDHJKLMPSTXfhlmnrsu@`abcde78DEMc=()0B'
                    b'\r\n\t\x08\x07\x0e\x0f\x18 xyz\\\xc3\xa9\xe4\xb8\xad'
                    b'\xf0\x9f\x98\x80\xff')
        for cols, rows in ((85, 34), (10, 4), (1, 1), (2, 3)):
            data = bytes(rng.choice(alphabet) for _ in range(60000))
            s = make(cols, rows)
            self.run_chunks(s, rng, data, 97)

    def test_random_sequences(self):
        rng = random.Random(8675309)
        for cols, rows in ((85, 34), (20, 6), (5, 2), (1, 1), (2, 2), (80, 1),
                           (1, 9)):
            s = make(cols, rows)
            total = 0
            for _ in range(6000 if cols * rows > 100 else 3000):
                chunk = b''.join(random_sequence(rng)
                                 for _ in range(rng.randint(1, 4)))
                total += len(chunk)
                out = s.feed(chunk)
                self.assertIs(type(out), bytes)
                self.check(s)
            self.assertGreater(total, 10000)

    def test_absurd_parameters(self):
        s = make(10, 4)
        for final in ALL_FINALS:
            f = bytes([final])
            for params in (b'', b'0', b'99999999999999999999999999',
                           b';' * 3000, b'1;' * 3000, b'1:' * 3000,
                           b'9' * 5000, b'0;0;0;0;0;0;0;0;0;0',
                           b'65535;65535', b'65536;65536;65536',
                           b'4294967296;18446744073709551616'):
                for prefix in (b'', b'?', b'>'):
                    out = s.feed(CSI + prefix + params + f)
                    self.assertIs(type(out), bytes)
            self.check(s)
        s.feed(b'still alive')
        self.check(s)

    def test_errors_are_absorbed_outside_strict_mode(self):
        s = Screen(10, 4)
        self.assertIs(s._strict, False)

        def boom(final):
            s.cells[0] = 'garbage'
            s.cursor_row = 99
            raise RuntimeError('boom')

        s._csi_dispatch = boom
        self.assertEqual(s.feed(b'ab' + CSI + b'2Jcd'), b'')
        self.check(s)
        # the damaged row was rebuilt, the cursor pulled back on screen and
        # the rest of the input still processed
        self.assertEqual(lines(s), ['', '', '', '  cd'])
        self.assertEqual(cur(s), (3, 4))
        del s._csi_dispatch
        s.feed(CSI + b'2J' + CSI + b'H' + b'fine')
        self.assertEqual(line(s, 0), 'fine')
        self.check(s)


if __name__ == '__main__':
    unittest.main()
