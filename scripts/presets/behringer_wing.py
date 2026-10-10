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

"""Writes presets/devices/behringer-wing-osc.json: a Behringer WING over OSC
(namespace draft §57, AFN).

The source is Patrick-Gilles Maillot's "OSC Documentation for WING", which
Behringer authorises him to publish: the console listens on UDP 2223 and
answers to the port a datagram came from; an OSC address is the path through
the console's JSON data, so /ch/1/fdr is channel 1's fader, a float in
decibels with -144 for off; mute is 0 or 1, pan -100 to 100, width -150 to
150. The strips are 40 channels, 8 auxes, 16 buses, 4 mains, 8 matrices and
8 DCAs, each with its name, colour, icon, mute and fader, a channel or aux
with its 4 main assignments and 16 sends, a bus with its sends to the
matrices, and the 8 mute groups. The channels' processing (filters, EQ,
gate, dynamics, inserts) and the console's library actions - scene recall
among them - are not in the pages read and are left out: a cue to them is
refused until a later version of this preset describes them.

Run from anywhere: `python3 scripts/presets/behringer_wing.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "Patrick-Gilles Maillot, OSC Documentation for WING, V 0.3.2 (wing-docs.com/pdf/OSC_Documentation.pdf, "
    "authorised by Behringer): OSC Remote Protocol (port 2223, the reply to the caller's port, the address as "
    "the JSON path, /ch/1/fdr and /ch/1/mute as read and written), and the appendix's JSON data structure for "
    "ch, aux, bus, main, mtx, dca and mgrp",
]

DB = span(-144.0, 10.0)


def strip(path, label, mains=0, sends=0, send_label="bus", has_pan=True):
    contents = {
        "name": leaf(path + "/name", "s", 3, [""], "%s's name." % label),
        "col": leaf(path + "/col", "i", 3, [1], "%s's colour, 1 to 12." % label, {"VALS": list(range(1, 13))}),
        "icon": leaf(path + "/icon", "i", 3, [0], "%s's icon, by number." % label, span(0, 500)),
        "led": leaf(path + "/led", "i", 3, [0], "%s's LED lit, 1, or not, 0." % label, vals(0, 1)),
        "mute": leaf(path + "/mute", "i", 3, [0], "%s muted, 1, or not, 0." % label, vals(0, 1), None, "strip.mute"),
        "fdr": leaf(path + "/fdr", "f", 3, [-144.0], "%s's fader, in decibels; -144 is off." % label, DB, "dB", "strip.level"),
    }
    if has_pan:
        contents["pan"] = leaf(path + "/pan", "f", 3, [0.0], "%s's pan, -100 left to 100 right." % label, span(-100.0, 100.0))
        contents["wid"] = leaf(path + "/wid", "f", 3, [100.0], "%s's stereo width, -150 to 150." % label, span(-150.0, 150.0))
    if mains:
        contents["main"] = numbered(path + "/main", "%s's assignment to the mains." % label, mains,
                                    lambda p, n: container(p, "Main %d." % n, {
                                        "on": leaf(p + "/on", "i", 3, [1], "Sent to main %d, 1, or not, 0." % n, vals(0, 1)),
                                        "lvl": leaf(p + "/lvl", "f", 3, [0.0], "The level to main %d, in decibels." % n, DB, "dB"),
                                    }))
    if sends:
        contents["send"] = numbered(path + "/send", "%s's sends." % label, sends,
                                    lambda p, n: container(p, "Send to %s %d." % (send_label, n), {
                                        "on": leaf(p + "/on", "i", 3, [0], "The send on, 1, or off, 0.", vals(0, 1)),
                                        "lvl": leaf(p + "/lvl", "f", 3, [-144.0], "The send level, in decibels.", DB, "dB", "send.level"),
                                        "pan": leaf(p + "/pan", "f", 3, [0.0], "The send's pan, -100 to 100.", span(-100.0, 100.0)),
                                    }))
    return container(path, label + ".", contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "ch": numbered("/ch", "The 40 input channels.", 40, lambda p, n: strip(p, "Channel %d" % n, 4, 16)),
        "aux": numbered("/aux", "The 8 auxes.", 8, lambda p, n: strip(p, "Aux %d" % n, 4, 16)),
        "bus": numbered("/bus", "The 16 buses.", 16, lambda p, n: strip(p, "Bus %d" % n, 4, 8, "matrix")),
        "main": numbered("/main", "The 4 mains.", 4, lambda p, n: strip(p, "Main %d" % n, 0, 8, "matrix")),
        "mtx": numbered("/mtx", "The 8 matrices.", 8, lambda p, n: strip(p, "Matrix %d" % n)),
        "dca": numbered("/dca", "The 8 DCAs.", 8, lambda p, n: strip(p, "DCA %d" % n, has_pan=False)),
        "mgrp": numbered("/mgrp", "The 8 mute groups.", 8, lambda p, n: container(p, "Mute group %d." % n, {
            "name": leaf(p + "/name", "s", 3, [""], "Mute group %d's name." % n),
            "mute": leaf(p + "/mute", "i", 3, [0], "Mute group %d engaged, 1, or not, 0." % n, vals(0, 1)),
        })),
    }
    for n in range(1, 9):
        contents["dca"]["CONTENTS"][str(n)]["CONTENTS"]["fdr"]["GODOT"] = {"ROLE": "dca.level"}

    tree = root("/", "A Behringer WING over OSC on UDP 2223: every strip's name, colour, mute and fader in decibels, the channels' "
                     "and auxes' mains and sends, the buses' and mains' sends to the matrices, the DCAs and the mute groups. "
                     "Processing and the library actions are not described yet.",
                "behringer-wing-osc", "Behringer", "WING (OSC)", SOURCES, "scripts/presets/behringer_wing.py",
                transport="udp", port=2223, contents=contents)
    write(tree, "behringer-wing-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
