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

"""Writes presets/devices/behringer-x32-midi.json: a Behringer X32 or Midas
M32 over MIDI, on a declared port (namespace draft §57, AFJ; DP.9).

The source is the X32's MIDI implementation as its user manual gives it: a
Program Change on MIDI channel 1 loads the scene of that number from the
show memory, once MIDI Scene Recall is enabled in Setup / Remote; the
console's own numbering starts at 0 and the manual's range reads 1 to 100,
which users report as the scene number itself; snippets by Program Change
on channel 2 and cues on channel 3 as the community reports, unverified
here. The console's confirmation pop-up (Setup / Global, Scene Load) has to
be off, or each recall waits for a hand. A console that answers would settle
the numbering; until then the file says scene n sends program n.

Run from anywhere: `python3 scripts/presets/behringer_x32_midi.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, midi, numbered, root, write  # noqa: E402

SOURCES = [
    "Behringer, X32 user manual, MIDI implementation: Program Change 1 to 100 on MIDI channel 1 loads the scene of "
    "its number from the show memory, with MIDI Scene Recall enabled in Setup / Remote and the Scene Load pop-up off",
    "community reports (Show Cue Systems and QLab users' notes on the X32): snippets by Program Change on channel 2, "
    "cues on channel 3, the numbering from 0 - unverified, the file says scene n sends program n",
    "Midas, M32: the same console firmware and the same implementation",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--scenes", type=int, default=100)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    def recall(path, kind, n, channel):
        return {"FULL_PATH": path, "DESCRIPTION": "%s %d." % (kind.capitalize(), n), "CONTENTS": {
            "recall": midi(command(path + "/recall", "Load %s %d: Program Change %d on MIDI channel %d." % (kind, n, n, channel),
                                   None, "scene.recall"),
                           kind="pc", program=n, channel=channel)}}

    contents = {
        "scene": numbered("/scene", "The scenes, Program Change on channel 1.", args.scenes, lambda p, n: recall(p, "scene", n, 1), first=0),
        "snippet": numbered("/snippet", "The snippets, Program Change on channel 2 (as reported).", args.scenes,
                            lambda p, n: recall(p, "snippet", n, 2), first=0),
        "cue": numbered("/cue", "The cues, Program Change on channel 3 (as reported).", args.scenes, lambda p, n: recall(p, "cue", n, 3), first=0),
    }

    tree = root("/", "A Behringer X32 or Midas M32 over MIDI on a declared port: scenes, snippets and cues loaded by Program "
                     "Change on channels 1, 2 and 3. The numbering is the console's, from 0; the recall waits for a hand "
                     "unless the Scene Load pop-up is off.",
                "behringer-x32-midi", "Behringer", "X32, Midas M32 (MIDI)", SOURCES, "scripts/presets/behringer_x32_midi.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "behringer-x32-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
