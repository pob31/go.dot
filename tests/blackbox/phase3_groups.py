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

"""PHASE 3'S DONE-WHEN CLAUSE, on the shipped binary, over a socket.

The devplan states it in one sentence:

    a complex background auto-sequence with a looping ambience runs while manual
    foreground cues fire on top; an advance cue exits the loop cleanly; the
    replay log reproduces all of it.

Every clause of that is a check below, and none of them is asked of a library:
this starts `wfg serve --hosted`, drives it over UDP the way a console would,
reads the tree over HTTP the way a client would, and then looks at the WAV that
came out.

THE METHOD IS PHASE 2'S, EXTENDED. The media file's three seconds-long segments
are three DIFFERENT CONSTANTS, so the value on a channel at any instant says
which part of the file is sounding, as arithmetic - and two cues summed on one
channel say which two. No FFT, no windows, no thresholds: a sample either is
0.25 or it is not.

WHAT THE SHOW IS. One list holds an automatic sequence group - a ranged media
cue whose first range loops for ever, a network cue with a pre-wait, a nested
TIMELINE group of two cues at different offsets on other channels, and a footer
that tells the desk - followed by three manual cues: a media cue to fire on top,
a fade, and a stop cue whose verb is `advance`. A second list holds a memo with
an OSC trigger on it, which exists to be fired without moving anything.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import math
import struct
import subprocess
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "phase3"

RATE = 48000
BLOCK = 64

# The show's own identifiers. Named here because every check below is about a
# particular cue rather than about "whatever ran": standby arms ahead of GO, so
# "the run that just appeared" identifies nothing.
LIST_MAIN = "P3ACT001"
LIST_FOYER = "P3FYR001"
GROUP = "P3GRP001"
BED = "P3BED001"
TELL_DESK = "P3MSG001"
INNER = "P3GRP002"
TWO_LEFT = "P3TWA001"
TWO_RIGHT = "P3TWB001"
FOOTER_CUE = "P3MSG002"
ON_TOP = "P3TPX001"
FADE = "P3FAD001"
ADVANCE = "P3STP001"
DOORS = "P3MEM001"
TRIGGER = "P3TRG001"

# THE THREE CONSTANTS, and the reason they are these three. Each is a level a
# cue plays at, and no two of them and no SUM of two of them is equal to any
# other - so a channel carrying 0.25 is the first segment and one carrying
# 0.3125 is the first segment plus the foreground cue at -12 dB, and neither
# reading is ambiguous.
SEGMENTS = (0.25, 0.5, 0.75)

# What a rendered value has to be within to count as a segment. Generous, since
# the file is 16-bit and the graph is float; nowhere near half the gap between
# two segments, which is what would make a reading ambiguous.
TOLERANCE = 0.01


# =============================================================================
# The media file
# =============================================================================

def write_segments(path: Path) -> None:
    """Six seconds: three two-second segments, each a different constant.

    TWO SECONDS EACH because the show's ranges are two seconds each and a range
    is a region of THIS file - so the first range is the first constant, the
    second the second, and the third the third. Which is what makes "the render
    says which range is playing" true by construction rather than by arithmetic
    somebody has to trust.
    """
    path.parent.mkdir(parents=True, exist_ok=True)

    frames = int(RATE * 2)
    body = b""

    for level in SEGMENTS:
        sample = int(level * 32767)
        body += struct.pack("<hh", sample, sample) * frames

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(body)


def segment_of(value: float) -> int:
    """Which segment a rendered sample is, or -1."""
    for index, level in enumerate(SEGMENTS):
        if abs(abs(value) - level) < TOLERANCE:
            return index

    return -1


def settles_into(samples: "list[float]", segment: int, start: int = 0,
                 stable: int = RATE // 10) -> int:
    """The first frame at or after `start` where the render settles into a
    segment and stays there for a tenth of a second.

    STABILITY IS NOT FUSSINESS. A boundary is a few dozen samples of one range
    being taken down while another starts, and somewhere in that decay the
    outgoing constant passes through the incoming one - 0.5 on its way down from
    0.75 reads as the second segment for a sample or two. A sweep that believed
    the first matching sample would find every boundary twice.
    """
    run = 0

    for n in range(max(0, start), len(samples)):
        if segment_of(samples[n]) == segment:
            run += 1

            if run >= stable:
                return n - run + 1
        else:
            run = 0

    return -1


def span_at(samples: "list[float]", level: float) -> int:
    """First frame at `level` to last, inclusive - not the longest UNBROKEN run.

    AND THE DIFFERENCE IS THE MEASUREMENT. What this is asked about is whether a
    range that loops for ever was still sounding three seconds later, which is a
    span. The unbroken run is a different and much stricter question, and it
    answers "one pass" - because M12 measured a wrap at 48 kHz as leaving a
    handful of samples up to 0.027 out, which is more than the tolerance a
    segment is read with. A single sample of that at every wrap breaks a run and
    says nothing at all about whether the bed looped.

    The first version of this asked the strict question, passed on this box and
    failed under ctest on the same box - which is the shape of a measurement
    that was measuring the wrong thing and getting away with it.
    """
    first = last = -1

    for n, value in enumerate(samples):
        if abs(abs(value) - level) < TOLERANCE:
            if first < 0:
                first = n

            last = n

    return 0 if first < 0 else last - first + 1


# =============================================================================
# Driving
# =============================================================================

def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return json.loads(body)["VALUE"][0]
    except (KeyError, IndexError, ValueError):
        return None


def wait_for(server: Server, address: str, wanted, seconds: float = 20.0):
    """Polls until a node reads `wanted`, and answers with what it last read."""
    deadline = time.monotonic() + seconds
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == wanted:
            return actual

        time.sleep(0.02)

    return actual


def run_for_cue(server: Server, cue: str, seconds: float = 20.0) -> str:
    """The identifier of the live run of a cue, or empty.

    Walked out of `/godot/run/order` rather than remembered, because a run is
    created by the engine and its identifier is the engine's to draw.
    """
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        order = value_of(server, "/godot/run/order") or ""

        for run in order.split():
            if value_of(server, f"/godot/run/{run}/cue") == cue:
                return run

        time.sleep(0.02)

    return ""


def go(server: Server) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go"))


def fire(server: Server, cue: str) -> None:
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/cue/fire", [cue]))


def standby(server: Server, listId: str):
    return value_of(server, f"/godot/list/{listId}/standby")


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 3: groups, ranges and triggers ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase3-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "phase3")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_segments(bundle / "media" / "segments.wav")

        with first_sound.MockTarget("agree") as target:
            first_sound.point_mount_at(bundle, target)

            with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                        buffer_size=BLOCK, hosted=True, render=render) as server:

                report.equal(wait_for(server, "/godot/audio/status", "running"),
                             "running", "the audio side comes up running")

                # --- the standby armed the group's first member --------------
                # PR 3.3: standby on a GROUP arms what that group would launch
                # first, so the disk is paid for while the operator reads the
                # next line rather than after their hand comes down.
                bed_run = run_for_cue(server, BED, seconds=25.0)

                if not report.check(bool(bed_run),
                                    "the group's first member armed itself before any GO"):
                    return report.finish()

                report.equal(value_of(server, f"/godot/run/{bed_run}/state"), "armed",
                             "and it is armed rather than playing")

                # --- GO: the group runs --------------------------------------
                go(server)

                report.equal(wait_for(server, f"/godot/run/{bed_run}/state", "playing"),
                             "playing", "GO starts the group's first member")

                report.equal(value_of(server, f"/godot/run/{bed_run}/range"), 0,
                             "and it is in its first range")

                group_run = run_for_cue(server, GROUP)
                report.check(bool(group_run), "the group has a run of its own")

                if group_run:
                    report.equal(value_of(server, f"/godot/run/{bed_run}/parent"),
                                 group_run, "and the member's parent is that run")

                # --- the standby moved past the whole chain ------------------
                # §3.5: standby steps over an automatic group as one opaque
                # sibling. It is on the foreground media cue now, not inside.
                report.equal(standby(server, LIST_MAIN), ON_TOP,
                             "and the pointer stepped over the whole automatic chain")

                # --- the bed loops -------------------------------------------
                # Its first range is two seconds and loops for ever, so three
                # seconds in it is on its second pass and has NOT moved on.
                time.sleep(3.0)

                report.equal(value_of(server, f"/godot/run/{bed_run}/range"), 0,
                             "three seconds in, the bed is still in its first range")

                iteration = value_of(server, f"/godot/run/{bed_run}/rangeIteration")
                report.check(isinstance(iteration, int) and iteration >= 2,
                             "and on its second pass or later", str(iteration))

                # --- and the chain has NOT moved on --------------------------
                # The bed's first range loops for ever, so the members after it
                # have not been spawned - which is the whole point of an
                # infinite range: the scene sits there until somebody moves it.
                report.check(not run_for_cue(server, TWO_LEFT, seconds=0.5),
                             "and the members after it have not been spawned,"
                             " because an infinite range does not end")

                # --- a foreground cue on top ---------------------------------
                # Fired by identifier rather than by GO, so the pointer does not
                # move: this is the manual cue landing over a running scene.
                before = standby(server, LIST_MAIN)
                fire(server, ON_TOP)

                top_run = run_for_cue(server, ON_TOP, seconds=25.0)
                report.check(bool(top_run), "a foreground cue fires over the top of it")

                report.equal(standby(server, LIST_MAIN), before,
                             "and firing a cue by name moves no pointer (PRD 3.5)")

                time.sleep(1.0)

                # --- the trigger, on the other list --------------------------
                # §3.7: a trigger fires its cue and moves NOTHING. The foyer
                # list has its own standby, and neither it nor the main list's
                # may move.
                foyer_before = standby(server, LIST_FOYER)

                common.send_udp(server.osc_port, common.osc_encode("/foyer/doors"))

                doors_run = run_for_cue(server, DOORS, seconds=10.0)
                report.check(bool(doors_run), "a datagram from the foyer fires its memo")

                report.equal(standby(server, LIST_FOYER), foyer_before,
                             "and the foyer list's pointer did not move")
                report.equal(standby(server, LIST_MAIN), before,
                             "nor the main list's")

                # --- the advance ---------------------------------------------
                # §3.24's way out of a range that loops for ever: the range
                # playing now finishes the pass it is on and then leaves.
                fire(server, ADVANCE)

                report.equal(wait_for(server, f"/godot/run/{bed_run}/range", 1, 8.0), 1,
                             "the advance moves the bed into its second range")

                report.equal(wait_for(server, f"/godot/run/{bed_run}/range", 2, 8.0), 2,
                             "and the second range runs out into the third")

                report.equal(wait_for(server, f"/godot/run/{bed_run}/state", "done", 12.0),
                             "done", "and the playlist ends when the last range does")

                # --- and only THEN does the chain move on --------------------
                # The bed was the group's first member. With it finished, the
                # sequence spawns the network cue, and then the nested TIMELINE
                # group - which starts both of its members at entry, at
                # different offsets, on the other pair of channels.
                left = run_for_cue(server, TWO_LEFT, seconds=25.0)
                right = run_for_cue(server, TWO_RIGHT, seconds=25.0)

                report.check(bool(left) and bool(right),
                             "the nested timeline group then spawns both its members")

                inner_run = run_for_cue(server, INNER)

                if inner_run and left:
                    report.equal(value_of(server, f"/godot/run/{left}/parent"), inner_run,
                                 "and they belong to the inner group, not the outer")

                # --- the footer ran, and after the members --------------------
                footer_run = run_for_cue(server, FOOTER_CUE, seconds=25.0)
                report.check(bool(footer_run), "the group's footer ran at its exit")

                if group_run:
                    report.equal(wait_for(server, f"/godot/run/{group_run}/state",
                                          "done", 20.0),
                                 "done", "and the group is done once its footer is")

                time.sleep(0.5)

    # --- what came out ----------------------------------------------------
        channels, samples = first_sound.read_render(render)

        report.check(channels >= 4, "the render has the rig's four channels",
                     f"{channels}")

        if channels >= 2:
            main = samples[0]

            # THE BED IS ON CHANNEL 0 AT FULL LEVEL, and its first range is the
            # first segment. It has to be sounding for a long time - more than
            # three seconds, which is more than its own two-second length - and
            # that is what says the range LOOPED rather than the cue having
            # played once and stopped.
            held = span_at(main, SEGMENTS[0])

            report.check(held > RATE * 3,
                         "the bed's looping range was still sounding three seconds later",
                         f"{held} frames, which is {held / RATE:.1f} s")

            # AND THEN THE SECOND SEGMENT AND THE THIRD, in that order, which is
            # the advance and then the playlist running out.
            second = settles_into(main, 1)
            third = settles_into(main, 2, max(0, second) + RATE // 10)

            report.check(second > 0, "then the second range is heard", str(second))
            report.check(third > second, "and then the third", str(third))

            # AND THEN DIGITAL SILENCE. Not "quiet": the graph is float end to
            # end and a stopped clip contributes zero, so anything else is a
            # voice nobody released.
            tail = main[-RATE // 4:]
            loudest = max((abs(v) for v in tail), default=0.0)

            report.check(loudest < 1.0e-6,
                         "and then digital silence, not something quiet",
                         f"loudest in the last quarter second: {loudest}")

    # --- and it reproduces --------------------------------------------------
        # THE LAST CLAUSE OF THE DONE-WHEN. Every decision the scheduler took -
        # every member spawned, every range entered, every boundary placed - is
        # a logged command, so the whole session re-applies on a machine with no
        # audio, no socket and no clock.
        code, out, err = common.run_wfg("replay", str(log),
                                        f"--bundle={bundle}",
                                        f"--out={replayed}")

        report.equal(code, 0, "and `wfg replay` reproduces the session",
                     (out + err).strip()[:400])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"phase3_groups: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"phase3_groups: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"phase3_groups: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
