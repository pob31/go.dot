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

WITH --transport tcp (namespace draft 57, AFJ; DP.6) the socket is a LISTENER
instead, an Eos's on 3032: each connection's stream is cut by --framing -
`length`, a four-byte size before each packet (OSC 1.0 over TCP), or `slip` -
and every packet is noted as a datagram's would be. `/_mock/connections` counts
the connections taken since the start, and `/_mock/drop` closes the open ones
and keeps listening, which is the console going away and coming back: what a
link's retry is for. With --framing lines each line is noted as a message whose
address is the line, and with --wire rcp (DP.7) the device is a Yamaha console
as far as the grammar goes: it answers `OK` with the line echoed, and
`/_mock/say?<line>` has it say a line of its own down every open connection -
`NOTIFY set ...`, the console reporting a fader moved.

AND, WITH --listen, A DEVICE THAT PUSHES (namespace draft 45, O.10), as WFS-DIY
does: HOST_INFO offers LISTEN, the HTTP port takes a WebSocket, a LISTEN or an
IGNORE names an address, and a value that changes is pushed down the socket as
one binary OSC message - never to the IP whose datagram caused the change,
which is WFS-DIY's rule and why Go.dot playing a curve into it cannot loop back.
`/_mock/move<address>?<value>` is a hand on the device's own screen: a change
nobody's datagram caused, pushed to every listener. A bundle arriving is taken
apart and counted, and every message is kept with the moment it arrived.

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
import base64
import hashlib
import json
import socket
import struct
import sys
import threading
import urllib.parse
import time
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer

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


def osc_unbundle(data: bytes, out: list) -> bool:
    """Every message of a packet into `out`, a bundle taken apart however deep.
    True when the packet was a bundle."""
    if not data.startswith(b"#bundle\0"):
        decoded = osc_decode(data)

        if decoded is not None:
            out.append(decoded)

        return False

    at = 16                                         # the marker and the time tag

    while at + 4 <= len(data):
        size = struct.unpack_from(">i", data, at)[0]
        at += 4

        if size < 0 or at + size > len(data):
            break

        osc_unbundle(data[at:at + size], out)
        at += size

    return True


def osc_encode(address: str, args) -> bytes:
    """One OSC message of floats, integers and strings: what a push carries."""
    def pad(raw: bytes) -> bytes:
        raw += b"\0"
        return raw + b"\0" * (-len(raw) % 4)

    tags, body = ",", b""

    for arg in args:
        if isinstance(arg, bool):
            tags += "T" if arg else "F"
        elif isinstance(arg, int):
            tags += "i"; body += struct.pack(">i", arg)
        elif isinstance(arg, float):
            tags += "f"; body += struct.pack(">f", arg)
        else:
            tags += "s"; body += pad(str(arg).encode("utf-8"))

    return pad(address.encode("utf-8")) + pad(tags.encode("ascii")) + body


# --------------------------------------------------------------------------- WebSocket
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class Listener:
    """One WebSocket that asked to be told about some addresses."""

    def __init__(self, sock: socket.socket, ip: str):
        self.sock = sock
        self.ip = ip
        self.lock = threading.Lock()
        self.addresses = set()

    def send(self, opcode: int, payload: bytes) -> None:
        header = bytes([0x80 | opcode])
        size = len(payload)

        if size < 126:
            header += bytes([size])
        elif size < 65536:
            header += bytes([126]) + struct.pack(">H", size)
        else:
            header += bytes([127]) + struct.pack(">Q", size)

        with self.lock:
            try:
                self.sock.sendall(header + payload)
            except OSError:
                pass


def read_exactly(sock: socket.socket, count: int) -> bytes:
    data = b""

    while len(data) < count:
        chunk = sock.recv(count - len(data))

        if not chunk:
            raise ConnectionError("closed")

        data += chunk

    return data


def read_frame(sock: socket.socket):
    """`(opcode, payload)` of one frame from a client - masked, as RFC 6455
    requires of every client frame."""
    first, second = read_exactly(sock, 2)
    size = second & 0x7F

    if size == 126:
        size = struct.unpack(">H", read_exactly(sock, 2))[0]
    elif size == 127:
        size = struct.unpack(">Q", read_exactly(sock, 8))[0]

    mask = read_exactly(sock, 4) if second & 0x80 else b"\0\0\0\0"
    payload = bytearray(read_exactly(sock, size))

    for at in range(len(payload)):
        payload[at] ^= mask[at % 4]

    return first & 0x0F, bytes(payload)


# --------------------------------------------------------------------------- state
class Device:
    """What the box currently believes, and how honest it is about it."""

    def __init__(self, behaviour: str, alter_to):
        self.behaviour = behaviour
        self.alter_to = alter_to
        self.lock = threading.Lock()
        self.values = {}
        self.received = 0
        self.messages = []
        self.timed = []
        self.bundles = 0
        self.connections = 0           # taken since the start (--transport tcp)
        self.open = []                 # the connections open now
        self.listeners = []
        self.started = time.monotonic()

    def note(self, address: str, args, sender_ip: "str | None" = None, bundle: int = -1):
        with self.lock:
            self.received += 1
            self.messages.append([address, list(args)])
            self.timed.append([time.monotonic() - self.started, bundle, address, list(args)])

            if not args:
                return

            if self.behaviour == "alter":
                self.values[address] = self.alter_to
            else:
                self.values[address] = args[0]

        #  AND TOLD TO WHOEVER LISTENS - but never to the IP that said it (WFS-DIY).
        self.push(address, list(args), skip_ip=sender_ip)

    def move(self, address: str, value) -> None:
        """A hand on the device's own screen: nobody's datagram, every listener told."""
        with self.lock:
            self.values[address] = value

        self.push(address, [value], skip_ip=None)

    def push(self, address: str, args, skip_ip: "str | None") -> None:
        with self.lock:
            targets = [listener for listener in self.listeners
                       if address in listener.addresses and listener.ip != skip_ip]

        packet = osc_encode(address, args)

        for listener in targets:
            listener.send(0x2, packet)

    def count_bundle(self) -> int:
        with self.lock:
            self.bundles += 1
            return self.bundles

    def value_of(self, address: str):
        with self.lock:
            if self.behaviour == "silent":
                return None

            return self.values.get(address)

    def count(self) -> int:
        with self.lock:
            return self.received

    def everything(self) -> list:
        with self.lock:
            return [list(message) for message in self.messages]

    def everything_timed(self) -> list:
        with self.lock:
            return [list(message) for message in self.timed]

    def listening(self) -> int:
        with self.lock:
            return sum(len(listener.addresses) for listener in self.listeners)


# --------------------------------------------------------------------------- UDP
def listen_udp(device: Device, port_out) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, 0))
    port_out.append(sock.getsockname()[1])

    def run():
        while True:
            try:
                data, sender = sock.recvfrom(65536)
            except OSError:
                return

            messages = []
            bundle = device.count_bundle() if osc_unbundle(data, messages) else -1

            for address, args in messages:
                device.note(address, args, sender_ip=sender[0], bundle=bundle)

    threading.Thread(target=run, daemon=True).start()
    return sock


def unslip(frame: bytes) -> bytes:
    """One SLIP frame's bytes: 0xDB 0xDC is a 0xC0 inside it, 0xDB 0xDD a 0xDB."""
    out = bytearray()
    escaped = False

    for byte in frame:
        if escaped:
            out.append(0xC0 if byte == 0xDC else 0xDB if byte == 0xDD else byte)
            escaped = False
        elif byte == 0xDB:
            escaped = True
        else:
            out.append(byte)

    return bytes(out)


def cut_frames(framing: str, buffer: bytes):
    """The whole packets at the front of `buffer`, and what is left of it."""
    packets = []

    if framing == "lines":
        while True:
            end = buffer.find(b"\n")

            if end < 0:
                break

            packets.append(buffer[:end].rstrip(b"\r"))
            buffer = buffer[end + 1:]

        return packets, buffer

    if framing == "length":
        while len(buffer) >= 4:
            size = int.from_bytes(buffer[:4], "big")

            if len(buffer) < 4 + size:
                break

            packets.append(buffer[4:4 + size])
            buffer = buffer[4 + size:]

        return packets, buffer

    #  SLIP: a frame ends on 0xC0; an empty one between two is nothing.
    while True:
        end = buffer.find(b"\xC0")

        if end < 0:
            break

        frame = buffer[:end]
        buffer = buffer[end + 1:]

        if frame:
            packets.append(unslip(frame))

    return packets, buffer


def listen_tcp(device: Device, port_out, framing: str, wire: str = "osc") -> socket.socket:
    """A listener for OSC over connections (DP.6), its port reported as the OSC
    port; each connection read on a thread of its own until the far end or
    `/_mock/drop` closes it."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind((HOST, 0))
    sock.listen(4)
    port_out.append(sock.getsockname()[1])

    def serve(conn: socket.socket, peer):
        buffer = b""

        try:
            while True:
                try:
                    chunk = conn.recv(65536)
                except OSError:
                    return

                if not chunk:
                    return

                buffer += chunk
                packets, buffer = cut_frames(framing, buffer)

                for data in packets:
                    #  A LINE IS A MESSAGE WHOSE ADDRESS IS THE LINE (DP.7), and
                    #  a console on the rcp wire answers OK with it echoed.
                    if framing == "lines":
                        line = data.decode("utf-8", "replace")
                        device.note(line, [], sender_ip=peer[0])

                        if wire == "rcp":
                            try:
                                conn.sendall(("OK " + line + "\n").encode("utf-8"))
                            except OSError:
                                return

                        continue

                    messages = []
                    bundle = device.count_bundle() if osc_unbundle(data, messages) else -1

                    for address, args in messages:
                        device.note(address, args, sender_ip=peer[0], bundle=bundle)
        finally:
            with device.lock:
                if conn in device.open:
                    device.open.remove(conn)

            conn.close()

    def run():
        while True:
            try:
                conn, peer = sock.accept()
            except OSError:
                return

            with device.lock:
                device.connections += 1
                device.open.append(conn)

            threading.Thread(target=serve, args=(conn, peer), daemon=True).start()

    threading.Thread(target=run, daemon=True).start()
    return sock


# --------------------------------------------------------------------------- HTTP
def make_handler(device: Device, listens: bool, osc_port_of):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass                                    # quiet: the driver owns stdout

        def do_GET(self):                           # noqa: N802 (http.server's name)
            path, _, query = self.path.partition("?")

            #  A WEBSOCKET, on the HTTP port as WFS-DIY's is (O.10).
            if listens and self.headers.get("Upgrade", "").lower() == "websocket":
                self.websocket()
                return

            if listens and query == "HOST_INFO":
                self.reply(200, {"NAME": "mock", "OSC_PORT": osc_port_of(), "OSC_TRANSPORT": "UDP",
                                 "EXTENSIONS": {"VALUE": True, "LISTEN": True}})
                return

            #  A HAND ON THE DEVICE'S OWN SCREEN, and what arrived when, and how
            #  many bundles (O.12).
            if path.startswith("/_mock/move/"):
                try:
                    device.move(path[len("/_mock/move"):], float(query))
                except ValueError:
                    self.send_response(400)
                    self.end_headers()
                    return

                self.reply(200, {"VALUE": [True]})
                return

            #  THE CONNECTIONS TAKEN (DP.6), and a hand pulling the cable: every
            #  open one closed, the listener kept, so a link is seen to come back.
            if path == "/_mock/connections":
                with device.lock:
                    taken = device.connections
                self.reply(200, {"VALUE": [taken]})
                return

            #  THE CONSOLE SAYING A LINE OF ITS OWN (DP.7), down every open
            #  connection: what a NOTIFY is.
            if path == "/_mock/say":
                line = urllib.parse.unquote(query)
                with device.lock:
                    sockets = list(device.open)
                for conn in sockets:
                    try:
                        conn.sendall((line + "\n").encode("utf-8"))
                    except OSError:
                        pass
                self.reply(200, {"VALUE": [len(sockets)]})
                return

            if path == "/_mock/drop":
                with device.lock:
                    sockets = list(device.open)
                for conn in sockets:
                    try:
                        conn.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    conn.close()
                self.reply(200, {"VALUE": [len(sockets)]})
                return

            if path == "/_mock/timed":
                self.reply(200, {"VALUE": device.everything_timed()})
                return

            if path == "/_mock/bundles":
                self.reply(200, {"VALUE": [device.bundles]})
                return

            if path == "/_mock/listening":
                self.reply(200, {"VALUE": [device.listening()]})
                return

            #  HOW MANY DATAGRAMS ARRIVED, which is not OSCQuery and is under a
            #  path no namespace can collide with. A driver that means "only
            #  what differed was sent" has to be able to COUNT what arrived;
            #  reading the values back cannot tell one write from three of the
            #  same value, and that is exactly the difference a minimal
            #  correction is supposed to make.
            if path == "/_mock/received":
                self.reply(200, {"VALUE": [device.count()]})
                return

            #  AND WHAT ARRIVED, IN ORDER (2026-10-03, Doh! D5): every message as
            #  `[address, [args]]`. A count says how many; a device left to its
            #  operator through a Doh! is asked WHICH - that it got Q12 once and
            #  Q13 once, never Q12 twice and Q13 not at all, which is the same
            #  count. Under the same reserved path as the count.
            if path == "/_mock/messages":
                self.reply(200, {"VALUE": device.everything()})
                return

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

        def websocket(self) -> None:
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()

            listener = Listener(self.connection, self.client_address[0])

            with device.lock:
                device.listeners.append(listener)

            try:
                while True:
                    opcode, payload = read_frame(self.connection)

                    if opcode == 0x8:                       # close
                        listener.send(0x8, payload[:2])
                        break

                    if opcode == 0x9:                       # ping
                        listener.send(0xA, payload)
                        continue

                    if opcode != 0x1:
                        continue

                    try:
                        said = json.loads(payload.decode("utf-8"))
                    except ValueError:
                        continue

                    with device.lock:
                        if said.get("COMMAND") == "LISTEN":
                            listener.addresses.add(said.get("DATA", ""))
                        elif said.get("COMMAND") == "IGNORE":
                            listener.addresses.discard(said.get("DATA", ""))
            except (ConnectionError, OSError, ValueError):
                pass
            finally:
                with device.lock:
                    if listener in device.listeners:
                        device.listeners.remove(listener)

                self.close_connection = True

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
    parser.add_argument("--listen", action="store_true",
                        help="offer LISTEN on a WebSocket at the HTTP port, as WFS-DIY does")
    parser.add_argument("--transport", default="udp", choices=("udp", "tcp"),
                        help="udp, a socket for datagrams; tcp, a listener for connections (DP.6)")
    parser.add_argument("--framing", default="length", choices=("length", "slip", "lines"),
                        help="how a connection's stream is cut: a size before each packet, SLIP, or lines")
    parser.add_argument("--wire", default="osc", choices=("osc", "rcp"),
                        help="what the bytes are: OSC, or lines of Yamaha's RCP answered with OK (DP.7)")
    args = parser.parse_args()

    device = Device(args.behaviour, args.alter_to)

    ports = []

    if args.transport == "tcp":
        listen_tcp(device, ports, args.framing, args.wire)
    else:
        listen_udp(device, ports)

    query_port = 0
    server = None

    if args.behaviour != "deaf":
        #  Threaded with --listen: a WebSocket holds its request thread for as
        #  long as it is open, and the questions must still be answered.
        kind = ThreadingHTTPServer if args.listen else HTTPServer
        server = kind((HOST, 0), make_handler(device, args.listen, lambda: ports[0]))
        server.daemon_threads = True
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
