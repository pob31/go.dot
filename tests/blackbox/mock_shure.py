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

"""A Shure Axient Digital receiver, as far as its command strings go
(namespace draft §124, BER; WR.2).

Written from Shure's "Axient Digital Command Strings" (version 4, 2019-L) and
nothing of Go.dot's: a TCP listener, ASCII, every string `< ... >` with nothing
ending a line. GET answers a REP; SET changes the value and answers a REP of
what it became - AUDIO_GAIN with INC and DEC, AUDIO_MUTE with TOGGLE, a
FREQUENCY set directly turning the GROUP_CHANNEL to `--,--` and saying so;
`GET 0 ALL` answers every property of the device and its channels; a SET on a
read-only or unknown property, or a value out of range, answers `< REP ERR >`.
`SET x METER_RATE n` sends `< SAMPLE x ALL ... >` every n milliseconds (100 at
the least, nought off), the standard nine fields. Numbers are fixed width and
names padded in braces, as the receiver sends them.

A driver scripts it over HTTP, every answer an OSCQuery-like `{"VALUE": [...]}`:

    /_mock/frames          every frame received, in order
    /_mock/connections     the connections taken since the start
    /_mock/state?1/AUDIO_GAIN   a property's value as the receiver holds it
    /_mock/hand?1/TX_BATT_BARS=001   a change nobody's command made - a pack's
                           battery dropping - told as a REP down every connection
    /_mock/say?< REP ERR >   a frame of its own, as it is, down every connection
    /_mock/drop            closes every connection, and keeps listening

The ports go out on one line, `osc <tcp> query <http>`, as mock_target.py's do.
Stdlib only.
"""

import argparse
import socket
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json

HOST = "127.0.0.1"
CHANNELS = 4

#  The device's own properties: value, settable, width (0 a word), braced pad.
DEVICE = {
    "DEVICE_ID": ["AD4Q-A", True, 0, 31],
    "MODEL": ["AD4Q-A", False, 0, 32],
    "FW_VER": ["2.0.15.2", False, 0, 24],
    "RF_BAND": ["G55", False, 0, 8],
    "TRANSMISSION_MODE": ["STANDARD", False, 0, 0],
    "ENCRYPTION_MODE": ["OFF", False, 0, 0],
    "QUADVERSITY_MODE": ["OFF", False, 0, 0],
}

#  A channel's: the same columns, and a range for a number.
CHANNEL = {
    "AUDIO_GAIN": ["030", True, 3, 0, (0, 60)],
    "AUDIO_MUTE": ["OFF", True, 0, 0, ("ON", "OFF", "TOGGLE")],
    "CHAN_NAME": ["", True, 0, 31, None],
    "FREQUENCY": ["0578350", True, 7, 0, (470000, 960000)],
    "GROUP_CHANNEL": ["1,1", True, 0, 10, None],
    "FD_MODE": ["OFF", False, 0, 0, None],
    "INTERFERENCE_STATUS": ["NONE", False, 0, 0, None],
    "ENCRYPTION_STATUS": ["OK", False, 0, 0, None],
    "METER_RATE": ["00000", True, 5, 0, (0, 65535)],
    "TX_BATT_BARS": ["005", False, 3, 0, None],
    "TX_BATT_MINS": ["00300", False, 5, 0, None],
    "TX_MODEL": ["AD2", False, 0, 0, None],
    "TX_MUTE_MODE_STATUS": ["ON", False, 0, 0, None],
    "TX_DEVICE_ID": ["", False, 0, 31, None],
    "CHAN_QUALITY": ["005", False, 3, 0, None],
    "AUDIO_LEVEL_PEAK": ["102", False, 3, 0, None],
    "AUDIO_LEVEL_RMS": ["102", False, 3, 0, None],
    "ANTENNA_STATUS": ["BB", False, 0, 0, None],
}


class Receiver:
    def __init__(self):
        self.lock = threading.Lock()
        self.device = {k: list(v) for k, v in DEVICE.items()}
        self.channels = {ch: {k: list(v) for k, v in CHANNEL.items()} for ch in range(1, CHANNELS + 1)}
        self.rssi = {ch: ["086", "065"] for ch in range(1, CHANNELS + 1)}
        self.frames = []
        self.connections = []
        self.taken = 0
        self.meters = {}

    #  How the receiver spells a value: fixed width for a number, padded braces for a name.
    @staticmethod
    def spelled(row):
        value, _, width, pad = row[0], row[1], row[2], row[3]
        if pad:
            return "{" + value.ljust(pad)[:pad] + "}"
        if width:
            return value.rjust(width, "0")
        return value

    def tell(self, frame):
        data = frame.encode("ascii", "replace")
        for conn in list(self.connections):
            try:
                conn.sendall(data)
            except OSError:
                pass

    def rep(self, channel, name):
        row = (self.device if channel is None else self.channels[channel])[name]
        where = "" if channel is None else "%d " % channel
        return "< REP %s%s %s >" % (where, name, self.spelled(row))

    def everything(self, channel):
        out = [self.rep(None, name) for name in self.device]
        for ch in ([channel] if channel else range(1, CHANNELS + 1)):
            out += [self.rep(ch, name) for name in CHANNEL]
        return out

    def sample(self, ch):
        c = self.channels[ch]
        a, b = self.rssi[ch]
        return "< SAMPLE %d ALL %s 031 %s %s %s 31 %s 31 %s >" % (
            ch, c["CHAN_QUALITY"][0], c["AUDIO_LEVEL_PEAK"][0], c["AUDIO_LEVEL_RMS"][0], c["ANTENNA_STATUS"][0], a, b)

    def meter(self, ch, ms):
        stop = self.meters.pop(ch, None)
        if stop:
            stop.set()
        if ms <= 0:
            return
        event = threading.Event()
        self.meters[ch] = event

        def run():
            while not event.wait(max(ms, 100) / 1000.0):
                with self.lock:
                    frame = self.sample(ch)
                self.tell(frame)

        threading.Thread(target=run, daemon=True).start()

    def answer(self, frame):
        """What the receiver says back to one frame, as a list of frames."""
        tokens, current, braces = [], "", False
        for c in frame.strip()[1:-1]:
            if braces:
                if c == "}":
                    braces = False
                    tokens.append(("{", current))
                    current = ""
                else:
                    current += c
            elif c == "{":
                braces = True
            elif c in " \t":
                if current:
                    tokens.append(("", current))
                current = ""
            else:
                current += c
        if current:
            tokens.append(("", current))
        if not tokens:
            return ["< REP ERR >"]

        verb = tokens[0][1]
        at = 1
        channel = None
        if at < len(tokens) and tokens[at][1].isdigit() and tokens[at][0] == "":
            channel = int(tokens[at][1])
            at += 1
        if at >= len(tokens):
            return ["< REP ERR >"]
        name = tokens[at][1]
        rest = tokens[at + 1:]

        if verb == "GET" and name == "ALL":
            if channel not in (None, 0) and channel not in self.channels:
                return ["< REP ERR >"]
            return self.everything(channel or None)

        if channel == 0:
            channels = list(self.channels)
        elif channel is None:
            channels = [None]
        elif channel in self.channels:
            channels = [channel]
        else:
            return ["< REP ERR >"]

        out = []
        for ch in channels:
            table = self.device if ch is None else self.channels[ch]
            if name == "FLASH":
                out.append("< REP %sFLASH ON >" % ("" if ch is None else "%d " % ch))
                continue
            if name not in table:
                return ["< REP ERR >"]
            row = table[name]

            if verb == "GET":
                out.append(self.rep(ch, name))
                continue
            if verb != "SET" or not row[1] or not rest:
                return ["< REP ERR >"]

            value = " ".join(t[1] for t in rest)
            if name == "AUDIO_MUTE":
                if value == "TOGGLE":
                    value = "OFF" if row[0] == "ON" else "ON"
                if value not in ("ON", "OFF"):
                    return ["< REP ERR >"]
                row[0] = value
            elif row[3]:
                if len(value) > 8 and name != "GROUP_CHANNEL":
                    return ["< REP ERR >"]
                row[0] = value
            else:
                words = value.split()
                number = int(row[0])
                try:
                    if words[0] in ("INC", "DEC") and len(words) == 2:
                        number += int(words[1]) * (1 if words[0] == "INC" else -1)
                    elif len(words) == 1:
                        number = int(words[0])
                    else:
                        return ["< REP ERR >"]
                except ValueError:
                    return ["< REP ERR >"]
                low, high = row[4]
                if not low <= number <= high:
                    return ["< REP ERR >"]
                row[0] = str(number)
                if name == "METER_RATE":
                    self.meter(ch, number)
                if name == "FREQUENCY":
                    table["GROUP_CHANNEL"][0] = "--,--"
                    out.append(self.rep(ch, "GROUP_CHANNEL"))
            out.append(self.rep(ch, name))
        return out


def cut(buffer):
    """The frames a buffer holds, `<` to the `>` outside braces, and what is left."""
    frames, start, braces = [], None, False
    for at, c in enumerate(buffer):
        if start is None:
            if c == "<":
                start, braces = at, False
            continue
        if c == "{":
            braces = True
        elif c == "}":
            braces = False
        elif c == ">" and not braces:
            frames.append(buffer[start:at + 1])
            start = None
    return frames, (buffer[start:] if start is not None else "")


def listen(receiver, ports):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, 0))
    server.listen(8)
    ports.append(server.getsockname()[1])

    def serve(conn):
        buffer = ""
        while True:
            try:
                data = conn.recv(4096)
            except OSError:
                break
            if not data:
                break
            buffer += data.decode("ascii", "replace")
            frames, buffer = cut(buffer)
            for frame in frames:
                with receiver.lock:
                    receiver.frames.append(frame)
                    replies = receiver.answer(frame)
                try:
                    conn.sendall("".join(replies).encode("ascii", "replace"))
                except OSError:
                    break
        with receiver.lock:
            if conn in receiver.connections:
                receiver.connections.remove(conn)
        conn.close()

    def accept():
        while True:
            conn, _ = server.accept()
            with receiver.lock:
                receiver.connections.append(conn)
                receiver.taken += 1
            threading.Thread(target=serve, args=(conn,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return server


def handler_for(receiver):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, values):
            body = json.dumps({"VALUE": values}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802
            path, _, query = self.path.partition("?")
            query = urllib.parse.unquote(query)
            with receiver.lock:
                if path == "/_mock/frames":
                    return self.reply(list(receiver.frames))
                if path == "/_mock/connections":
                    return self.reply([receiver.taken])
                if path == "/_mock/state":
                    ch, _, name = query.partition("/")
                    table = receiver.device if ch == "" or not ch.isdigit() else receiver.channels.get(int(ch), {})
                    if not ch.isdigit():
                        name = ch
                    row = table.get(name)
                    return self.reply([row[0]] if row else [])
                if path == "/_mock/say":
                    frame = query
                elif path == "/_mock/hand":
                    where, _, value = query.partition("=")
                    ch, _, name = where.partition("/")
                    row = receiver.channels[int(ch)][name]
                    row[0] = value
                    frame = receiver.rep(int(ch), name)
                else:
                    frame = None
                if path == "/_mock/drop":
                    for conn in list(receiver.connections):
                        try:
                            conn.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                    return self.reply([len(receiver.connections)])
            if frame is not None:
                receiver.tell(frame)
                return self.reply([frame])
            self.send_response(404)
            self.end_headers()

    return Handler


def main() -> int:
    argparse.ArgumentParser(description="A Shure Axient Digital receiver's command strings.").parse_args()
    receiver = Receiver()
    ports = []
    listen(receiver, ports)
    server = ThreadingHTTPServer((HOST, 0), handler_for(receiver))
    server.daemon_threads = True
    print("osc %d query %d" % (ports[0], server.server_address[1]), flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
