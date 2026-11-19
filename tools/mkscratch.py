#!/usr/bin/env python3
"""Make a scratch hard-disk image for testing the SCSI driver.

Block 0 starts with the marker SSH OS's write test looks for before it will
write anything. Every other block starts with a line naming its own number,
followed by a byte pattern that depends on the block number, so a block read
from the wrong place or shifted by a byte is easy to spot.
"""
import sys

BLOCKS = 8192          # 4 MB
MAGIC = b"SSHOS-SCRATCH-DISK\n"


def block(n):
    head = MAGIC if n == 0 else b"SSHOS scratch block %08d\n" % n
    body = bytes((n + i) & 0xFF for i in range(len(head), 512))
    return head + body


with open(sys.argv[1], "wb") as f:
    for n in range(BLOCKS):
        f.write(block(n))
