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

"""A media cue's level lane, heard (namespace draft §20.6), and M45.

WHAT THIS IS FOR. The unit suite proves the lane's arithmetic, the Runner's
term against a fake player and the window's gestures against a component. What
none of them says is that the SHIPPED BINARY, serving a bundle whose media cues
carry lanes, plays a real Tracktion voice at the levels the lanes draw - on the
file's own clock, the same stretch on every pass of a looping slice, and a new
lane written over the network while a loop sounds.

THE SOUND IS ONE TONE, a 750 Hz sine at a quarter of full scale, eight
seconds long. Its period, 64 samples at 48 kHz, divides the block - the hosted
render on a loaded Debug build drops whole blocks now and then (§17.12), and a
tone that keeps its phase across a dropped block keeps its reading. The level
is read as the RMS of 512 samples - eight whole periods - taken as the median
of five windows a few milliseconds apart, so one block Tracktion muted under a
loaded runner (§19.11) cannot move a reading.

THE BUNDLE IS tests/fixtures/bundles/lane:

  1  "Ramp and step" - the whole file, its lane `1 0 3 -20 6 -20 6.001 0`:
     unity for a second, straight down to -20 dB by the third, held, and back
     to unity in a millisecond at the sixth - a step, as near as a lane draws
     one.
  2  "Loop" - one slice of the same file, 2 s to 4 s, looping for ever, its
     lane `2 0 4 -20` down across the slice: every pass hears the same stretch.

M45 (namespace draft §20.6) is two numbers this prints: the largest distance
between the rendered level and the drawn one away from the corners, and how
far the step's midpoint lands from where it was drawn. §3.4 says a lane is
worked out at the tick and the voice slews to it over one; the lane is read one
slew ahead (decision DC), so the step is expected inside a tick either side.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import math
import statistics
import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "lane"

RATE = 48000
BLOCK = 64
SAMPLES_PER_TICK = RATE // 50

RAMP = "VC000002"
LOOP = "VC000005"

TONE_HZ = 750.0
AMPLITUDE = 0.25
FILE_SECONDS = 8.0
SLICE_IN, SLICE_OUT = 2.0, 4.0

# What the first cue's lane draws, as the engine plays it: straight in dB
# between points, held beyond both ends.
RAMP_LANE = [(1.0, 0.0), (3.0, -20.0), (6.0, -20.0), (6.001, 0.0)]
STEP_AT = 6.0005

LEVEL_TOLERANCE_DB = 0.5
STEP_TOLERANCE_S = 0.020        # one tick


# =============================================================================
# The media file, and reading a level out of the render
# =============================================================================

def write_tone(path: Path) -> None:
    """A stereo 750 Hz sine at a quarter of full scale."""
    path.parent.mkdir(parents=True, exist_ok=True)

    out = bytearray()

    for n in range(int(RATE * FILE_SECONDS)):
        sample = int(round(AMPLITUDE * math.sin(2.0 * math.pi * TONE_HZ * n / RATE) * 32767.0))
        out += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(bytes(out))


def lane_at(points: "list[tuple[float, float]]", seconds: float) -> float:
    """What a lane asks for at a second of the file (doc::laneLevelDb)."""
    if seconds <= points[0][0]:
        return points[0][1]

    for (s0, l0), (s1, l1) in zip(points, points[1:]):
        if seconds <= s1:
            return l0 + (l1 - l0) * (seconds - s0) / (s1 - s0)

    return points[-1][1]


def window_db(samples: "list[float]", centre: int, width: int = 512) -> float:
    start = max(0, centre - width // 2)
    part = samples[start:start + width]

    if len(part) < width:
        return -400.0

    rms = math.sqrt(sum(x * x for x in part) / len(part))
    return 20.0 * math.log10(rms * math.sqrt(2.0)) if rms > 0.0 else -400.0


def level_db(samples: "list[float]", centre: int) -> float:
    """The median of five windows 4 ms apart: one muted block moves none of it."""
    spread = int(RATE * 0.004)
    return statistics.median(window_db(samples, centre + k * spread) for k in range(-2, 3))


# =============================================================================
# The session
# =============================================================================

def wait_for_frames(render: Path, frames: int, timeout: float = 30.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


def clock_frame(server: Server) -> int:
    """WHERE THE AUDIO HAD GOT TO when the engine last ticked: the tick in
    frames and how late that tick was processed (`engine/lateness`), from one
    snapshot - `phase9c_take.py`'s mark, for its reason: the render reaches the
    disk about a second at a time, so a mark from its length just after an event
    can be a second before it (2238346)."""
    try:
        _, answer = common.http_get(server.http_port, "/godot/engine")
        contents = common.json.loads(answer)["CONTENTS"]
        tick = int(contents["tick"]["VALUE"][0])
        lateness = int(contents["lateness"]["VALUE"][0])
        return tick * SAMPLES_PER_TICK + max(0, lateness)
    except Exception:
        return 0


def run(locale: "str | None") -> int:
    report = Report(f"a media cue's level lane, heard ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-lane-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "lane")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_tone(bundle / "media" / "tone.wav")

        edit_at = 0

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")

            report.equal(first_sound.value_of(server, f"/godot/cue/{RAMP}/levelLane"), 1.0,
                         "the first cue's lane is published, its first second first")

            # --- the ramp and the step: the whole file, and then some silence --
            gone = first_sound.frames_on_disk(render)
            first_sound.go(server)

            report.check(wait_for_frames(render, gone + int(RATE * (FILE_SECONDS + 1.0))),
                         "the render runs past the end of the first cue")

            # --- the loop, then a new lane written while it sounds -------------
            gone = first_sound.frames_on_disk(render)
            first_sound.go(server)

            report.check(wait_for_frames(render, gone + int(RATE * 5.0)),
                         "the loop plays two passes and more")

            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/node/set", [f"/godot/cue/{LOOP}/levelLane", "2 -12"]))
            edit_at = clock_frame(server)

            report.equal(first_sound.wait_for(server, f"/godot/cue/{LOOP}/levelLane", 2.0), 2.0,
                         "the new lane lands on the looping cue")

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
        ramp = first_sound.first_above(left, 0.005)
        report.check(ramp >= 0, "the first cue was heard at all")

        if ramp < 0 or len(left) < ramp + int(RATE * FILE_SECONDS):
            report.check(False, "the render is long enough to read the first cue",
                         f"{len(left)} frames from {ramp}")
            return report.finish()

        def at_file_second(start: int, seconds: float) -> int:
            return start + int(round(seconds * RATE))

        # Unity: the first second of the file, before the lane's first point.
        unity = level_db(left, at_file_second(ramp, 0.5))
        report.check(abs(unity - first_sound.db(AMPLITUDE, 1.0)) <= 1.0,
                     "before the lane's first point the cue plays as written",
                     f"{unity:.2f} dB")

        # --- the ramp, down in dB, and the hold --------------------------------
        for seconds in (1.5, 2.0, 2.5, 4.5):
            drawn = lane_at(RAMP_LANE, seconds)
            heard = level_db(left, at_file_second(ramp, seconds)) - unity
            report.check(abs(heard - drawn) <= LEVEL_TOLERANCE_DB,
                         f"at the file's {seconds} s the level is the lane's {drawn:.1f} dB",
                         f"heard {heard:.2f} dB")

        heard = level_db(left, at_file_second(ramp, 7.0)) - unity
        report.check(abs(heard) <= LEVEL_TOLERANCE_DB,
                     "and after the step, unity again", f"heard {heard:.2f} dB")

        # --- M45: how far from the drawing, away from its corners ---------------
        worst = 0.0
        worst_at = 0.0
        seconds = 1.1

        while seconds <= 5.9:
            if abs(seconds - 3.0) >= 0.1:
                drawn = lane_at(RAMP_LANE, seconds)
                heard = level_db(left, at_file_second(ramp, seconds)) - unity

                if abs(heard - drawn) > worst:
                    worst, worst_at = abs(heard - drawn), seconds

            seconds += 0.05

        # And where the step's midpoint lands against where it was drawn.
        crossed = None
        frame = at_file_second(ramp, STEP_AT - 0.1)

        while frame < at_file_second(ramp, STEP_AT + 0.1):
            if window_db(left, frame, 256) - unity > -10.0:
                crossed = (frame - ramp) / RATE
                break

            frame += 16

        print(f"M45: the rendered level is within {worst:.3f} dB of the lane away from its corners "
              f"(worst at the file's {worst_at:.2f} s)"
              + (f"; the step's midpoint lands {1000.0 * (crossed - STEP_AT):+.1f} ms from where "
                 f"it was drawn" if crossed is not None else "; the step was not found"))

        report.check(worst <= LEVEL_TOLERANCE_DB,
                     "M45: the level follows the lane within half a decibel away from its corners",
                     f"{worst:.3f} dB at {worst_at:.2f} s")
        report.check(crossed is not None and abs(crossed - STEP_AT) <= STEP_TOLERANCE_S,
                     "M45: the step lands within a tick of where it was drawn",
                     "not found" if crossed is None else f"{1000.0 * (crossed - STEP_AT):+.1f} ms")

        # --- the loop: the same stretch of its lane on every pass ----------------
        tail = at_file_second(ramp, FILE_SECONDS + 0.2)
        loop = first_sound.first_above(left[tail:], 0.005)
        report.check(loop >= 0, "the loop was heard")

        if loop >= 0:
            loop += tail
            slice_length = SLICE_OUT - SLICE_IN
            loop_lane = [(SLICE_IN, 0.0), (SLICE_OUT, -20.0)]

            for passage in (0, 1):
                for into in (0.5, 1.5):
                    drawn = lane_at(loop_lane, SLICE_IN + into)
                    heard = level_db(left, loop + int(round((passage * slice_length + into) * RATE))) - unity
                    report.check(abs(heard - drawn) <= LEVEL_TOLERANCE_DB,
                                 f"pass {passage + 1}, {into} s in: the slice's stretch of the lane, "
                                 f"{drawn:.1f} dB",
                                 f"heard {heard:.2f} dB")

            # The lane written while it loops: a constant -12 once it has landed.
            for after in (1.25, 2.0):
                heard = level_db(left, edit_at + int(RATE * after)) - unity
                report.check(abs(heard - (-12.0)) <= LEVEL_TOLERANCE_DB,
                             f"{after} s after the new lane was sent, the loop plays at its -12 dB",
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
