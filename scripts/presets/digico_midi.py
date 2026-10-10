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

"""Writes presets/devices/digico-midi.json: a DiGiCo SD, Quantum or S series
console over MIDI, on a declared port (namespace draft §57, AFJ; DP.9).

The source is DiGiCo's SD software reference, the Snapshots chapter and its
Snapshot MIDI List: each snapshot can be given a MIDI message that recalls
it when received on the console's MIDI port, a Program Change with a bank
being the usual choice, and the list fills them in order when asked; the S
series' snapshot MIDI works the same way. The console decides the mapping,
so this file says snapshot n sends bank and program for n from 1 - what the
list's default fill produces - and a console whose list was set otherwise is
followed by editing the cue's value. Unverified until a console has answered.

Run from anywhere: `python3 scripts/presets/digico_midi.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "DiGiCo, SD series software reference (the SD10 V726+ appendix's chapter 1.1, Snapshots: Recalling a Snapshot, "
    "the Snapshot MIDI List): a MIDI message per snapshot recalls it, Program Change with a bank the usual kind, the "
    "list filled in order on request - the mapping is the console's, this file assumes the ordered fill from 1",
    "DiGiCo, S series user guide, Snapshots MIDI: the same per-snapshot message",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--snapshots", type=int, default=256)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "snapshot": numbered("/snapshot", "The snapshots, by the console's Snapshot MIDI List.", args.snapshots, lambda p, n: {
            "FULL_PATH": p, "DESCRIPTION": "Snapshot %d." % n, "CONTENTS": {
                "recall": midi(command(p + "/recall", "Recall snapshot %d: Bank Select then Program Change for %d, as the "
                                                      "Snapshot MIDI List fills them in order." % (n, n), None, "scene.recall"),
                               kind="pc", program=n, banked=True, start=1)}}),
        "recall": midi(command("/recall", "Recall a snapshot by number, 1 to %d, as a Bank Select and a Program Change." % args.snapshots,
                               "i", "scene.recall", span(1, args.snapshots)),
                       kind="pc", banked=True, start=1),
    }

    tree = root("/", "A DiGiCo SD, Quantum or S series console over MIDI on a declared port: snapshots recalled by Bank Select "
                     "and Program Change as the console's Snapshot MIDI List assigns them, assumed filled in order from 1. The "
                     "device's midiChannel row is the console's MIDI channel.",
                "digico-midi", "DiGiCo", "SD, Quantum, S series (MIDI)", SOURCES, "scripts/presets/digico_midi.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "digico-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
