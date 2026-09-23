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

"""A show that declares a Mackie surface, served on a machine with no MIDI.

WHAT THIS IS FOR. Every CI runner is a machine without the surface the show was
written for, and so is a laptop on the train: a show must open there, say
plainly which surface it cannot reach and why, and go on working from the
surfaces it can - the virtual panel, the page, the network. That is PRD §3.17's
redundancy path, and a bridge that crashed, hung or went quiet about a port
with nothing behind it would take the whole show down with one cable.

WHAT IS REAL HERE. The shipped binary with the serve wiring Phase 6 added: the
surface bridge between the MIDI ports and the tick, the surface table the tree
publishes, the live door a DCA trim is written through. The fixture
(tests/fixtures/bundles/surfaces) is namespace draft §16.8's standing-test rig:
a virtual panel and an MCU surface, each with a strip pinned to one DCA, and a
MIDI port whose devices are on no machine. The unit suite drives the bridge
with bytes; this drives the product with no bytes at all.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "surfaces"

PANEL = "SRFS0001"
DESK = "SRFS0002"
PORT = "SRFP0001"
BAND = "SRFD0001"
PADS = "SRF00002"
BED = "SRF00005"

PANEL_DCA_STRIP = "SRFT0001"
DESK_DCA_STRIP = "SRFT0009"
KNOCK_STRIP = "SRFT0002"      # the panel's first sampler strip: Pads' first member
CHIME_STRIP = "SRFT0003"
DESK_FIRST_SAMPLER = "SRFT000A"


class Hand:
    """One UDP socket for everything, as a console or a script would send."""

    def __init__(self, server: Server):
        self.port = server.osc_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, address: str, args: "list | None" = None) -> None:
        self.sock.sendto(common.osc_encode(address, args), ("127.0.0.1", self.port))

    def close(self) -> None:
        self.sock.close()


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return json.loads(body)["VALUE"][0]
    except (KeyError, IndexError, ValueError):
        return None


def wait_for(server: Server, address: str, wanted, seconds: float = 20.0):
    deadline = time.monotonic() + seconds
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == wanted:
            return actual

        time.sleep(0.02)

    return actual


def wait_until(predicate, seconds: float = 20.0) -> bool:
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        if predicate():
            return True

        time.sleep(0.02)

    return predicate()


def run_for_cue(server: Server, cue: str, seconds: float = 20.0) -> str:
    found = ""

    def look() -> bool:
        nonlocal found

        for run in (value_of(server, "/godot/run/order") or "").split():
            if value_of(server, f"/godot/run/{run}/cue") == cue and \
                    value_of(server, f"/godot/run/{run}/state") not in ("done", "failed"):
                found = run
                return True

        return False

    wait_until(look, seconds)
    return found


def run(locale: "str | None") -> int:
    report = Report(f"phase 6: surfaces on a machine with no MIDI ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase6-surfaces-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "surfaces")
        log = room / "session.wfglog"
        replayed = room / "replayed"

        for name in ("knock", "chime", "bed"):
            first_sound.write_tone(bundle / "media" / f"{name}.wav")

        with Server(bundle, log=log, locale=locale, hosted=True) as server:
            hand = Hand(server)

            try:
                report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                             "the show opens, and the audio side comes up running")

                # --- what the machine says about the surfaces ---------------
                # The panel is the client's own; the Mackie is on a port whose
                # devices are on no machine, and says so in a sentence rather
                # than a colour or a silence.
                report.equal(value_of(server, f"/godot/surface/{PANEL}/connected"), True,
                             "the virtual panel is connected")
                report.equal(value_of(server, f"/godot/surface/{PANEL}/problem"), "",
                             "and has nothing to say about it")
                report.equal(value_of(server, f"/godot/surface/{DESK}/connected"), False,
                             "the Mackie surface is not")
                report.equal(value_of(server, f"/godot/surface/{DESK}/problem"),
                             'the port "Desk port" has no device behind it',
                             "and says why, naming the port the way the show does")
                report.equal(value_of(server, f"/godot/port/{PORT}/bound"), False,
                             "which the port itself agrees with")
                report.equal(value_of(server, f"/godot/surface/{DESK}/strips"), 8,
                             "and its strips are still part of the layout")

                # --- one node, two strips -----------------------------------
                trim = f"/godot/dca/{BAND}/trim"

                report.equal(value_of(server, f"/godot/slot/{PANEL_DCA_STRIP}/target"), trim,
                             "the panel's DCA strip rides the DCA's trim")
                report.equal(value_of(server, f"/godot/slot/{DESK_DCA_STRIP}/target"), trim,
                             "and so does the Mackie's, the same node")
                report.equal(value_of(server, f"/godot/slot/{PANEL_DCA_STRIP}/word"), "dca",
                             "a DCA strip says so in a word")
                report.equal(value_of(server, trim), 0.0,
                             "and the trim opens at its resting value")

                # --- GO arms the pads onto the strips in layout order -------
                # Surface order, then index: the panel comes first, and its
                # first strip is a DCA strip, so the pads start on its second.
                hand.send("/godot/cmd/go")

                armed = wait_until(lambda: value_of(server, f"/godot/slot/{KNOCK_STRIP}/word") == "armed"
                                   and value_of(server, f"/godot/slot/{CHIME_STRIP}/word") == "armed",
                                   seconds=30.0)

                report.check(armed, "GO armed the pads onto the panel's sampler strips",
                             f"{value_of(server, f'/godot/slot/{KNOCK_STRIP}/word')!r} "
                             f"{value_of(server, f'/godot/slot/{CHIME_STRIP}/word')!r}")
                report.equal(value_of(server, f"/godot/slot/{DESK_FIRST_SAMPLER}/word"), "free",
                             "and left the Mackie's strips free, there being two members")

                # --- a press over the network plays -------------------------
                knock = value_of(server, f"/godot/slot/{KNOCK_STRIP}/holder") or ""
                hand.send("/godot/cmd/strip/press", [KNOCK_STRIP, 100])

                report.equal(wait_for(server, f"/godot/run/{knock}/state", "playing"), "playing",
                             "a press on the panel's strip plays the clip, no MIDI anywhere")

                # --- the DCA, written from the network ----------------------
                # GO again: the standby moved past the pads to the bed, which
                # is marked with the DCA.
                hand.send("/godot/cmd/go")
                bed = run_for_cue(server, BED)

                if report.check(bool(bed), "GO plays the bed"):
                    report.equal(wait_for(server, f"/godot/run/{bed}/state", "playing"), "playing",
                                 "which is playing")

                    hand.send(trim, [-6.0])

                    report.equal(wait_for(server, trim, -6.0), -6.0,
                                 "a write to the DCA's trim lands through the live door")
                    report.equal(wait_for(server, f"/godot/run/{bed}/level", -6.0), -6.0,
                                 "and the bed marked with it is six decibels down")
                    report.equal(value_of(server, "/godot/document/canUndo"), False,
                                 "and a ride is nothing anybody decided: there is nothing to undo")

                # --- and everything stops -----------------------------------
                hand.send("/godot/cmd/run/killAll")

                stopped = wait_until(lambda: not any(
                    value_of(server, f"/godot/run/{run}/state") not in ("done", "failed", None)
                    for run in (value_of(server, "/godot/run/order") or "").split()))

                report.check(stopped, "double Esc from the network ends every run")
                report.equal(wait_for(server, f"/godot/slot/{KNOCK_STRIP}/word", "free"), "free",
                             "and the strips are free again")
                report.equal(value_of(server, trim), -6.0,
                             "while the DCA stays where the hand left it (Esc is not a fader)")
            finally:
                hand.close()

        # --- and it reproduces ------------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}")

        report.equal(code, 0, "and `wfg replay` reproduces the session",
                     (out + err).strip()[:400])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"phase6_surfaces: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"phase6_surfaces: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"phase6_surfaces: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
