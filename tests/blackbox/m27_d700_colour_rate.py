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

"""M27 — how fast the D700 takes colour before it stutters.

AN INSTRUMENT FOR THE BENCH, NOT A GATE: it needs the unit on the desk, and a
person watching it. It is not a ctest and never will be. The figure goes in
namespace draft §16.9 and PRD §6.11, and it revises
`surface::colourMaxPerSecond` in src/wfg/engine/surface/SurfaceProfile.h - ten
writes a second per element today, which is PRD §3.30's "no faster than about
ten times a second", a guess nobody has measured.

WHAT IT DOES. Every one of the 17 RGB elements - the sixteen encoders across
both banks and the master dial - is walked round the colour wheel at a set
rate, for a few seconds, and the operator says whether the rings followed
smoothly. The rate steps up until they do not. The bytes are McuCodec's
exactly (docs/D700_CONTROL_GUIDE.md §4.4): note-on on channels 2, 3 and 4 at the
element's own button note, red, green, then BLUE LAST, which is what makes the
ring refresh.

WHAT IT NEEDS.
  - The D700 under the Mackie preset (Configurator preset 2), on USB.
  - The Asparion Configurator CLOSED: it has been seen overriding colour
    writes (control guide §1.3). The Connector is harmless.
  - No DAW, Max patch or `wfg serve` holding the ports: Windows MIDI is
    exclusive.
  - python-rtmidi:  pip install python-rtmidi

    python tests/blackbox/m27_d700_colour_rate.py [--rates 5,10,20,30,50,100] [--seconds 6]

Nothing is sent but colour notes on channels 2 to 4 - no SysEx at all, so none
of the command bytes the control guide warns about can be reached from here.
"""

from __future__ import annotations

import argparse
import colorsys
import sys
import time

MASTER_NOTE = 0x38


def open_banks():
    """Both output ports, bank 1 first, matched by name as the guide says:
    never by number, which Windows reassigns when another device comes."""
    try:
        import rtmidi
    except ImportError:
        print("m27: needs python-rtmidi (pip install python-rtmidi)", file=sys.stderr)
        sys.exit(2)

    probe = rtmidi.MidiOut()
    names = probe.get_ports()
    del probe

    bank2 = [i for i, name in enumerate(names) if "D 700" in name and "MIDIOUT2" in name]
    bank1 = [i for i, name in enumerate(names) if "D 700" in name and "MIDIOUT2" not in name]

    if not bank1:
        print("m27: no D700 output port on this machine; it has: " + ", ".join(names),
              file=sys.stderr)
        sys.exit(2)

    ports = []

    for index in bank1[:1] + bank2[:1]:
        port = rtmidi.MidiOut()

        try:
            port.open_port(index)
        except Exception as problem:     # rtmidi raises its own SystemError subclasses
            print(f"m27: could not open {names[index]!r} - is something else holding it? "
                  f"({problem})", file=sys.stderr)
            sys.exit(2)

        ports.append((names[index], port))

    return ports


def paint(ports, hue: float) -> int:
    """One colour step on every RGB element: sixteen encoders and the master.
    Each element a little further round the wheel than the one before, so a
    ring that stalls is a ring that visibly falls out of the sweep."""
    sent = 0

    for bank, (_, port) in enumerate(ports):
        notes = [0x20 + n for n in range(8)] + ([MASTER_NOTE] if bank == 0 else [])

        for position, note in enumerate(notes):
            shade = (hue + (bank * 8 + position) / 17.0) % 1.0
            red, green, blue = (int(round(c * 127)) for c in colorsys.hsv_to_rgb(shade, 1.0, 1.0))

            port.send_message([0x91, note, red])
            port.send_message([0x92, note, green])
            port.send_message([0x93, note, blue])       # blue last: the refresh
            sent += 3

    return sent


def dark(ports) -> None:
    for bank, (_, port) in enumerate(ports):
        for note in [0x20 + n for n in range(8)] + ([MASTER_NOTE] if bank == 0 else []):
            port.send_message([0x91, note, 0])
            port.send_message([0x92, note, 0])
            port.send_message([0x93, note, 0])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rates", default="5,10,20,30,50,100",
                        help="writes per second per element, comma-separated")
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--all", action="store_true", help="keep going after the first stutter")
    options = parser.parse_args()

    rates = [float(word) for word in options.rates.split(",") if word.strip()]
    ports = open_banks()

    print("M27 D700 colour rate - on " + " and ".join(name for name, _ in ports))
    print(f"  {17 if len(ports) > 1 else 9} RGB elements, {options.seconds:.0f} s a rate")

    verdicts = []

    try:
        for rate in rates:
            input(f"\n  {rate:g} writes a second per element: press Enter, then watch the rings... ")

            period = 1.0 / rate
            start = time.perf_counter()
            deadline = start + options.seconds
            steps = 0
            messages = 0
            behind = 0.0

            while True:
                now = time.perf_counter()

                if now >= deadline:
                    break

                messages += paint(ports, (steps * 0.02) % 1.0)
                steps += 1

                due = start + steps * period
                wait = due - time.perf_counter()

                if wait > 0:
                    time.sleep(wait)
                else:
                    behind = max(behind, -wait)

            achieved = steps / options.seconds
            answer = input(f"  sent {achieved:.1f} a second ({messages / options.seconds:.0f} "
                           f"messages a second), worst {behind * 1000:.0f} ms behind.\n"
                           "  Did every ring follow smoothly? [y] smooth, [n] stuttered, froze "
                           "or lagged: ").strip().lower()

            smooth = answer.startswith("y") or answer == ""
            verdicts.append((rate, achieved, smooth))

            if not smooth and not options.all:
                break
    finally:
        dark(ports)

    print("\n  rate asked   rate sent   rings")

    for rate, achieved, smooth in verdicts:
        print(f"  {rate:10g}   {achieved:9.1f}   {'smooth' if smooth else 'STUTTERED'}")

    good = [rate for rate, _, smooth in verdicts if smooth]
    bad = [rate for rate, _, smooth in verdicts if not smooth]

    print(f"\n  M27: smooth up to {max(good):g} a second" if good else "\n  M27: nothing was smooth")

    if bad:
        print(f"       first stutter at {min(bad):g} a second")

    print("  (surface::colourMaxPerSecond is 10; the bridge never writes an element faster)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
