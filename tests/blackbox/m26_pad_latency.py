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

"""M26 — a pad press to sound: how long, in ticks and in milliseconds.

AN INSTRUMENT, NOT A GATE, in the idiom of M25 and section 14.14: it prints and
asserts nothing, it is not registered as a ctest, and the number that counts is
the one taken on a machine nobody else is using. The author runs it and reads
it; the figure goes in namespace draft §16.9 and PRD §6.11.

WHAT IT IS FOR. A pad is an instrument only if it answers like one. A press
reaches the engine as a command, is applied at the next tick boundary, and the
clip's launch is then PLACED a whole number of ticks ahead - far enough that
Tracktion never renders a block with a hole in it (Runner.h: the launch instant
is a sample, decided by Go.dot). So the delay from the command to the sound is
arithmetic: the tick the press is applied on, plus `launchLatencyTicks`, plus
whatever the audio side adds. This measures the whole of it out of a real
render, rather than trusting the arithmetic.

WHAT IT CANNOT SEE. The time between the hand and the tick boundary - a press
queued up to one tick, twenty milliseconds at 50 Hz, before it is applied - is
not in the render or the log. It is printed as the bound it is.

THE LEVER, if the author finds a pad slow: the tick rate, parked at 50 Hz on
2026-09-18 with 100 Hz named as the thing to reach for if latency ever
matters. A smaller audio buffer shortens `launchLatencyTicks` as well; run
with --buffer to see by how much.

HOW IT READS. The sampler fixture, given a bus and a constant tone per member
(phase6_sampler.py's copy), served hosted with a render. Thunder - a hold clip
on the first strip - is pressed, held for a few ticks, let go, and pressed
again once it has armed again, `--presses` times. The log says the tick each
press was applied on and the tick each launch was placed on; the render says
the sample each onset landed on.

    python tests/blackbox/m26_pad_latency.py [--presses 20] [--buffer 64]
"""

from __future__ import annotations

import argparse
import statistics
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common                                    # noqa: E402
import first_sound                               # noqa: E402
import phase6_sampler                            # noqa: E402
from common import HarnessError, Server          # noqa: E402

RATE = 48000
STRIP = "STRP0001"          # Thunder: a hold clip, pressed with no velocity, at unity


def ticks_of(log: Path, command: str, strip_or_run: "str | None" = None) -> "list[tuple[int, str]]":
    """(tick, first argument) of every applied record of one command, in order."""
    out = []

    for line in log.read_text(encoding="utf-8").splitlines():
        parts = line.split()

        if len(parts) < 6 or parts[0] != "A" or parts[4] != command:
            continue

        argument = parts[5].removeprefix('s:"').removesuffix('"')

        if strip_or_run is None or argument == strip_or_run:
            out.append((int(parts[1]), argument))

    return out


def onsets(samples: "list[float]", level: float, quiet: float = 1.0e-4,
           gap: int = RATE // 20) -> "list[int]":
    """Every frame where the render rises past `level` after at least `gap`
    frames of silence - a clip starting, and not a clip continuing."""
    out = []
    silent = gap

    for n, value in enumerate(samples):
        magnitude = abs(value)

        if magnitude < quiet:
            silent += 1
            continue

        if magnitude > level and silent >= gap:
            out.append(n)

        silent = 0

    return out


def describe(label: str, values: "list[float]", unit: str) -> None:
    if not values:
        print(f"  {label:<40} (none)")
        return

    print(f"  {label:<40} median {statistics.median(values):8.2f} {unit}   "
          f"min {min(values):8.2f}   max {max(values):8.2f}   n={len(values)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--presses", type=int, default=20)
    parser.add_argument("--buffer", type=int, default=64)
    parser.add_argument("--keep", action="store_true", help="keep the scratch folder")
    options = parser.parse_args()

    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"m26: {problem}", file=sys.stderr)
        return 2

    scratch = Path(tempfile.mkdtemp(prefix="wfg-m26-"))
    bundle = common.copy_bundle(phase6_sampler.FIXTURE, scratch / "sampler")
    render = scratch / "out.wav"
    log = scratch / "session.wfglog"

    phase6_sampler.give_it_a_bus(bundle)

    print(f"M26 pad press to sound: {options.presses} presses, {RATE} Hz, "
          f"buffer {options.buffer}, scratch {scratch}")

    with Server(bundle, log=log, sample_rate=RATE, buffer_size=options.buffer,
                hosted=True, render=render) as server:
        hand = phase6_sampler.Hand(server)
        value = lambda address: phase6_sampler.value_of(server, address)   # noqa: E731

        try:
            if phase6_sampler.wait_for(server, "/godot/audio/status", "running") != "running":
                print("m26: the audio side did not come up", file=sys.stderr)
                return 2

            latency_ticks = value("/godot/engine/launchLatencyTicks")
            hand.send("/godot/cmd/go")

            for press in range(options.presses):
                if not phase6_sampler.wait_until(
                        server, lambda: value(f"/godot/slot/{STRIP}/word") == "armed", 30.0):
                    print(f"m26: the strip never armed before press {press + 1}", file=sys.stderr)
                    return 2

                # A breath of silence between presses, so every onset is one.
                phase6_sampler.wait_ticks(server, 5)
                hand.send("/godot/cmd/strip/press", [STRIP])

                phase6_sampler.wait_until(
                    server, lambda: value(f"/godot/slot/{STRIP}/word") == "held", 30.0)
                phase6_sampler.wait_ticks(server, 8)
                hand.send("/godot/cmd/strip/release", [STRIP])
                phase6_sampler.wait_until(
                    server, lambda: value(f"/godot/slot/{STRIP}/word") != "held", 30.0)

            phase6_sampler.wait_ticks(server, 10)
            first_sound.wait_for_render_tail(render)
            lateness = value("/godot/engine/latenessMax")
        finally:
            hand.close()

    # --- what the records and the render say ---------------------------------
    header = [line for line in log.read_text(encoding="utf-8").splitlines()
              if line.startswith("# clock ")]
    per_tick = 960

    for word in (header[0].split() if header else []):
        if word.startswith("samplesPerTick="):
            per_tick = int(word.split("=", 1)[1])

    presses = [tick for tick, _ in ticks_of(log, "strip.press", STRIP)]
    started = [tick for tick, _ in ticks_of(log, "run.started")]

    _, samples = first_sound.read_render(render)
    heard = onsets(samples[0], first_sound.AMPLITUDE / 2.0)

    print(f"  samplesPerTick {per_tick}, launchLatencyTicks {latency_ticks}, "
          f"latenessMax {lateness} samples")
    print(f"  {len(presses)} presses applied, {len(started)} launches placed, "
          f"{len(heard)} onsets heard")

    if not presses or len(heard) < len(presses):
        print("m26: fewer onsets than presses - read the render by hand "
              f"({render})", file=sys.stderr)
        return 1

    # Onsets pair with presses in order: every press is one clip from silence.
    heard = heard[:len(presses)]

    """ON TIME AND LATE, SEPARATED. A launch is placed from where the audio
    really is when the hook runs, so a tick thread running behind the audio
    clock shows up here as a sound that lands long after its tick - which is a
    fact about that machine at that moment (a Debug build, a box shared with a
    compiler), not about the path. A press is ON TIME when its launch sounded
    within `launchLatencyTicks` plus one tick of the tick that placed it; the
    late ones are counted and shown apart, beside `latenessMax`, and the
    figure that goes in §16.9 is the on-time one, taken on a quiet machine."""
    on_time = []
    late = []

    for onset, press in zip(heard, presses):
        placed = [tick for tick in started if tick >= press]

        if not placed:
            continue

        applied = onset - press * per_tick
        after_placing = onset - placed[0] * per_tick
        row = (applied, after_placing, placed[0] - press)

        if after_placing <= (int(latency_ticks or 0) + 1) * per_tick:
            on_time.append(row)
        else:
            late.append(row)

    to_ms = 1000.0 / RATE

    print(f"  ON TIME ({len(on_time)} of {len(presses)}):")
    describe("press applied -> launch placed (ticks)", [float(r[2]) for r in on_time], "tk ")
    describe("launch placed -> sound (ms)", [r[1] * to_ms for r in on_time], "ms ")
    describe("press applied -> sound (ms)", [r[0] * to_ms for r in on_time], "ms ")
    describe("press applied -> sound (ticks)", [r[0] / per_tick for r in on_time], "tk ")
    print(f"  plus the wait for the tick boundary, before `press applied`: "
          f"0 to {1000.0 * per_tick / RATE:.0f} ms")

    if late:
        print(f"  LATE ({len(late)}): the tick thread was behind the audio clock "
              f"(latenessMax {lateness} samples) - not the path, the machine:")
        describe("press applied -> sound (ms)", [r[0] * to_ms for r in late], "ms ")

    if not options.keep:
        import shutil
        shutil.rmtree(scratch, ignore_errors=True)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as problem:
        print(f"m26: {problem}", file=sys.stderr)
        sys.exit(2)
