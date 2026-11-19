#!/usr/bin/env python3
"""Put together what someone needs to run SSH OS, as one zip ("make release").

    mkrelease.py BOOT-IMAGE OUT-DIRECTORY

The zip holds a folder with the three files for the BlueSCSI's card (the
boot image given here, a fresh empty SSH OS disk, and the empty file that
switches on the network adapter), the guide docs/INSTALL.txt and the
licence. The boot image must come from the release settings
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


def version():
    text = open(os.path.join(ROOT, "kernel", "kernel.h")).read()
    return re.search(r'#define SSHOS_VERSION "([^"]+)"', text).group(1)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip())
    boot, out = sys.argv[1], sys.argv[2]
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
            z.write(os.path.join(ROOT, "LICENSE"), name + "/LICENSE")
    print("%s: %d KB" % (path, os.path.getsize(path) // 1024))
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            print("  %9d  %s" % (info.file_size, info.filename))


if __name__ == "__main__":
    main()
