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

"""Writes presets/devices/sennheiser-ewdx-ssc.json and sennheiser-d6000.json:
Sennheiser's wireless receivers over the Sound Control Protocol, version 1 -
JSON, a datagram each, UDP 45 (namespace draft §124, BES).

The address under the device's prefix is the JSON's path: `/rx1/mute` true is
`{"rx1":{"mute":true}}`, a question the same with null. The EW-DX's nodes come
from its developer's guide's method list, transcribed whole into
sources/sennheiser/ewdx-em-ssc-3.0.tsv (the protocol's own /osc part left out);
the Digital 6000's are written here from its guide, which is short and words
its methods as numbered sections. Each file's root carries the subscription a
device made from it sends as its socket opens and every nine seconds after
(GODOT.SUBSCRIBE, readback subscribe): what the panel watches - names, mutes,
gains, frequencies, warnings, the packs and the meters - throttled to one
notification a second, a lifetime of fifteen.

An EW-DX from firmware 4.0 speaks version 2 (HTTPS) unless its legacy mode is on
in the Wireless Systems Manager; this file is for version 1.

Run from anywhere: `python3 scripts/presets/sennheiser_ssc.py`.
"""

import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, root, span, vals, write  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
EWDX_TABLE = os.path.join(HERE, "sources", "sennheiser", "ewdx-em-ssc-3.0.tsv")

EWDX_SOURCES = [
    "Sennheiser, Sound Control Protocol (SSC), Developer's guide for EW-DX (EW-DX EM 2, EM 2 Dante, EM 4 Dante, "
    "firmware 3.0.x), publ. 03/2024: the data structure, subscriptions (min, max, count, lifetime), UDP 45, the "
    "method list - transcribed as scripts/presets/sources/sennheiser/ewdx-em-ssc-3.0.tsv",
    "cross-read, not copied: bitfocus/companion-module-sennheiser-ewdx (MIT), its help page: legacy mode needed "
    "from firmware 4.0.0",
]
D6000_SOURCES = [
    "Sennheiser, Sound Control Protocol (SSC), Developer's guide for Digital 6000 (EM 6000, L 6000), TI 1109 v2.2: "
    "the EM 6000 method list (8.1 to 8.104), /mm's metering array and its subscription example",
    "cross-read, not copied: bitfocus/companion-module-sennheiser-digital6000 (MIT), its help page: the actions, "
    "feedbacks and variables per device",
]


def subscription(tree, throttle_ms=1000, lifetime=15):
    """The one message that subscribes to `tree` (paths to null), throttled, with its lifetime."""
    request = {"#": {"max": throttle_ms, "lifetime": lifetime}}
    request.update(tree)
    return json.dumps({"osc": {"state": {"subscribe": [request]}}}, separators=(",", ":"))


def put(tree, path, node):
    """Place `node` at `path` under the root `tree`, making containers on the way, each FULL_PATH under the root's."""
    here = tree
    parts = path.strip("/").split("/")
    built = tree["FULL_PATH"]
    for name in parts[:-1]:
        built += "/" + name
        here = here.setdefault("CONTENTS", {}).setdefault(name, {"FULL_PATH": built})
    here.setdefault("CONTENTS", {})[parts[-1]] = node


def nested(paths):
    """Paths as a JSON tree of nulls: ["rx1/mute"] -> {"rx1": {"mute": None}}."""
    out = {}
    for path in paths:
        here = out
        parts = path.strip("/").split("/")
        for part in parts[:-1]:
            here = here.setdefault(part, {})
        here[parts[-1]] = None
    return out


def ewdx():
    tree = root("/ewdx", "A Sennheiser EW-DX receiver (EM 2, EM 2 Dante, EM 4 Dante, firmware 3.0.x or legacy mode) over "
                         "the Sound Control Protocol on UDP 45: every method of its developer's guide - each channel's "
                         "name, mute, gain, frequency and warnings, its pack's battery, name, mute and trim, the device's "
                         "own - and its meters. Four channels described; an EM 2 has two.",
                "sennheiser-ewdx-ssc", "Sennheiser", "EW-DX EM 2, EM 2 Dante, EM 4 Dante (SSC v1)", EWDX_SOURCES,
                "scripts/presets/sennheiser_ssc.py", transport="udp", wire="ssc", port=45, readback="subscribe")

    with open(EWDX_TABLE, encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))

    for row in rows:
        address = row["address"]
        if address.startswith("/osc/"):
            continue

        kind = row["value"].strip(". ").lower()
        access = {"read/write": 3, "write-only": 2}.get(row["access"].lower(), 1)
        description = row["description"] or ("The receiver's %s." % address)
        if row["length"]:
            description += " At most %s characters." % row["length"]

        if kind in ("boolean", "bool"):
            types = "T"
        elif kind in ("number", "int"):
            fractional = "." in row["inc"] or "double" in description
            types = "f" if fractional else "i"
        else:
            types = "s"

        ranges = None
        if types in "if" and row["min"] and row["max"]:
            number = float if types == "f" else int
            try:
                ranges = span(number(float(row["min"])), number(float(row["max"])))
            except ValueError:
                ranges = None
        elif row["options"] and row["count"] != "-1":
            ranges = vals(*[o for o in row["options"].split("|") if o])

        path = "/ewdx" + address
        node = command(path, description, types, None, ranges) if access == 2 \
            else leaf(path, types, access, None, description, ranges, row["units"] or None)
        put(tree, address, node)

    channels = range(1, 5)
    watched = []
    for ch in channels:
        watched += ["rx%d/%s" % (ch, n) for n in ("name", "mute", "gain", "frequency", "warnings")]
        watched += ["mates/tx%d/%s" % (ch, n) for n in ("battery/gauge", "battery/lifetime", "battery/type", "name",
                                                      "mute", "type", "warnings")]
        watched += ["m/rx%d/%s" % (ch, n) for n in ("rssi", "rsqi", "divi", "af")]
    tree["GODOT"]["SUBSCRIBE"] = subscription(nested(watched))
    return tree


#  The EM 6000's per channel, from TI 1109's section 8: (name, type, access, description, range, unit).
D6000_CHANNEL = [
    ("audio_mute", "T", 3, "The channel's audio output muted (8.3).", None, None),
    ("carrier", "i", 3, "The carrier frequency in kHz, 470100 to 713900 in steps of 25 (8.17).", span(470100, 713900), "kHz"),
    ("name", "s", 3, "The receiver channel's name (8.44).", None, None),
    ("encryption", "T", 3, "Encryption on (8.45).", None, None),
    ("identify", "T", 1, "Asked, the receiver shows Identified and blinks; answers true (8.41).", None, None),
    ("testtone", "i", 1, "The test tone: -60 to 0 dBFS on, 1 off (8.43).", None, "dBFS"),
    ("active_warnings", "s", 1, "The warnings now: RFPeak, AFPeak, LowSignal, NoLink, LowBattery, BadClock, NoClock, "
     "Aes256Error, AnTxYBNCShorted (8.46).", None, None),
    ("active_status", "ss", 1, "The status and its substatus: SyncOK, SyncFail, Identified, SwUpdatePass, SwUpdateFail "
     "(8.47).", None, None),
    ("skx/type", "sss", 1, "The pack: its type and the lowest and highest of its range (8.20).", None, None),
    ("skx/name", "s", 1, "The pack's name; empty with none (8.21).", None, None),
    ("skx/gain", "s", 1, "The pack's gain as it words it, \"42 dB\"; empty with none (8.23).", None, None),
    ("skx/battery", "ss", 1, "The pack's battery: a state - 100%, 70%, 30% or low - and the time left, x:xx or -:--; "
     "empty with none (8.27).", None, None),
]
D6000_DEVICE = [
    ("sys/brightness", "i", 3, "The display's brightness (8.98).", None, None),
    ("sys/booster", "T", 3, "The antenna booster on (8.99).", None, None),
    ("sys/clock", "s", 3, "The word clock source (8.97).", None, None),
    ("sys/dante/name", "s", 1, "The Dante device name (8.94).", None, None),
    ("device/identity/product", "s", 1, "The product (8.103).", None, None),
    ("device/identity/version", "s", 1, "The firmware (8.101).", None, None),
    ("audio/out1/level_db", "i", 3, "Channel 1's analogue output level, -10 to +18 dB (8.1).", span(-10, 18), "dB"),
    ("audio/out2/level_db", "i", 3, "Channel 2's analogue output level, -10 to +18 dB (8.2).", span(-10, 18), "dB"),
    ("mm", "i" * 18, 1, "The meters of both channels, nine each: RF1, RF1 peak, RF2, RF2 peak, diversity 1, diversity 2, "
     "link quality (255 best), AF, AF peak - raw: dBm is (value - 255) / 2, dBFS (value + 1) / 2 - 128 (8.100).",
     None, None),
]


def d6000():
    tree = root("/em6000", "A Sennheiser Digital 6000 receiver, an EM 6000 or EM 6000 Dante, over the Sound Control Protocol "
                           "on UDP 45: each channel's mute, carrier, name and warnings, its pack's name, gain and battery, "
                           "the device's own, and the meters of both channels.",
                "sennheiser-d6000", "Sennheiser", "Digital 6000 EM 6000, EM 6000 Dante (SSC v1)", D6000_SOURCES,
                "scripts/presets/sennheiser_ssc.py", transport="udp", wire="ssc", port=45, readback="subscribe")

    def place(path, row):
        _, types, access, description, ranges, unit = row
        put(tree, path, leaf("/em6000" + path, types, access, None, description, ranges, unit))

    for ch in (1, 2):
        for row in D6000_CHANNEL:
            place("/rx%d/%s" % (ch, row[0]), row)
    for row in D6000_DEVICE:
        place("/" + row[0], row)

    watched = ["mm"]
    for ch in (1, 2):
        watched += ["rx%d/%s" % (ch, n) for n in ("audio_mute", "carrier", "name", "active_warnings",
                                                  "skx/name", "skx/battery", "skx/gain")]
    tree["GODOT"]["SUBSCRIBE"] = subscription(nested(watched), throttle_ms=500)
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    write(ewdx(), "sennheiser-ewdx-ssc", args.out)
    write(d6000(), "sennheiser-d6000", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
