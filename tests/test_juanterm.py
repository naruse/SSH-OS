"""Tests of the terminal helper: the update encoder on its own, then the
whole helper with a pretend SE on a local socket.

    python3 -m unittest tests.test_juanterm
"""
import hashlib
import hmac
import os
import random
import socket
import struct
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tools.juanterm import diff  # noqa: E402

COLS, ROWS = 85, 34


def random_row(rng, density=0.6):
    row = []
    attr = 0
    for _ in range(COLS):
        if rng.random() < 0.05:
            attr = rng.choice([0, 0, 0, 1, 2, 4, 3])
        ch = rng.randint(33, 126) if rng.random() < density else 32
        row.append((ch, attr))
    if rng.random() < 0.4:      # a long run, as in a ruler or a blank tail
        start = rng.randrange(COLS)
        fill = (rng.choice([32, 45, 61]), rng.choice([0, 1]))
        for c in range(start, min(COLS, start + rng.randint(6, 60))):
            row[c] = fill
    return row


def random_screen(rng):
    return [random_row(rng) if rng.random() < 0.8 else [diff.BLANK] * COLS for _ in range(ROWS)]


def mutate(rng, screen):
    new = diff.copy_screen(screen)
    kind = rng.randrange(6)
    if kind == 0:               # a few cells
        for _ in range(rng.randint(1, 30)):
            new[rng.randrange(ROWS)][rng.randrange(COLS)] = (rng.randint(32, 126), rng.choice([0, 1, 2, 4]))
    elif kind == 1:             # output scrolled up, new lines at the bottom
        n = rng.randint(1, ROWS)
        new = new[n:] + [random_row(rng) for _ in range(n)]
    elif kind == 2:             # scrolled inside a region, status line kept
        top, bottom = rng.randint(0, 2), ROWS - 1 - rng.randint(0, 2)
        n = rng.randint(1, 8)
        op = rng.choice([diff.OP_SCROLL_UP, diff.OP_SCROLL_DOWN])
        diff.apply_scroll(new, op, top, bottom, n)
        new[rng.randrange(ROWS)] = random_row(rng)
    elif kind == 3:             # whole new screen
        new = random_screen(rng)
    elif kind == 4:             # cleared
        new = diff.blank_screen(COLS, ROWS)
    else:                       # one line retyped
        new[rng.randrange(ROWS)] = random_row(rng, 0.9)
    return new


class EncoderTest(unittest.TestCase):
    def check(self, old, new, cursor=(0, 0, True), bell=False):
        messages = diff.encode_update(old, new, cursor, bell)
        self.assertTrue(messages)
        shown = diff.copy_screen(old) if old is not None else random_screen(random.Random(5))
        got_cursor, got_bell = None, False
        for m in messages:
            self.assertLessEqual(len(m), diff.MAX_MESSAGE)
            c, b = diff.apply_update(shown, m)
            got_cursor = c or got_cursor
            got_bell = got_bell or b
        self.assertEqual(shown, new)
        self.assertEqual(got_cursor, cursor)
        self.assertEqual(got_bell, bell)
        return messages

    def test_first_update_clears(self):
        rng = random.Random(1)
        self.check(None, random_screen(rng))
        self.check(None, diff.blank_screen(COLS, ROWS))

    def test_no_change_is_tiny(self):
        s = random_screen(random.Random(2))
        messages = self.check(s, diff.copy_screen(s), cursor=(3, 4, False))
        self.assertEqual(len(messages), 1)
        self.assertEqual(len(messages[0]), 4)

    def test_one_character(self):
        s = diff.blank_screen(COLS, ROWS)
        n = diff.copy_screen(s)
        n[5][10] = (65, 0)
        messages = self.check(s, n)
        self.assertLess(len(messages[0]), 12)

    def test_scrolling_output_is_cheap(self):
        rng = random.Random(3)
        s = [random_row(rng, 0.9) for _ in range(ROWS)]
        n = s[1:] + [random_row(rng, 0.9)]
        messages = self.check(s, n)
        self.assertLess(sum(len(m) for m in messages), 150)
        n = [random_row(rng, 0.9)] + s[:-1]
        messages = self.check(s, n)
        self.assertLess(sum(len(m) for m in messages), 150)

    def test_scroll_with_status_line(self):
        rng = random.Random(4)
        s = [random_row(rng, 0.9) for _ in range(ROWS)]
        n = [s[0]] + s[2:ROWS - 1] + [random_row(rng, 0.9), s[ROWS - 1]]
        messages = self.check(s, n)
        self.assertLess(sum(len(m) for m in messages), 150)

    def test_worst_case_fits_in_messages(self):
        # Every cell different in character and attribute from its neighbours.
        n = [[(33 + (r + c) % 90, (r + c) % 8 & 7) for c in range(COLS)] for r in range(ROWS)]
        messages = self.check(diff.blank_screen(COLS, ROWS), n)
        self.assertGreater(len(messages), 1)

    def test_bell_and_cursor(self):
        s = random_screen(random.Random(6))
        self.check(s, diff.copy_screen(s), cursor=(33, 84, True), bell=True)

    def test_random_sequences(self):
        rng = random.Random(7)
        for _ in range(60):
            s = random_screen(rng)
            for _ in range(25):
                n = mutate(rng, s)
                self.check(s, n, cursor=(rng.randrange(ROWS), rng.randrange(COLS), rng.random() < 0.8))
                s = n

    def test_other_sizes(self):
        global COLS, ROWS
        saved = COLS, ROWS
        try:
            for COLS, ROWS in ((20, 5), (132, 60), (80, 24)):
                rng = random.Random(COLS)
                s = random_screen(rng)
                for _ in range(40):
                    n = mutate(rng, s)
                    self.check(s, n)
                    s = n
        finally:
            COLS, ROWS = saved


class FakeSE:
    """Speaks the SE's side of the protocol to a running helper."""

    def __init__(self, port, secret, cols=85, rows=34):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        hello = self._read(19)
        assert hello[:3] == b"JT1"
        mac = hmac.new(secret, hello[3:], hashlib.sha256).digest()
        self.sock.sendall(mac + bytes([cols, rows]))
        self.accepted = self._read(1) == b"\x01"
        self.screen = diff.blank_screen(cols, rows)
        self.cursor = None
        self.updates = 0
        self.ended = False
        self.ack = True

    def _read(self, n):
        data = b""
        while len(data) < n:
            piece = self.sock.recv(n - len(data))
            if not piece:
                raise EOFError
            data += piece
        return data

    def send(self, kind, body=b""):
        self.sock.sendall(kind + struct.pack(">H", len(body)) + body)

    def pump(self, until, timeout=15):
        """Take updates until until() is true."""
        deadline = time.time() + timeout
        while not until():
            self.sock.settimeout(max(0.01, deadline - time.time()))
            try:
                head = self._read(3)
            except (socket.timeout, EOFError):
                return False
            body = self._read(struct.unpack(">H", head[1:])[0])
            if head[:1] == b"U":
                cursor, _ = diff.apply_update(self.screen, body)
                self.cursor = cursor or self.cursor
                self.updates += 1
                if self.ack:
                    self.send(b"A")
            elif head[:1] == b"X":
                self.ended = True
                return until()
        return True

    def at_prompt(self):
        """The cursor sits right after a "> " at the start of its line."""
        if not self.cursor:
            return False
        row, col, _ = self.cursor
        return col == 2 and self.screen[row][0][0] == ord(">") and self.screen[row][1][0] == 32

    def text(self):
        return "\n".join("".join(chr(c) for c, _ in row).rstrip() for row in self.screen)

    def close(self):
        self.sock.close()


class HelperTest(unittest.TestCase):
    PORT = 23999
    SECRET = b"a-test-secret"

    def setUp(self):
        env = dict(os.environ, JUANTERM_SECRET=self.SECRET.decode())
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.dir = os.path.join(root, "build", "helper-test")
        os.makedirs(self.dir, exist_ok=True)
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "tools.juanterm.server", "--listen", "127.0.0.1",
             "--port", str(self.PORT), "--env", "PS1=> ", "--env", "HOME=" + self.dir,
             "/bin/sh"],
            cwd=root, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        time.sleep(0.2)

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)
        self.proc.stderr.close()

    def test_wrong_secret_is_refused(self):
        se = FakeSE(self.PORT, b"not the secret")
        self.assertFalse(se.accepted)
        se.close()

    def test_shell_session(self):
        se = FakeSE(self.PORT, self.SECRET)
        self.assertTrue(se.accepted)
        self.assertTrue(se.pump(se.at_prompt))
        se.send(b"K", b"echo result-$((6*7))\r")
        self.assertTrue(se.pump(lambda: "result-42" in se.text() and se.at_prompt()), se.text())
        # The typed command and its output are on separate lines, cursor after a new prompt.
        lines = se.text().split("\n")
        self.assertIn("> echo result-$((6*7))", lines)
        self.assertIn("result-42", lines)
        self.assertEqual(se.cursor[1], 2)
        # Arrow keys reach the shell as escape sequences.
        se.send(b"K", b"cat -v\r")
        se.send(b"S", bytes([1]))
        se.send(b"S", bytes([4]))
        se.send(b"K", b"\r")
        self.assertTrue(se.pump(lambda: "^[[A^[[D" in se.text()), se.text())
        se.send(b"K", b"\x04")
        se.send(b"K", b"exit\r")
        self.assertTrue(se.pump(lambda: se.ended))
        se.close()

    def test_flood_skips_ahead(self):
        """With the SE slow to draw, a long listing must not arrive line by line."""
        se = FakeSE(self.PORT, self.SECRET)
        self.assertTrue(se.pump(se.at_prompt))
        before = se.updates
        se.send(b"K", b"i=0; while [ $i -lt 3000 ]; do echo line-$i; i=$((i+1)); done; echo finished\r")
        self.assertTrue(se.pump(lambda: "finished" in se.text().split("\n") and se.at_prompt(),
                                timeout=30), se.text())
        self.assertIn("line-2999", se.text())
        self.assertLess(se.updates - before, 1500)
        se.close()

    def test_full_screen_program(self):
        se = FakeSE(self.PORT, self.SECRET)
        self.assertTrue(se.pump(se.at_prompt))
        # Clear, draw at a position in inverse, and report the cursor position.
        se.send(b"K", b"printf '\\033[2J\\033[5;10H\\033[7mINVERSE\\033[0m\\033[20;1H'\r")
        self.assertTrue(se.pump(lambda: "INVERSE" in se.text()), se.text())
        row = se.screen[4]
        self.assertEqual("".join(chr(c) for c, _ in row[9:16]), "INVERSE")
        self.assertTrue(all(a & 1 for _, a in row[9:16]))
        self.assertEqual(row[16], (32, 0))
        se.close()

    def test_typed_accents(self):
        """Accented letters typed on the SE reach the shell as UTF-8."""
        from tools.juanterm import charset, server
        typed = "".join(chr(c) for c in range(0xA1, 0x100) if c != 0xAD) + "•…—"
        codes = bytes(charset.CODE[ch] for ch in typed)
        self.assertEqual(server.typed_bytes(codes).decode("utf-8"), typed)
        self.assertEqual(server.typed_bytes(b"plain \x1b[A\r\x7f"), b"plain \x1b[A\r\x7f")
        self.assertEqual(server.typed_bytes(b"a\x7f\xf1\x1bb"), "a\x7f\u00f1\x1bb".encode("utf-8"))

        se = FakeSE(self.PORT, self.SECRET)
        self.assertTrue(se.pump(se.at_prompt))
        # od shows the bytes the shell really received: c3 b1 is an n with tilde.
        se.send(b"K", b"printf '%s' 'a" + bytes([0xF1, 0xBF, 0x9C]) + b"' | od -An -tx1\r")
        self.assertTrue(se.pump(lambda: "61 c3 b1 c2 bf e2 80 a6" in " ".join(se.text().split())),
                        se.text())
        # ...and its echo comes back as the same single characters.
        row = next(r for r in se.screen if "printf" in "".join(chr(c) if c < 128 else "?" for c, _ in r))
        self.assertIn(bytes([0x61, 0xF1, 0xBF, 0x9C]), bytes(c for c, _ in row))
        # Delete takes away a whole accented letter, not half of one.
        se.send(b"K", b"cat\r")
        se.send(b"K", b"se" + bytes([0xF1]) + b"x\x7f\x7for\r")
        self.assertTrue(se.pump(lambda: se.text().split("\n").count("seor") == 2), se.text())
        se.send(b"K", b"\x04")
        se.close()

    def test_accents_and_line_drawing(self):
        """UTF-8 from the shell arrives as SSH OS character codes."""
        from tools.juanterm import charset
        se = FakeSE(self.PORT, self.SECRET)
        self.assertTrue(se.pump(se.at_prompt))
        # "ni\u00f1o" then a box corner, a line and an arrow, as UTF-8 in octal.
        se.send(b"K", b"printf '\\033[2J\\033[3;1Hni\\303\\261o \\342\\224\\214\\342\\224\\200 \\342\\206\\222 done'\r")
        self.assertTrue(se.pump(lambda: "done" in se.text()), se.text())
        row = se.screen[2]
        self.assertEqual("".join(charset.char(c) for c, _ in row[:14]), "ni\u00f1o \u250c\u2500 \u2192 done")
        self.assertEqual(row[2][0], 0xF1)
        self.assertEqual(row[5][0], 0x82)
        se.close()


if __name__ == "__main__":
    unittest.main()
