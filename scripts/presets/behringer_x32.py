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

"""Writes presets/devices/behringer-x32-osc.json: a Behringer X32 or Midas
M32 over OSC (namespace draft §57, AFN).

The source is Patrick-Gilles Maillot's "Unofficial X32/M32 OSC Remote
Protocol" (the public document, V4.x firmware): the console listens on UDP
10023; a strip's mix is under /ch/01-32, /auxin/01-08, /fxrtn/01-08,
/bus/01-16, /mtx/01-06, /main/st, /main/m and /dca/1-8 - fader as a float
0 to 1 on the console's own law, on as 0 or 1, pan, and for the inputs a send
to each bus under /mix/01-16/level and /on; a strip's name and colour under
/config; the mute groups under /config/mute/1-6; and the actions that recall
a scene, a snippet or a cue by number under /-action. /xremote asks the
console to report every change for ten seconds (DP.10).

Run from anywhere: `python3 scripts/presets/behringer_x32.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "Patrick-Gilles Maillot, Unofficial X32/M32 OSC Remote Protocol (the public PDF, 2020 edition for "
    "firmware 4.x): the /ch, /auxin, /fxrtn, /bus, /mtx, /main and /dca mix and config nodes, the "
    "/config/mute groups, the /-action commands, /xremote, the fader law 0..1 as the console's",
    "cross-read, not copied: bitfocus/companion-module-behringer-x32 (MIT) for the actions it offers",
]

COLOURS = list(range(0, 16))


def strip(path, label, sends, has_pan=True, two_digits=True):
    mix = {
        "on": leaf(path + "/mix/on", "i", 3, [1], "%s on, 1, or muted, 0." % label, vals(0, 1), None, "strip.mute"),
        "fader": leaf(path + "/mix/fader", "f", 3, [0.75], "%s's fader, 0 to 1 on the console's own law (0.75 is 0 dB)." % label,
                      span(0.0, 1.0), None, "strip.level"),
    }
    if has_pan:
        mix["pan"] = leaf(path + "/mix/pan", "f", 3, [0.5], "%s's pan, 0 left to 1 right." % label, span(0.0, 1.0))
    if sends:
        for n in range(1, sends + 1):
            name = "%02d" % n
            mix[name] = container(path + "/mix/" + name, "%s's send to bus %d." % (label, n), {
                "on": leaf(path + "/mix/" + name + "/on", "i", 3, [1], "The send on or off.", vals(0, 1)),
                "level": leaf(path + "/mix/" + name + "/level", "f", 3, [0.0], "The send level, 0 to 1 on the fader law.", span(0.0, 1.0), None, "send.level"),
            })
    config = container(path + "/config", "%s's scribble strip." % label, {
        "name": leaf(path + "/config/name", "s", 3, [""], "%s's name, up to 12 characters." % label),
        "color": leaf(path + "/config/color", "i", 3, [1], "%s's colour, 0 to 15 (8 to 15 inverted)." % label, {"VALS": COLOURS}),
    })
    return container(path, label + ".", {"mix": container(path + "/mix", label + "'s mix.", mix), "config": config})


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "ch": numbered("/ch", "The 32 input channels.", 32, lambda p, n: strip(p, "Channel %d" % int(n), 16), names=["%02d" % n for n in range(1, 33)]),
        "auxin": numbered("/auxin", "The 8 aux inputs.", 8, lambda p, n: strip(p, "Aux in %d" % int(n), 16), names=["%02d" % n for n in range(1, 9)]),
        "fxrtn": numbered("/fxrtn", "The 8 effect returns.", 8, lambda p, n: strip(p, "FX return %d" % int(n), 16), names=["%02d" % n for n in range(1, 9)]),
        "bus": numbered("/bus", "The 16 mix buses.", 16, lambda p, n: strip(p, "Bus %d" % int(n), 0), names=["%02d" % n for n in range(1, 17)]),
        "mtx": numbered("/mtx", "The 6 matrices.", 6, lambda p, n: strip(p, "Matrix %d" % int(n), 0, has_pan=False), names=["%02d" % n for n in range(1, 7)]),
        "main": container("/main", "The mains.", {
            "st": strip("/main/st", "Main stereo", 0),
            "m": strip("/main/m", "Main mono", 0, has_pan=False),
        }),
        "dca": numbered("/dca", "The 8 DCAs.", 8, lambda p, n: container(p, "DCA %d." % n, {
            "on": leaf(p + "/on", "i", 3, [1], "DCA %d on, 1, or muted, 0." % n, vals(0, 1), None, "strip.mute"),
            "fader": leaf(p + "/fader", "f", 3, [0.75], "DCA %d's fader, 0 to 1 on the fader law." % n, span(0.0, 1.0), None, "dca.level"),
            "config": container(p + "/config", "DCA %d's scribble strip." % n, {
                "name": leaf(p + "/config/name", "s", 3, [""], "DCA %d's name." % n),
                "color": leaf(p + "/config/color", "i", 3, [1], "DCA %d's colour, 0 to 15." % n, {"VALS": COLOURS}),
            }),
        })),
        "config": container("/config", "The console's configuration.", {
            "mute": numbered("/config/mute", "The 6 mute groups.", 6,
                             lambda p, n: leaf(p, "i", 3, [0], "Mute group %d engaged, 1, or not, 0." % n, vals(0, 1))),
        }),
        "-action": container("/-action", "The console's actions.", {
            "goscene": command("/-action/goscene", "Recall scene 0 to 99.", "i", "scene.recall", span(0, 99)),
            "gosnippet": command("/-action/gosnippet", "Recall snippet 0 to 99.", "i", None, span(0, 99)),
            "gocue": command("/-action/gocue", "Recall cue 0 to 499.", "i", "scene.recall", span(0, 499)),
            "setclock": command("/-action/setclock", "Set the console's clock, as YYYYMMDDhhmmss.", "s"),
        }),
        "xremote": command("/xremote", "Ask the console to report every change for ten seconds; sent again before they run out (DP.10)."),
    }

    tree = root("/", "A Behringer X32 or Midas M32 over OSC on UDP 10023: every strip's mix and scribble, the sends, the mute "
                     "groups, the DCAs, and the scene, snippet and cue recalls. Faders are 0 to 1 on the console's own law.",
                "behringer-x32-osc", "Behringer, Midas", "X32, M32 (OSC)", SOURCES, "scripts/presets/behringer_x32.py",
                transport="udp", port=10023, contents=contents)
    write(tree, "behringer-x32-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
