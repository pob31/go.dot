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

"""The remote's road into the shipped binary: STREAM over a real serve
(namespace draft §68, ANQ, ANS; RM.1; docs/godot-remote-protocol-0.1.md).

WHAT THIS FILE IS FOR. OscQueryTests drives the server against a fake
namespace: the bundle's shape, the prefix, UNSTREAM, a client that stops reading.
What it cannot prove is that `wfg serve` streams the ENGINE'S ticks - one bundle
for every tick, in order, none skipped - and that what a remote does with the
contract's sequence (STREAM, then the snapshot, then every bundle later than the
snapshot's tick) leaves it holding exactly what the engine holds, while a
datagram from elsewhere, its own write and a cue made under it all arrive as the
contract says: a value in the tick's bundle, a new node announced by PATH_ADDED
before the bundle that carries its value, and its description one HTTP read
away.

THE REMOTE HERE is a few lines of Python keeping a dictionary of address to
values, which is all the Android app's copy of the state is.

The minimal fixture, copied: the autosave writes beside the show it serves.
"""
import json
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

import common
from common import HarnessError, Report, Server, WSClient

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "minimal"

LIST = "7K2QM9X4"
CUE = "B3N8R5TW"          # "House to half"
TICK = "/godot/engine/tick"


def decode_bundle(data: bytes):
    """(tick-first list of (address, values)) or None for anything else.
    Messages only inside: the stream nests nothing."""
    if not data.startswith(b"#bundle\0") or len(data) < 16:
        return None

    pos = 16
    out = []

    while pos < len(data):
        if pos + 4 > len(data):
            return None
        size = struct.unpack_from(">i", data, pos)[0]
        pos += 4
        if size < 0 or pos + size > len(data):
            return None
        element = common.osc_decode(data[pos:pos + size])
        if element is None:
            return None
        out.append(element)
        pos += size

    return out


class Remote(WSClient):
    """A WebSocket client that keeps every frame in the order it came - a
    text frame or a decoded bundle - which is the order the contract speaks of."""

    def __init__(self, port: int, name: str):
        self.events: "list[tuple[str, object]]" = []
        self.raw_sizes: "list[int]" = []
        super().__init__(port, name)

    def _read_loop(self) -> None:
        try:
            while not self._closing:
                first = self._recv_exact(2)
                opcode = first[0] & 0x0F
                length = first[1] & 0x7F

                if length == 126:
                    length = struct.unpack(">H", self._recv_exact(2))[0]
                elif length == 127:
                    length = struct.unpack(">Q", self._recv_exact(8))[0]

                payload = self._recv_exact(length) if length else b""

                if opcode == 0x8:
                    with self._lock:
                        self.events.append(("close", payload))
                    return
                if opcode == 0x1:
                    with self._lock:
                        self.events.append(("text", payload.decode("utf-8", "replace")))
                elif opcode == 0x2:
                    bundle = decode_bundle(payload)
                    with self._lock:
                        self.raw_sizes.append(len(payload))
                        self.events.append(("bundle", bundle))
        except (OSError, ConnectionError):
            return

    def stream(self, prefix: str = "/godot") -> None:
        self.send_text(json.dumps({"COMMAND": "STREAM", "DATA": prefix}))

    def unstream(self, prefix: str = "/godot") -> None:
        self.send_text(json.dumps({"COMMAND": "UNSTREAM", "DATA": prefix}))

    def bundles(self) -> "list[list]":
        with self._lock:
            return [payload for kind, payload in self.events if kind == "bundle"]

    def snapshot_events(self) -> list:
        with self._lock:
            return list(self.events)


def ticks_of(bundles: list) -> "list[int]":
    return [bundle[0][1][0] for bundle in bundles if bundle and bundle[0][0] == TICK and bundle[0][1]]


def flatten(node: dict, out: dict) -> None:
    """The snapshot's JSON as address -> VALUE, as the app's copy holds it."""
    address = node.get("FULL_PATH")
    if address is not None and "VALUE" in node:
        out[address] = node["VALUE"]
    for child in (node.get("CONTENTS") or {}).values():
        flatten(child, out)


def wait_for(predicate, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


def value_in_stream(remote: Remote, address: str, value) -> bool:
    for bundle in remote.bundles():
        for element_address, values in (bundle or [])[1:]:
            if element_address == address and values == [value]:
                return True
    return False


def session(report: Report, server: Server) -> None:
    port = server.http_port

    info = common.http_json(port, "/?HOST_INFO")
    extensions = info.get("EXTENSIONS", {})
    report.check(extensions.get("STREAM") is True, "HOST_INFO says the server streams", json.dumps(extensions))
    report.check(extensions.get("PATH_CHANGED") is False, "and no longer claims PATH_CHANGED, which nothing sends")

    # A client that asks for nothing is sent nothing.
    quiet = Remote(port, "quiet")
    remote = Remote(port, "remote")

    try:
        # THE CONTRACT'S ORDER (§2): STREAM first, then the snapshot.
        remote.stream()
        report.check(wait_for(lambda: len(remote.bundles()) >= 5),
                     "STREAM brings bundles from the next tick on")

        status, body = common.http_get(port, "/godot")
        report.equal(status, 200, "the snapshot is read after STREAM")
        tree = json.loads(body)
        held: dict = {}
        flatten(tree, held)
        snapshot_tick = held.get(TICK, [None])[0]
        report.check(isinstance(snapshot_tick, int), "the snapshot says which tick it is", str(snapshot_tick))

        # Every tick once, in order, none skipped, tick first in each.
        time.sleep(1.0)
        bundles = remote.bundles()
        ticks = ticks_of(bundles)
        report.check(all(bundle and bundle[0][0] == TICK for bundle in bundles),
                     "every bundle opens with /godot/engine/tick")
        report.check(len(ticks) >= 25, "a bundle a tick: at least 25 in the first second and more",
                     f"{len(ticks)} bundles")
        gaps = [(a, b) for a, b in zip(ticks, ticks[1:]) if b != a + 1]
        report.check(not gaps, "consecutive ticks, none skipped and none twice", str(gaps[:5]))

        idle = remote.raw_sizes[-20:]
        if idle:
            print(f"  (M74, idle: {sum(idle) / len(idle):.0f} bytes a bundle, "
                  f"{sum(idle) / len(idle) * 50 / 1024:.1f} KiB a second)")

        # A datagram from elsewhere, and this remote's own write: both arrive.
        common.send_udp(server.osc_port, common.osc_encode(f"/godot/cue/{CUE}/name", ["Streamed"]))
        report.check(wait_for(lambda: value_in_stream(remote, f"/godot/cue/{CUE}/name", "Streamed")),
                     "a value written over UDP is in the next tick's bundle")

        remote.send_osc(f"/godot/cue/{CUE}/notes", ["mine"])
        report.check(wait_for(lambda: value_in_stream(remote, f"/godot/cue/{CUE}/notes", "mine")),
                     "the remote's own write comes back in its stream: nothing withheld (ANQ)")

        # A cue made under it: PATH_ADDED before the bundle that carries its value.
        before = len(remote.snapshot_events())
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/cue/create", [LIST, 0, "memo", "Fresh"]))

        def made():
            for kind, payload in remote.snapshot_events()[before:]:
                if kind == "text" and "PATH_ADDED" in payload and payload.rstrip('"}').endswith("/name"):
                    return json.loads(payload)["DATA"]
            return None

        report.check(wait_for(lambda: made() is not None), "cue.create announces its rows with PATH_ADDED")
        name_address = made() or ""
        new_cue = name_address.split("/")[3] if name_address.count("/") >= 4 else ""

        events = remote.snapshot_events()[before:]
        announced_at = next((i for i, (kind, payload) in enumerate(events)
                             if kind == "text" and name_address in payload), None)
        carried_at = next((i for i, (kind, payload) in enumerate(events)
                           if kind == "bundle" and any(a == name_address and v == ["Fresh"]
                                                       for a, v in (payload or [])[1:])), None)
        report.check(carried_at is not None, "the new cue's name is in the tick's bundle, with its value",
                     name_address)
        report.check(announced_at is not None and carried_at is not None and announced_at < carried_at,
                     "announced before the bundle that carries it (§4)")

        status, body = common.http_get(port, f"/godot/cue/{new_cue}")
        report.equal(status, 200, "and its description is one HTTP read away", new_cue)
        if status == 200:
            described = json.loads(body).get("CONTENTS", {}).get("name", {})
            report.check(described.get("TYPE") == "s" and "DESCRIPTION" in described,
                         "the read carries TYPE and DESCRIPTION", json.dumps(described)[:300])

        # THE REMOTE'S COPY is the engine's: the snapshot plus every later bundle.
        copy = dict(held)
        for bundle in remote.bundles():
            if not bundle or bundle[0][1][0] <= snapshot_tick:
                continue
            for address, values in bundle:
                copy[address] = values
        now: dict = {}
        flatten(common.http_json(port, "/godot"), now)
        last_tick = ticks_of(remote.bundles())[-1]
        # Compare what does not move by itself: the cues' rows.
        cue_rows = [a for a in now if a.startswith("/godot/cue/")]
        differing = [a for a in cue_rows if copy.get(a) != now[a]]
        report.check(cue_rows and not differing,
                     "the snapshot and the bundles after it hold what the engine holds",
                     f"{len(differing)} of {len(cue_rows)} rows differ, e.g. {differing[:3]} (last tick {last_tick})")

        # Nothing to a client that did not ask; UNSTREAM stops it.
        with quiet._lock:
            quiet_bundles = [e for e in quiet.events if e[0] == "bundle"]
        report.check(not quiet_bundles, "a connection that did not ask is streamed nothing")

        remote.unstream()
        time.sleep(0.3)
        count = len(remote.bundles())
        time.sleep(0.5)
        report.equal(len(remote.bundles()), count, "UNSTREAM stops the bundles")
    finally:
        remote.close()
        quiet.close()


def run(locale: "str | None") -> int:
    report = Report(f"The remote's stream, through a real serve ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-remote-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "minimal")

        with Server(bundle, locale=locale) as server:
            session(report, server)

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as error:
        print(f"HARNESS ERROR: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
