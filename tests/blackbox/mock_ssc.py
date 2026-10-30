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

"""A Sennheiser EW-DX receiver, as far as its Sound Control Protocol goes
(namespace draft §124, BES; WR.3).

Written from Sennheiser's "Sound Control Protocol (SSC), Developer's guide for
EW-DX" (firmware 3.0.x) and nothing of Go.dot's: one UDP socket, one JSON
object per datagram, every message answered to the port it came from with the
same shape holding what each property now is. A value sets a writeable
property - a gain adapted to its range and step, as the guide's own example
adapts 20000 to 6 - and null asks; an address it does not have answers
`{"osc":{"error":[{...:404}]}}`. `osc/state/subscribe` takes one address tree,
answers with it, sends every subscribed property at once, and then whatever
changes; its lifetime (ten seconds unless the request says) runs out with a
310 unless the client asks again.

A driver scripts it over HTTP, every answer an OSCQuery-like `{"VALUE": [...]}`:

    /_mock/messages        every message received, as text, in order
    /_mock/state?rx1/gain  a property as the receiver holds it
    /_mock/hand?mates/tx1/battery/gauge=20   a change nobody's message made -
                           a pack's battery dropping - told to every subscriber
    /_mock/subscribers     how many subscriptions are alive

The ports go out on one line, `osc <udp> query <http>`. Stdlib only.
"""

import json
import socket
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"

#  The receiver's properties: value, writeable, (min, max, step) for a number.
def initial():
    state = {
        "device/name": ["EW-DX EM 4", True, None],
        "device/identity/product": ["EW-DX EM 4 Dante", False, None],
        "device/identity/version": ["3.0.2", False, None],
    }
    for ch in range(1, 5):
        state.update({
            "rx%d/name" % ch: ["RX%d" % ch, True, None],
            "rx%d/mute" % ch: [False, True, None],
            "rx%d/gain" % ch: [12, True, (-3, 42, 3)],
            "rx%d/frequency" % ch: [520000 + ch * 1000, True, (470200, 1999000, 25)],
            "rx%d/warnings" % ch: [[], False, None],
            "mates/tx%d/battery/gauge" % ch: [90, False, None],
            "mates/tx%d/battery/lifetime" % ch: [420, False, None],
            "mates/tx%d/battery/type" % ch: ["Battery", False, None],
            "mates/tx%d/name" % ch: ["TX%d" % ch, False, None],
            "mates/tx%d/mute" % ch: [False, False, None],
            "mates/tx%d/type" % ch: ["SK", False, None],
            "mates/tx%d/warnings" % ch: [[], False, None],
            "m/rx%d/rssi" % ch: [-51.5, False, None],
            "m/rx%d/rsqi" % ch: [100, False, None],
            "m/rx%d/divi" % ch: [1, False, None],
            "m/rx%d/af" % ch: [-24.0, False, None],
        })
    return state


def leaves(tree, path=""):
    """A JSON tree's leaves as (path, value), `osc` left out at the top."""
    if not isinstance(tree, dict):
        yield path, tree
        return
    for key, child in tree.items():
        if path == "" and key in ("osc", "#"):
            continue
        yield from leaves(child, (path + "/" if path else "") + key)


def tree_of(pairs):
    out = {}
    for path, value in pairs:
        here = out
        parts = path.split("/")
        for part in parts[:-1]:
            here = here.setdefault(part, {})
        here[parts[-1]] = value
    return out


class Receiver:
    def __init__(self):
        self.lock = threading.Lock()
        self.state = initial()
        self.messages = []
        self.subscriptions = {}     # address -> {"paths": set, "until": t}
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((HOST, 0))

    def send(self, message, to):
        self.sock.sendto(json.dumps(message, separators=(",", ":")).encode("utf-8"), to)

    def answer(self, message, sender):
        """The reply to one message, and the subscriber's first notification."""
        if not isinstance(message, dict):
            return [{"osc": {"error": [{"": 400}]}}]
        replies = []
        subscribe = message.get("osc", {}).get("state", {}).get("subscribe") if isinstance(message.get("osc"), dict) else None

        if subscribe is not None:
            request = subscribe[0] if isinstance(subscribe, list) and subscribe else {}
            options = request.get("#", {})
            paths = {p for p, _ in leaves({k: v for k, v in request.items() if k != "#"}) if p in self.state}
            self.subscriptions[sender] = {"paths": paths,
                                          "until": time.monotonic() + float(options.get("lifetime", 10))}
            replies.append({"osc": {"state": {"subscribe": [request]}}})
            replies.append(tree_of((p, self.state[p][0]) for p in sorted(paths)))
            return replies

        answered, errors, changed = [], [], []
        for path, value in leaves(message):
            row = self.state.get(path)
            if row is None:
                errors.append((path, 404))
                continue
            if value is not None and row[1]:
                if row[2] is not None and isinstance(value, (int, float)):
                    low, high, step = row[2]
                    value = max(low, min(high, int(round((value - low) / step)) * step + low))
                if row[0] != value:
                    changed.append(path)
                row[0] = value
            answered.append((path, row[0]))
        reply = tree_of(answered)
        if errors:
            reply["osc"] = {"error": [tree_of(errors)]}
        replies.append(reply)
        self.changed_by(changed, sender)
        return replies

    def changed_by(self, paths, skip):
        """Tell every live subscriber what changed - but not the one whose message did, which had its reply."""
        now = time.monotonic()
        for who, sub in list(self.subscriptions.items()):
            if sub["until"] < now:
                continue
            told = [p for p in paths if p in sub["paths"]]
            if told and who != skip:
                self.send(tree_of((p, self.state[p][0]) for p in told), who)

    def expire(self):
        while True:
            time.sleep(0.25)
            with self.lock:
                now = time.monotonic()
                for who, sub in list(self.subscriptions.items()):
                    if sub["until"] < now:
                        self.send({"osc": {"error": [{"osc": {"state": {"subscribe": 310}}}]}}, who)
                        del self.subscriptions[who]

    def serve(self):
        while True:
            data, sender = self.sock.recvfrom(65535)
            text = data.decode("utf-8", "replace")
            with self.lock:
                self.messages.append(text)
                try:
                    message = json.loads(text)
                except ValueError:
                    replies = [{"osc": {"error": [{"": 400}]}}]
                else:
                    replies = self.answer(message, sender)
                for reply in replies:
                    self.send(reply, sender)


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
                if path == "/_mock/messages":
                    return self.reply(list(receiver.messages))
                if path == "/_mock/state":
                    row = receiver.state.get(query)
                    return self.reply([row[0]] if row else [])
                if path == "/_mock/subscribers":
                    now = time.monotonic()
                    return self.reply([sum(1 for s in receiver.subscriptions.values() if s["until"] >= now)])
                if path == "/_mock/hand":
                    where, _, text = query.partition("=")
                    value = json.loads(text)
                    receiver.state[where][0] = value
                    receiver.changed_by([where], None)
                    return self.reply([where])
            self.send_response(404)
            self.end_headers()

    return Handler


def main() -> int:
    receiver = Receiver()
    threading.Thread(target=receiver.serve, daemon=True).start()
    threading.Thread(target=receiver.expire, daemon=True).start()
    server = ThreadingHTTPServer((HOST, 0), handler_for(receiver))
    server.daemon_threads = True
    print("osc %d query %d" % (receiver.sock.getsockname()[1], server.server_address[1]), flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
