#!/usr/bin/env python3
"""Add SSH OS's crypto hand-off to a copy of the BlueSCSI firmware source.

    prepare.py BLUESCSI-SOURCE-DIRECTORY

Copies the arithmetic (kernel/x25519.c, ed25519.c, sha512.c, poly1305.c) and the
request handler (firmware/sshos_crypto.c) into lib/SSHOS of that source
tree, and applies firmware/bluescsi.patch, which makes the network adapter
answer SCSI command 0x1D and adds the new files to the build. After that
the firmware is built the BlueSCSI way (its BUILDING.md). See README.md
here.

The patch was made against BlueSCSI-v2 commit 5e64e3b.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TYPES = """/* The exact-width types SSH OS's sources expect, in place of its own
 * kernel/types.h (whose "long is 32 bits" holds on the 68000 only). */
#ifndef SSHOS_TYPES_H
#define SSHOS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#endif
"""


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip())
    tree = sys.argv[1]
    if not os.path.exists(os.path.join(tree, "lib", "SCSI2SD", "src", "firmware", "network.c")):
        raise SystemExit("%s does not look like the BlueSCSI-v2 source" % tree)
    dest = os.path.join(tree, "lib", "SSHOS")
    os.makedirs(dest, exist_ok=True)
    for name in ("x25519.c", "ed25519.c", "sha512.c", "poly1305.c", "fe25519.h", "kernel.h", "types.h"):
        shutil.copy(os.path.join(ROOT, "kernel", name), dest)
    for name in ("sshos_crypto.c", "sshos_crypto.h"):
        shutil.copy(os.path.join(HERE, name), dest)
    with open(os.path.join(dest, "sshos_types.h"), "w") as f:
        f.write(TYPES)
    # one small file for each source, so that it is compiled with our types
    for name in ("x25519", "ed25519", "sha512", "poly1305"):
        with open(os.path.join(dest, "w_%s.c" % name), "w") as f:
            f.write('#include "sshos_types.h"\n#include "%s.c"\n' % name)
    subprocess.run(["git", "apply", os.path.join(HERE, "bluescsi.patch")], cwd=tree, check=True)
    print("SSH OS's crypto hand-off is in %s; now build the firmware as its BUILDING.md says." % tree)


if __name__ == "__main__":
    main()
