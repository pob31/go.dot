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

"""Writes presets/devices/yamaha-osc.json: a Yamaha DM7, DM3 or RIVAGE PM
over Yamaha's own OSC (namespace draft §57, AFN).

The source is Yamaha's public "DM7 Series OSC Specifications" V1.1.0 (the
DM3's and RIVAGE PM's are the same grammar on their own parameter sets): the
console listens on UDP 49900 at its "For Mixer Control" address; a parameter
is set at /yosc:req/set/MIXER:Current/<parameter>/<X>/<Y> with an integer or
string value - X the channel number from 1, Y a second index where the
parameter has one (the mix a send goes to, a PEQ band), absent otherwise; a
level is an integer of hundredths of a decibel, -32768 for -infinity and
1000 for +10 dB; a frequency is tenths of a hertz. A scene is recalled with
/yosc:req/ssrecallt_ex, the list (scene_a or scene_b) and the number as
"x.xx", or stepped with /yosc:req/event and MIXER:Lib/Scene/RecallInc or
RecallDec. The YAMAHA_PARAMETERS table here is the same parameter identifier
list RCP speaks (yamaha-rcp, DP.7). Channel counts are the DM7's by default
and --inputs, --mixes, --matrices, --dcas, --mute-groups write a file for the
desk at hand; the sends carry their level and on switch, the pan and pre/post
rows too with --send-pans.

Run from anywhere: `python3 scripts/presets/yamaha_osc.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, put, root, span, vals, write  # noqa: E402

SOURCES = [
    "Yamaha, DM7 Series OSC Specifications, Version 1.1.0 (July 2025), sections 1.3 (UDP 49900), 1.4.1 (the "
    "address pattern, the scene recall and inc/dec patterns), 2.1 (the parameter list with X, Y, type, min, max, "
    "scaling and unit) and 2.2 (Table 1 the fader law, Table 2 pan, Table 3 colours, Table 4 icons)",
    "Yamaha, DM3 OSC Specifications V1.0.0 and RIVAGE PM OSC Specifications V1.0.2: the same grammar on UDP 49900",
]

LEVEL = ("i", span(-32768, 1000), [0], "dB x 100: -32768 is -infinity, 0 is 0 dB, 1000 is +10 dB (Table 1).")
ONOFF = ("i", vals(0, 1), [0], "0 off, 1 on.")
PAN = ("i", span(-63, 63), [0], "-63 left to 63 right (Table 2).")
FREQ = ("i", span(200, 200000), [1000], "tenths of a hertz: 200 is 20 Hz, 200000 is 20 kHz.")
NAME = ("s", None, [""], "up to 8 characters.")
COLOUR = ("s", None, [""], "a colour name, Table 3.")
ICON = ("s", None, [""], "an icon name, Table 4.")

# (parameter id after MIXER:Current/, Y kind or None, the value spec, the role or None, the description)
# Y kinds: "mix" (a mix send), "mtrx" (a matrix send), "band" (a PEQ band), "dca" (a DCA assign), "st" (a stereo)
STRIP_ROWS = {
    "InCh": [
        ("Fader/Level", None, LEVEL, "strip.level", "The input channel's fader"),
        ("Fader/On", None, ONOFF, "strip.mute", "The input channel on"),
        ("ToSt/Pan", None, PAN, None, "The pan to stereo"),
        ("ToSt/On", "st", ONOFF, None, "The send to the stereo bus on"),
        ("Port/HA/Gain", None, ("i", span(-600, 6600), [0], "dB x 100, -6 to 66 dB (Table 5)."), None, "The head amp gain"),
        ("Label/Name", None, NAME, None, "The name"),
        ("Label/Color", None, COLOUR, None, "The colour"),
        ("Label/Icon", None, ICON, None, "The icon"),
        ("HPF/On", None, ONOFF, None, "The high-pass filter on"),
        ("HPF/Freq", None, FREQ, None, "The high-pass frequency"),
        ("LPF/On", None, ONOFF, None, "The low-pass filter on"),
        ("LPF/Freq", None, FREQ, None, "The low-pass frequency"),
        ("PEQ/On", None, ONOFF, None, "The PEQ on"),
        ("PEQ/Band/Bypass", "band", ONOFF, None, "The band bypassed"),
        ("PEQ/Band/Freq", "band", FREQ, None, "The band's frequency"),
        ("PEQ/Band/Gain", "band", ("i", span(-1800, 1800), [0], "dB x 100, -18 to +18 dB."), None, "The band's gain"),
        ("PEQ/Band/Q", "band", ("i", span(10, 1600), [100], "Q x 100."), None, "The band's Q"),
        ("ToMix/Level", "mix", LEVEL, "send.level", "The send to the mix"),
        ("ToMix/On", "mix", ONOFF, None, "The send to the mix on"),
        ("ToMtrx/Level", "mtrx", LEVEL, "send.level", "The send to the matrix"),
        ("ToMtrx/On", "mtrx", ONOFF, None, "The send to the matrix on"),
        ("DCA/Assign", "dca", ONOFF, None, "Assigned to the DCA"),
    ],
    "Mix": [
        ("Fader/Level", None, LEVEL, "strip.level", "The mix's fader"),
        ("Fader/On", None, ONOFF, "strip.mute", "The mix on"),
        ("ToSt/Pan", None, PAN, None, "The pan to stereo"),
        ("ToSt/On", "st", ONOFF, None, "The send to the stereo bus on"),
        ("Label/Name", None, NAME, None, "The name"),
        ("Label/Color", None, COLOUR, None, "The colour"),
        ("Label/Icon", None, ICON, None, "The icon"),
        ("ToMtrx/Level", "mtrx", LEVEL, "send.level", "The send to the matrix"),
        ("ToMtrx/On", "mtrx", ONOFF, None, "The send to the matrix on"),
        ("DCA/Assign", "dca", ONOFF, None, "Assigned to the DCA"),
    ],
    "St": [
        ("Fader/Level", None, LEVEL, "master.level", "The stereo bus's fader"),
        ("Fader/On", None, ONOFF, "strip.mute", "The stereo bus on"),
        ("Out/Balance", None, PAN, None, "The output balance"),
        ("Label/Name", None, NAME, None, "The name"),
        ("ToMtrx/Level", "mtrx", LEVEL, "send.level", "The send to the matrix"),
        ("ToMtrx/On", "mtrx", ONOFF, None, "The send to the matrix on"),
    ],
    "Mtrx": [
        ("Fader/Level", None, LEVEL, "strip.level", "The matrix's fader"),
        ("Fader/On", None, ONOFF, "strip.mute", "The matrix on"),
        ("Label/Name", None, NAME, None, "The name"),
        ("Out/Balance", None, PAN, None, "The output balance"),
    ],
    "DCA": [
        ("Fader/Level", None, LEVEL, "dca.level", "The DCA's fader"),
        ("Fader/On", None, ONOFF, "strip.mute", "The DCA on"),
        ("Label/Name", None, NAME, None, "The name"),
        ("Label/Color", None, COLOUR, None, "The colour"),
    ],
}

SEND_PAN_ROWS = {
    "InCh": [
        ("ToMix/Pan", "mix", PAN, None, "The send's pan"),
        ("ToMix/PrePost", "mix", ONOFF, None, "The send post fader, 1, or pre, 0"),
        ("ToMtrx/Pan", "mtrx", PAN, None, "The send's pan"),
        ("ToMtrx/PrePost", "mtrx", ONOFF, None, "The send post fader, 1, or pre, 0"),
    ],
}

STRIP_NAMES = {"InCh": "Input channel", "Mix": "Mix", "St": "Stereo bus", "Mtrx": "Matrix", "DCA": "DCA"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inputs", type=int, default=72, help="input channels (72; the DM7 has 120, the DM3 22)")
    parser.add_argument("--mixes", type=int, default=24, help="mix buses (24; the DM7 has 48, the DM3 6)")
    parser.add_argument("--matrices", type=int, default=8, help="matrices (8; the DM7 has 12)")
    parser.add_argument("--stereo", type=int, default=2, help="stereo buses (2)")
    parser.add_argument("--dcas", type=int, default=24, help="DCA groups (24)")
    parser.add_argument("--mute-groups", type=int, default=12, help="mute groups (12)")
    parser.add_argument("--bands", type=int, default=4, help="PEQ bands (4; the DM7 has 8)")
    parser.add_argument("--send-pans", action="store_true", help="write the sends' pan and pre/post rows too")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    counts = {"InCh": args.inputs, "Mix": args.mixes, "St": args.stereo, "Mtrx": args.matrices, "DCA": args.dcas}
    y_counts = {"mix": args.mixes, "mtrx": args.matrices, "band": args.bands, "dca": args.dcas, "st": args.stereo}

    tree = root("/", "A Yamaha DM7, DM3 or RIVAGE PM over Yamaha's own OSC on UDP 49900: every strip's fader, on, pan, "
                             "name, filters and PEQ, the sends with their level and on, the DCA assigns, the mute groups, and "
                             "the scene recalls. Levels are hundredths of a decibel. %d inputs, %d mixes, %d matrices, %d DCAs."
                             % (args.inputs, args.mixes, args.matrices, args.dcas),
                "yamaha-osc", "Yamaha", "DM7, DM3, RIVAGE PM (OSC)", SOURCES, "scripts/presets/yamaha_osc.py",
                transport="udp", port=49900)

    count = 0
    for strip, rows in STRIP_ROWS.items():
        rows = list(rows) + (SEND_PAN_ROWS.get(strip, []) if args.send_pans else [])
        for x in range(1, counts[strip] + 1):
            for parameter, y_kind, (tag, rng, value, how), role, what in rows:
                base = "/yosc:req/set/MIXER:Current/%s/%s/%d" % (strip, parameter, x)
                label = "%s %d: %s" % (STRIP_NAMES[strip], x, what.lower())
                if y_kind is None:
                    put(tree, base, leaf(base, tag, 3, value, "%s; %s" % (label, how), rng, None, role))
                    count += 1
                else:
                    for y in range(1, y_counts[y_kind] + 1):
                        path = "%s/%d" % (base, y)
                        put(tree, path, leaf(path, tag, 3, value, "%s %d; %s" % (label, y, how), rng, None, role))
                        count += 1

    for g in range(1, args.mute_groups + 1):
        path = "/yosc:req/set/MIXER:Current/MuteGrpCtrl/On/%d" % g
        put(tree, path, leaf(path, "i", 3, [0], "Mute group %d engaged, 1, or not, 0." % g, vals(0, 1)))
        path = "/yosc:req/set/MIXER:Current/MuteGrpCtrl/Label/Name/%d" % g
        put(tree, path, leaf(path, "s", 3, [""], "Mute group %d's name, up to 8 characters." % g))
        count += 2

    put(tree, "/yosc:req/ssrecallt_ex",
        command("/yosc:req/ssrecallt_ex", "Recall a scene: the list, scene_a or scene_b, and the number as \"x.xx\" (1.00 to 499.99).",
                "ss", "scene.recall"))
    put(tree, "/yosc:req/event",
        command("/yosc:req/event", "Step the scene list: MIXER:Lib/Scene/RecallInc or MIXER:Lib/Scene/RecallDec, then scene_a or scene_b.",
                "ss", "scene.next"))
    count += 2

    yosc = tree["CONTENTS"]["yosc:req"]
    yosc["DESCRIPTION"] = "Yamaha's request root: set a parameter, recall a scene, step the scene list."
    yosc["CONTENTS"]["set"]["DESCRIPTION"] = "Set a parameter: MIXER:Current, the parameter, the channel X and the index Y."
    yosc["CONTENTS"]["set"]["CONTENTS"]["MIXER:Current"]["DESCRIPTION"] = "The console's current state."
    write(tree, "yamaha-osc", args.out)
    print("%d messages" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
