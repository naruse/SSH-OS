#!/usr/bin/env python3
"""Check that tests/charset-sample.txt reached the emulated SE's screen as
the right SSH OS character codes. The argument is the serial log of
"make test-term", where term copies the text it draws."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "juanterm"))
import charset  # noqa: E402

log = open(sys.argv[1], "rb").read()


def codes(text):
    return bytes(charset.CODE[ch] for ch in text)


# The sample has no character more than three times in a row: longer runs
# are sent as a "repeat" instruction, which the log does not record.
wanted = [
    "El pingüino Ñandú preguntó: ¿dónde está el niño? ¡Aquí!",
    # double and heavy lines are drawn with the one plain line
    "╭───┬───╮  ┌───┬───┐  ┌───┬───┐",
    "│año│▓▒░│  │ a │ b │  │ c │ d │",
    "├───┼───┤  ├───┼───┤  ├───┼───┤",
    "│←↑→│✓●•│  └───┴───┘  └───┴───┘",
    "╰───┴───╯  ↓ █▀▄▌▐ 50 °C ± 2 · «hola» — fin…",
]
missing = [w for w in wanted if codes(w) not in log]
for w in missing:
    print("NOT ON THE SE'S SCREEN:", w)
print("%d of %d sample lines found as SSH OS character codes" % (len(wanted) - len(missing), len(wanted)))
sys.exit(1 if missing else 0)
