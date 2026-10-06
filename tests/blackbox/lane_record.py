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

"""A cue's level and a send recorded from the flipped faders, heard and written
(namespace draft §20.9, §34).

WHAT THIS IS FOR. `LaneRecordTests` drives the pass against a fake audio side;
this drives the SHIPPED BINARY with a real Tracktion graph, the way the
virtual panel does - every step a named command over OSC: the faders flipped
to the cue, two lanes armed by their REC - the cue's level, and a mix the cue
does not send to yet - a pass recorded, both rides touched, moved, let go
(latched) and stopped - and reads back both what was HEARD (the render, the
main pair and the mix) and what was WRITTEN (the level's lane, and the send
the pass made with its lane), that the faders stayed on the cue and the pass
said what it wrote (§30.4, §34 UM), then takes the pass away in one undo and
replays the session.

THE BUNDLE IS tests/fixtures/bundles/lane-record: one media cue with no lane,
and a virtual panel of two strips. The copy this driver serves gains a stereo
mix, "Face", on outputs 3-4, which the cue does not send to - so strip one is
the level's lane and strip two Face's (UN), and what the render hears on
channels 3-4 is the send the pass is making (UQ). The fixture is left as its
replay log knows it. The media is `lane_level.py`'s tone, and so are the level
reading and the engine-clock mark.

A SEND IS BELOW THE CUE'S LEVEL (§28): the mix hears the level's ride and the
send's together, so -6 on the level and -10 on Face is -16 on the mix.

TIMING IS JUDGED OFF CI ONLY (the author, 2026-09-28): on GitHub Actions the
checks whose answer depends on the tick keeping time - where in the render the
hands' levels are heard, where in the lane its turns were written - are voided
in words. What a stall cannot move is judged everywhere: the flip, the RECs,
the rows, the lanes' levels, the latch, the send made, the stop, the undo, the
replay.

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
LEVEL_STRIP = "VR000006"
FACE_STRIP = "VR000007"
FACE = "VR000008"
RIDE = "/godot/surface/laneRide"
FACE_RIDE = f"/godot/bus/{FACE}/laneRide"


def send(server: Server, address: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args or []))


def points_of(server: Server, address: str) -> "list[tuple[float, float]]":
    """A lane as the tree publishes it: every value of the list node."""
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return []

    try:
        values = [float(v) for v in json.loads(body)["VALUE"]]
    except (KeyError, ValueError, TypeError):
        return []

    return list(zip(values[0::2], values[1::2]))


def lane_of(server: Server) -> "list[tuple[float, float]]":
    return points_of(server, f"/godot/cue/{CUE}/levelLane")


def send_into(server: Server, bus: str) -> str:
    """The identifier of the send into `bus`, or empty: the cue has one send at most."""
    try:
        contents = common.http_json(server.http_port, "/godot/send").get("CONTENTS", {})
    except (HarnessError, ValueError):
        return ""

    for send_id, node in contents.items():
        if node.get("CONTENTS", {}).get("bus", {}).get("VALUE", [""])[0] == bus:
            return send_id

    return ""


def with_face(bundle: Path) -> None:
    """The copy's show gains Face, a stereo mix on outputs 3-4 the cue does not
    send to, and says the file is two channels wide, which a send is routed by."""
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('<Bus id="VR000004" name="Main L/R" width="2"/>',
                        '<Bus id="VR000004" name="Main L/R" width="2"/>\n'
                        f'    <Bus id="{FACE}" firstChannel="2" kind="mix" name="Face" width="2"/>')
    text = text.replace('<Media id="VR000002" file="tone.wav"', '<Media id="VR000002" channels="2" file="tone.wav"')

    if FACE not in text or 'channels="2"' not in text:
        raise HarnessError("the lane-record bundle is not the one this driver knows")

    show.write_text(text, encoding="utf-8", newline="\n")


def wait_frames(render: Path, frames: int) -> bool:
    return lane_level.wait_for_frames(render, frames)


def run(locale: "str | None") -> int:
    report = Report(f"a level and a send recorded from the flipped faders ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-lane-record-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "lane-record")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        with_face(bundle)
        lane_level.write_tone(bundle / "media" / "tone.wav")

        marks = {}
        written = []
        sent = []
        made = ""

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=lane_level.BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")

            # --- the flip, and the two RECs (UI) ---------------------------------
            send(server, "/godot/cmd/lane/arm", [CUE])
            report.equal(first_sound.wait_for(server, "/godot/surface/lane", CUE), CUE,
                         "lane.arm: the faders flip to the cue")
            report.equal(first_sound.wait_for(server, f"/godot/slot/{LEVEL_STRIP}/target", RIDE), RIDE,
                         "strip one rides the level's lane (UN)")
            report.equal(first_sound.value_of(server, f"/godot/slot/{FACE_STRIP}/target"), FACE_RIDE,
                         "strip two rides Face's")
            report.equal(first_sound.value_of(server, RIDE), 0.0,
                         "the level's fader sits where it is heard - unity, with no lane yet (UK)")
            report.equal(first_sound.value_of(server, FACE_RIDE), -120.0,
                         "Face's at the bottom: the cue does not send there")

            send(server, "/godot/cmd/lane/rec", ["level", True])
            send(server, "/godot/cmd/lane/rec", [FACE, True])
            report.equal(first_sound.wait_for(server, "/godot/surface/laneRec", f"level {FACE}"),
                         f"level {FACE}", "lane.rec: both lanes armed, in strip order")
            report.equal(first_sound.value_of(server, f"/godot/slot/{FACE_STRIP}/word"), "rec",
                         "and Face's strip says rec")

            # --- the pass: the cue plays, the faders read, then are ridden -------
            send(server, "/godot/cmd/lane/record")
            report.equal(first_sound.wait_for(server, "/godot/surface/laneRecording", True), True,
                         "lane.record: a pass runs")

            start = lane_level.clock_frame(server)
            report.check(wait_frames(render, start + RATE), "the cue plays a second untouched")

            marks["touch"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/touch", [RIDE])
            send(server, "/godot/cmd/node/set", [RIDE, -6.0])
            send(server, "/godot/cmd/node/touch", [FACE_RIDE])
            send(server, "/godot/cmd/node/set", [FACE_RIDE, -10.0])
            report.check(wait_frames(render, marks["touch"] + RATE), "a second at -6, Face at -10")
            report.equal(first_sound.value_of(server, f"/godot/slot/{FACE_STRIP}/word"), "recording",
                         "Face's strip says it is being written")

            marks["move"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/set", [RIDE, -12.0])
            report.check(wait_frames(render, marks["move"] + RATE), "a second at -12")

            marks["release"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/node/release", [RIDE])
            send(server, "/godot/cmd/node/release", [FACE_RIDE])
            report.check(wait_frames(render, marks["release"] + RATE), "a second after the hands let go")

            marks["stop"] = lane_level.clock_frame(server)
            send(server, "/godot/cmd/lane/stop")
            report.equal(first_sound.wait_for(server, "/godot/surface/laneRecording", False), False,
                         "lane.stop: the pass ends")

            common.wait_until(lambda: len(lane_of(server)) > 0 and send_into(server, FACE) != "", timeout=10.0)
            written = lane_of(server)
            made = send_into(server, FACE)
            sent = points_of(server, f"/godot/send/{made}/levelLane") if made else []

            report.check(len(written) >= 3, "the level's ride is written into the cue's lane",
                         f"{len(written)} points")
            report.check(made != "", "a send into Face is made, the mix the cue did not send to (UQ)")
            report.equal(first_sound.value_of(server, f"/godot/send/{made}/level") if made else None, 0.0,
                         "at nought, its lane holding the ride")
            report.check(len(sent) >= 3, "and Face's ride is written into it", f"{len(sent)} points")

            # --- the faders stay on the cue (UM), and the pass says what it wrote --
            report.equal(first_sound.value_of(server, "/godot/surface/lane"), CUE,
                         "the faders stay flipped to the cue (UM)")
            report.equal(first_sound.value_of(server, "/godot/surface/laneRec"), f"level {FACE}",
                         "with their RECs")
            said = str(first_sound.value_of(server, "/godot/surface/lanePass") or "").split()

            def inside(points: "list[tuple[float, float]]") -> int:
                """The points a pass wrote: those in the span it says, joins included -
                not the silence a made send's lane holds from the file's start."""
                low, high = float(said[4]), float(said[5])
                return sum(1 for s, _ in points if low - 1.0e-4 <= s <= high + 1.0e-4)

            report.check(len(said) == 8 and said[1] == CUE and said[2] == "kept"
                         and int(said[3]) == inside(written) + inside(sent) and said[6:] == ["level", FACE],
                         "and what the pass wrote is said: kept, its points, the seconds and both lanes",
                         f"{said}")

            report.equal(first_sound.value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            # --- one undo takes the pass away, the send it made with it ----------
            send(server, "/godot/cmd/undo", ["document", "lane.write"])
            common.wait_until(lambda: len(lane_of(server)) == 0 and send_into(server, FACE) == "", timeout=10.0)
            report.check(len(lane_of(server)) == 0 and send_into(server, FACE) == "",
                         "one undo takes the whole pass away - both lanes, and the send it made")

            send(server, "/godot/cmd/redo", ["document", "lane.write"])
            common.wait_until(lambda: len(lane_of(server)) > 0, timeout=10.0)

            send(server, "/godot/cmd/lane/free")
            report.equal(first_sound.wait_for(server, f"/godot/slot/{LEVEL_STRIP}/target", ""), "",
                         "lane.free: the faders flip back")

            first_sound.wait_for_render_tail(render)

        # --- what was written ----------------------------------------------------
        levels = [level for _, level in written]
        report.check(any(abs(level - (-6.0)) <= 0.05 for level in levels),
                     "the level's lane holds the -6 the hand set", f"{levels}")
        report.check(any(abs(level - (-12.0)) <= 0.05 for level in levels),
                     "and the -12 it moved to", f"{levels}")

        rides = [level for level in levels if level < -0.05]
        report.check(bool(rides) and abs(rides[-1] - (-12.0)) <= 0.05,
                     "LATCHED: after the hand let go, -12 is held to the end of the pass (DH)",
                     f"{levels}")
        report.check(bool(written) and abs(written[0][1]) <= 0.05 and abs(written[-1][1]) <= 0.05,
                     "joined to unity either side of the ride - the lane it replaced (DL)",
                     f"{written[:1]} ... {written[-1:]}")

        face_levels = [level for _, level in sent]
        report.check(any(abs(level - (-10.0)) <= 0.05 for level in face_levels),
                     "Face's lane holds the -10 its hand set", f"{face_levels}")
        report.check(bool(sent) and sent[0][1] <= -119.9 and sent[-1][1] <= -119.9,
                     "joined to silence either side of the ride - the mix it was not sent to (UQ)",
                     f"{sent[:1]} ... {sent[-1:]}")

        # --- what was heard --------------------------------------------------------
        channels, data = first_sound.read_render(render)
        left = data[0] if data else []
        face = data[2] if len(data) > 2 else []
        begin = first_sound.first_above(left, 0.005)
        report.check(begin >= 0, "the cue was heard at all")
        report.check(len(data) >= 4, "the render carries Face's outputs", f"{channels} channels")

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

            if face:
                before = lane_level.level_db(face, begin + int(0.5 * RATE))
                report.check(before < -60.0, "Face is silent before its fader is touched",
                             f"{before:.1f} dBFS")

                for name, after, wanted, words in (
                        ("touch", 0.5, -16.0, "Face hears the send's -10 under the level's -6"),
                        ("move", 0.5, -22.0, "and the level's -12 over it after the move")):
                    heard = lane_level.level_db(face, marks[name] + int(after * RATE)) - unity
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
