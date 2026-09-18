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

"""M25 — what the window costs the thread the show runs on.

AN INSTRUMENT, NOT A GATE. It prints and asserts nothing, in the idiom section
14.14 sets for every measurement in this project, and it is deliberately NOT
registered as a ctest: it takes minutes, it wants a machine nobody else is
using, and a number from a shared CI runner under load is not the number. The
author runs it and reads it.

WHAT IT IS FOR. The compiled client runs in the same process as the engine
(namespace draft section 14.16, question E), so the one thing worth measuring
before the window grows is whether its message thread costs the TICK thread
anything. The risk is not repaint cost. It is that
`elevateCurrentThreadForTicking()` still returns false and does nothing
(clock/TickThread.cpp:68-74), so the tick thread runs at ordinary priority and
competes with a GUI for a core.

THE CONDITIONS, as the plan draws them:

  A   no window          the baseline: serve as every driver runs it
  B   window, idle       the window open, nothing touched
  A'  no window, firing  a GO every two seconds, for C to be read against
  C   window, firing     the same, with the window open      (--firing)
  D   C, cache defeated  the model rebuilt every pass        (not yet: M4)

C arrived with M3, which gave the window a list to draw; it is half of the
plan's condition C, since nobody is scrolling. D wants a way to defeat the
model's cache and comes with M4. A take says which conditions it ran rather
than pretending to all of them.

THE THRESHOLDS, which are the whole point of taking it early:

  green  B within one tick of A (960 samples at 48 kHz), worst repaint < 10 ms
  amber  one to two ticks, or 10-20 ms: fix before the next view
  red    more than two ticks, OR `go -> sound` worsens at all, OR an
         rtViolation A does not have. The work STOPS, and the remedy is making
         `elevateCurrentThreadForTicking()` real with spatcore's
         rt/RtThreadPriority.h - owed since PR 1.D - not redesigning the client.

HOW IT READS. `/godot/engine/lateness` says how late the last tick was
processed, in samples; `latenessMax` is the worst since the engine started.
Sampling over HTTP costs the server a socket per sample, which is why both
conditions are sampled the same way: what is being compared is A against B, and
a cost paid by both cancels.

    python tests/blackbox/m25_window_cost.py [--seconds 120] [--cues 500]

A window opens on condition B, so this wants a desktop session. On a machine
with no display, B fails to start and the script says so instead of hanging.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "fixtures"))

import common                                    # noqa: E402
import make_large_show                           # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
THEME = REPO / "clients" / "desktop" / "theme.json"

SAMPLE_HZ = 10.0


def read_int(server, address, default=0):
    try:
        reply = common.http_json(server.http_port, address + "?VALUE")
        values = reply.get("VALUE") if isinstance(reply, dict) else None
        return int(values[0]) if isinstance(values, list) and values else default
    except Exception:
        return default


def measure(bundle: Path, seconds: float, window: bool, label: str, firing: bool = False):
    """One condition. Returns a dict of readings, or None if it would not start.

    `firing` sends a GO every two seconds, which is what makes a condition the
    engine's busy case rather than its idle one: runs spawning and ending mean
    the runtime half of the tree is rebuilt and republished every tick, and
    every one of those is a snapshot the window reads and rows whose
    decoration changes. It is half of the plan's condition C - the half that
    does not need a hand on the scrollbar.
    """
    print(f"\n-- {label} --", flush=True)

    try:
        server = common.Server(bundle, hosted=True, window=window,
                               theme=THEME if window else None)
    except Exception as exc:
        print(f"   could not start: {exc}")
        return None

    try:
        with server:
            rate = read_int(server, "/godot/engine/sampleRate", 48000)
            tick_samples = read_int(server, "/godot/engine/samplesPerTick", rate // 50 or 960)

            #  A moment to settle: the graph is built and the first ticks carry
            #  the cost of building it, which is a fact about opening a show
            #  rather than about running one.
            time.sleep(3.0)

            start_errors = read_int(server, "/godot/engine/errorCount")
            start_violations = read_int(server, "/godot/engine/rtViolations")

            #  WHETHER A HAND ARRIVED. Condition B puts a window on somebody's
            #  screen, and on 2026-09-17 a reading this harness could not
            #  explain turned out to be the author pressing the lock button
            #  (namespace draft section 14.11). A click during a measurement
            #  does not corrupt the lateness much, but it does mean the run was
            #  not the idle one it claims to be - so the show's revision and
            #  its lock are read at both ends, and a difference is reported
            #  rather than silently averaged in.
            start_revision = read_int(server, "/godot/document/revision")
            start_locked = read_int(server, "/godot/document/locked", -1)

            #  THE SERIES, NOT ONLY ITS SUMMARY. A median and a maximum cannot
            #  tell a stall at startup from a stall in the middle of a show,
            #  and that difference is the difference between a wrinkle and a
            #  reason to stop: the first take read 2.57 s of worst-case
            #  lateness with the window open and 16 ms without, which no amount
            #  of thread priority explains. When it happened is the question.
            samples = []
            began = time.monotonic()
            deadline = began + seconds
            next_at = began

            fired_at = began

            while time.monotonic() < deadline:
                samples.append((time.monotonic() - began,
                                read_int(server, "/godot/engine/lateness")))

                if firing and time.monotonic() - fired_at >= 2.0:
                    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go"))
                    fired_at = time.monotonic()

                next_at += 1.0 / SAMPLE_HZ
                pause = next_at - time.monotonic()
                if pause > 0:
                    time.sleep(pause)

            values = [v for _, v in samples]

            reading = {
                "label": label,
                "rate": rate,
                "tick_samples": tick_samples,
                "n": len(samples),
                "series": samples,
                "median": statistics.median(values) if values else 0,
                "p95": (statistics.quantiles(values, n=20)[-1]
                        if len(values) >= 20 else max(values or [0])),
                "max": max(values) if values else 0,
                "at_max": (max(samples, key=lambda pair: pair[1])[0] if samples else 0.0),
                "over_tick": sum(1 for v in values if v > tick_samples),
                "latenessMax": read_int(server, "/godot/engine/latenessMax"),
                "errors": read_int(server, "/godot/engine/errorCount") - start_errors,
                "violations": read_int(server, "/godot/engine/rtViolations") - start_violations,
                "ticks": read_int(server, "/godot/engine/tick"),
                "touched": (read_int(server, "/godot/document/revision") != start_revision
                            or read_int(server, "/godot/document/locked", -1) != start_locked),
            }
    except Exception as exc:
        print(f"   failed while running: {exc}")
        return None

    ms = lambda n: n / reading["rate"] * 1000.0        # noqa: E731
    print(f"   {reading['n']} samples over {seconds:.0f} s, {reading['ticks']} ticks")
    print(f"   lateness   median {reading['median']:>7.0f} samples ({ms(reading['median']):.2f} ms)")
    print(f"              p95    {reading['p95']:>7.0f} samples ({ms(reading['p95']):.2f} ms)")
    print(f"              max    {reading['max']:>7.0f} samples ({ms(reading['max']):.2f} ms)")
    print(f"   latenessMax       {reading['latenessMax']:>7.0f} samples "
          f"({ms(reading['latenessMax']):.2f} ms)")
    print(f"   the worst sample came {reading['at_max']:.1f} s into the run; "
          f"{reading['over_tick']} of {reading['n']} samples exceeded one tick")

    #  Every sample over a tick, with its time, because a handful of stalls
    #  with their timestamps says more than any statistic of them.
    spikes = [(at, v) for at, v in reading["series"] if v > reading["tick_samples"]]

    for at, v in spikes[:12]:
        print(f"      t+{at:6.1f}s  {v:>8.0f} samples ({ms(v):8.1f} ms)")

    if len(spikes) > 12:
        print(f"      … and {len(spikes) - 12} more")
    print(f"   errors {reading['errors']}, rtViolations {reading['violations']}")

    #  AND WHETHER THE CLOCK RAN AT ALL, which is the check this instrument
    #  did not have and needed most. On its first take, condition B reported a
    #  flawless zero for every reading and the script called the whole thing
    #  GREEN - because the window had HUNG before the clock was started, so
    #  every sample was a failed HTTP read falling back to a default of 0. A
    #  measurement that cannot tell "nothing went wrong" from "nothing
    #  happened" is worse than no measurement: it says the thing you hoped for.
    if reading["ticks"] <= 1:
        print("   *** THE CLOCK DID NOT RUN. Every reading above is a default, not a")
        print("       measurement - the engine never ticked, so this condition measured")
        print("       nothing. Do not read the numbers; find out why it did not start.")
        reading["dead"] = True
    else:
        reading["dead"] = False

    if reading["touched"]:
        print("   *** THE SHOW CHANGED DURING THIS CONDITION - somebody used the window, or")
        print("       something else wrote to the engine. This is not an idle run; take it again.")

    return reading


def verdict(a, b):
    """The plan's thresholds, applied to A and B. Says what it sees."""
    print("\n" + "=" * 72)
    print("M25 — the window against the baseline")
    print("=" * 72)

    if a is None or b is None:
        print("  one condition did not run; there is nothing to compare.")
        return

    if a.get("dead") or b.get("dead"):
        print("  ONE CONDITION'S CLOCK NEVER RAN, so there is nothing to compare and no")
        print("  verdict to give. A B that reads zero everywhere is a hung window, not a")
        print("  free one.")
        return

    tick = a["tick_samples"] or 960
    ms = lambda n: n / a["rate"] * 1000.0               # noqa: E731

    for key, name in (("median", "median lateness"), ("p95", "p95 lateness"),
                      ("latenessMax", "worst lateness")):
        delta = b[key] - a[key]
        #  The conditions' own labels, not a hardcoded A and B: this compares
        #  A' against C on a second pass, and a table that said "A" and "B"
        #  there would be reporting the wrong pair by name.
        print(f"  {name:<18} {a['label'].split()[0]:>3} {a[key]:>7.0f}   "
              f"{b['label'].split()[0]:>3} {b[key]:>7.0f}   "
              f"delta {delta:>+8.0f} samples ({ms(delta):+.2f} ms, {delta / tick:+.2f} ticks)")

    worst = b["latenessMax"] - a["latenessMax"]
    print(f"\n  one tick is {tick} samples at {a['rate']} Hz")

    if b["violations"] > a["violations"]:
        print("  RED — the window brought an rtViolation the baseline does not have.")
    elif worst > 2 * tick:
        print("  RED — more than two ticks worse with the window open.")
        print("        The work stops. The remedy is making "
              "elevateCurrentThreadForTicking() real")
        print("        (spatcore rt/RtThreadPriority.h, owed since PR 1.D), not "
              "redesigning the client.")
    elif worst > tick:
        print("  AMBER — one to two ticks worse. Fix before the next view: repaint fewer")
        print("          components, per-row not whole-list, cache laid-out glyphs, drop the")
        print("          timer rate; an OpenGLContext last.")
    else:
        print("  GREEN — within one tick of the baseline.")

    print("\n  NOT MEASURED HERE: `go -> sound`, which wants first_sound.py in A and C, and")
    print("  the client's own repaint times, which want an instrumented build. Conditions C")
    print("  and D want a cue list and a running pane, so they arrive with M4.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=120.0)
    parser.add_argument("--cues", type=int, default=500)
    parser.add_argument("--keep", action="store_true",
                        help="leave the generated bundle on disk")
    parser.add_argument("--firing", action="store_true",
                        help="also take the busy case: a GO every two seconds")
    args = parser.parse_args()

    print(f"M25: {args.cues} cues, {args.seconds:.0f} s per condition, hosted clock.")
    print("A hosted clock rather than a real device: the device is the author's to")
    print("choose, and a take at night should not make a sound. A device take is the")
    print("one to quote.")

    holder = Path(tempfile.mkdtemp(prefix="wfg-m25-"))
    bundle = holder / "large"

    try:
        print("\n" + make_large_show.write(bundle, cues=args.cues, force=True))

        a = measure(bundle, args.seconds, window=False, label="A  no window")
        b = measure(bundle, args.seconds, window=True, label="B  window, idle")
        verdict(a, b)

        if args.firing:
            #  THE BUSY CASE, which is the one a show cares about: a GO every
            #  two seconds, so runs are spawning and ending and the runtime
            #  half of the tree is rebuilt under the window the whole time.
            #  Not the plan's full C - nobody is scrolling - but it is the half
            #  of C that does not need somebody sitting there, and it could not
            #  be taken at all until M3 gave the window a list to draw.
            print("\n\nAND AGAIN WITH THE SHOW RUNNING - a GO every two seconds.")
            a2 = measure(bundle, args.seconds, window=False,
                         label="A' no window, firing", firing=True)
            c = measure(bundle, args.seconds, window=True,
                        label="C  window, firing", firing=True)
            verdict(a2, c)
    finally:
        if not args.keep:
            import shutil
            shutil.rmtree(holder, ignore_errors=True)
        else:
            print(f"\nbundle kept at {bundle}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
