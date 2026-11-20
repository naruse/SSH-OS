#!/usr/bin/env python3
"""Put together what someone needs to run SSH OS, as one zip ("make release").

    mkrelease.py BOOT-IMAGE OUT-DIRECTORY [FIRMWARE-DIRECTORY]

The zip holds a folder with the three files for the BlueSCSI's card (the
boot image given here, a fresh empty SSH OS disk, and the empty file that
switches on the network adapter), the guide docs/INSTALL.txt and the
licence. With a third argument, the BlueSCSI firmware found there
(BlueSCSI_Pico_DaynaPORT_SSHOS.uf2 and .bin, see firmware/README.md) goes
in as well, in a folder of its own. The boot image must come from the release settings
(config.release.txt), not from your own: the Makefile sees to that.
"""
import os
import re
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import sshosdisk  # noqa: E402


FIRMWARE = "BlueSCSI_Pico_DaynaPORT_SSHOS"
FIRMWARE_NOTE = """The BlueSCSI firmware with SSH OS's addition
=============================================

For a BlueSCSI v2 with a Pico W (RP2040). INSTALL.txt says how to put it
on.

This is the BlueSCSI v2 firmware (https://github.com/BlueSCSI/BlueSCSI-v2),
which is free software under the GNU General Public License, version 3,
with one addition: a command on the network adapter through which SSH OS
hands SSH's arithmetic to the BlueSCSI's processor. The complete source
of exactly this firmware is the branch

    %s

and the same licence applies to it. There is no warranty.
""" % "https://github.com/naruse/BlueSCSI-v2/tree/sshos-crypto"


def version():
    text = open(os.path.join(ROOT, "kernel", "kernel.h")).read()
    return re.search(r'#define SSHOS_VERSION "([^"]+)"', text).group(1)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__.strip())
    boot, out = sys.argv[1], sys.argv[2]
    firmware = sys.argv[3] if len(sys.argv) == 4 else None
    if firmware and not any(f.startswith(FIRMWARE) for f in os.listdir(firmware)):
        raise SystemExit("no %s*.uf2 or .bin in %s" % (FIRMWARE, firmware))
    name = "SSH-OS-" + version()
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, name + ".zip")
    with tempfile.TemporaryDirectory() as tmp:
        disk = os.path.join(tmp, "disk.hda")
        sshosdisk.make(disk, 16)
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.write(boot, name + "/HD0_512 SSH OS boot.hda")
            z.write(disk, name + "/HD1_512 SSH OS.hda")
            z.writestr(name + "/NE4.hda", b"")
            z.write(os.path.join(ROOT, "docs", "INSTALL.txt"), name + "/INSTALL.txt")
            for f in sorted(os.listdir(firmware)) if firmware else []:
                if f.startswith(FIRMWARE) and f.endswith((".uf2", ".bin")):
                    z.write(os.path.join(firmware, f), name + "/firmware/" + f)
            if firmware:
                z.writestr(name + "/firmware/README.txt", FIRMWARE_NOTE)
            z.write(os.path.join(ROOT, "LICENSE"), name + "/LICENSE")
    print("%s: %d KB" % (path, os.path.getsize(path) // 1024))
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            print("  %9d  %s" % (info.file_size, info.filename))


if __name__ == "__main__":
    main()
