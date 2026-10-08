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

"""An OSC cue's curves, played into a device and recorded from it, end to end
(namespace draft 45, O.12).

WHAT THIS IS FOR. The unit suite plays curves into a socket in the same
process and feeds a pass its reports as `mount.heard` records. This drives the
shipped `wfg serve` against `mock_target.py --listen` - a device written from
the specification, in another language, that pushes what changes down a
WebSocket as WFS-DIY does - with nothing but named commands over OSC, and reads
back what the device RECEIVED, what it was asked to LISTEN to, and what the
engine SAYS. Then it replays the session.

THE SHOW is tests/fixtures/bundles/osc-curves: a device under /wfs that takes
bundles and is heard (`rx`), described by a hand-written namespace; a cue moving
source one's x, y and z along three curves over two seconds, as three messages;
and a cue whose flat curve on source two's x is there to be recorded over.

THE SESSION:

 1. GO on the three-message cue: the device receives bundles - each carrying
    x, y and z together - and every x it received lies on the drawing at the
    moment it arrived, counted from the first (M51, judged off CI only).
 2. The second cue armed, its curve armed, a pass started: Go.dot LISTENs to
    source two's x on the device's socket. A hand moves it on the device's own
    screen, three times; each value reaches the curve's ride (M52, the time from
    the move to the ride, judged off CI only), and Go.dot sends that address
    nothing from the first move on - the device is the one moving it.
 3. The pass stopped: the curve is what the hand did, one step of undo, and the
    LISTEN let go of when the arming is.
 4. The session's log replays record for record.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "osc-curves"
MOCK = Path(__file__).resolve().parent / "mock_target.py"

MOUNT = "WFSMNT01"
MOVE = "M0VE0001"
RECORD = "REC00002"
CURVE_X = "CRVX0001"
RECORDED = "CRVR0001"

SOURCE_X = "/wfs/input/1/positionX"
SOURCE_Y = "/wfs/input/1/positionY"
SOURCE_Z = "/wfs/input/1/positionZ"
RECORDED_X = "/wfs/input/2/positionX"


# =============================================================================
# The device
# =============================================================================

class Device(first_sound.MockTarget):
    """`mock_target.py --listen`, on ports it chose."""

    def __init__(self):                             # noqa: super-init: a different command line
        self.process = subprocess.Popen(
            [sys.executable, str(MOCK), "--behaviour=agree", "--listen"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        line = self.process.stdout.readline().split()

        if len(line) != 4 or line[0] != "osc" or line[2] != "query":
            self.stop()
            raise HarnessError(f"the mock target did not report its ports: {line!r}")

        self.osc_port = int(line[1])
        self.query_port = int(line[3])

    def ask(self, path: str):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{self.query_port}{path}", timeout=4) as answer:
                if answer.status != 200:
                    return None
                return json.loads(answer.read().decode("utf-8"))["VALUE"]
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            return None

    def timed(self) -> "list":
        return self.ask("/_mock/timed") or []

    def bundles(self) -> int:
        got = self.ask("/_mock/bundles")
        return got[0] if got else 0

    def listening(self) -> int:
        got = self.ask("/_mock/listening")
        return got[0] if got else 0

    def move(self, address: str, value: float) -> bool:
        return self.ask(f"/_mock/move{address}?{value}") is not None


def point_mount_at(bundle: Path, device: Device) -> None:
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('port="9000"', f'port="{device.osc_port}"')
    text = text.replace('queryPort="5005"', f'queryPort="{device.query_port}"')
    show.write_text(text, encoding="utf-8", newline="\n")


# =============================================================================
# The engine
# =============================================================================

def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def value_of(server: Server, address: str):
    return first_sound.value_of(server, address)


def values_of(server: Server, address: str) -> list:
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return []

    try:
        return list(json.loads(body)["VALUE"])
    except (ValueError, KeyError, TypeError):
        return []


def curve_at(points: list, seconds: float) -> float:
    """The drawing at a second, as the engine reads it: straight between points,
    held beyond them."""
    pairs = [(points[at], points[at + 1]) for at in range(0, len(points) - 1, 2)]

    if not pairs:
        return 0.0

    if seconds <= pairs[0][0]:
        return pairs[0][1]

    for (t0, v0), (t1, v1) in zip(pairs, pairs[1:]):
        if seconds <= t1:
            return v0 + (v1 - v0) * (seconds - t0) / (t1 - t0)

    return pairs[-1][1]


# =============================================================================
# The session
# =============================================================================

def played(report: Report, server: Server, device: Device) -> None:
    """1. Three messages on three curves, into a device that takes bundles."""
    command(server, "cue.fire", [MOVE])

    finished = common.wait_until(lambda: any(m[2] == SOURCE_X and abs(m[3][0] - 5.0) < 1e-3 for m in device.timed()),
                                 timeout=15.0)
    report.check(bool(finished), "GO: source one's x reaches the curve's end, 5")

    arrived = device.timed()
    xs = [m for m in arrived if m[2] == SOURCE_X]
    bundled = [m for m in arrived if m[1] >= 0]

    report.check(device.bundles() > 0, "the device receives bundles", f"{device.bundles()} bundles")
    report.check(len(bundled) == len(arrived), "and every message of the cue arrives in one",
                 f"{len(arrived) - len(bundled)} of {len(arrived)} alone")

    #  X, Y AND Z TOGETHER: a tick that moves all three sends one bundle of three.
    by_bundle: "dict[int, set]" = {}

    for message in bundled:
        by_bundle.setdefault(message[1], set()).add(message[2])

    together = sum(1 for addresses in by_bundle.values() if {SOURCE_X, SOURCE_Y, SOURCE_Z} <= addresses)
    report.check(together >= len(by_bundle) - 2, "a bundle carries x, y and z together",
                 f"{together} of {len(by_bundle)} bundles")

    #  M51: EVERY X ON THE DRAWING, at the moment it arrived counted from the first.
    if xs:
        start = xs[0][0]
        drawing = [0.0, -5.0, 2.0, 5.0]
        errors = [abs(m[3][0] - curve_at(drawing, m[0] - start)) for m in xs]
        worst = max(errors)
        lane_level.timed(report, worst <= 0.25,
                         "M51: each x the device received lies on the drawing where it arrived",
                         f"{len(xs)} values, the worst {worst:.3f} m off (a tick of the slope is 0.1)")
        print(f"  M51: {len(xs)} values of x over the curve, worst {worst:.3f} m from the drawing")

    def run_state() -> "str | None":
        for run in first_sound.runs_in(server):
            if value_of(server, f"/godot/run/{run}/cue") == MOVE:
                return value_of(server, f"/godot/run/{run}/state")
        return None

    report.check(bool(common.wait_until(lambda: run_state() in ("done", None), timeout=5.0)),
                 "the cue is done by its wait at the end of its duration", f"{run_state()}")


def recorded(report: Report, server: Server, device: Device, facts: dict) -> None:
    """2 and 3. A pass, recorded from the device's pushes."""
    command(server, "curve.arm", [RECORD])
    command(server, "curve.rec", [RECORDED, True])

    listening = first_sound.wait_for(server, f"/godot/mount/{MOUNT}/listen", "listening", timeout=15.0)
    report.equal(listening, "listening", "armed: Go.dot LISTENs on the device's socket")
    report.check(bool(common.wait_until(lambda: device.listening() == 1, timeout=10.0)),
                 "and asks for exactly the armed curve's address", f"{device.listening()} subscriptions")

    command(server, "curve.record", [0.0])
    report.equal(first_sound.wait_for(server, "/godot/curves/recording", True, timeout=10.0), True,
                 "a pass starts")

    time.sleep(0.4)
    sent_before = sum(1 for m in device.timed() if m[2] == RECORDED_X)

    latencies = []

    for value in (2.0, 6.0, -3.0):
        moved_at = time.monotonic()
        report.check(device.move(RECORDED_X, value), f"a hand moves source two's x to {value} on the device")

        reached = common.wait_until(lambda v=value: abs((value_of(server, f"/godot/curve/{RECORDED}/ride") or 99.0) - v) < 1e-3,
                                    timeout=5.0)
        latencies.append(time.monotonic() - moved_at)
        report.check(bool(reached), f"the curve's ride follows it to {value}")
        time.sleep(0.5)

    worst = max(latencies)
    lane_level.timed(report, worst < 0.25, "M52: a pushed value reaches the curve's ride within a quarter second",
                     f"worst {worst * 1000:.0f} ms, " + ", ".join(f"{l * 1000:.0f}" for l in latencies))
    print(f"  M52: push to ride {', '.join(f'{l * 1000:.0f} ms' for l in latencies)}")

    sent_after = sum(1 for m in device.timed() if m[2] == RECORDED_X)
    report.equal(sent_after, sent_before, "Go.dot sends source two's x nothing while the device moves it",
                 f"{sent_before} sent before the first move")

    facts["stop_tick"] = value_of(server, "/godot/engine/tick")
    command(server, "curve.stop")

    report.equal(first_sound.wait_for(server, "/godot/curves/recording", False, timeout=10.0), False,
                 "the pass ends when the hand stops it")

    passed = value_of(server, "/godot/curves/pass") or ""
    report.check(" kept " in f" {passed} " and RECORDED in passed, "and says what it kept", passed)

    points = values_of(server, f"/godot/curve/{RECORDED}/points")
    values = points[1::2]
    report.check(len(points) >= 8, "the curve is written", f"{len(points) // 2} points")

    if values:
        report.check(abs(max(values) - 6.0) < 1e-3 and abs(min(values) - (-3.0)) < 1e-3,
                     "it holds what the hand did: up to 6, down to -3", f"{values}")
        report.check(abs(values[0]) < 1e-3, "and the old curve before the first move", f"starts at {values[0]}")

    #  LET GO: nothing armed, nothing listened to.
    command(server, "curve.free")
    report.equal(first_sound.wait_for(server, f"/godot/mount/{MOUNT}/listen", "off", timeout=10.0), "off",
                 "freed: the LISTEN is let go of")
    report.check(bool(common.wait_until(lambda: device.listening() == 0, timeout=10.0)),
                 "and the device forgets the subscription with the socket")


def run(locale: "str | None", keep_log: "Path | None") -> int:
    report = Report(f"OSC curves, played and recorded ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-osc-curves-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "osc-curves")
        log = room / "session.wfglog"
        replayed = room / "replayed"
        facts: dict = {}

        with Device() as device:
            point_mount_at(bundle, device)

            with Server(bundle, log=log, locale=locale) as server:
                report.equal(value_of(server, f"/godot/mount/{MOUNT}/loaded"), True,
                             "the device's description is loaded")

                played(report, server, device)
                recorded(report, server, device, facts)

        if keep_log is not None:
            keep_log.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(log, keep_log)
            print(f"  (the session's log is kept at {keep_log})")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        report.check(" mount.heard " in text, "what the device pushed is in the log as mount.heard")
        report.check(" curve.ride " not in text, "and no hand rode a curve")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))
        report.equal(code, 0, "`wfg replay` reproduces the session record for record", (out + err).strip()[-2000:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None
    keep_log = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
        elif argument.startswith("--keep-log="):
            keep_log = Path(argument.split("=", 1)[1])

    try:
        common.find_binary()
        return run(locale, keep_log)
    except HarnessError as error:
        print(f"osc curves: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
