#!/usr/bin/env python3
"""Make the hard disk image the SE starts SSH OS from (on the BlueSCSI's card).

    mkbootdisk.py IMAGE LOADER KERNEL KERNEL_BLOCK [MEGABYTES]

Layout, in 512-byte blocks:

    0               the map the ROM looks for: "a driver is at block 2"
    1               empty (a partition map here would make the ROM want more)
    2 ...           the loader (boot/boot.S built with SCSI_BOOT)
    KERNEL_BLOCK    the kernel

The ROM reads block 0 of every SCSI disk at start-up. Where it finds this
map it loads the "driver" and calls it; ours reads the kernel and never
returns.
"""
import struct
import sys

SECTOR = 512
LOADER_AT = 2


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__.strip())
    image, loader_path, kernel_path = sys.argv[1:4]
    kernel_at = int(sys.argv[4])
    megabytes = int(sys.argv[5]) if len(sys.argv) > 5 else 4
    loader = open(loader_path, "rb").read()
    kernel = open(kernel_path, "rb").read()
    total = megabytes * 1024 * 1024 // SECTOR
    loader_blocks = -(-len(loader) // SECTOR)
    if LOADER_AT + loader_blocks > kernel_at:
        raise SystemExit("the loader runs into the kernel")
    if kernel_at * SECTOR + len(kernel) > total * SECTOR:
        raise SystemExit("the kernel does not fit in %d MB" % megabytes)
    if kernel_at + -(-len(kernel) // SECTOR) > 0x1FFFFF:
        raise SystemExit("the kernel ends past what the loader's read command can reach")

    first = bytearray(SECTOR)
    # 'ER', block size, blocks, device type and id, data, number of drivers,
    # then for the one driver: its block, its size in blocks, 1 = for Macs.
    struct.pack_into(">2sHIHHIHIHH", first, 0, b"ER", SECTOR, total, 1, 1, 0, 1,
                     LOADER_AT, loader_blocks, 1)
    with open(image, "wb") as f:
        f.truncate(total * SECTOR)
        f.write(first)
        f.seek(LOADER_AT * SECTOR)
        f.write(loader)
        f.seek(kernel_at * SECTOR)
        f.write(kernel)


if __name__ == "__main__":
    main()
