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

"""Writes presets/devices/yamaha-rcp.json: a Yamaha CL, QL, TF, DM3, DM7 or
RIVAGE PM over RCP, Yamaha's remote control protocol, lines of text on TCP
49280 (namespace draft §57, AFB, AFH; DP.7).

NO NDA DOCUMENT WAS READ (AFA). The parameter identifiers are the same list
Yamaha's public DM7 OSC specification carries - `MIXER:Current/InCh/Fader/
Level` with X the channel from nought and Y a second index - and that list
is yamaha_osc.py's, imported here and spelled the RCP way: the address is
the parameter with X and Y as its last two segments from one, and the node's
GODOT.RCP says the verb and how many trailing segments are indexes, so the
rcp wire renders `set MIXER:Current/InCh/Fader/Level 0 0 -32768` from
/MIXER:Current/InCh/Fader/Level/1/1 (AFH). The grammar - set, get, the OK,
OKm, ERROR and NOTIFY replies, ssrecall_ex, devinfo - is as the Companion
yamaha-rcp module (MIT) and the community's RCP notes describe it, read for
the shape of the lines and never copied. Two scene recalls are offered,
because the families differ: ssrecall_ex MIXER:Lib/Scene <n> on a CL or QL,
and ssrecallt_ex scene_a "<x.xx>" on a DM3, DM7, TF or RIVAGE PM; a console
answers ERROR to the one it does not take, which the Network tab shows.

Run from anywhere: `python3 scripts/presets/yamaha_rcp.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, put, root, span, vals, write  # noqa: E402
from yamaha_osc import SEND_PAN_ROWS, STRIP_NAMES, STRIP_ROWS  # noqa: E402

SOURCES = [
    "Yamaha, DM7 Series OSC Specifications, Version 1.1.0 (July 2025), section 2.1: the parameter identifier list "
    "under MIXER:Current with X, Y, type, min, max, scaling and unit, which is RCP's own; the scene list names",
    "Yamaha, CL/QL/TF, DM3 and RIVAGE PM product pages: the Remote Control Protocol port, 49280, named in the "
    "consoles' network settings and third-party controller guides",
    "cross-read, not copied: bitfocus/companion-module-yamaha-rcp (MIT), src/rcpNames.json and the help page: the "
    "line grammar - set, get, the OK, OKm, ERROR and NOTIFY replies, ssrecall_ex, ssrecallt_ex, devinfo - and the "
    "parameters a console of each family answers",
]


def rcp(node, indexes, verb="set"):
    """The node's RCP spelling: the verb, and how many trailing segments are X and Y."""
    node.setdefault("GODOT", {})["RCP"] = {"VERB": verb, "XY": indexes}
    return node


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inputs", type=int, default=72, help="input channels (72; the DM7 has 120, a CL5 72, a QL1 32)")
    parser.add_argument("--mixes", type=int, default=24, help="mix buses (24; the DM7 has 48, a QL1 16)")
    parser.add_argument("--matrices", type=int, default=8, help="matrices (8; the DM7 has 12)")
    parser.add_argument("--stereo", type=int, default=2, help="stereo buses (2)")
    parser.add_argument("--dcas", type=int, default=24, help="DCA groups (24; a CL or QL has 16)")
    parser.add_argument("--mute-groups", type=int, default=12, help="mute groups (12; a CL or QL has 8)")
    parser.add_argument("--bands", type=int, default=4, help="PEQ bands (4; the DM7 has 8)")
    parser.add_argument("--send-pans", action="store_true", help="write the sends' pan and pre/post rows too")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    counts = {"InCh": args.inputs, "Mix": args.mixes, "St": args.stereo, "Mtrx": args.matrices, "DCA": args.dcas}
    y_counts = {"mix": args.mixes, "mtrx": args.matrices, "band": args.bands, "dca": args.dcas, "st": args.stereo}

    tree = root("/", "A Yamaha CL, QL, TF, DM3, DM7 or RIVAGE PM over RCP, lines of text on TCP 49280: every strip's fader, "
                     "on, pan, name, filters and PEQ, the sends with their level and on, the DCA assigns, the mute groups, "
                     "and the scene recalls. Levels are hundredths of a decibel. The channel X and the index Y are the "
                     "address's last segments, from one; the wire counts them from nought as the console does. "
                     "%d inputs, %d mixes, %d matrices, %d DCAs." % (args.inputs, args.mixes, args.matrices, args.dcas),
                "yamaha-rcp", "Yamaha", "CL, QL, TF, DM3, DM7, RIVAGE PM (RCP)", SOURCES, "scripts/presets/yamaha_rcp.py",
                transport="tcp", wire="rcp", port=49280, readback="notify")

    count = 0
    for strip, rows in STRIP_ROWS.items():
        rows = list(rows) + (SEND_PAN_ROWS.get(strip, []) if args.send_pans else [])
        for x in range(1, counts[strip] + 1):
            for parameter, y_kind, (tag, rng, value, how), role, what in rows:
                base = "/MIXER:Current/%s/%s/%d" % (strip, parameter, x)
                label = "%s %d: %s" % (STRIP_NAMES[strip], x, what.lower())
                if y_kind is None:
                    put(tree, base, rcp(leaf(base, tag, 3, value, "%s; %s" % (label, how), rng, None, role), 1))
                    count += 1
                else:
                    for y in range(1, y_counts[y_kind] + 1):
                        path = "%s/%d" % (base, y)
                        put(tree, path, rcp(leaf(path, tag, 3, value, "%s %d; %s" % (label, y, how), rng, None, role), 2))
                        count += 1

    for g in range(1, args.mute_groups + 1):
        path = "/MIXER:Current/MuteGrpCtrl/On/%d" % g
        put(tree, path, rcp(leaf(path, "i", 3, [0], "Mute group %d engaged, 1, or not, 0." % g, vals(0, 1)), 1))
        path = "/MIXER:Current/MuteGrpCtrl/Label/Name/%d" % g
        put(tree, path, rcp(leaf(path, "s", 3, [""], "Mute group %d's name, up to 8 characters." % g), 1))
        count += 2

    put(tree, "/MIXER:Lib/Scene",
        rcp(command("/MIXER:Lib/Scene", "Recall a scene by number on a CL or QL: ssrecall_ex MIXER:Lib/Scene <n>.",
                    "i", "scene.recall", span(1, 300)), 0, "ssrecall_ex"))
    put(tree, "/scene_a",
        rcp(command("/scene_a", "Recall a scene of list A on a DM3, DM7, TF or RIVAGE PM by its number as \"x.xx\": "
                                "ssrecallt_ex scene_a \"4.00\".", "s", "scene.recall"), 0, "ssrecallt_ex"))
    put(tree, "/scene_b",
        rcp(command("/scene_b", "Recall a scene of list B, the same way.", "s", "scene.recall"), 0, "ssrecallt_ex"))
    count += 3

    current = tree["CONTENTS"]["MIXER:Current"]
    current["DESCRIPTION"] = "The console's current state: a parameter, the channel X and the index Y as the last segments."
    tree["CONTENTS"]["MIXER:Lib"]["DESCRIPTION"] = "The console's libraries: the scene list."
    write(tree, "yamaha-rcp", args.out)
    print("%d messages" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
