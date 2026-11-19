#!/usr/bin/env python3
"""Check the serial log of "make test-desk": every part of the home screen
that tests/desk_keys.py asks for must have opened, in order."""
import sys

log = open(sys.argv[1], "rb").read().decode("latin-1")
wanted = ["DISK: no SSH OS disk", "DESK: network up", "DESK: home", "DESK: menu open", "DESK: about open",
          "DESK: network panel open", "DESK: hardware test", "key code 07 'x'",
          "DESK: menu open", "DESK: console", "DESK: menu open", "DESK: home",
          "DESK: volume 6", "DESK: volume 7", "DESK: volume 6", "DESK: volume 5",
          "DESK: ssh", "DESK: new connection", "DESK: ping 1",
          "DESK: settings", "DESK: mouse speed 3", "DESK: mouse speed 4", "DESK: mouse speed 3",
          "DESK: volume 6", "DESK: settings", "DESK: mouse speed 2",
          "stray interrupts"]
at = 0
for w in wanted:
    found = log.find(w, at)
    if found < 0:
        print("MISSING (or out of order):", w)
        sys.exit(1)
    at = found + len(w)
print("Home screen test passed.")
