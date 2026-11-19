#!/usr/bin/env python3
"""TCP servers for testing SSH OS in the emulator. They listen on this
computer only (127.0.0.1), which the emulator's gateway can reach.

  port 7001  sends a fixed test stream and closes
  port 7002  echoes whatever it receives

Usage: testservers.py [BYTES]     (length of the test stream, default 262144)
"""
import socket
import sys
import threading

SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 262144
# kernel/cmd_tcp.c computes the same bytes to check what it receives.
STREAM = bytes((i * 131 + (i >> 8) * 7 + (i >> 16) + 3) & 0xFF for i in range(SIZE))


def source(conn):
    with conn:
        conn.sendall(STREAM)


def echo(conn):
    with conn:
        while True:
            data = conn.recv(4096)
            if not data:
                return
            conn.sendall(data)


def serve(port, handler):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen()
    while True:
        conn, _ = s.accept()
        threading.Thread(target=handler, args=(conn,), daemon=True).start()


threading.Thread(target=serve, args=(7001, source), daemon=True).start()
print("test servers on 127.0.0.1: 7001 (stream of %d bytes), 7002 (echo)" % SIZE, flush=True)
serve(7002, echo)
