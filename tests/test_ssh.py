#!/usr/bin/env python3
"""The SE's SSH client against a real OpenSSH server.

build/ssh_host is kernel/ssh.c built for this Mac (see tests/ssh_host.c).
The server is this Mac's own /usr/sbin/sshd, started here without special
rights on 127.0.0.1 with a throw-away host key, accepting one throw-away
user key, and stopped again at the end.   make test-ssh-host
"""
import base64
import getpass
import os
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ssh_fixture  # noqa: E402

ROOT =os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "build", "sshd")
BIN = os.path.join(ROOT, "build", "ssh_host")
PORT = 22022
PROXY_PORT = 22023
USER = getpass.getuser()



class Client:
    """One run of the SE's client, with what it shows and a way to type."""

    started = []        # every client process, so a failed test leaves none behind

    def __init__(self, settings, port=PORT, user=USER, disk=None):
        path = os.path.join(DIR, "settings-%d.txt" % threading.get_ident())
        with open(path, "w") as f:
            f.write("\n".join(settings) + "\n")
        self.proc = subprocess.Popen([BIN, path, "%s@127.0.0.1:%d" % (user, port)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0,
                                     env=dict(os.environ, SSHOS_DISK=disk) if disk else None)
        self.out = b""
        Client.started.append(self.proc)

    def read(self, timeout):
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if ready:
            data = os.read(self.proc.stdout.fileno(), 65536)
            self.out += data
            return bool(data)
        return False

    def expect(self, text, timeout=20):
        """Wait until text has been shown; returns whether it was."""
        want = text.encode("utf-8")
        end = time.time() + timeout
        while want not in self.out and time.time() < end and self.proc.poll() is None:
            self.read(0.2)
        while self.read(0):
            pass
        return want in self.out

    def type(self, data):
        if isinstance(data, str):
            data = data.encode("ascii")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def text(self):
        return self.out.decode("utf-8", "replace")

    def finish(self, timeout=20):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        end = time.time() + timeout
        while self.proc.poll() is None and time.time() < end:
            self.read(0.2)
        if self.proc.poll() is None:
            self.proc.kill()
        while self.read(0):
            pass
        err = self.proc.stderr.read().decode("utf-8", "replace")
        self.proc.stdout.close()
        self.proc.stderr.close()
        self.proc.wait()
        return self.proc.returncode, err


class Proxy(threading.Thread):
    """Passes one connection through to the server, changing one byte on the
    way back: `flip` says which, given the server's stream so far."""

    def __init__(self, flip):
        super().__init__(daemon=True)
        self.flip = flip
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", PROXY_PORT))
        self.listener.listen(1)
        self.changed = False
        self.start()

    def run(self):
        client, _ = self.listener.accept()
        self.listener.close()
        server = socket.create_connection(("127.0.0.1", PORT))
        stream = b""
        try:
            while True:
                ready, _, _ = select.select([client, server], [], [], 30)
                if not ready:
                    break
                if client in ready:
                    data = client.recv(65536)
                    if not data:
                        break
                    server.sendall(data)
                if server in ready:
                    data = server.recv(65536)
                    if not data:
                        break
                    at = None if self.changed else self.flip(stream + data)
                    if at is not None and at >= len(stream):
                        i = at - len(stream)
                        data = data[:i] + bytes([data[i] ^ 0x20]) + data[i + 1:]
                        self.changed = True
                    stream += data
                    client.sendall(data)
        except OSError:
            pass
        client.close()
        server.close()


def packet_starts(stream):
    """Where the unencrypted packets after the version line start."""
    nl = stream.find(b"\n")
    starts, at = [], nl + 1
    while nl >= 0 and at + 4 <= len(stream) and len(starts) < 3:
        starts.append(at)
        at += 4 + struct.unpack(">L", stream[at:at + 4])[0]
    return starts + ([at] if len(starts) == 3 and at <= len(stream) else [])


class SshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A change of keys every 256 KB, to have several in the long-output test.
        cls.known, cls.key, cls.other_key = ssh_fixture.prepare(DIR, PORT,
                                                                extra=["RekeyLimit 256K"])
        cls.fingerprint = cls.known.split()[-1]     # as OpenSSH itself prints it
        cls.logfile = os.path.join(DIR, "sshd.log")
        if os.path.exists(cls.logfile):
            os.remove(cls.logfile)
        cls.sshd = subprocess.Popen(["/usr/sbin/sshd", "-D", "-E", cls.logfile, "-f",
                                     os.path.join(DIR, "sshd_config")])
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.sshd.terminate()
        cls.sshd.wait(timeout=5)

    def tearDown(self):
        for proc in Client.started:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        del Client.started[:]

    def server_log(self):
        with open(self.logfile, errors="replace") as f:
            return f.read()

    def login(self, settings=None, **kw):
        c = Client(settings or [self.known, self.key], **kw)
        self.assertTrue(c.expect("Logged in"), c.text())
        self.assertTrue(c.expect("ready> "), c.text())
        return c

    def clean_end(self, c):
        code, err = c.finish()
        self.assertEqual((code, err), (0, ""), c.text())

    def test_login_and_shell(self):
        c = self.login()
        self.assertNotIn("first connection", c.text())      # the fingerprint was known
        c.type("echo result-$((6*7))\n")
        self.assertTrue(c.expect("\nresult-42"), c.text())
        # UTF-8 from the server: an accent, a line-drawing character, and one we lack
        # (an emoji, which counts as two columns: a ? and a blank)
        c.type("printf 'ni\\303\\261o \\342\\224\\200 \\360\\237\\230\\200 x\\n'\n")
        self.assertTrue(c.expect("\nniño ─ ?  x"), c.text())
        # an accented letter typed on the SE (0xF1 is its code for n with tilde)
        c.type(b"echo se\xf1or | od -An -tx1\n")
        self.assertTrue(c.expect("73 65 c3 b1 6f 72 0a") or
                        "73 65 c3 b1 6f 72 0a" in " ".join(c.text().split()), c.text())
        # escape sequences are swallowed by the plain screen
        c.type("printf '\\033[1mBOLD\\033[0m \\033]0;a title\\007done\\n'\n")
        self.assertTrue(c.expect("\nBOLD done"), c.text())
        # the Left arrow (byte 1 here) reaches the shell's line editor
        c.type(b"echo abc\x01\x01X\n")
        self.assertTrue(c.expect("\naXbc"), c.text())
        c.type("exit 3\n")
        self.assertTrue(c.expect("ended with status 3"), c.text())
        self.assertTrue(c.expect("[Connection to 127.0.0.1 ended.]"), c.text())
        self.clean_end(c)
        log = self.server_log()
        self.assertIn("Accepted publickey for " + USER, log)

    def test_first_connection_asks(self):
        c = Client([self.key])
        self.assertTrue(c.expect("Continue? Type yes: "), c.text())
        self.assertIn(self.fingerprint, c.text())           # the same as ssh-keygen prints
        c.type("yes\n")
        self.assertTrue(c.expect("ready> "), c.text())
        self.assertIn(self.known, c.text())                 # the line to add to config.txt
        c.type("exit\n")
        self.assertTrue(c.expect("[Connection to 127.0.0.1 ended.]"), c.text())
        self.clean_end(c)

        c = Client([self.key])
        self.assertTrue(c.expect("Continue? Type yes: "), c.text())
        c.type("no\n")
        self.assertTrue(c.expect("the server's key was not accepted"), c.text())
        self.assertNotIn("Logged in", c.text())
        self.clean_end(c)

    def test_disk_remembers_hosts_and_connections(self):
        disk = os.path.join(DIR, "disk-%d" % os.getpid())
        os.makedirs(disk, exist_ok=True)
        for name in os.listdir(disk):
            os.remove(os.path.join(disk, name))
        target = "%s@127.0.0.1:%d" % (USER, PORT)

        c = Client([self.key], disk=disk)                   # first time: asked, then kept
        self.assertTrue(c.expect("Continue? Type yes: "), c.text())
        c.type("yes\n")
        self.assertTrue(c.expect("ready> "), c.text())
        self.assertIn("Remembered on the SSH OS disk", c.text())
        c.type("exit\n")
        self.clean_end(c)
        with open(os.path.join(disk, "HOSTS.TXT")) as f:
            self.assertEqual(f.read(), "127.0.0.1 %s\n" % self.fingerprint)
        with open(os.path.join(disk, "RECENT.TXT")) as f:
            self.assertEqual(f.read(), target + "\n")

        with open(os.path.join(disk, "RECENT.TXT"), "w") as f:     # older ones, and too many
            f.write("".join("user%d@host%d\n" % (i, i) for i in range(12)))
        c = Client([self.key], disk=disk)                   # second time: not asked
        self.assertTrue(c.expect("ready> "), c.text())
        self.assertNotIn("Continue?", c.text())
        c.type("exit\n")
        self.clean_end(c)
        with open(os.path.join(disk, "RECENT.TXT")) as f:   # newest first, eight kept
            self.assertEqual(f.read().split(), [target] + ["user%d@host%d" % (i, i) for i in range(7)])

        with open(os.path.join(disk, "HOSTS.TXT"), "w") as f:      # someone else answers now
            f.write("elsewhere %s\n127.0.0.1 SHA256:%s\n" % (
                self.fingerprint, base64.b64encode(bytes(32)).decode().rstrip("=")))
        c = Client([self.key], disk=disk)
        self.assertTrue(c.expect("the server's key has changed"), c.text())
        self.assertNotIn("Logged in", c.text())
        self.clean_end(c)

    def test_changed_host_key_is_refused(self):
        wrong = "ssh_hosts = 127.0.0.1 SHA256:" + base64.b64encode(bytes(32)).decode().rstrip("=")
        c = Client([wrong, self.key])
        self.assertTrue(c.expect("the server's key has changed"), c.text())
        self.assertIn("WARNING", c.text())
        self.assertNotIn("Logged in", c.text())
        self.assertNotIn("Signing in", c.text())
        self.clean_end(c)
        # a fingerprint on record for another host does not count for this one
        c = Client(["ssh_hosts = elsewhere " + self.fingerprint, self.key])
        self.assertTrue(c.expect("Continue? Type yes: "), c.text())
        c.type("\x1b")
        self.assertTrue(c.expect("not accepted"), c.text())
        self.clean_end(c)

    def test_password_when_key_is_refused(self):
        marker = "password-test-%d" % os.getpid()
        c = Client([self.known, self.other_key])
        self.assertTrue(c.expect("does not accept the key"), c.text())
        for i in range(3):
            self.assertTrue(c.expect("password: " if i == 0 else "Not accepted.\n%s@127.0.0.1's password: " % USER), c.text())
            c.out = b""
            c.type(marker + "\n")
        self.assertTrue(c.expect("could not log in"), c.text())
        self.assertNotIn(marker, c.text())                  # passwords are not shown
        self.assertNotIn("Logged in", c.text())
        self.clean_end(c)
        # The server understood three password attempts (it cannot check
        # passwords the way it runs here, so it refused them all).
        time.sleep(0.3)
        self.assertGreaterEqual(self.server_log().count("Failed password for " + USER), 3)

    def test_no_key_and_wrong_user(self):
        c = Client([self.known], user="nobody-such-user")
        self.assertTrue(c.expect("password: "), c.text())
        c.type("\x1b")
        self.assertTrue(c.expect("stopped at the password"), c.text())
        self.clean_end(c)

    def test_long_output_and_new_keys(self):
        """600 KB of output: every line arrives, across several changes of keys."""
        c = self.login()
        c.type("seq 1 90000; echo finished\n")
        self.assertTrue(c.expect("\nfinished", timeout=120), c.text()[-2000:])
        # (this Mac's terminal layer now and then ends a line with two
        # carriage returns; that is the server side, and harmless)
        # The notice about new keys can land in the middle of a line; with it
        # taken out again, not a character may be missing.
        notice = (r"\r*\n\[The server asks for fresh keys[^\]]*\]\r*\n"
                  r"Making a one-time key\.*\r*\nWorking out the shared secret\.*\r*\n"
                  r"Checking the server's signature\.*\r*\n")
        self.assertGreaterEqual(len(re.findall(notice, c.text())), 2)
        numbers = re.findall(r"^(\d+)\r*$", re.sub(notice, "", c.text()), re.M)
        self.assertEqual(numbers, [str(i) for i in range(1, 90001)])
        c.type("echo still-here\n")
        self.assertTrue(c.expect("\nstill-here"), c.text()[-2000:])
        c.type("exit\n")
        self.assertTrue(c.expect("[Connection to 127.0.0.1 ended.]"), c.text()[-2000:])
        self.clean_end(c)

    def tampered(self, flip):
        proxy = Proxy(flip)
        c = Client([self.known, self.key], port=PROXY_PORT)
        c.expect("[Connection to 127.0.0.1 ended", timeout=60)
        self.clean_end(c)
        proxy.join(timeout=5)
        self.assertTrue(proxy.changed, "the proxy never got to change a byte")
        self.assertNotIn("Logged in", c.text())
        return c.text()

    def test_forged_key_exchange_is_noticed(self):
        # A byte of the signature, which ends the server's second packet
        # (before its padding).
        def in_signature(stream):
            starts = packet_starts(stream)
            if len(starts) < 3:
                return None
            pad = stream[starts[1] + 4]
            return starts[2] - pad - 20
        self.assertIn("the server's signature is wrong", self.tampered(in_signature))

        # A byte of the server's half of the key exchange (32 bytes, just
        # before the signature block of 4 + 4 + 11 + 4 + 64 bytes).
        def in_exchange_value(stream):
            starts = packet_starts(stream)
            if len(starts) < 3:
                return None
            pad = stream[starts[1] + 4]
            return starts[2] - pad - 87 - 10
        self.assertIn("the server's signature is wrong", self.tampered(in_exchange_value))

    def test_damaged_packet_is_noticed(self):
        # A byte of the first encrypted packet (the fourth one).
        def in_encrypted(stream):
            starts = packet_starts(stream)
            if len(starts) < 4 or len(stream) < starts[3] + 12:
                return None
            return starts[3] + 10
        self.assertIn("check sum wrong", self.tampered(in_encrypted))

        def in_length(stream):
            starts = packet_starts(stream)
            if len(starts) < 4 or len(stream) < starts[3] + 4:
                return None
            return starts[3] + 3
        text = self.tampered(in_length)
        self.assertTrue("check sum wrong" in text or "cannot take" in text or
                        "no answer in time" in text, text)


if __name__ == "__main__":
    unittest.main()
