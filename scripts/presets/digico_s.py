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

"""Writes presets/devices/digico-s-osc.json: a DiGiCo S21 or S31 reached
directly on its S-series GP OSC (namespace draft §57, AFN).

The source is DiGiCo's S-series OSC command set, the two tables S21_HiJack
keeps and that are copied under scripts/presets/sources/digico/: the methods
(`/channel/{channel}/...`, each marked for the strip kinds it applies to) and
the channel numbers (inputs 1-60, aux 70-77, groups 78-93, control groups
110-119, matrices 120-127) with the console's own commands (`/console/ping`,
`/console/pong`, `/console/resend`, `/console/channel/counts`) and the snapshot
recall (`/digico/snapshots/fire`, `/next`, `/previous`). Three roots with
nothing above them, so the file is rooted at "/" and the device answers at
/channel, /console and /digico (AFK). Two conventions come from S21_HiJack's
encoder, which the author's own desk has answered: EQ bands and Dyn1 bands are
0-based on the wire (four and three of them), and a send is numbered by its mix
bus, 1-based, aux and group buses together.

Run from anywhere: `python3 scripts/presets/digico_s.py`.
"""

import argparse
import csv
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sources", "digico")

SOURCES = [
    "DiGiCo, S-series OSC command set (GP OSC): the method table and the channel numbers, as "
    "S21_HiJack keeps them in Documentation/ and as copied under scripts/presets/sources/digico/ "
    "(DiGiCo S OSC Commandset_OSCpaths.csv, DiGiCo S OSC Commandset_channelNumbers.csv)",
    "S21_HiJack (pob31), src/osc/parse.rs: EQ bands and Dyn1 bands are 0-based on the wire, four "
    "and three of them; src/model/config.rs: a send is numbered by its mix bus, 1-based, aux and "
    "group buses together (8 + 16 on the S21)",
]

KINDS = [
    # column, name, first channel, last channel
    ("input", "Input", 1, 60),
    ("aux", "Aux", 70, 77),
    ("grp", "Group", 78, 93),
    ("CG", "Control group", 110, 119),
    ("mtx", "Matrix", 120, 127),
]

EQ_BANDS = range(0, 4)
DYN1_BANDS = range(0, 3)
SENDS = range(1, 25)

ROLES = {
    "fader": "strip.level",
    "mute": "strip.mute",
}


def read_methods():
    path = os.path.join(SOURCES_DIR, "DiGiCo S OSC Commandset_OSCpaths.csv")
    with open(path, encoding="utf-8", newline="") as f:
        rows = list(csv.reader(f, delimiter="\t"))
    header = [h.strip() for h in rows[0]]
    methods = []
    for row in rows[1:]:
        if not row or not row[0].strip():
            continue
        cells = dict(zip(header, [c.strip() for c in row]))
        methods.append(cells)
    return methods


def typed(value_type, value_range):
    """The OSC type tag, the default VALUE and the RANGE for one method."""
    kind = value_type.lower()
    if kind == "boolean":
        return "T", [False], None
    if kind == "string":
        return "s", [""], None
    if kind == "int":
        if "/" in value_range:
            return "i", [0], [{"VALS": [int(v) for v in value_range.split("/")]}]
        if "~" in value_range:
            lo, hi = [int(float(v.strip().replace("+", ""))) for v in value_range.split("~")]
            return "i", [lo], [{"MIN": lo, "MAX": hi}]
        return "i", [0], None
    if kind == "float":
        if "~" in value_range:
            lo, hi = [float(v.strip().replace("+", "")) for v in value_range.split("~")]
            default = 0.0 if lo <= 0.0 <= hi else lo
            return "f", [default], [{"MIN": lo, "MAX": hi}]
        return "f", [0.0], None
    raise SystemExit("unknown value type %r" % value_type)


def put(tree, parts, node):
    """Place `node` at parts (a list of names) under `tree`, making containers on the way."""
    here = tree
    path = ""
    for name in parts[:-1]:
        path += "/" + name
        here = here.setdefault("CONTENTS", {}).setdefault(name, {"FULL_PATH": path})
    here.setdefault("CONTENTS", {})[parts[-1]] = node


def expansions(method):
    """Every concrete path a method row stands for: bands and sends spelled out."""
    paths = [method]
    if "{band}" in method:
        bands = EQ_BANDS if "/eq/" in method else DYN1_BANDS
        paths = [p.replace("{band}", str(b)) for p in paths for b in bands]
    if "{send}" in method:
        paths = [p.replace("{send}", str(s)) for p in paths for s in SENDS]
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=os.path.join(REPO, "presets", "devices"), help="the folder to write into")
    args = parser.parse_args()

    methods = read_methods()
    tree = {"FULL_PATH": "/"}
    count = 0

    for column, kind_name, first, last in KINDS:
        for channel in range(first, last + 1):
            put(tree, ["channel", str(channel)], {"FULL_PATH": "/channel/%d" % channel,
                                                   "DESCRIPTION": "%s %d." % (kind_name, channel - first + 1)})
            for method in methods:
                if method.get(column, "") != "X":
                    continue
                for path in expansions(method["Method"].replace("{channel}", str(channel))):
                    tag, value, value_range = typed(method["Value Type"], method.get("Range", ""))
                    tail = path.split("/", 3)[3]
                    node = {"FULL_PATH": path, "TYPE": tag, "ACCESS": 3, "VALUE": value}
                    if value_range:
                        node["RANGE"] = value_range
                    if tail == "fader" or tail.endswith("/level"):
                        node["UNIT"] = ["dB"]
                    node["DESCRIPTION"] = "%s %d: %s." % (kind_name, channel - first + 1, tail.replace("/", " "))
                    role = ROLES.get(tail)
                    if tail.startswith("send/") and tail.endswith("/level"):
                        role = "send.level"
                    if role:
                        node["GODOT"] = {"ROLE": role}
                    put(tree, path.strip("/").split("/"), node)
                    count += 1

    tree["CONTENTS"]["channel"]["DESCRIPTION"] = ("The strips, by GP OSC channel number: inputs 1-60, aux 70-77, "
                                                  "groups 78-93, control groups 110-119, matrices 120-127.")

    console = {
        "ping": {"FULL_PATH": "/console/ping", "ACCESS": 2, "DESCRIPTION": "Ask the console to answer /console/pong."},
        "pong": {"FULL_PATH": "/console/pong", "ACCESS": 1, "DESCRIPTION": "The console's answer to a ping.", "GODOT": {"KIND": "event"}},
        "resend": {"FULL_PATH": "/console/resend", "ACCESS": 2, "DESCRIPTION": "Ask the console to send every value again."},
        "channel": {"FULL_PATH": "/console/channel", "CONTENTS": {
            "counts": {"FULL_PATH": "/console/channel/counts", "TYPE": "i", "ACCESS": 1, "VALUE": [60],
                       "DESCRIPTION": "How many input channels the session has; what the console reports."},
        }},
    }
    put(tree, ["console"], {"FULL_PATH": "/console", "DESCRIPTION": "The console itself.", "CONTENTS": console})
    count += 4

    snapshots = {
        "fire": {
            "FULL_PATH": "/digico/snapshots/fire", "TYPE": "i", "ACCESS": 2,
            "DESCRIPTION": "Recall the snapshot with this number.",
            "GODOT": {"ROLE": "scene.recall"},
            "CONTENTS": {
                "next": {"FULL_PATH": "/digico/snapshots/fire/next", "ACCESS": 2, "DESCRIPTION": "Recall the next snapshot.", "GODOT": {"ROLE": "scene.next"}},
                "previous": {"FULL_PATH": "/digico/snapshots/fire/previous", "ACCESS": 2, "DESCRIPTION": "Recall the previous snapshot.", "GODOT": {"ROLE": "scene.previous"}},
            },
        },
    }
    put(tree, ["digico"], {"FULL_PATH": "/digico", "DESCRIPTION": "Snapshots.", "CONTENTS": {
        "snapshots": {"FULL_PATH": "/digico/snapshots", "CONTENTS": snapshots}}})
    count += 3

    tree["DESCRIPTION"] = ("A DiGiCo S21 or S31 reached directly on its S-series GP OSC: three roots with nothing above them. "
                           "The port is the console's GP OSC port, set in its Setup.")
    tree["GODOT"] = {
        "PRESET": "digico-s-osc",
        "VERSION": 1,
        "VENDOR": "DiGiCo",
        "MODEL": "S21, S31 (S-series GP OSC)",
        "TRANSPORT": "udp",
        "WIRE": "osc",
        "PORT": 0,
        "SOURCES": SOURCES,
        "GENERATED": "scripts/presets/digico_s.py",
    }

    ordered = {"FULL_PATH": "/", "DESCRIPTION": tree["DESCRIPTION"], "GODOT": tree["GODOT"], "CONTENTS": tree["CONTENTS"]}

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "digico-s-osc.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(ordered, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print("wrote %s: %d leaves" % (path, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
