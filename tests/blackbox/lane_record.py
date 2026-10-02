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

"""A lane recorded from a fader, heard and written (namespace draft §20.9).

WHAT THIS IS FOR. `LaneRecordTests` drives the pass against a fake audio side;
this drives the SHIPPED BINARY with a real Tracktion graph, the way the
virtual panel does - every step a named command over OSC: the lane armed, a
strip of the show's virtual panel taken, a pass recorded, the ride touched,
moved, let go (latched) and stopped - and reads back both what was HEARD (the
render) and what was WRITTEN (the lane), then takes the pass away in one undo
and replays the session.

THE BUNDLE IS tests/fixtures/bundles/lane-record: one media cue with no lane,
and a virtual panel of two strips. The media is `lane_level.py`'s tone, and
so are the level reading and the engine-clock mark.

TIMING IS JUDGED OFF CI ONLY (the author, 2026-09-28): on GitHub Actions the
checks whose answer depends on the tick keeping time - where in the render the
hand's level is heard, where in the lane its turns were written - are voided
in words. What a stall cannot move is judged everywhere: the pick, the rows,
the lane's levels, the latch, the stop, the undo, the replay.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "lane-record"

RATE = lane_level.RATE
CUE = "VR000002"
STRIP = "VR000006"
RIDE = "/godot/surface/laneRide"


def send(server: Server, address: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args or []))


def lane_of(server: Server) -> "list[tuple[float, float]]":
    """The cue's lane as the tree publishes it: every value of the list node."""
    status, body = common.http_get(server.http_port, f"/godot/cue/{CUE}/levelLane?VALUE")

    if status != 200:
        return []

    try:
        values = [float(v) for v in json.loads(body)["VALUE"]]
    except (KeyError, ValueError, TypeError):
        return []

    return list(zip(values[0::2], values[1::2]))


def wait_frames(render: Path, frames: int) -> bool:
    return lane_level.wait_for_frames(render, frames)


def run(locale: "str | None") -> int:
    report = Report(f"a lane recorded from a fader ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-lane-record-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "lane-record")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        lane_level.write_tone(bundle / "media" / "tone.wav")

        marks = {}
        written = []

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=lane_level.BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")

            # --- the pick: armed, and a strip of the panel taken (DF) ---------
            send(server, "/godot/cmd/lane/arm", [CUE])
            report.equal(first_sound.wait_for(server, "/godot/surface/lane", CUE), CUE,
                         "lane.arm: the cue's lane waits for a fader")

            send(server, "/godot/cmd/lane/take", [STRIP])
            report.equal(first_sound.wait_for(server, "/godot/surface/laneFader", STRIP), STRIP,
                         "lane.take: the panel's first strip is taken")
            report.equal(first_sound.value_of(server, f"/godot/slot/{STRIP}/target"), RIDE,
                         "and it rides the lane's node")
            report.equal(first_sound.value_of(server, RIDE), 0.0,
                         "which sits where the lane starts - unity, with no lane yet (DG)")

            # --- the pass: the cue plays, the fader reads, then is ridden --------
            send(server, "/godot/cmd/lane/record")
            report.equal(first_sound.wait_for(server, "/godot/surface/laneRecording", True), True,
                         "lane.record: a pass runs")

            start = lane_level.clock_frame(server)
            report.check(wait_frames(render, start + RATE), "the cue plays a second untouched")

            marks["touch"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/touch", [RIDE])
            send(server, "/godot/cmd/node/set", [RIDE, -6.0])
            report.check(wait_frames(render, marks["touch"] + RATE), "a second at -6")

            marks["move"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/set", [RIDE, -12.0])
            report.check(wait_frames(render, marks["move"] + RATE), "a second at -12")

            marks["release"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/release", [RIDE])
            report.check(wait_frames(render, marks["release"] + RATE), "a second after the hand let go")

            marks["stop"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/lane/stop")
            report.equal(first_sound.wait_for(server, "/godot/surface/laneRecording", False), False,
                         "lane.stop: the pass ends")

            common.wait_until(lambda: len(lane_of(server)) > 0, timeout=10.0)
            written = lane_of(server)
            report.check(len(written) >= 3, "and the ride is written into the cue's lane, one pass one write",
                         f"{len(written)} points")

            report.equal(first_sound.value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            # --- one undo takes the pass away ------------------------------------
            send(server, "/godot/cmd/undo", ["document", "node.set"])
            common.wait_until(lambda: len(lane_of(server)) == 0, timeout=10.0)
            report.check(len(lane_of(server)) == 0, "one undo takes the whole pass away")

            send(server, "/godot/cmd/redo", ["document", "node.set"])
            common.wait_until(lambda: len(lane_of(server)) > 0, timeout=10.0)

            first_sound.wait_for_render_tail(render)

        # --- what was written ----------------------------------------------------
        levels = [level for _, level in written]
        report.check(any(abs(level - (-6.0)) <= 0.05 for level in levels),
                     "the lane holds the -6 the hand set", f"{levels}")
        report.check(any(abs(level - (-12.0)) <= 0.05 for level in levels),
                     "and the -12 it moved to", f"{levels}")

        rides = [level for level in levels if level < -0.05]
        report.check(bool(rides) and abs(rides[-1] - (-12.0)) <= 0.05,
                     "LATCHED: after the hand let go, -12 is held to the end of the pass (DH)",
                     f"{levels}")
        report.check(bool(written) and abs(written[0][1]) <= 0.05 and abs(written[-1][1]) <= 0.05,
                     "joined to unity either side of the ride - the lane it replaced (DL)",
                     f"{written[:1]} ... {written[-1:]}")

        # --- what was heard --------------------------------------------------------
        channels, data = first_sound.read_render(render)
        left = data[0] if data else []
        begin = first_sound.first_above(left, 0.005)
        report.check(begin >= 0, "the cue was heard at all")

        if begin >= 0:
            unity = lane_level.level_db(left, begin + int(0.5 * RATE))
            report.check(abs(unity - first_sound.db(lane_level.AMPLITUDE, 1.0)) <= 1.0,
                         "untouched, the pass plays the cue as written", f"{unity:.2f} dB")

            for name, after, wanted, words in (
                    ("touch", 0.5, -6.0, "half a second after the touch the hand's -6 is heard"),
                    ("move", 0.5, -12.0, "after the move, its -12"),
                    ("release", 0.5, -12.0, "after the hand let go, still -12 - the latch")):
                heard = lane_level.level_db(left, marks[name] + int(after * RATE)) - unity
                lane_level.timed(report, abs(heard - wanted) <= lane_level.LEVEL_TOLERANCE_DB, words,
                                 f"heard {heard:.2f} dB")

            silent = lane_level.level_db(left, marks["stop"] + int(0.6 * RATE))
            #  Read at a wall-clock mark, like the levels above: since K4 the
            #  hand's stop is a graceful stop, not a kill, and on a starved CI
            #  runner the mark can land before the stop reaches the render.
            lane_level.timed(report, silent < -60.0, "and the stop stopped the cue", f"{silent:.1f} dBFS")

            # Where the turn from -6 to -12 was written, against when it was sent.
            turn = next((s for s, level in written if abs(level - (-12.0)) <= 0.05), None)
            sent_at = (marks["move"] - begin) / RATE
            lane_level.timed(report, turn is not None and abs(turn - sent_at) <= 0.1,
                             "the ride's turn is written at the second it was heard",
                             "none" if turn is None else f"written at {turn:.3f} s, sent at {sent_at:.3f} s")

        # --- and the session replays -------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session", (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
