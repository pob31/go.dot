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

"""Writes presets/devices/midas-hd96-midi.json: a Midas HD96-24 over MIDI
on a declared port (namespace draft §57, AFJ; DP.9).

The HD96-24 publishes no network control protocol - its Ethernet carries
audio - and its MIDI implementation, like the Midas PRO series before it,
recalls scenes by Program Change on the console's MIDI channel, with a Bank
Select for scenes past 128 as the PRO2's MIDI notes describe. No HD96 chart
was read when this was written: the file says scene n sends bank and
program for n from 1, and is unverified until a console has answered.

Run from anywhere: `python3 scripts/presets/midas_hd96_midi.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "Midas, HD96-24 user guide: no network control protocol, MIDI in and out on the console; the scene recall by "
    "Program Change assumed from the Midas PRO series' MIDI notes (a Program Change per recalled scene, Bank Select "
    "for scenes past 128) - no HD96 implementation chart was read, unverified until a console has answered",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--scenes", type=int, default=256)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "scene": numbered("/scene", "The scenes.", args.scenes, lambda p, n: {
            "FULL_PATH": p, "DESCRIPTION": "Scene %d." % n, "CONTENTS": {
                "recall": midi(command(p + "/recall", "Recall scene %d: Bank Select then Program Change for %d." % (n, n), None, "scene.recall"),
                               kind="pc", program=n, banked=True, start=1)}}),
        "recall": midi(command("/recall", "Recall a scene by number, 1 to %d." % args.scenes, "i", "scene.recall", span(1, args.scenes)),
                       kind="pc", banked=True, start=1),
    }

    tree = root("/", "A Midas HD96-24 over MIDI on a declared port: scenes recalled by Bank Select and Program Change on the "
                     "console's MIDI channel, the device's midiChannel row. Assumed from the Midas PRO series' MIDI, unverified.",
                "midas-hd96-midi", "Midas", "HD96-24 (MIDI)", SOURCES, "scripts/presets/midas_hd96_midi.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "midas-hd96-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
