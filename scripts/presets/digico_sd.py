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

"""Writes presets/devices/digico-sd-osc.json: a DiGiCo SD or Quantum console
on its "Other OSC" command set (namespace draft §57, AFN).

The source is DiGiCo's "OSC Command List for Other OSC" of 17 November
2014, the document the console's External Control panel loads as a command
set, as S21_HiJack keeps it and as kept here as text under
scripts/presets/sources/digico/: 580 rows under /sd, every level a float
0 to 1 on the console's own law, switches as 0 or 1, names as strings. The
rows are spelled out for --inputs (48) input channels, --aux (16) aux
outputs, --groups (16) group outputs, --matrix (8) matrix outputs,
--control-groups (12) control groups and --geq (8) graphic EQs - a bigger
desk regenerates with its counts -, and the
snapshot recall the S21_HiJack field notes report (/sd/Snapshots/
Recall_Snapshot) is added with the note that the 2014 list predates it.
Ports: the console's External Control device says; 8000 and 9000 in the SD
App guide, 7000 and 7001 in Companion's setup.

Run from anywhere: `python3 scripts/presets/digico_sd.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, put, root, span, vals, write  # noqa: E402

SOURCES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sources", "digico")

SOURCES = [
    "DiGiCo, OSC Command List for Other OSC, 17/11/2014 (DiGiCo_OTHER_OSC_List_17_11_14.pdf, the command set the "
    "External Control panel loads), as S21_HiJack keeps it in Documentation/ and as kept here as text: "
    "scripts/presets/sources/digico/digico-other-osc-2014.tsv, 580 rows",
    "S21_HiJack (pob31), Documentation/OSC_FIELD_NOTES.md: the /sd/ surface, ports 8000 and 9000 in the SD App "
    "guide, /Snapshots/Recall_Snapshot reported on the SD9 (unverified there), levels normalised 0..1",
]

ROLES = {"fader": "strip.level", "mute": "strip.mute", "send_level": "send.level"}


def read_rows():
    path = os.path.join(SOURCES_DIR, "digico-other-osc-2014.tsv")
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            cells = line.rstrip("\n").split("\t")
            if len(cells) >= 2 and cells[0].startswith("/sd/"):
                rows.append(cells)
    return rows


def typed(value_type, lo, hi):
    kind = value_type.lower()
    if kind == "string":
        return "s", [""], None
    if kind == "int":
        lo_i, hi_i = int(float(lo or 0)), int(float(hi or 1))
        rng = vals(*range(lo_i, hi_i + 1)) if hi_i - lo_i <= 8 else span(lo_i, hi_i)
        return "i", [lo_i], rng
    lo_f, hi_f = float(lo or 0), float(hi or 1)
    return "f", [lo_f], span(lo_f, hi_f)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inputs", type=int, default=48)
    parser.add_argument("--aux", type=int, default=16)
    parser.add_argument("--groups", type=int, default=16)
    parser.add_argument("--matrix", type=int, default=8)
    parser.add_argument("--control-groups", type=int, default=12)
    parser.add_argument("--geq", type=int, default=8)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    counts = {
        "Input_Channels": args.inputs,
        "Aux_Outputs": args.aux,
        "Group_Outputs": args.groups,
        "Matrix_Outputs": args.matrix,
        "Control_Groups": args.control_groups,
        "Graphic_EQ": args.geq,
    }
    send_counts = {"Aux_Send": args.aux, "Group_Send": args.groups, "Matrix_Send": args.matrix}

    tree = root("/", "A DiGiCo SD or Quantum console on its Other OSC command set (2014): every input, aux, group, matrix and "
                     "control group strip, the graphic EQs, and the snapshot recall. Levels are 0 to 1 on the console's own law. "
                     "The port is the External Control device's.",
                "digico-sd-osc", "DiGiCo", "SD, Quantum (Other OSC)", SOURCES, "scripts/presets/digico_sd.py",
                transport="udp", port=0)

    count = 0
    for cells in read_rows():
        address, value_type = cells[0].rstrip("/"), cells[1]
        lo = cells[2] if len(cells) > 2 else ""
        hi = cells[3] if len(cells) > 3 else ""
        parts = address.split("/")[1:]   # ["sd", "Input_Channels", "*", ...]
        section = parts[1] if len(parts) > 1 else ""
        if section not in counts:
            # Filing and the like: one node, as written.
            tags, value, rng = typed(value_type, lo, hi)
            node = command(address, "DiGiCo: %s." % " ".join(parts[1:]), tags if tags != "s" else "s")
            put(tree, address, node)
            count += 1
            continue

        stars = [i for i, p in enumerate(parts) if p == "*"]
        first = counts[section]
        second_kind = parts[stars[1] - 1] if len(stars) > 1 else ""
        second = send_counts.get(second_kind, 0)
        tail = parts[-1]
        tags, value, rng = typed(value_type, lo, hi)
        role = ROLES.get(tail)

        for n in range(1, first + 1):
            for m in range(1, (second if second else 1) + 1):
                concrete = list(parts)
                concrete[stars[0]] = str(n)
                if len(stars) > 1:
                    concrete[stars[1]] = str(m)
                path = "/" + "/".join(concrete)
                words = " ".join(p.replace("_", " ") for p in concrete[1:])
                node = leaf(path, tags, 3, value, words + ".", rng, "dB" if tail == "fader" and False else None, role)
                put(tree, path, node)
                count += 1

    recall = command("/sd/Snapshots/Recall_Snapshot", "Recall a snapshot by number (reported on the SD range; not in the 2014 list).", "i", "scene.recall", span(1, 999))
    put(tree, "/sd/Snapshots/Recall_Snapshot", recall)
    count += 1

    tree["CONTENTS"]["sd"]["DESCRIPTION"] = "The console, as its Other OSC command set spells it."
    write(tree, "digico-sd-osc", args.out)
    print("%d messages" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
