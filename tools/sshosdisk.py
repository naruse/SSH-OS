#!/usr/bin/env python3
"""Work with an SSH OS disk image from this computer.

    sshosdisk.py make IMAGE [MEGABYTES] [--blank] [--partition]
    sshosdisk.py ls IMAGE
    sshosdisk.py get IMAGE NAME            (prints the file)
    sshosdisk.py put IMAGE NAME FILE
    sshosdisk.py rm IMAGE NAME
    sshosdisk.py check IMAGE

An SSH OS disk is a FAT16 volume named SSH OS (see kernel/fs.c). "make"
creates an empty one, 16 MB unless told otherwise; with --blank it only
makes a file of zeros, for SSH OS to format itself ("format ID" in the
OSConsole); with --partition the volume sits behind a PC partition table.
On the real machine the image goes on the BlueSCSI's card, named for
example HD1_512 SSH OS.hda (SCSI ID 1).

"check" goes through the whole volume and fails if anything is inconsistent.

This is written separately from the kernel's code and shares nothing with
it, so the tests can use each to check the other.
"""
import struct
import sys
import time

SECTOR = 512
LABEL = b"SSH OS     "
OLD_LABEL = b"JUANOS     "      # what these disks were named at first


class Damaged(Exception):
    pass


def short_name(name):
    base, _, ext = name.upper().partition(".")
    ok = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
    if not base or len(base) > 8 or len(ext) > 3 or any(c not in ok for c in base + ext):
        raise SystemExit("names are up to 8 letters or digits, a dot, and up to 3: %r" % name)
    return (base.ljust(8) + ext.ljust(3)).encode()


def stamp():
    t = time.localtime()
    return ((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2),
            ((max(t.tm_year, 1980) - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday)


def make(path, megabytes, blank=False, partition=False):
    total = megabytes * 1024 * 1024 // SECTOR
    with open(path, "wb") as f:
        f.truncate(total * SECTOR)
        if blank:
            return
        start = 63 if partition else 0
        size = total - start
        reserved, fats, root_entries = 1, 2, 512
        root_sectors = root_entries * 32 // SECTOR
        per = 1
        while True:
            # sectors left for clusters, given the tables those clusters need
            clusters = (size - reserved - root_sectors) * SECTOR // (per * SECTOR + 2 * fats)
            if clusters <= 65524:
                break
            per *= 2
        if clusters < 4085:
            raise SystemExit("too small: an SSH OS disk needs about 3 MB")
        fat_sectors = -(-(clusters + 2) * 2 // SECTOR)
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x3C\x90"
        boot[3:11] = b"SSH OS  "
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, per, reserved, fats, root_entries,
                         size if size < 65536 else 0, 0xF8, fat_sectors, 32, 64, start,
                         size if size >= 65536 else 0)
        boot[36], boot[38] = 0x80, 0x29
        struct.pack_into("<I", boot, 39, int(time.time()) & 0xFFFFFFFF)
        boot[43:54] = LABEL
        boot[54:62] = b"FAT16   "
        boot[510:512] = b"\x55\xAA"
        if partition:
            table = bytearray(SECTOR)
            struct.pack_into("<BBBBBBBBII", table, 446, 0, 1, 1, 0, 0x06, 0xFE, 0xFF, 0xFF,
                             start, size)
            table[510:512] = b"\x55\xAA"
            f.seek(0)
            f.write(table)
        f.seek(start * SECTOR)
        f.write(boot)
        for copy in range(fats):
            f.seek((start + reserved + copy * fat_sectors) * SECTOR)
            f.write(b"\xF8\xFF\xFF\xFF")
        f.seek((start + reserved + fats * fat_sectors) * SECTOR)
        t, d = stamp()
        f.write(LABEL + b"\x08" + bytes(10) + struct.pack("<HH", t, d) + bytes(6))


class Volume:
    def __init__(self, path, write=False):
        self.f = open(path, "r+b" if write else "rb")
        self.start = 0
        boot = self.sector(0)
        if not self.layout(boot):
            kind, first = boot[446 + 4], struct.unpack_from("<I", boot, 446 + 8)[0]
            if boot[510:512] != b"\x55\xAA" or kind not in (4, 6, 14) or not first:
                raise Damaged("no FAT16 volume here")
            self.start = first
            boot = self.sector(0)
            if not self.layout(boot):
                raise Damaged("the first partition is not a FAT16 volume")
        self.label = bytes(boot[43:54]) if boot[38] == 0x29 else b""
        self.fat = [bytearray(self.sectors(self.fat_at + i * self.fat_sectors, self.fat_sectors))
                    for i in range(self.fats)]

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.f.close()

    def layout(self, b):
        if b[510:512] != b"\x55\xAA":
            return False
        (bps, self.per, reserved, self.fats, self.root_entries, total16, _, self.fat_sectors,
         _, _, _, total32) = struct.unpack_from("<HBHBHHBHHHII", b, 11)
        total = total16 or total32
        if bps != SECTOR or self.per == 0 or self.per & (self.per - 1) or not reserved \
                or not self.fats or not self.fat_sectors or not self.root_entries:
            return False
        self.fat_at = reserved
        self.root_at = reserved + self.fats * self.fat_sectors
        self.data_at = self.root_at + -(-self.root_entries * 32 // SECTOR)
        if total <= self.data_at:
            return False
        self.clusters = (total - self.data_at) // self.per
        return 4085 <= self.clusters <= 65524 and self.fat_sectors * 256 >= self.clusters + 2

    def sector(self, n):
        return self.sectors(n, 1)

    def sectors(self, n, count):
        self.f.seek((self.start + n) * SECTOR)
        data = self.f.read(count * SECTOR)
        if len(data) != count * SECTOR:
            raise Damaged("the volume runs past the end of the image")
        return bytearray(data)

    def put_sectors(self, n, data):
        self.f.seek((self.start + n) * SECTOR)
        self.f.write(data)

    def next(self, cluster, copy=0):
        return struct.unpack_from("<H", self.fat[copy], cluster * 2)[0]

    def root(self):
        return self.sectors(self.root_at, self.root_entries * 32 // SECTOR)

    def entries(self):
        """(offset in the folder, the 32 bytes) of every plain file."""
        root = self.root()
        for o in range(0, len(root), 32):
            e = root[o:o + 32]
            if e[0] == 0:
                return
            if e[0] != 0xE5 and not e[11] & 0x18 and e[11] != 0x0F:
                yield o, e

    @staticmethod
    def shown(e):
        base, ext = bytes(e[0:8]).decode("latin-1").rstrip(), bytes(e[8:11]).decode("latin-1").rstrip()
        return base + ("." + ext if ext else "")

    def chain(self, first, size):
        out, c = [], first
        while size > len(out) * self.per * SECTOR:
            if not 2 <= c < self.clusters + 2:
                raise Damaged("a file's chain leaves the volume (cluster %d)" % c)
            if c in out:
                raise Damaged("a file's chain goes round in a circle")
            out.append(c)
            c = self.next(c)
        if size == 0 and first:
            raise Damaged("an empty file has clusters")
        if size and c < 0xFFF8:
            raise Damaged("a file's chain is longer than its size")
        return out

    def find(self, name):
        want = short_name(name)
        for o, e in self.entries():
            if bytes(e[0:11]) == want:
                return o, e
        return None, None

    def read(self, name):
        o, e = self.find(name)
        if e is None:
            raise SystemExit("no file named " + name)
        first, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
        data = b"".join(bytes(self.sectors(self.data_at + (c - 2) * self.per, self.per))
                        for c in self.chain(first, size))
        return data[:size]

    def files(self):
        return [(self.shown(e), struct.unpack_from("<I", e, 28)[0]) for _, e in self.entries()]

    def flush_fat(self):
        for i in range(self.fats):
            self.fat[i][:] = self.fat[0]
            self.put_sectors(self.fat_at + i * self.fat_sectors, self.fat[i])

    def release(self, first, size):
        for c in self.chain(first, size):
            struct.pack_into("<H", self.fat[0], c * 2, 0)

    def remove(self, name):
        o, e = self.find(name)
        if e is None:
            raise SystemExit("no file named " + name)
        self.release(struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0])
        root = self.root()
        root[o] = 0xE5
        self.put_sectors(self.root_at, root)
        self.flush_fat()

    def write(self, name, data):
        want = short_name(name)
        o, e = self.find(name)
        if e is not None:
            self.release(struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0])
        root = self.root()
        if o is None:
            for o in range(0, len(root), 32):
                if root[o] in (0, 0xE5):
                    break
            else:
                raise SystemExit("the folder is full")
        need = -(-len(data) // (self.per * SECTOR))
        free = [c for c in range(2, self.clusters + 2) if self.next(c) == 0][:need]
        if len(free) < need:
            raise SystemExit("the disk is full")
        for i, c in enumerate(free):
            struct.pack_into("<H", self.fat[0], c * 2, free[i + 1] if i + 1 < need else 0xFFFF)
            piece = data[i * self.per * SECTOR:(i + 1) * self.per * SECTOR]
            self.put_sectors(self.data_at + (c - 2) * self.per,
                             piece.ljust(self.per * SECTOR, b"\0"))
        t, d = stamp()
        root[o:o + 32] = want + b"\x20" + bytes(2) + struct.pack(
            "<HHHHHHHI", t, d, d, 0, t, d, free[0] if free else 0, len(data))
        self.put_sectors(self.root_at, root)
        self.flush_fat()

    def check(self, copies=True):
        """Raises Damaged, or returns (files, clusters in use, clusters lost).
        With copies False the spare copies of the table may differ from the
        first, which is the one that counts (as after a power cut)."""
        if self.label not in (LABEL, OLD_LABEL):
            raise Damaged("the volume is not named SSH OS")
        for i in range(1, self.fats):
            if copies and self.fat[i] != self.fat[0]:
                raise Damaged("the copies of the table differ")
        owner, names = {}, set()
        for _, e in self.entries():
            name = self.shown(e)
            if name in names:
                raise Damaged("two files are named " + name)
            names.add(name)
            first, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            for c in self.chain(first, size):
                if c in owner:
                    raise Damaged("%s and %s share cluster %d" % (owner[c], name, c))
                owner[c] = name
        used = sum(1 for c in range(2, self.clusters + 2) if self.next(c) != 0)
        return len(names), len(owner), used - len(owner)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 2:
        raise SystemExit(__doc__.strip())
    cmd, image = args[0], args[1]
    try:
        if cmd == "make":
            make(image, int(args[2]) if len(args) > 2 else 16, "--blank" in flags,
                 "--partition" in flags)
            return
        with Volume(image, write=cmd in ("put", "rm")) as v:
            if cmd == "ls":
                for name, size in v.files():
                    print("%-12s %7d" % (name, size))
            elif cmd == "get" and len(args) == 3:
                sys.stdout.buffer.write(v.read(args[2]))
            elif cmd == "put" and len(args) == 4:
                with open(args[3], "rb") as f:
                    v.write(args[2], f.read())
            elif cmd == "rm" and len(args) == 3:
                v.remove(args[2])
            elif cmd == "check":
                files, used, lost = v.check()
                print("SSH OS disk in order: %d file%s in %d cluster%s%s." % (
                    files, "" if files == 1 else "s", used, "" if used == 1 else "s",
                    ", and %d marked as used that no file has" % lost if lost else ""))
            else:
                raise SystemExit(__doc__.strip())
    except Damaged as e:
        raise SystemExit("DAMAGED: %s" % e)


if __name__ == "__main__":
    main()
