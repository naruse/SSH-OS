#!/usr/bin/env python3
"""Prints what "make test-desk" has the test runner type and click on the
home screen (the runner's escapes: \\p pause, \\s screenshot, \\M(dx,dy) move
the mouse, \\c click, \\> Right arrow).

The pointer starts at 256, 211. A pause must come between moving the mouse
and clicking: the emulated mouse takes a moment to get there.
"""
ESC = "\033"
steps = [
    r"\p\p\p\p\s",              # 1: the home screen, left alone for a few seconds
    r"j\p",                     # the SSH OS menu, by its key
    r"\M(-216,-181)\p\s",       # 2: pointer on "About SSH OS" (40, 30): it lights up
    r"\c\p\s",                  # 3: the About window
    r" \p",                     # any key closes it
    r"w\p\s",                   # 4: the network panel
    r" \p",
    r"\>\p",                    # Right arrow: Hardware test is chosen
    r"\M(216,90)\p\c" + r"\p" * 4,  # click its icon (256, 120); it looks at the SCSI bus first
    r"x\p\s",                   # 5: the hardware test, after pressing x
    ESC + r"\p",                # back
    r"\M(-236,-110)\p\c\p",     # click the SSH OS title (20, 10)
    r"\M(20,50)\p\s",           # 6: pointer on "OSConsole" (40, 60)
    r"\c\p",
    r"help\n\p",                # the OSConsole, with the bar still above it
    r"\M(-20,-50)\p\c\p\s",     # 7: the SSH OS menu, opened over the OSConsole's text
    r"\M(20,35)\p\c\p\p\s",     # 8: its Home item (40, 45): the home screen again
    r"\M(200,150)\p",           # pointer out of the way (the menu follows it)
    r"\M(118,-186)\p\c\p\c\p",  # the volume's + button (358, 9), twice: 5 to 7
    r"\M(-52,0)\p\c\p\s",       # 9: its - button (306, 9): 6
    r"\M(-66,186)\p-\p\s",      # 10: pointer away again; and the - key: 5
    r"\M(-112,-75)\p\c\p",      # click the SSH icon (128, 120): no connections yet, so
    r"juan@10.0.0.9\p\s",       # 11: the New connection window, with something typed
    r"\M(30,81)\p\c" + r"\p" * 6 + r"\s",  # its Ping button (158, 201): nobody at that address
    ESC + r"\p\s",              # 12: Esc: the home screen again
    r"\M(226,-81)\p\c\p\s",     # 13: click the Settings icon (384, 120): its window
    r"\>\>\p",                   # mouse speed, by the Right arrow twice: 2 to 4
    r"\M(-74,8)\p\c\p",          # at that speed the pointer goes twice as far: to the
                                # - box of the mouse speed (236, 136), and click: 3
    r"\v+\p\s",                  # 14: Down to the volume, and louder: 6
    r"\n\p",                     # Return: done
    r"\n\p\<\n\p\s",            # 15: Settings again, by Return; speed back to 2
    r"j\p\v\v\v\n",             # and back to the OSConsole by keys: menu, third item
    r"uptime\n",
]
print("".join(steps), end="")
