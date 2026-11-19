#!/usr/bin/env python3
"""juanterm: the Mac-side helper for the SSH OS "term" program.

It listens for the Macintosh SE, checks that it knows the shared secret,
runs a shell for it and keeps the SE's screen up to date. The protocol is
described at the top of kernel/term.c.

    python3 -m tools.juanterm.server --secret-file ~/.juanterm-secret

Security: whoever passes the secret check gets a shell as you. The secret
is never sent over the network, but everything typed and shown afterwards
is, unencrypted. Use it on your own home network only, choose a long
secret, and prefer --allow with the SE's address.
"""
import argparse
import fcntl
import hashlib
import hmac
import os
import pty
import select
import signal
import socket
import struct
import sys
import termios
import time

from . import charset
from .diff import copy_screen, encode_update
from .vt import Screen

HELLO = b"JT1"
SETTLE = 0.008          # wait this long for more output before sending an update
MAX_SETTLE = 0.040      # ...but never hold an update longer than this

ARROWS = {1: b"A", 2: b"B", 3: b"C", 4: b"D", 5: b"H", 6: b"F"}
TILDE_KEYS = {7: 5, 8: 6, 9: 3, 15: 15, 16: 17, 17: 18, 18: 19, 19: 20, 20: 21, 21: 23, 22: 24}
F1_TO_F4 = {11: b"P", 12: b"Q", 13: b"R", 14: b"S"}


def special_key(code, screen):
    """The bytes a terminal sends for a special key, in the mode the program
    on the other side has asked for."""
    if code in ARROWS:
        return (b"\x1bO" if screen.app_cursor_keys else b"\x1b[") + ARROWS[code]
    if code in F1_TO_F4:
        return b"\x1bO" + F1_TO_F4[code]
    if code in TILDE_KEYS:
        return b"\x1b[%d~" % TILDE_KEYS[code]
    return b""


def typed_bytes(body):
    """What the SE typed, as the bytes a UTF-8 terminal would send. The SE
    sends one byte per character in its own character set."""
    if all(b < 0x80 for b in body):
        return body
    return b"".join(bytes([b]) if b < 0x80 else (charset.char(b) or "").encode("utf-8")
                    for b in body)


def read_exact(sock, n, deadline):
    data = b""
    while len(data) < n:
        left = deadline - time.monotonic()
        if left <= 0:
            return None
        ready, _, _ = select.select([sock], [], [], left)
        if not ready:
            return None
        piece = sock.recv(n - len(data))
        if not piece:
            return None
        data += piece
    return data


def handshake(sock, secret):
    """Returns (cols, rows), or None if the caller did not know the secret."""
    nonce = os.urandom(16)
    sock.sendall(HELLO + nonce)
    answer = read_exact(sock, 34, time.monotonic() + 10)
    if answer is None:
        return None
    want = hmac.new(secret, nonce, hashlib.sha256).digest()
    if not hmac.compare_digest(want, answer[:32]):
        time.sleep(2)               # slow down guessing
        try:
            sock.sendall(b"\x00")
        except OSError:
            pass
        return None
    sock.sendall(b"\x01")
    cols = max(20, min(132, answer[32]))
    rows = max(5, min(60, answer[33]))
    return cols, rows


def spawn_shell(command, cols, rows, env_extra):
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ)
        env.update({"TERM": "xterm", "COLUMNS": str(cols), "LINES": str(rows), "NO_COLOR": "1"})
        env.pop("COLORTERM", None)
        if not any(env.get(name) for name in ("LC_ALL", "LC_CTYPE", "LANG")):
            # Started without a language setting (from a script, say): the
            # shell must still take typed accented letters as UTF-8.
            env["LC_CTYPE"] = "UTF-8" if sys.platform == "darwin" else "C.UTF-8"
        env.update(env_extra)
        try:
            os.execvpe(command[0], command, env)
        finally:
            os._exit(127)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    try:
        # Tell the terminal driver the text is UTF-8, so Delete removes a
        # whole accented letter in programs that leave line editing to it.
        attrs = termios.tcgetattr(fd)
        attrs[0] |= getattr(termios, "IUTF8", 0x4000)
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except termios.error:
        pass
    return pid, fd


def session(sock, secret, command, env_extra, log):
    size = handshake(sock, secret)
    if size is None:
        log("refused: wrong secret or no answer")
        return
    cols, rows = size
    log("connected, %d x %d" % (cols, rows))
    screen = Screen(cols, rows)
    pid, fd = spawn_shell(command, cols, rows, env_extra)
    shown = None                    # the screen the SE has, None = unknown
    shown_cursor = None
    bells = 0
    unacked = 0                     # updates sent and not yet drawn
    dirty_since = None              # when output first arrived after the last update
    last_output = 0.0
    inbuf = b""
    alive = True
    try:
        while True:
            now = time.monotonic()
            timeout = 1.0
            if dirty_since is not None and unacked == 0:
                due = min(last_output + SETTLE, dirty_since + MAX_SETTLE)
                if now >= due or not alive:
                    cursor = (screen.cursor_row, screen.cursor_col, screen.cursor_visible)
                    if shown is None or screen.cells != shown or cursor != shown_cursor \
                            or screen.bell_count != bells:
                        messages = encode_update(shown, screen.cells, cursor,
                                                 screen.bell_count != bells)
                        for m in messages:
                            sock.sendall(b"U" + struct.pack(">H", len(m)) + m)
                        unacked = len(messages)
                        shown = copy_screen(screen.cells)
                        shown_cursor = cursor
                        bells = screen.bell_count
                    dirty_since = None
                    continue
                timeout = due - now
            if not alive and dirty_since is None and unacked == 0:
                sock.sendall(b"X\x00\x00")
                log("shell ended")
                return
            watch = [sock] + ([fd] if alive else [])
            ready, _, _ = select.select(watch, [], [], timeout)
            if fd in ready:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    data = b""
                if not data:
                    alive = False
                    dirty_since = dirty_since or time.monotonic()
                else:
                    reply = screen.feed(data)
                    if reply:
                        os.write(fd, reply)
                    last_output = time.monotonic()
                    if dirty_since is None:
                        dirty_since = last_output
            if sock in ready:
                data = sock.recv(4096)
                if not data:
                    log("the SE closed the connection")
                    return
                inbuf += data
                while len(inbuf) >= 3:
                    kind, length = inbuf[0:1], struct.unpack(">H", inbuf[1:3])[0]
                    if len(inbuf) < 3 + length:
                        break
                    body, inbuf = inbuf[3:3 + length], inbuf[3 + length:]
                    if kind == b"K" and alive:
                        os.write(fd, typed_bytes(body))
                    elif kind == b"S" and alive and body:
                        os.write(fd, special_key(body[0], screen))
                    elif kind == b"A":
                        unacked = max(0, unacked - 1)
                        if unacked == 0 and dirty_since is None:
                            dirty_since = time.monotonic() - MAX_SETTLE   # check for news at once
    finally:
        try:
            os.kill(pid, signal.SIGHUP)
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="Terminal helper for SSH OS on a Macintosh SE")
    ap.add_argument("--listen", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--secret-file", help="file holding the shared secret (first line)")
    ap.add_argument("--allow", action="append", default=[],
                    help="only accept connections from this address (may be repeated)")
    ap.add_argument("--once", action="store_true", help="serve one connection, then exit")
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                    help="extra environment for the shell")
    ap.add_argument("command", nargs="*", help="what to run (default: your login shell)")
    args = ap.parse_args(argv)

    secret = os.environ.get("JUANTERM_SECRET", "")
    if args.secret_file:
        with open(os.path.expanduser(args.secret_file)) as f:
            secret = f.readline().strip()
    if len(secret) < 8 or secret == "change-this-secret":
        sys.exit("juanterm: set a secret of at least 8 characters, with --secret-file FILE "
                 "or the JUANTERM_SECRET environment variable (and the same in config.txt).")
    command = args.command or [os.environ.get("SHELL", "/bin/zsh"), "-l"]
    env_extra = dict(item.split("=", 1) for item in args.env)

    def log(text):
        print(time.strftime("%H:%M:%S"), text, flush=True)

    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.listen, args.port))
    server.listen(1)
    log("juanterm listening on %s port %d" % (args.listen, args.port))
    while True:
        sock, peer = server.accept()
        if args.allow and peer[0] not in args.allow:
            log("turned away %s (not in --allow)" % peer[0])
            sock.close()
            continue
        log("connection from %s" % peer[0])
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            session(sock, secret.encode(), command, env_extra, log)
        except (OSError, ValueError) as e:
            log("session ended: %s" % e)
        finally:
            sock.close()
        if args.once:
            return


if __name__ == "__main__":
    main()
