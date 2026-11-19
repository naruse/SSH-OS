#!/usr/bin/env python3
"""The SSH OS file system (kernel/fs.c, built for this Mac as build/fs_host)
checked against tools/sshosdisk.py, which was written separately: what one
writes the other must read, and the volume must stay consistent whatever is
done to it, including switching the machine off part-way through a write."""
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import sshosdisk  # noqa: E402

HOST = os.path.join(ROOT, os.environ.get("B", "build"), "fs_host")
NO_DISK, NOT_FOUND, FULL, NAME, TOO_BIG, IN_USE, TOO_SMALL = -1, -3, -4, -5, -7, -8, -9


class Kernel:
    """One run of the kernel's code on an image."""

    def __init__(self, image):
        self.p = subprocess.Popen([HOST, image], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True)

    def ask(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        files = []
        while True:
            words = self.p.stdout.readline().split()
            if not words:
                raise AssertionError("fs_host stopped (a checker tripped?) on: " + line)
            if words[0] == "file":
                files.append((words[1], int(words[2])))
                continue
            value = [int(w) for w in words[1:]]
            if line == "list":
                return files if words[0] == "ok" else value[0]
            return value[0] if len(value) == 1 else value

    def close(self):
        self.p.stdin.close()
        self.assert_clean = self.p.wait()
        self.p.stdout.close()
        return self.assert_clean


class FsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="sshosfs-")
        self.image = os.path.join(self.dir, "disk.img")
        self.kernels = []

    def tearDown(self):
        for k in self.kernels:
            if k.p.poll() is None:
                self.assertEqual(k.close(), 0)
            else:
                self.assertEqual(k.p.returncode, 0)
        shutil.rmtree(self.dir, ignore_errors=True)

    def kernel(self, mount=True, image=None):
        k = Kernel(image or self.image)
        self.kernels.append(k)
        if mount:
            self.assertEqual(k.ask("mount"), 0)
        return k

    def path(self, name, data=None):
        p = os.path.join(self.dir, name)
        if data is not None:
            with open(p, "wb") as f:
                f.write(data)
        return p

    def put(self, k, name, data):
        return k.ask("write %s %s" % (name, self.path("in.bin", data)))

    def get(self, k, name):
        r = k.ask("read %s %s" % (name, self.path("out.bin")))
        if r != 0:
            return r
        with open(self.path("out.bin"), "rb") as f:
            return f.read()

    def check(self, image=None, copies=True):
        with sshosdisk.Volume(image or self.image) as v:
            return v.check(copies)

    def tool_read(self, name):
        with sshosdisk.Volume(self.image) as v:
            return v.read(name)

    def tool_write(self, name, data):
        with sshosdisk.Volume(self.image, write=True) as v:
            v.write(name, data)

    # ------------------------------------------------------------------

    def test_sizes_both_ways(self):
        sshosdisk.make(self.image, 4)
        k = self.kernel()
        rnd = random.Random(1)
        sizes = [0, 1, 511, 512, 513, 1024, 5000, 70000]
        for i, size in enumerate(sizes):
            data = rnd.randbytes(size)
            self.assertEqual(self.put(k, "F%d.BIN" % i, data), 0)
            self.assertEqual(self.get(k, "f%d.bin" % i), data)                  # any case
            self.assertEqual(self.tool_read("F%d.BIN" % i), data)
        self.assertEqual(k.ask("list"), [("F%d.BIN" % i, s) for i, s in enumerate(sizes)])
        self.assertEqual(self.check()[0], len(sizes))
        self.assertEqual(self.check()[2], 0)
        # and the other way: written here, read by the kernel
        text = b"volume = 3\n" * 200
        self.tool_write("settings.txt", text)
        k2 = self.kernel()
        self.assertEqual(self.get(k2, "SETTINGS.TXT"), text)

    def test_replace_and_delete_give_space_back(self):
        sshosdisk.make(self.image, 4)
        k = self.kernel()
        before = k.ask("space")
        for size in (30000, 10, 0, 90000, 512):
            data = bytes([size & 255]) * size
            self.assertEqual(self.put(k, "A.TXT", data), 0)
            self.assertEqual(self.get(k, "A.TXT"), data)
            self.assertEqual(self.check(), (1, -(-size // 512), 0))
        self.assertEqual(k.ask("delete A.TXT"), 0)
        self.assertEqual(k.ask("delete A.TXT"), NOT_FOUND)
        self.assertEqual(self.get(k, "A.TXT"), NOT_FOUND)
        self.assertEqual(k.ask("space"), before)
        self.assertEqual(self.check(), (0, 0, 0))

    def test_names(self):
        sshosdisk.make(self.image, 4)
        k = self.kernel()
        for bad in ("toolongname.txt", "a.text", ".txt", "a.b.c", "sp?ce", "a/b"):
            self.assertEqual(self.put(k, bad, b"x"), NAME, bad)
        for good in ("A", "noext", "HOSTS.TXT", "a-b_c.1"):
            self.assertEqual(self.put(k, good, good.encode()), 0, good)
            self.assertEqual(self.get(k, good.upper()), good.encode())
        self.assertEqual(self.check()[0], 4)

    def test_random_work_against_a_model(self):
        sshosdisk.make(self.image, 4)
        k = self.kernel()
        rnd = random.Random(7)
        model = {}
        names = ["F%d.DAT" % i for i in range(12)]
        for step in range(400):
            name = rnd.choice(names)
            what = rnd.random()
            if what < 0.55:
                data = rnd.randbytes(rnd.choice([0, 3, 512, 700, 4096, 20000]))
                self.assertEqual(self.put(k, name, data), 0)
                model[name] = data
            elif what < 0.75:
                self.assertEqual(k.ask("delete " + name), 0 if name in model else NOT_FOUND)
                model.pop(name, None)
            else:
                self.assertEqual(self.get(k, name), model.get(name, NOT_FOUND))
            if step % 25 == 0:
                files, used, lost = self.check()
                self.assertEqual((files, lost), (len(model), 0))
                self.assertEqual(sorted(k.ask("list")),
                                 sorted((n, len(d)) for n, d in model.items()))
        for name, data in model.items():
            self.assertEqual(self.tool_read(name), data)

    def test_formats_a_blank_disk_itself(self):
        for megabytes in (3, 4, 16, 200, 3000):         # the big ones are mostly holes
            sshosdisk.make(self.image, megabytes, blank=True)
            k = self.kernel(mount=False)
            self.assertEqual(k.ask("mount"), NO_DISK)
            self.assertEqual(k.ask("look"), 1)
            self.assertEqual(k.ask("format"), 0)
            self.assertEqual(k.ask("look"), 3)
            self.assertEqual(k.ask("mount"), 0)
            self.assertEqual(self.check(), (0, 0, 0))
            data = bytes(range(256)) * 300
            self.assertEqual(self.put(k, "BIG.BIN", data), 0)
            self.assertEqual(self.tool_read("BIG.BIN"), data)
            total, free = k.ask("space")
            self.assertGreater(total, megabytes * 1024 * (0.95 if megabytes < 2000 else 0.6))
            self.assertLess(total, megabytes * 1024)
            self.assertEqual(self.check()[2], 0)
            self.assertEqual(k.close(), 0)
        sshosdisk.make(self.image, 1, blank=True)
        self.assertEqual(self.kernel(mount=False).ask("format"), TOO_SMALL)

    def test_big_volume_made_here(self):
        sshosdisk.make(self.image, 500)
        k = self.kernel()
        self.assertEqual(self.put(k, "X.BIN", b"q" * 100000), 0)
        self.assertEqual(self.get(k, "X.BIN"), b"q" * 100000)
        self.assertEqual(self.check()[2], 0)

    def test_behind_a_partition_table(self):
        sshosdisk.make(self.image, 8, partition=True)
        k = self.kernel()
        self.assertEqual(self.put(k, "P.TXT", b"partitioned"), 0)
        self.assertEqual(self.tool_read("P.TXT"), b"partitioned")
        self.assertEqual(self.check(), (1, 1, 0))

    def test_never_touches_a_disk_that_is_not_ours(self):
        def unchanged(make_it):
            make_it()
            with open(self.image, "rb") as f:
                before = f.read()
            k = self.kernel(mount=False)
            self.assertEqual(k.ask("mount"), NO_DISK)
            self.assertEqual(k.ask("look"), IN_USE)
            self.assertEqual(k.ask("format"), IN_USE)
            self.assertEqual(self.put(k, "A.TXT", b"x"), NO_DISK)
            self.assertEqual(k.ask("delete A.TXT"), NO_DISK)
            self.assertEqual(k.close(), 0)
            with open(self.image, "rb") as f:
                self.assertEqual(f.read(), before)

        def other_name():                   # a FAT16 volume, but somebody else's
            sshosdisk.make(self.image, 4)
            with open(self.image, "r+b") as f:
                f.seek(43)
                f.write(b"MY PHOTOS  ")

        def mac_disk():                     # nothing in the first blocks, a catalogue after
            sshosdisk.make(self.image, 4, blank=True)
            with open(self.image, "r+b") as f:
                f.seek(1024)
                f.write(b"BD" + bytes(range(100)))

        def mac_partitions():
            sshosdisk.make(self.image, 4, blank=True)
            with open(self.image, "r+b") as f:
                f.write(b"ER\x02\x00")

        def far_in():                       # only the 60th block has anything
            sshosdisk.make(self.image, 4, blank=True)
            with open(self.image, "r+b") as f:
                f.seek(60 * 512 + 7)
                f.write(b"\x01")

        for case in (other_name, mac_disk, mac_partitions, far_in):
            unchanged(case)

    def test_scratch_disk_may_be_formatted(self):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkscratch.py"), self.image],
                       check=True)
        k = self.kernel(mount=False)
        self.assertEqual(k.ask("look"), 2)
        self.assertEqual(k.ask("format"), 0)
        self.assertEqual(k.ask("mount"), 0)
        self.assertEqual(self.check(), (0, 0, 0))

    def test_full_disk(self):
        sshosdisk.make(self.image, 3)
        k = self.kernel()
        chunk = b"z" * 400000
        n = 0
        while self.put(k, "FILL%d.BIN" % n, chunk) == 0:
            n += 1
        self.assertGreater(n, 5)
        self.assertEqual(self.get(k, "FILL%d.BIN" % n), NOT_FOUND)      # nothing half-made
        self.assertEqual(self.check(), (n, n * -(-len(chunk) // 512), 0))
        # replacing a file needs room for the new one first; the old one survives
        self.assertEqual(self.put(k, "FILL0.BIN", chunk), FULL)
        self.assertEqual(self.get(k, "FILL0.BIN"), chunk)
        self.assertEqual(k.ask("delete FILL1.BIN"), 0)
        self.assertEqual(self.put(k, "FILL0.BIN", b"new"), 0)
        self.assertEqual(self.get(k, "FILL0.BIN"), b"new")
        self.assertEqual(self.check()[2], 0)

    def test_full_folder(self):
        sshosdisk.make(self.image, 4)
        k = self.kernel()
        for i in range(511):                # 512 entries, one is the volume's name
            self.assertEqual(self.put(k, "N%d" % i, b"%d" % i), 0)
        self.assertEqual(self.put(k, "ONEMORE", b"x"), FULL)
        self.assertEqual(self.put(k, "N5", b"again"), 0)
        self.assertEqual(self.get(k, "N510"), b"510")
        self.assertEqual(self.check(), (511, 511, 0))

    def test_too_big_for_the_buffer_is_refused(self):
        sshosdisk.make(self.image, 4)
        self.tool_write("HUGE.BIN", b"h" * (1 << 20) + b"!")
        self.assertEqual(self.get(self.kernel(), "HUGE.BIN"), TOO_BIG)

    def test_switched_off_during_a_write(self):
        """Wherever the writing stops, the file is the old one or the new one
        and the volume is in order."""
        sshosdisk.make(self.image, 4)
        old, new = b"old " * 700, b"new! " * 900
        k = self.kernel()
        self.assertEqual(self.put(k, "KEEP.TXT", b"untouched"), 0)
        self.assertEqual(self.put(k, "A.TXT", old), 0)
        self.assertEqual(k.close(), 0)
        seen = set()
        for cases, first, second in (("replace", old, new), ("create", None, new),
                                     ("delete", old, None)):
            for dies_at in range(0, 40):
                copy = self.path("copy.img")
                shutil.copy(self.image, copy)
                k = self.kernel(image=copy)
                if first is None:
                    self.assertEqual(k.ask("delete A.TXT"), 0)
                k.ask("die %d" % dies_at)
                r = self.put(k, "A.TXT", second) if second is not None else k.ask("delete A.TXT")
                self.assertEqual(k.close(), 0)
                k = self.kernel(image=copy)                 # switched on again
                got = self.get(k, "A.TXT")
                want = [first if first is not None else NOT_FOUND,
                        second if second is not None else NOT_FOUND]
                self.assertIn(got, want, "%s, stopped after %d blocks" % (cases, dies_at))
                if r == 0:
                    self.assertEqual(got, want[1])
                seen.add((cases, got == want[1]))
                self.assertEqual(self.get(k, "KEEP.TXT"), b"untouched")
                # Raises if inconsistent. The spare copy of the table can be
                # a block behind the first: they are written one after the other.
                self.check(copy, copies=False)
                # and it still works afterwards
                self.assertEqual(self.put(k, "AFTER.TXT", b"fine"), 0)
                self.assertEqual(self.get(k, "AFTER.TXT"), b"fine")
                self.check(copy, copies=False)
                self.assertEqual(k.close(), 0)
        self.assertEqual(len(seen), 6)      # each case was seen both failing and succeeding


if __name__ == "__main__":
    unittest.main()
