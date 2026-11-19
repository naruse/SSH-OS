#!/usr/bin/env python3
"""A stand-in for Home Assistant, for testing the SSH OS "home" panel.

It answers the two kinds of request the panel makes (see kernel/home.c) for
a handful of made-up devices, checks the access token, and writes one line
per request to a log so a test can see what the panel asked for. It listens
on this computer only.

    mockha.py [--port 8123] [--token test-token] [--log FILE] [--chunked]
"""
import argparse
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8123)
ap.add_argument("--token", default="test-token")
ap.add_argument("--log", default="-")
ap.add_argument("--chunked", action="store_true", help="answer with chunked transfer encoding")
args = ap.parse_args()

# entity: [name, state, brightness or None, dimmable]
DEVICES = {
    "light.living_room": ["Living room", "on", 204, True],
    "light.kitchen": ["Kitchen", "off", None, True],
    "light.porch": ["Porch", "on", None, False],
    "switch.coffee_maker": ["Coffee maker", "off", None, False],
    "switch.fan": ["Fan", "on", None, False],
    "light.bedroom": ["Lámpara dormitorio", "off", None, True],
    "switch.printer": ["Printer", "unavailable", None, False],
}
for _n in range(1, 9):          # enough extra switches to need a second page
    DEVICES["switch.socket_%d" % _n] = ["Socket %d" % _n, "off", None, False]

log = sys.stdout if args.log == "-" else open(args.log, "w")


def note(text):
    log.write(text + "\n")
    log.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, body, content_type="application/json"):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if args.chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for i in range(0, len(data), 50):       # several small chunks
                piece = data[i:i + 50]
                self.wfile.write(b"%X\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
        self.close_connection = True

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        if self.headers.get("Authorization") != "Bearer " + args.token:
            note("refused: bad token")
            return self.reply(401, '{"message": "Unauthorized"}')
        try:
            body = json.loads(raw)
        except ValueError:
            note("bad json: %r" % raw)
            return self.reply(400, '{"message": "Invalid JSON"}')

        if self.path == "/api/template":
            template = body.get("template", "")
            for part in ("s.entity_id", "s.name", "s.state", "brightness", "supported_color_modes"):
                if part not in template:
                    note("template lacks %s" % part)
                    return self.reply(400, '{"message": "bad template"}')
            wanted = list(DEVICES)
            m = re.search(r"set all = \[(.*?)\]", template)
            if m:
                wanted = [e for e in re.findall(r"'([^']*)'", m.group(1)) if e in DEVICES]
            m = re.search(r"in all\[(\d+):(\d+)\]", template)
            if not m or "#{{ all | count }}" not in template:
                note("template lacks the page or the count")
                return self.reply(400, '{"message": "bad template"}')
            first, last = int(m.group(1)), int(m.group(2))
            note("template devices %d to %d of %d" % (first, last, len(wanted)))
            lines = ["#%d\n" % len(wanted)]
            for e in wanted[first:last]:
                name, state, brightness, dimmable = DEVICES[e]
                lines.append("%s|%s|%s|%s|%s\n" % (e, name, state, brightness or 0, dimmable))
            return self.reply(200, "".join(lines), "text/plain; charset=utf-8")

        m = re.match(r"/api/services/(\w+)/(\w+)$", self.path)
        if m:
            entity = body.get("entity_id")
            if entity not in DEVICES:
                note("unknown entity %s" % entity)
                return self.reply(400, '{"message": "unknown entity"}')
            dev = DEVICES[entity]
            service = m.group(2)
            if service == "toggle":
                dev[1] = "off" if dev[1] == "on" else "on"
                note("toggle %s -> %s" % (entity, dev[1]))
            elif service == "turn_on":
                dev[1] = "on"
                if "brightness" in body:
                    dev[2] = int(body["brightness"])
                note("turn_on %s %s" % (entity, body.get("brightness", "")))
            elif service == "turn_off":
                dev[1] = "off"
                note("turn_off %s" % entity)
            else:
                return self.reply(400, '{"message": "unknown service"}')
            if dev[1] == "on" and dev[3] and not dev[2]:
                dev[2] = 255
            return self.reply(200, "[]")
        note("unknown path %s" % self.path)
        self.reply(404, "404: Not Found", "text/plain")


note("mock Home Assistant on 127.0.0.1 port %d" % args.port)
HTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
