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

"""A send's lane, heard (namespace draft §28.4), and M48.

WHAT THIS IS FOR. The unit suite proves that the Runner reads a send's lane
and hands the voice its matrix; what it cannot say is that the SHIPPED BINARY,
serving a bundle whose sends carry lanes, plays a real Tracktion voice into a
mix at the levels the lanes draw - on the file's own clock, the same stretch on
every pass of a looping slice, and a new lane written over the network while a
loop sounds.

`lane_level.py`'s method, on a send instead of the cue's level: the same tone,
the same readings, the same witness for a tick that did not come, and the same
rule that a shared CI runner's timing is not judged. Its helpers are imported
rather than copied, so the two drivers read a render one way.

THE BUNDLE IS tests/fixtures/bundles/send-lane: two media cues with no direct
out, each sending into one stereo mix, the only output there is - so what the
render hears IS the send.

  1  "Travel" - the whole file, its send's lane the level lane driver's ramp
     and step: `1 0 3 -20 5.5 -20 6 -25 6.001 -5 7 0`.
  2  "Loop" - one slice, 2 s to 4 s, looping for ever, its send's lane
     `2 0 4 -20` down across the slice.

WHAT DIFFERS FROM THE LEVEL'S, and the reason M48 exists. A send reaches the
voice as a matrix COEFFICIENT, and a coefficient glides over 50 ms
(`CueMatrix::slewSeconds`) where a level glides over one tick. The lane is
therefore read one glide ahead (decision PZ): on a ramp that puts the sound on
the drawing, and a step becomes a 50 ms ramp that ends where the step was
drawn - linear in gain, so its middle in decibels comes early. M48 prints the
error on the slopes and where the step's middle lands; the step is judged
within one glide of the drawing.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server
from lane_level import (BLOCK, FILE_SECONDS, LEVEL_TOLERANCE_DB, MIN_JUDGED, RATE,
                        SLICE_IN, SLICE_OUT, check_level, clock_frame, lane_at, level_db,
                        starved, stood_still, timed, wait_for_frames, window_db, write_tone)


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "send-lane"

TRAVEL_SEND = "SE000003"
LOOP_SEND = "SE000006"

TRAVEL_LANE = lane_level.RAMP_LANE
LOOP_LANE = lane_level.LOOP_LANE
STEP_AT = lane_level.STEP_AT
STEP_MIDDLE_DB = lane_level.STEP_MIDDLE_DB

# One coefficient glide, `CueMatrix::slewSeconds`: a step drawn on a send is a
# ramp that long, read that far ahead.
GLIDE_S = 0.050


def run(locale: "str | None") -> int:
    report = Report(f"a send's lane, heard ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-send-lane-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "send-lane")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_tone(bundle / "media" / "tone.wav")

        edit_at = 0

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")

            report.equal(first_sound.value_of(server, f"/godot/send/{TRAVEL_SEND}/levelLane"), 1.0,
                         "the first cue's send publishes its lane, its first second first")

            # --- the ramp and the step: the whole file, and then some silence --
            gone = first_sound.frames_on_disk(render)
            first_sound.go(server)

            report.check(wait_for_frames(render, gone + int(RATE * (FILE_SECONDS + 1.0))),
                         "the render runs past the end of the first cue")

            # --- the loop, then a new lane written on its send while it sounds --
            gone = first_sound.frames_on_disk(render)
            first_sound.go(server)

            report.check(wait_for_frames(render, gone + int(RATE * 5.0)),
                         "the loop plays two passes and more")

            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/node/set",
                                              [f"/godot/send/{LOOP_SEND}/levelLane", "2 -12"]))
            edit_at = clock_frame(server)

            report.equal(first_sound.wait_for(server, f"/godot/send/{LOOP_SEND}/levelLane", 2.0), 2.0,
                         "the new lane lands on the looping cue's send")

            report.check(wait_for_frames(render, edit_at + int(RATE * 3.0)),
                         "and the render runs three seconds past it")

            report.equal(first_sound.value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/run/stopAll"))

            first_sound.wait_for_render_tail(render)

        # --- what came out ---------------------------------------------------
        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render is stereo", f"{channels} channels")

        left = data[0] if data else []
        travel = first_sound.first_above(left, 0.005)
        report.check(travel >= 0, "the first cue was heard in its mix at all")

        if travel < 0 or len(left) < travel + int(RATE * FILE_SECONDS):
            report.check(False, "the render is long enough to read the first cue",
                         f"{len(left)} frames from {travel}")
            return report.finish()

        def at(seconds: float) -> int:
            return travel + int(round(seconds * RATE))

        def travel_second(frame: int) -> "tuple[float, float]":
            seconds = (frame - travel) / RATE
            return seconds, lane_at(TRAVEL_LANE, seconds)

        unity = level_db(left, at(0.5))
        report.check(abs(unity - first_sound.db(lane_level.AMPLITUDE, 1.0)) <= 1.0,
                     "before the lane's first point the send is as written",
                     f"{unity:.2f} dB")

        spans = []
        stood = 0.0

        for a, b in ((1.05, 2.95), (5.55, 6.95)):
            found, share = stood_still(left, at(a), at(b), travel_second, TRAVEL_LANE)
            spans += found
            stood = max(stood, share)

        if spans:
            print(f"       the level stood still for {100.0 * stood:.0f}% of a slope, {len(spans)} time(s): "
                  "the tick thread did not run at the file's "
                  + ", ".join(f"{(a - travel) / RATE:.3f}-{(b - travel) / RATE:.3f} s" for a, b in spans))

        for seconds in (1.5, 2.0, 2.5, 4.5, 5.75, 6.5):
            drawn = lane_at(TRAVEL_LANE, seconds)
            check_level(report, spans, at(seconds), level_db(left, at(seconds)) - unity, drawn,
                        f"at the file's {seconds} s the send is its lane's {drawn:.1f} dB",
                        on_a_slope=seconds != 4.5)

        check_level(report, spans, at(7.5), level_db(left, at(7.5)) - unity, 0.0,
                    "and past the lane's last point, the send as written again")

        # --- M48: how far from the drawing, away from its corners ---------------
        corners = [s for s, _ in TRAVEL_LANE]
        worst, worst_at, voided, judged_on_slopes = 0.0, 0.0, 0, 0
        seconds = 1.1

        while seconds <= 7.9:
            if all(abs(seconds - corner) >= 0.1 for corner in corners):
                on_slope = abs(lane_at(TRAVEL_LANE, seconds + 0.01) - lane_at(TRAVEL_LANE, seconds - 0.01)) > 0.001

                heard = level_db(left, at(seconds)) - unity
                distance = abs(heard - lane_at(TRAVEL_LANE, seconds))

                if distance > LEVEL_TOLERANCE_DB and starved(spans, at(seconds)):
                    voided += 1
                else:
                    judged_on_slopes += 1 if on_slope else 0

                    if distance > worst:
                        worst, worst_at = distance, seconds

            seconds += 0.05

        crossed = None
        frame = at(STEP_AT - 0.15)

        while frame < at(STEP_AT + 0.1):
            if window_db(left, frame, 256) - unity > STEP_MIDDLE_DB:
                crossed = (frame - travel) / RATE
                break

            frame += 16

        step_starved = any(starved(spans, at(STEP_AT + d)) for d in (-0.09, -0.06, -0.03, 0.0, 0.03))

        print(f"M48: the rendered send is within {worst:.3f} dB of its lane away from the corners "
              f"(worst at the file's {worst_at:.2f} s"
              + (f", {voided} reading(s) voided where the tick did not run" if voided else "") + ")"
              + (f"; the step's middle lands {1000.0 * (crossed - STEP_AT):+.1f} ms from where "
                 f"it was drawn" if crossed is not None else "; the step was not found")
              + (" (a starved tick beside it)" if step_starved else ""))

        timed(report, worst <= LEVEL_TOLERANCE_DB,
              "M48: the send follows its lane within half a decibel away from the corners",
              f"{worst:.3f} dB at {worst_at:.2f} s")

        timed(report, judged_on_slopes >= MIN_JUDGED,
              "and enough of the slopes were heard in step with the tick to judge them",
              f"{judged_on_slopes} reading(s) judged on the slopes, {voided} voided")

        step_words = "M48: the step lands within one coefficient glide of where it was drawn"

        if step_starved and crossed is not None and abs(crossed - STEP_AT) > GLIDE_S:
            report.void(step_words, f"{1000.0 * (crossed - STEP_AT):+.1f} ms, with the tick thread held "
                                    f"beside it")
        else:
            timed(report, crossed is not None and abs(crossed - STEP_AT) <= GLIDE_S, step_words,
                  "not found" if crossed is None else f"{1000.0 * (crossed - STEP_AT):+.1f} ms")

        # --- the loop: the same stretch of its send's lane on every pass --------
        tail = at(FILE_SECONDS + 0.2)
        loop = first_sound.first_above(left[tail:], 0.005)
        report.check(loop >= 0, "the loop was heard in its mix")

        if loop >= 0:
            loop += tail
            slice_length = SLICE_OUT - SLICE_IN

            def loop_second(frame_: int) -> "tuple[float, float]":
                into = ((frame_ - loop) / RATE) % slice_length
                return SLICE_IN + into, lane_at(LOOP_LANE, SLICE_IN + into)

            loop_spans = []

            for passage in (0, 1):
                found, _ = stood_still(left, loop + int(RATE * (passage * slice_length + 0.1)),
                                       loop + int(RATE * (passage * slice_length + 1.9)), loop_second,
                                       LOOP_LANE)
                loop_spans += found

            for passage in (0, 1):
                for into in (0.5, 1.5):
                    frame_ = loop + int(round((passage * slice_length + into) * RATE))
                    drawn = lane_at(LOOP_LANE, SLICE_IN + into)
                    check_level(report, loop_spans, frame_, level_db(left, frame_) - unity, drawn,
                                f"pass {passage + 1}, {into} s in: the slice's stretch of the send's lane, "
                                f"{drawn:.1f} dB", on_a_slope=True)

            for after in (1.25, 2.0):
                heard = level_db(left, edit_at + int(RATE * after)) - unity
                report.check(abs(heard - (-12.0)) <= LEVEL_TOLERANCE_DB,
                             f"{after} s after the new lane was sent, the loop's send is at its -12 dB",
                             f"heard {heard:.2f} dB")

        # --- and the session replays -------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session",
                     (out + err).strip()[-400:])
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
