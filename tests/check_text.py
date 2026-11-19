#!/usr/bin/env python3
"""check_text.py LOG TEXT...: pass if every TEXT is in the serial log LOG,
written in SSH OS character codes (one byte per character)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "model"))
import charset  # noqa: E402

log = open(sys.argv[1], "rb").read()
missing = [t for t in sys.argv[2:] if bytes(charset.CODE[ch] for ch in t) not in log]
for t in missing:
    print("NOT IN THE SERIAL LOG:", t)
print("%d of %d texts found as SSH OS character codes" % (len(sys.argv) - 2 - len(missing), len(sys.argv) - 2))
sys.exit(1 if missing else 0)
