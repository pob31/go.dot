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

"""Writes presets/devices/yamaha-midi.json: a Yamaha CL, QL, TF, DM3, DM7 or
RIVAGE PM over MIDI, on a declared port (namespace draft §57, AFJ; DP.9).

The source is Yamaha's reference manuals' MIDI chapters: a scene is recalled
by a Program Change on the console's MIDI channel through its Program
Change table (Setup / MIDI / Program Change on a CL or QL, the MIDI screen
of a DM3 or DM7, the Program Change assignment of a RIVAGE PM), whose
factory assignment maps program n to scene n, 1 to 128, in Single mode on
one channel; a bank where the console offers one extends it. The device's
midiChannel row is that channel; scene n here sends program n from 1, and a
console whose table was changed is followed by regenerating the file or
editing the cue's value.

Run from anywhere: `python3 scripts/presets/yamaha_midi.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "Yamaha, CL/QL series reference manual, MIDI: Program Change table (Setup / MIDI / Program Change), Single and "
    "Multi modes, the factory assignment program n to scene n",
    "Yamaha, RIVAGE PM series reference manual, Using Program Changes to recall scenes (manual.yamaha.com "
    "5498118027): Single mode on one MIDI channel, Multi mode across channels",
    "Yamaha, DM3 reference manual, MIDI (Program Change) screen (manual.yamaha.com 6296282763)",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--scenes", type=int, default=128)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "scene": numbered("/scene", "The scenes, by the console's Program Change table.", args.scenes, lambda p, n: {
            "FULL_PATH": p, "DESCRIPTION": "Scene %d." % n, "CONTENTS": {
                "recall": midi(command(p + "/recall", "Recall scene %d: Program Change %d on the console's MIDI channel, "
                                                      "through its Program Change table." % (n, n), None, "scene.recall"),
                               kind="pc", program=n, start=1)}}),
        "recall": midi(command("/recall", "Recall a scene by number, 1 to %d, as one Program Change." % args.scenes, "i",
                               "scene.recall", span(1, args.scenes)),
                       kind="pc", start=1),
    }

    tree = root("/", "A Yamaha CL, QL, TF, DM3, DM7 or RIVAGE PM over MIDI on a declared port: scenes recalled by Program "
                     "Change through the console's own table, program n for scene n as the factory sets it. The device's "
                     "midiChannel row is the console's MIDI channel.",
                "yamaha-midi", "Yamaha", "CL, QL, TF, DM3, DM7, RIVAGE PM (MIDI)", SOURCES, "scripts/presets/yamaha_midi.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "yamaha-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
