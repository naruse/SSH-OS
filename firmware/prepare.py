#!/usr/bin/env python3
"""Add SSH OS's crypto hand-off to a copy of the BlueSCSI firmware source.

    prepare.py BLUESCSI-SOURCE-DIRECTORY

Copies the arithmetic (kernel/x25519.c, ed25519.c, sha512.c, poly1305.c) and the
request handler (firmware/sshos_crypto.c) into lib/SSHOS of that source
tree, and applies firmware/bluescsi.patch, which makes the network adapter
answer SCSI command 0xF5 and adds the new files to the build. After that
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

KERNEL = """/* What SSH OS's arithmetic expects of its kernel.h, and no more: the
 * sources here are SSH OS's own files, unchanged, and include it by this
 * name. */
#ifndef SSHOS_KERNEL_H
#define SSHOS_KERNEL_H
#include <string.h>
#include "sshos_types.h"

void x25519(u8 *out32, const u8 *scalar32, const u8 *point32);
void chacha20(const u8 *key32, const u8 *nonce12, u32 counter, u8 *data, u32 len);
/* If set, called now and then during the long calculations. */
extern void (*crypto_idle)(void);
extern volatile u8 crypto_stop;         /* makes those calculations give up at once */

struct sha512 {
    u32 h[8][2];                /* each word as high half, low half */
    u32 total;
    u8  buf[128];
    u8  used;
};
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, u32 len);
void sha512_finish(struct sha512 *s, u8 *out64);

void ed25519_public(u8 *pub32, const u8 *seed32);
void ed25519_sign(u8 *sig64, const u8 *msg, u32 len, const u8 *seed32, const u8 *pub32);
int  ed25519_verify(const u8 *sig64, const u8 *msg, u32 len, const u8 *pub32);    /* 1 = genuine */

void poly1305(u8 *tag16, const u8 *key32, const u8 *msg, u32 len);
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
    for name in ("x25519.c", "ed25519.c", "sha512.c", "poly1305.c", "fe25519.h"):
        shutil.copy(os.path.join(ROOT, "kernel", name), dest)
    with open(os.path.join(dest, "kernel.h"), "w") as f:
        f.write(KERNEL)
    for name in ("sshos_crypto.c", "sshos_crypto.h"):
        shutil.copy(os.path.join(HERE, name), dest)
    with open(os.path.join(dest, "sshos_types.h"), "w") as f:
        f.write(TYPES)
    # one small file for each source, so that it is compiled with our types
    for name in ("x25519", "ed25519", "sha512", "poly1305"):
        with open(os.path.join(dest, "w_%s.c" % name), "w") as f:
            # the signature sums keep their tables in memory the firmware lends
            f.write('#include "sshos_types.h"\n%s#include "%s.c"\n'
                    % ("#define SSHOS_SCRATCH\n" if name == "ed25519" else "", name))
    subprocess.run(["git", "apply", os.path.join(HERE, "bluescsi.patch")], cwd=tree, check=True)
    print("SSH OS's crypto hand-off is in %s; now build the firmware as its BUILDING.md says." % tree)


if __name__ == "__main__":
    main()
