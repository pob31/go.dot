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

"""Writes presets/devices/ssl-live-midi.json: a Solid State Logic Live
console over MIDI on a declared port (namespace draft §57, AFJ; DP.9).

The source is SSL's Live online help, Automation, Input Actions: a scene is
fired by up to three input actions, each a GPIO, MIDI or event trigger; a
MIDI trigger is a Note On, Note Off, Program Change or Control Change, on
every channel or one, with the number the operator sets on the console.
The console has no fixed mapping, so this file offers the shapes an input
action can be set to match: a Program Change per scene number from 1, a
Note On per note number, a Control Change per controller; the operator
sets the scene's input action to the matching message. The console's own
OSC is outbound only (its Generic OSC device) and recalls nothing.

Run from anywhere: `python3 scripts/presets/ssl_live_midi.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "Solid State Logic, Live online help (livehelp.solidstatelogic.com), Automation: Input Actions - up to three per "
    "scene, GPIO, MIDI or Event; the MIDI message types Note On, Note Off, Program Change and Control Change, all "
    "channels or one; External Control - the Generic OSC device is the console's own outbound control",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--scenes", type=int, default=128)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "scene": numbered("/scene", "The scenes, each fired by the input action set to match: a Program Change of its number.",
                          args.scenes, lambda p, n: {
                              "FULL_PATH": p, "DESCRIPTION": "Scene %d." % n, "CONTENTS": {
                                  "recall": midi(command(p + "/recall", "Fire scene %d: Program Change %d on the device's channel - set the "
                                                                        "scene's input action to it." % (n, n), None, "scene.recall"),
                                                 kind="pc", program=n, start=1)}}),
        "note": numbered("/note", "A Note On by number, for a scene whose input action is a note.", 128, lambda p, n: midi(
            command(p, "Note On %d at velocity 127, then its Note Off." % n, None, "go"),
            kind="note", note=n, on=127, off=0, release=True), first=0),
        "cc": numbered("/cc", "A Control Change by number, for a scene whose input action is one.", 128, lambda p, n: midi(
            leaf(p, "i", 3, [127], "Control Change %d with the value given, 0 to 127." % n, span(0, 127)),
            kind="cc", cc=n), first=0),
    }

    tree = root("/", "A Solid State Logic Live console over MIDI on a declared port: scenes fired by the input actions the "
                     "operator sets - a Program Change of the scene's number, a Note On, a Control Change - on the device's "
                     "midiChannel. The console's OSC is its own outbound control and recalls nothing.",
                "ssl-live-midi", "Solid State Logic", "Live L100 to L650 (MIDI)", SOURCES, "scripts/presets/ssl_live_midi.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "ssl-live-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
