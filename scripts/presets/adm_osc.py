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

"""Writes presets/devices/adm-osc.json: the ADM-OSC v1.0 object grammar as a
device preset (namespace draft §57, AFN; PRD §3.22 "ADM-OSC ships built in").

The source is the specification's address table, immersive-audio-live/ADM-OSC
(the living standard, v1.0, MIT): one object is /adm/obj/<n>/ with azim, elev,
dist, aed, x, y, z, xy, xyz, w, gain, dref, dmax, mute and name; the
environment is /adm/env/change; the listener /adm/lis/xyz and /adm/lis/ypr.
Positions are normalised and the receiver scales them. The spec puts no upper
bound on an object count; this writes sixty-four, and --objects writes more
for a processor that has them, because a described device refuses an address
its file lacks.

Run from anywhere: `python3 scripts/presets/adm_osc.py [--objects 128]`.
"""

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOURCES = [
    "immersive-audio-live/ADM-OSC, ADM-OSC v1.0 (living standard, 15 June 2026), the object, "
    "environment and listener address tables - https://immersive-audio-live.github.io/ADM-OSC/",
    "AES, ADM-OSC specification v1.0 and implementation guide (the same tables, published)",
]


def leaf(path, types, access, value, description, ranges=None, unit=None, role=None, kind=None):
    node = {"FULL_PATH": path, "TYPE": types, "ACCESS": access}
    if value is not None:
        node["VALUE"] = value
    if ranges:
        node["RANGE"] = ranges
    if unit:
        node["UNIT"] = [unit] * len(types)
    node["DESCRIPTION"] = description
    godot = {}
    if role:
        godot["ROLE"] = role
    if kind:
        godot["KIND"] = kind
    if godot:
        node["GODOT"] = godot
    return node


def span(lo, hi):
    return {"MIN": lo, "MAX": hi}


def one_object(n):
    base = "/adm/obj/%d" % n
    pos = "Object %d." % n
    return {
        "FULL_PATH": base,
        "DESCRIPTION": pos,
        "CONTENTS": {
            "azim": leaf(base + "/azim", "f", 3, [0.0], "Azimuth, in degrees: nought ahead, positive to the left.", [span(-180.0, 180.0)], "deg"),
            "elev": leaf(base + "/elev", "f", 3, [0.0], "Elevation, in degrees: nought level, positive up.", [span(-90.0, 90.0)], "deg"),
            "dist": leaf(base + "/dist", "f", 3, [1.0], "Distance, normalised: one is the reference distance.", [span(0.0, 1.0)]),
            "aed":  leaf(base + "/aed", "fff", 3, [0.0, 0.0, 1.0], "Azimuth, elevation and distance in one message.",
                         [span(-180.0, 180.0), span(-90.0, 90.0), span(0.0, 1.0)], None, "object.position"),
            "x":    leaf(base + "/x", "f", 3, [0.0], "Left to right, normalised: minus one to one.", [span(-1.0, 1.0)]),
            "y":    leaf(base + "/y", "f", 3, [0.0], "Back to front, normalised: minus one to one.", [span(-1.0, 1.0)]),
            "z":    leaf(base + "/z", "f", 3, [0.0], "Bottom to top, normalised: minus one to one.", [span(-1.0, 1.0)]),
            "xy":   leaf(base + "/xy", "ff", 3, [0.0, 0.0], "X and Y in one message.", [span(-1.0, 1.0), span(-1.0, 1.0)]),
            "xyz":  leaf(base + "/xyz", "fff", 3, [0.0, 0.0, 0.0], "X, Y and Z in one message.",
                         [span(-1.0, 1.0), span(-1.0, 1.0), span(-1.0, 1.0)], None, "object.position"),
            "w":    leaf(base + "/w", "f", 3, [0.0], "Width, the horizontal extent, normalised.", [span(0.0, 1.0)]),
            "gain": leaf(base + "/gain", "f", 3, [1.0], "Gain, linear: one is unity; the receiver clamps what it cannot take.", [span(0.0, 1.0)], None, "object.gain"),
            "dref": leaf(base + "/dref", "f", 3, [1.0], "Reference distance, normalised.", [span(0.0, 1.0)]),
            "dmax": leaf(base + "/dmax", "f", 3, [10.0], "The distance, in metres, that a normalised distance of one means.", [{"MIN": 0.0}], "m"),
            "mute": leaf(base + "/mute", "i", 3, [0], "Muted: one, or nought.", [{"VALS": [0, 1]}], None, "object.mute"),
            "name": leaf(base + "/name", "s", 3, [""], "The object's label, up to 128 characters."),
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--objects", type=int, default=64, help="how many objects to describe (64)")
    parser.add_argument("--out", default=os.path.join(REPO, "presets", "devices"), help="the folder to write into")
    args = parser.parse_args()

    objects = {str(n): one_object(n) for n in range(1, args.objects + 1)}

    tree = {
        "FULL_PATH": "/adm",
        "DESCRIPTION": "ADM-OSC v1.0: object-based audio positions and gains for any processor that speaks it - "
                       "L-ISA, Spat Revolution, Holophonix, Fletcher Machine, SpaceMap Go, the DS100 through En-Bridge. "
                       "%d objects; a processor with more takes a file written with --objects." % args.objects,
        "GODOT": {
            "PRESET": "adm-osc",
            "VERSION": 1,
            "VENDOR": "ADM-OSC",
            "MODEL": "Any processor speaking ADM-OSC v1.0 (OSC)",
            "TRANSPORT": "udp",
            "WIRE": "osc",
            "PORT": 0,
            "SOURCES": SOURCES,
            "GENERATED": "scripts/presets/adm_osc.py",
        },
        "CONTENTS": {
            "obj": {"FULL_PATH": "/adm/obj", "DESCRIPTION": "The objects, by number from one.", "CONTENTS": objects},
            "env": {"FULL_PATH": "/adm/env", "DESCRIPTION": "The environment.", "CONTENTS": {
                "change": leaf("/adm/env/change", "s", 2, None, "A global scene or programme change, by name.", kind="event"),
            }},
            "lis": {"FULL_PATH": "/adm/lis", "DESCRIPTION": "The listener.", "CONTENTS": {
                "xyz": leaf("/adm/lis/xyz", "fff", 3, [0.0, 0.0, 0.0], "The listener's position, normalised.",
                            [span(-1.0, 1.0), span(-1.0, 1.0), span(-1.0, 1.0)]),
                "ypr": leaf("/adm/lis/ypr", "fff", 3, [0.0, 0.0, 0.0], "The listener's yaw, pitch and roll, in degrees.",
                            [span(-180.0, 180.0), span(-180.0, 180.0), span(-180.0, 180.0)], "deg"),
            }},
        },
    }

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "adm-osc.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(tree, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print("wrote %s: %d objects" % (path, args.objects))
    return 0


if __name__ == "__main__":
    sys.exit(main())
