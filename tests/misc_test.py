#!/usr/bin/env python3
"""Compare the digests printed by build/misc_test with Python's own, and
check what the keyboard code makes of key presses."""
import hashlib
import hmac
import os
import subprocess
import sys
import unicodedata

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "juanterm"))
import charset  # noqa: E402

big = bytes((i * 7 + 1) & 0xFF for i in range(1000))
key = bytes(255 - i for i in range(200))

out = subprocess.run([sys.argv[1]], capture_output=True, text=True)
sys.stderr.write(out.stderr)
bad = out.returncode != 0
count = 0
for line in out.stdout.splitlines():
    name, got = line.split()
    if name.startswith("sha"):
        want = hashlib.sha256(big[: int(name[3:])]).hexdigest()
    else:
        n = int(name[4:])
        # Key lengths 64 and 65 (either side of the block size) appear twice:
        # once in the series with a 16+n byte message, once with 16 bytes.
        wants = [hmac.new(key[:n], big[:m], hashlib.sha256).hexdigest() for m in (16 + n, 16)]
        want = got if got in wants else wants[0]
    count += 1
    if want != got:
        print("MISMATCH", name, got, want)
        bad = True
print("%d digests compared" % count)

# ---------------------------------------------------------------- keys --
# The US layout: ADB key code of each unshifted character, and its shifted
# partner. Written out here again on purpose, not taken from the kernel.
KEYCODE = {
    "a": 0x00, "s": 0x01, "d": 0x02, "f": 0x03, "h": 0x04, "g": 0x05, "z": 0x06, "x": 0x07,
    "c": 0x08, "v": 0x09, "b": 0x0B, "q": 0x0C, "w": 0x0D, "e": 0x0E, "r": 0x0F, "y": 0x10,
    "t": 0x11, "1": 0x12, "2": 0x13, "3": 0x14, "4": 0x15, "6": 0x16, "5": 0x17, "=": 0x18,
    "9": 0x19, "7": 0x1A, "-": 0x1B, "8": 0x1C, "0": 0x1D, "]": 0x1E, "o": 0x1F, "u": 0x20,
    "[": 0x21, "i": 0x22, "p": 0x23, "l": 0x25, "j": 0x26, "'": 0x27, "k": 0x28, ";": 0x29,
    "\\": 0x2A, ",": 0x2B, "/": 0x2C, "n": 0x2D, "m": 0x2E, ".": 0x2F, " ": 0x31, "`": 0x32,
}
SHIFT_OF = dict(zip("1234567890-=[]\\;',./`", "!@#$%^&*()_+{}|:\"<>?~"))
UNSHIFT = {v: k for k, v in SHIFT_OF.items()}
SHIFT, OPTION, CONTROL, CAPS, COMMAND = 0x38, 0x3A, 0x36, 0x39, 0x37
RETURN, DELETE, LEFT = 0x24, 0x33, 0x3B

# What a Mac's US layout types with Option, for the characters SSH OS has:
# (key, with Shift) -> character.
OPTION_CHARS = {
    ("1", 0): "¡", ("3", 0): "£", ("4", 0): "¢", ("6", 0): "§", ("7", 0): "¶", ("8", 0): "•",
    ("8", 1): "°", ("9", 0): "ª", ("9", 1): "·", ("0", 0): "º", ("-", 1): "—", ("=", 1): "±",
    ("r", 0): "®", ("y", 0): "¥", ("o", 0): "ø", ("o", 1): "Ø", ("\\", 0): "«", ("\\", 1): "»",
    ("a", 0): "å", ("a", 1): "Å", ("s", 0): "ß", ("g", 0): "©", ("l", 0): "¬", (";", 0): "…",
    ("'", 0): "æ", ("'", 1): "Æ", ("c", 0): "ç", ("c", 1): "Ç", ("m", 0): "µ", (",", 1): "¯",
    ("/", 0): "÷", ("/", 1): "¿",
    # Shift-Option with an accent key types the accent by itself
    ("e", 1): "´", ("u", 1): "¨", ("i", 1): "^", ("n", 1): "~", ("`", 1): "`",
}
# Option with this key, then a letter: the combining accent it adds.
DEAD = {"e": ("\u0301", "´"), "`": ("\u0300", "`"), "i": ("\u0302", "^"), "u": ("\u0308", "¨"),
        "n": ("\u0303", "~")}


def tap(ch, option=False):
    """Key transitions that type one ASCII character, with Option if asked."""
    shift = ch.isupper() or ch in UNSHIFT
    key = KEYCODE[UNSHIFT.get(ch, ch.lower())]
    mods = ([SHIFT] if shift else []) + ([OPTION] if option else [])
    return mods + [key, key | 0x80] + [m | 0x80 for m in reversed(mods)]


def code(ch):
    return "%02x" % charset.CODE[ch]


cases = []          # (prefix, key transitions, expected output, what it is)


def case(steps, want, what, prefix=""):
    cases.append((prefix, steps, " ".join(want), what))


typable = list(KEYCODE) + [c.upper() for c in KEYCODE if c.isalpha()] + list(UNSHIFT)
for ch in typable:
    case(tap(ch), [code(ch)], "plain %r" % ch)
    # Option with one key: a character of its own, an accent in waiting, or Meta.
    base, shift = UNSHIFT.get(ch, ch.lower()), int(ch.isupper() or ch in UNSHIFT)
    if (base, shift) in OPTION_CHARS:
        want = [code(OPTION_CHARS[(base, shift)])]
    elif base in DEAD and not shift:
        want = []
    else:
        want = ["M" + code(ch)]
    case(tap(ch, option=True), want, "Option-%r" % ch)
    case(tap(ch, option=True), ["M" + code(ch)], "Option-%r as Meta" % ch, prefix="meta ")
    # Every accent, then this character.
    for dead, (combining, alone) in DEAD.items():
        made = unicodedata.normalize("NFC", ch + combining)
        if ch == " ":
            want = [code(alone)]
        elif len(made) == 1 and made in charset.CODE:
            want = [code(made)]
        else:
            want = [code(alone), code(ch)]
        case(tap(dead, option=True) + tap(ch), want, "Option-%s then %r" % (dead, ch))

n_tilde, e_acute = code("ñ"), code("é")
case(tap("n", True) + [CAPS] + tap("n"), [code("Ñ")], "accent with Caps Lock")
case([CAPS] + tap("c", True), [code("Ç")], "Caps Lock counts as Shift for letters")
case([CAPS] + tap("/", True), [code("÷")], "...but not for other keys")
case(tap("e", True) + tap("u", True) + tap("u"), [code("´"), code("ü")], "two accents in a row")
case(tap("e", True) + tap("c", True), [code("´"), code("ç")], "accent, then an Option character")
case(tap("e", True) + [RETURN, RETURN | 0x80] + tap("e"), ["0a", "65"], "Return forgets the accent")
case(tap("e", True) + [DELETE, DELETE | 0x80] + tap("e"), ["08", "65"], "Delete forgets the accent")
case(tap("e", True) + [LEFT, LEFT | 0x80] + tap("e"), ["65"], "an arrow forgets the accent")
case(tap("e", True) + tap("b", True) + tap("e"), ["M62", "65"], "a Meta key forgets the accent")
case(tap("n", True) + tap("n") + tap("n"), [n_tilde, "6e"], "the accent is used once")
case([OPTION, KEYCODE["e"], KEYCODE["e"] | 0x80, KEYCODE["e"], KEYCODE["e"] | 0x80, OPTION | 0x80],
     [code("´")], "Option held for accent and letter: both are accent keys")
case([CONTROL, OPTION, KEYCODE["e"], OPTION | 0x80, CONTROL | 0x80] + tap("e"), ["M05", "65"],
     "Control-Option is Meta with a control character")
case([COMMAND, OPTION, KEYCODE["e"], OPTION | 0x80, COMMAND | 0x80] + tap("e"), ["M65", "65"],
     "Command-Option types no accent")
case(tap("e", True) + [SHIFT] + [k for _ in range(40) for k in (KEYCODE["a"], KEYCODE["a"] | 0x80)],
     [code("Á")] + ["41"] * 39, "many keys in one go")
case(tap("e", True) + tap("e") + tap(" ") + tap("n", True) + tap("n"), [e_acute, "20", n_tilde],
     "words")
case([0x7C, KEYCODE["n"], KEYCODE["n"] | 0x80, 0x7C | 0x80] + tap("n") + tap("n"), [n_tilde, "6e"],
     "the right-hand Option key of an extended keyboard")

text = "".join(prefix + " ".join("%02x" % k for k in steps) + "\n" for prefix, steps, _, _ in cases)
out = subprocess.run([sys.argv[1], "keys"], input=text, capture_output=True, text=True)
sys.stderr.write(out.stderr)
bad = bad or out.returncode != 0
got = out.stdout.split("\n")
for i, (_, _, want, what) in enumerate(cases):
    have = got[i].strip() if i < len(got) else "(nothing)"
    if have != want:
        print("KEYS %s: got [%s], want [%s]" % (what, have, want))
        bad = True
print("%d key sequences checked" % len(cases))
sys.exit(1 if bad else 0)
