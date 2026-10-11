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

"""Writes presets/devices/holophonix-osc.json: a HOLOPHONIX processor over
OSC (namespace draft §57, AFN).

The source is HOLOPHONIX's documentation: the OSC introduction (external
controls: the processor takes OSC on UDP 4003 by default, addresses are
/{element}/{index}/{parameter}, degrees are taken as -180..180 or 0..360,
a cartesian position as x, y, z or xyz and a polar one as azim, elev, dist
or aed) and the OSC specification of a mono source, /track: its general
parameters, equalizer, dynamics, buses A-H, LFE, delay, reverb, direct and
early sends. Tracks are written for --tracks (64) sources. HOLOPHONIX 2.4
speaks ADM-OSC too: the adm-osc preset.

Run from anywhere: `python3 scripts/presets/holophonix.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "HOLOPHONIX documentation, External controls > Open Sound Control (docs.holophonix.xyz/docs/"
    "external_controls/osc): port 4003, the /{element}/{index}/{parameter} grammar, degrees, xyz and aed, /get",
    "HOLOPHONIX documentation, OSC specifications > Sources > Virtual sources > Track (docs.holophonix.xyz/"
    "docs/osc_specifications/sources/virtual_sources/track): every row of the general, equalizer, dynamics, "
    "levels, bus, LFE, delay, reverb, direct, early and 3D view tables",
]


def db(lo, hi):
    return span(float(lo), float(hi))


def track(path, i):
    def f(name, lo, hi, default, description, unit=None, role=None):
        return leaf(path + "/" + name, "f", 3, [float(default)], description, db(lo, hi), unit, role)

    def b(name, description, default=False):
        return leaf(path + "/" + name, "T", 3, [default], description)

    filters = numbered(path + "/equalizer/filter", "The equalizer's filters.", 8,
                       lambda p, n: container(p, "Filter %d." % n, {
                           "active": leaf(p + "/active", "T", 3, [False], "Filter %d active." % n),
                           "freq": leaf(p + "/freq", "f", 3, [1000.0], "Filter %d's frequency." % n, span(30.0, 22000.0), "Hz"),
                           "order": leaf(p + "/order", "f", 3, [2.0], "Filter %d's order." % n, span(2.0, 20.0)),
                           "gain": leaf(p + "/gain", "f", 3, [0.0], "Filter %d's gain." % n, span(-30.0, 30.0), "dB"),
                           "q": leaf(p + "/q", "f", 3, [1.0], "Filter %d's Q." % n, span(0.25, 100.0)),
                       }))

    def bus(p, letter):
        return container(p, "Bus %s." % letter, {
            "destination": leaf(p + "/destination", "s", 3, [""], "The bus this send goes to."),
            "mute": leaf(p + "/mute", "T", 3, [False], "The send to bus %s muted." % letter),
            "gain": leaf(p + "/gain", "f", 3, [0.0], "The send to bus %s, in decibels." % letter, db(-80, 6), "dB", "send.level"),
            "delay": leaf(p + "/delay", "f", 3, [0.0], "The send's delay, in milliseconds.", span(0.0, 5000.0), "ms"),
        })

    def gains(p, what):
        return {
            "mute": leaf(p + "/mute", "T", 3, [False], "The %s muted." % what),
            "linkedtodistance": leaf(p + "/linkedtodistance", "T", 3, [True], "The %s gain follows the distance." % what),
            "gain": leaf(p + "/gain", "f", 3, [0.0], "The %s gain, in decibels." % what, db(-80, 30), "dB"),
            "freq": container(p + "/freq", "The %s's crossover frequencies." % what, {
                "low": leaf(p + "/freq/low", "f", 3, [200.0], "The low crossover.", span(20.0, 20000.0), "Hz"),
                "high": leaf(p + "/freq/high", "f", 3, [2000.0], "The high crossover.", span(20.0, 20000.0), "Hz"),
            }),
            "filter": container(p + "/filter", "The %s's filter." % what, {
                "bypass": leaf(p + "/filter/bypass", "T", 3, [False], "The filter bypassed."),
            }),
        }

    direct = container(path + "/direct", "The direct sound.", gains(path + "/direct", "direct sound"))
    direct["CONTENTS"]["gain"] = container(path + "/direct/gain", "The direct gain.", {
        "low": leaf(path + "/direct/gain/low", "f", 3, [0.0], "The low band's gain.", db(-30, 30), "dB"),
        "med": leaf(path + "/direct/gain/med", "f", 3, [0.0], "The mid band's gain.", db(-30, 30), "dB"),
        "high": leaf(path + "/direct/gain/high", "f", 3, [0.0], "The high band's gain.", db(-30, 30), "dB"),
        "offset": leaf(path + "/direct/gain/offset", "f", 3, [0.0], "The gain offset.", db(-80, 30), "dB"),
    })
    direct["CONTENTS"]["gain"].update({"TYPE": "f", "ACCESS": 3, "VALUE": [0.0], "RANGE": [db(-80, 30)], "UNIT": ["dB"]})

    early = container(path + "/early", "The early reflections.", gains(path + "/early", "early reflections"))
    early["CONTENTS"]["width"] = leaf(path + "/early/width", "f", 3, [60.0], "The early reflections' width, in degrees.", span(0.0, 180.0), "deg")
    early["CONTENTS"]["gain"] = container(path + "/early/gain", "The early gain.", {
        "low": leaf(path + "/early/gain/low", "f", 3, [0.0], "The low band's gain.", db(-30, 30), "dB"),
        "med": leaf(path + "/early/gain/med", "f", 3, [0.0], "The mid band's gain.", db(-30, 30), "dB"),
        "high": leaf(path + "/early/gain/high", "f", 3, [0.0], "The high band's gain.", db(-30, 30), "dB"),
        "offset": leaf(path + "/early/gain/offset", "f", 3, [0.0], "The gain offset.", db(-80, 30), "dB"),
    })
    early["CONTENTS"]["gain"].update({"TYPE": "f", "ACCESS": 3, "VALUE": [0.0], "RANGE": [db(-80, 30)], "UNIT": ["dB"]})

    contents = {
        "name": leaf(path + "/name", "s", 3, [""], "The track's name."),
        "color": leaf(path + "/color", "ffff", 3, [1.0, 1.0, 1.0, 1.0], "The track's colour, red, green, blue and alpha, 0 to 1.",
                      [span(0.0, 1.0)] * 4),
        "lock": b("lock", "The track locked."),
        "trim": f("trim", -80, 30, 0.0, "Trim, in decibels.", "dB"),
        "gain": f("gain", -60, 12, 0.0, "Gain, in decibels.", "dB", "object.gain"),
        "mute": leaf(path + "/mute", "i", 3, [0], "Muted: 1, or 0; 2 is read back only.", vals(0, 1, 2), None, "object.mute"),
        "azim": f("azim", -180, 180, 0.0, "Azimuth, in degrees; 0 to 360 is taken too.", "deg"),
        "elev": f("elev", -90, 90, 0.0, "Elevation, in degrees.", "deg"),
        "dist": f("dist", 0.1, 500, 1.0, "Distance, in metres.", "m"),
        "aed": leaf(path + "/aed", "fff", 3, [0.0, 0.0, 1.0], "Azimuth, elevation and distance in one message.",
                    [span(-180.0, 180.0), span(-90.0, 90.0), span(0.1, 500.0)], None, "object.position"),
        "xyz": leaf(path + "/xyz", "fff", 3, [0.0, 1.0, 0.0], "X, Y and Z in metres, in one message.", None, "m", "object.position"),
        "x": leaf(path + "/x", "f", 3, [0.0], "X, in metres.", None, "m"),
        "y": leaf(path + "/y", "f", 3, [1.0], "Y, in metres.", None, "m"),
        "z": leaf(path + "/z", "f", 3, [0.0], "Z, in metres.", None, "m"),
        "solo": b("solo", "Soloed."),
        "monitor": leaf(path + "/monitor", "s", 3, ["off"], "Monitoring: off or pfl.", {"VALS": ["off", "pfl"]}),
        "doppler": b("doppler", "The Doppler effect on."),
        "air": b("air", "Air absorption on."),
        "phaseinvert": b("phaseinvert", "Phase inverted."),
        "preOrPost": leaf(path + "/preOrPost", "s", 3, ["post"], "Sends pre or post fader.", {"VALS": ["pre", "post"]}),
        "admObjectNumber": leaf(path + "/admObjectNumber", "i", 3, [-1], "The ADM object number this track answers to; -1 for none.", span(-1, 256)),
        "equalizer": container(path + "/equalizer", "The equalizer.", {
            "bypass": leaf(path + "/equalizer/bypass", "T", 3, [False], "The equalizer bypassed."),
            "gain": leaf(path + "/equalizer/gain", "f", 3, [0.0], "The equalizer's gain.", db(-20, 20), "dB"),
            "filter": filters,
        }),
        "dynamics": container(path + "/dynamics", "The dynamics.", {
            "bypass": leaf(path + "/dynamics/bypass", "T", 3, [False], "The dynamics bypassed."),
            "attack": leaf(path + "/dynamics/attack", "f", 3, [10.0], "Attack, in milliseconds.", span(0.01, 3000.0), "ms"),
            "release": leaf(path + "/dynamics/release", "f", 3, [100.0], "Release, in milliseconds.", span(0.0, 5000.0), "ms"),
            "lookahead": leaf(path + "/dynamics/lookahead", "f", 3, [0.0], "Lookahead, in milliseconds.", span(0.0, 50.0), "ms"),
            "makeup": leaf(path + "/dynamics/makeup", "f", 3, [0.0], "Make-up gain.", db(-40, 40), "dB"),
            "link": leaf(path + "/dynamics/link", "s", 3, ["link all"], "How channels are linked.", {"VALS": ["multi mono", "link all", "link to 1st"]}),
            "compressor": container(path + "/dynamics/compressor", "The compressor.", {
                "threshold": leaf(path + "/dynamics/compressor/threshold", "f", 3, [0.0], "Threshold.", db(-120, 20), "dB"),
                "ratio": leaf(path + "/dynamics/compressor/ratio", "f", 3, [1.0], "Ratio.", span(1.0, 100.0)),
                "knee": leaf(path + "/dynamics/compressor/knee", "f", 3, [0.0], "Knee.", db(0, 30), "dB"),
            }),
            "expander": container(path + "/dynamics/expander", "The expander.", {
                "threshold": leaf(path + "/dynamics/expander/threshold", "f", 3, [-120.0], "Threshold.", db(-120, 20), "dB"),
                "ratio": leaf(path + "/dynamics/expander/ratio", "f", 3, [1.0], "Ratio.", span(0.009, 10.0)),
            }),
        }),
        "bus": numbered(path + "/bus", "The sends to buses A to H.", 8, bus, names=list("ABCDEFGH")),
        "lfe": container(path + "/lfe", "The LFE send.", {
            "send": leaf(path + "/lfe/send", "f", 3, [-80.0], "The LFE send, in decibels.", db(-80, 30), "dB"),
            "mute": leaf(path + "/lfe/mute", "T", 3, [False], "The LFE send muted."),
        }),
        "delay": leaf(path + "/delay", "f", 3, [0.0], "The track's delay, in milliseconds.", span(0.0, 5000.0), "ms"),
        "reverb": container(path + "/reverb", "The reverb send.", {
            "mute": leaf(path + "/reverb/mute", "T", 3, [False], "The reverb send muted."),
            "linkedtodistance": leaf(path + "/reverb/linkedtodistance", "T", 3, [True], "The reverb send follows the distance."),
            "send": leaf(path + "/reverb/send", "f", 3, [0.0], "The reverb send, in decibels.", db(-80, 30), "dB", "send.level"),
        }),
        "direct": direct,
        "early": early,
    }
    contents["delay"] = container(path + "/delay", "The track's delay.", {
        "linkedtodistance": leaf(path + "/delay/linkedtodistance", "T", 3, [True], "The delay follows the distance."),
    })
    contents["delay"].update({"TYPE": "f", "ACCESS": 3, "VALUE": [0.0], "RANGE": [span(0.0, 5000.0)], "UNIT": ["ms"]})
    return container(path, "Track %d, a mono source." % i, contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tracks", type=int, default=64, help="how many mono sources to describe (64)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    tree = root("/", "A HOLOPHONIX processor over OSC on UDP 4003: %d mono sources under /track with their position, gain, "
                     "sends, equalizer and dynamics. Stereo and multichannel sources, buses and the master are not described: "
                     "a cue to them is refused until a later version of this preset." % args.tracks,
                "holophonix-osc", "HOLOPHONIX", "HOLOPHONIX processor (OSC)", SOURCES, "scripts/presets/holophonix.py",
                transport="udp", port=4003, readback="get", get="/get {address}",
                contents={"track": numbered("/track", "The mono sources.", args.tracks, track)})
    write(tree, "holophonix-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
