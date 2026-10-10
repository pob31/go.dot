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

"""Writes presets/devices/flux-spat-osc.json: FLUX:: SPAT Revolution over
its own OSC grammar (namespace draft §57, AFN).

The source is FLUX's public "SPAT Revolution OSC Guidelines and Table", the
spreadsheet linked from doc.flux.audio's Appendix C, kept as text under
scripts/presets/sources/flux/: one row per message with its values, minimum,
maximum, default, unit and description. This reads the rows addressed to a
source ((k), the remote number), a room, a master or a snapshot, spells (k)
out for --sources (32) sources, (n) for the rooms and --bands (8) EQ bands, and leaves the global queries aside. The port is the one set in the
OSC Connections Matrix. SPAT Revolution 25.01 takes ADM-OSC too: the adm-osc
preset.

Run from anywhere: `python3 scripts/presets/flux_spat.py`.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, put, root, span, write  # noqa: E402

SOURCES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sources", "flux")

SOURCES = [
    "FLUX:: Immersive, SPAT Revolution OSC Guidelines and Table (Public Version), the spreadsheet linked "
    "from doc.flux.audio/spat-revolution/Appendix_C_OSC_Table.html, read 2026-10-10 and kept as "
    "scripts/presets/sources/flux/spat-revolution-osc-table.tsv: the source, room, master and snapshot rows",
    "FLUX:: Immersive, SPAT Revolution documentation, Ecosystem & integration > Open Sound Control "
    "(the OSC Connections Matrix: the port per connection, index as argument, packed aed and xyz)",
]

ROLES = {
    "gain": "object.gain",
    "mute": "object.mute",
    "aed": "object.position",
    "xyz": "object.position",
}


def read_rows():
    path = os.path.join(SOURCES_DIR, "spat-revolution-osc-table.tsv")
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            cells = [c.strip() for c in line.rstrip("\n").split("\t")]
            rows.append(cells)
    return rows


def typed(value_type, lo, hi, default, unit):
    """The OSC type tags, the default VALUE and the RANGE for one row of the table."""
    kind = value_type.lower()
    m = re.match(r"list of (\d+) floats?", kind)
    if m:
        n = int(m.group(1))
        return "f" * n, [0.0] * n, None
    def number(text, fallback):
        try:
            return float(str(text).replace(",", "."))
        except (TypeError, ValueError):
            return fallback

    if kind.startswith("bool"):
        return "i", [int(number(default, 0))], {"VALS": [0, 1]}
    if kind == "string":
        return "s", [""], None
    if kind == "float" or kind == "int":
        tag = "i" if kind == "int" else "f"
        lo_n, hi_n = number(lo, None), number(hi, None)
        d = number(default, lo_n if lo_n is not None else 0.0)
        if tag == "i":
            d = int(d)
            rng = span(int(lo_n), int(hi_n)) if lo_n is not None and hi_n is not None else None
        else:
            rng = span(lo_n, hi_n) if lo_n is not None and hi_n is not None else None
        return tag, [d], rng
    return "f", [0.0], None


def expand(address, sources, rooms, bands):
    """Every concrete address a table row stands for."""
    out = [address]
    if "(k)" in address:
        out = [a.replace("(k)", str(k)) for a in out for k in range(1, sources + 1)]
    if "rg(n)" in address:
        out = [a.replace("rg(n)", "rg%d" % n) for a in out for n in range(1, rooms + 1)]
    if "(n)" in address:
        out = [a.replace("(n)", str(n)) for a in out for n in range(1, bands + 1)]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--sources", type=int, default=32, help="how many sources (32; a bigger show regenerates with more)")
    parser.add_argument("--rooms", type=int, default=4, help="how many rooms (4)")
    parser.add_argument("--bands", type=int, default=8, help="EQ bands per source (8 of the table's 16)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    tree = root("/", "FLUX:: SPAT Revolution over its own OSC grammar: %d sources' positions, gains, sends and filters, "
                     "the rooms, the masters and the snapshots, each message as FLUX's table spells it. The port is the "
                     "one set in the OSC Connections Matrix." % args.sources,
                "flux-spat-osc", "FLUX:: Immersive", "SPAT Revolution (FLUX OSC)", SOURCES, "scripts/presets/flux_spat.py",
                transport="udp", port=0)

    count = 0
    seen = set()
    for cells in read_rows():
        # Category | Parameter Name | SPAT Message | Values | Minimum | Maximum | Default | Format | Description
        address = next((c for c in cells if c.startswith("/")), "")
        if not address or address.startswith("/global/") or address.endswith("/?"):
            continue
        if not address.startswith(("/source/", "/room/", "/master/", "/snapshot/")):
            continue
        at = cells.index(address)
        after = cells[at + 1:]
        value_type = after[0] if len(after) > 0 else "Float"
        lo = after[1] if len(after) > 1 else ""
        hi = after[2] if len(after) > 2 else ""
        default = after[3] if len(after) > 3 else ""
        unit_word = after[4] if len(after) > 4 else ""
        description = after[5] if len(after) > 5 else (cells[at - 1] if at > 0 else "")
        if not description:
            description = cells[at - 1] if at > 0 else address
        unit = {"Decibels": "dB", "Degrees": "deg", "Meters": "m", "Hertz": "Hz", "Percentage": "%", "%": "%"}.get(unit_word)
        tags, value, rng = typed(value_type, lo, hi, default, unit_word)
        tail = address.rsplit("/", 1)[-1]
        role = ROLES.get(tail)
        for concrete in expand(address, args.sources, args.rooms, args.bands):
            if concrete in seen:
                continue
            seen.add(concrete)
            if "/relative/" in concrete or tail in ("select", "params", "rotation"):
                node = command(concrete, description, tags, role)
            else:
                node = leaf(concrete, tags, 3, value, description, rng, unit, role)
            put(tree, concrete, node)
            count += 1

    if "source" in tree["CONTENTS"]:
        tree["CONTENTS"]["source"]["DESCRIPTION"] = "The sources, by remote number."
    write(tree, "flux-spat-osc", args.out)
    print("%d messages from the table" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
