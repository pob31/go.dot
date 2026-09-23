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

"""M28 — how soon the D700 takes its colours back when nobody is painting them.

AN INSTRUMENT FOR THE BENCH, NOT A GATE: the unit on the desk and a person with
a finger on Enter. The figure goes in namespace draft §16.9 and PRD §6.11, and
it revises `surface::idleColourReassertTicks` in
src/wfg/engine/surface/SurfaceProfile.h - an unchanged colour is written again
every two seconds today, a guess made before anyone timed the firmware.

WHY IT MATTERS. The D700's firmware runs an idle animation over its RGB rings
when nothing drives them (docs/D700_CONTROL_GUIDE.md §4.4), so a host that
paints a strip's colour and stops loses it - and a colour is how the operator
finds a sound on a bank at a glance. The bridge re-asserts every colour on a
timer; this says how short that timer has to be. If the animation never comes
back while the surface is merely idle, say so: the re-assert can then be much
rarer, and the field notes' guess that a configuration setting turns the
animation off becomes the more interesting question.

WHAT IT DOES. Paints every RGB element one colour, stops, and times how long
until the operator sees the animation take them back. Several trials; the
median is the figure.

WHAT IT NEEDS: what M27 needs - the unit under Mackie, the Configurator
closed, nothing else holding the ports, and python-rtmidi.

    python tests/blackbox/m28_d700_idle_resume.py [--trials 3] [--give-up 120]
"""

from __future__ import annotations

import argparse
import statistics
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import m27_d700_colour_rate as m27                 # noqa: E402


def paint_still(ports, red: int, green: int, blue: int) -> None:
    for bank, (_, port) in enumerate(ports):
        for note in [0x20 + n for n in range(8)] + ([m27.MASTER_NOTE] if bank == 0 else []):
            port.send_message([0x91, note, red])
            port.send_message([0x92, note, green])
            port.send_message([0x93, note, blue])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--give-up", type=float, default=120.0,
                        help="seconds after which a trial counts as 'never came back'")
    options = parser.parse_args()

    ports = m27.open_banks()
    print("M28 D700 idle resume - on " + " and ".join(name for name, _ in ports))

    times = []
    never = 0
    colours = [(0, 127, 0), (127, 0, 64), (0, 64, 127), (127, 96, 0)]

    try:
        for trial in range(options.trials):
            input(f"\n  trial {trial + 1}: press Enter to paint every ring, then press Enter "
                  "AGAIN the moment the animation takes them back... ")

            paint_still(ports, *colours[trial % len(colours)])
            painted = time.perf_counter()

            answered = threading.Event()

            def wait_for_enter() -> None:
                input()
                answered.set()

            threading.Thread(target=wait_for_enter, daemon=True).start()

            if answered.wait(options.give_up):
                seconds = time.perf_counter() - painted
                times.append(seconds)
                print(f"  back after {seconds:.2f} s")
            else:
                never += 1
                print(f"  not back after {options.give_up:.0f} s - counted as never "
                      "(press Enter to go on)")
                answered.wait()
    finally:
        m27.dark(ports)

    print()

    if times:
        print(f"  M28: the animation returns after {statistics.median(times):.2f} s "
              f"(median of {len(times)}; min {min(times):.2f}, max {max(times):.2f})")
        print(f"       the bridge re-asserts every 2.00 s "
              f"({'in time' if min(times) > 2.0 else 'TOO SLOW - shorten idleColourReassertTicks'})")

    if never:
        print(f"  {never} trial(s) never came back within {options.give_up:.0f} s")

    return 0


if __name__ == "__main__":
    sys.exit(main())
