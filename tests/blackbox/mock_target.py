#!/usr/bin/env python3
# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later

"""A device on the other end of a network cue, that a driver can script.

WHY A SECOND PROGRAM. The unit suite already points Go.dot's OSCQuery client at
Go.dot's own OSCQuery server, and that is a strong test of the client. What it
cannot be is a test of the SHIPPED BINARY talking to something that is not
itself: same process, same code, same idea of what a bare `?VALUE` means. This
is a device written from the specification, in another language, with no shared
code — so when the two agree, the agreement means something.

WHAT IT IS. A UDP socket that listens for OSC and remembers what it was told,
and an HTTP server that answers OSCQuery questions about it. Between them that
is the whole of what a mounted target does.

FOUR BEHAVIOURS, because those are the four a real device has and each sends a
different person to look at a different thing:

    agree     — report back exactly what was written. A working processor.
    alter     — report back something else. A clipped range, a mode that ignores
                the parameter, a channel somebody re-patched at the weekend.
                The failure that means the device is THERE and is not doing what
                it was told.
    silent    — take the message and answer 204: the node is real and has no
                value. A device that is thinking, or that never reports.
    deaf      — no HTTP server at all. Not plugged in.

STDLIB ONLY, like everything else in this directory, and a separate OSC decoder
from the one under test for the same reason common.py gives: a driver that
decoded through the implementation it is checking would agree with it about any
mistake they both made.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HOST = "127.0.0.1"


# --------------------------------------------------------------------------- OSC
def osc_decode(data: bytes):
    """`(address, [args])` from one OSC message, or None.

    Deliberately partial: this understands the type tags a network cue can
    carry — i, f, d, s, T, F — and refuses anything else rather than guessing.
    A mock that guessed would pass a test the real thing would fail.
    """
    def read_string(buf, at):
        end = buf.index(b"\0", at)
        text = buf[at:end].decode("utf-8", "replace")
        return text, (end + 4) & ~3

    try:
        address, at = read_string(data, 0)
        tags, at = read_string(data, at)
    except (ValueError, IndexError):
        return None

    if not address.startswith("/") or not tags.startswith(","):
        return None

    args = []

    for tag in tags[1:]:
        if tag == "i":
            args.append(struct.unpack_from(">i", data, at)[0]); at += 4
        elif tag == "f":
            args.append(struct.unpack_from(">f", data, at)[0]); at += 4
        elif tag == "d":
            args.append(struct.unpack_from(">d", data, at)[0]); at += 8
        elif tag == "s":
            text, at = read_string(data, at)
            args.append(text)
        elif tag == "T":
            args.append(True)
        elif tag == "F":
            args.append(False)
        else:
            return None

    return address, args


# --------------------------------------------------------------------------- state
class Device:
    """What the box currently believes, and how honest it is about it."""

    def __init__(self, behaviour: str, alter_to):
        self.behaviour = behaviour
        self.alter_to = alter_to
        self.lock = threading.Lock()
        self.values = {}
        self.received = 0

    def note(self, address: str, args):
        with self.lock:
            self.received += 1

            if not args:
                return

            if self.behaviour == "alter":
                self.values[address] = self.alter_to
            else:
                self.values[address] = args[0]

    def value_of(self, address: str):
        with self.lock:
            if self.behaviour == "silent":
                return None

            return self.values.get(address)

    def count(self) -> int:
        with self.lock:
            return self.received


# --------------------------------------------------------------------------- UDP
def listen_udp(device: Device, port_out) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, 0))
    port_out.append(sock.getsockname()[1])

    def run():
        while True:
            try:
                data, _ = sock.recvfrom(65536)
            except OSError:
                return

            decoded = osc_decode(data)

            if decoded is not None:
                device.note(*decoded)

    threading.Thread(target=run, daemon=True).start()
    return sock


# --------------------------------------------------------------------------- HTTP
def make_handler(device: Device):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass                                    # quiet: the driver owns stdout

        def do_GET(self):                           # noqa: N802 (http.server's name)
            path, _, query = self.path.partition("?")

            # THE BARE KEY IS THE WHOLE POINT. OSCQuery asks `?VALUE`, not
            # `?VALUE=`, and a client that helpfully re-encoded it would be
            # answered 400 here — which is exactly the mistake juce::URL makes
            # and the reason Go.dot writes its own HTTP request.
            if query and query != "VALUE":
                self.send_response(400)
                self.end_headers()
                return

            if not query:
                self.reply(200, {"FULL_PATH": path, "CONTENTS": {}})
                return

            value = device.value_of(path)

            if value is None:
                # 204: the node is real and has no value yet. A different
                # answer from 404, and both are different from a JSON null.
                self.send_response(204)
                self.end_headers()
                return

            self.reply(200, {"VALUE": [value]})

        def reply(self, status: int, body) -> None:
            encoded = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

    return Handler


# --------------------------------------------------------------------------- main
def main() -> int:
    parser = argparse.ArgumentParser(description="A scriptable OSC/OSCQuery device.")
    parser.add_argument("--behaviour", default="agree",
                        choices=("agree", "alter", "silent", "deaf"))
    parser.add_argument("--alter-to", type=float, default=0.0,
                        help="what an `alter` device reports instead")
    args = parser.parse_args()

    device = Device(args.behaviour, args.alter_to)

    ports = []
    listen_udp(device, ports)

    query_port = 0
    server = None

    if args.behaviour != "deaf":
        server = HTTPServer((HOST, 0), make_handler(device))
        query_port = server.server_address[1]

    # The ports, on one line, flushed: the driver reads this to find out where
    # to point Go.dot's mount. Binding zero and reporting back is this suite's
    # rule — a fixed number makes a test that cannot run twice at once.
    print(f"osc {ports[0]} query {query_port}", flush=True)

    if server is None:
        threading.Event().wait()
    else:
        server.serve_forever()

    return 0


if __name__ == "__main__":
    sys.exit(main())
