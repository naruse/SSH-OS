#!/usr/bin/env python3
"""Prints what "make test-wifi" has the test runner do: open the network
panel of an SSH OS built with a made-up WiFi adapter, wait for its list, and
join the second network by clicking it. The pointer starts at 256, 211."""
steps = [
    r"\p\p\p",
    r"w\p\p\p\s",               # 1: the panel with the list of networks
    r"\M(44,-75)\p\c\p\p\p\p",  # click "Guest" (300, 136): an open network, so no password
    r"\p\p\s",                  # 2: home again, now on Guest
    r"w\p\p\p\s",               # 3: the panel again
    r" \pj\p\v\v\v\n",          # close it; SSH OS menu, OSConsole
    r"uptime\n",
]
print("".join(steps), end="")
