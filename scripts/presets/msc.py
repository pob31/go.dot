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

"""Writes presets/devices/msc.json: MIDI Show Control, the generic lighting
and show device over a MIDI port or, for an MA desk, in a datagram
(namespace draft §57, AFJ; DP.9).

The source is the MIDI Manufacturers Association's MIDI Show Control 1.0
recommended practice (RP-002), as every console's manual restates it: a
System Exclusive message F0 7F <device> 02 <format> <command> <data> F7,
the device 00 to 6F for one, 70 to 7E for a group, 7F for all; the format
01 lighting (general), 10 sound (general), 7F all types, among others; the
commands Go 01, Stop 02, Resume 03, Timed Go 04, Load 05, Set 06, Fire 07,
All Off 08, Restore 09, Reset 0A, Go Off 0B; the data a cue number as ASCII
digits with a dot, then 00 and a cue list, then 00 and a cue path, each
optional. The device's mscDevice and mscFormat rows fill the two bytes; the
cue's atoms the number, the list and the path. On a MIDI port the message
goes as any SysEx; on UDP to a grandMA2 or 3 the bytes go as they are in a
datagram, which is how MA takes MSC over Ethernet (ports 6000 to 6100).

Run from anywhere: `python3 scripts/presets/msc.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, midi, root, write  # noqa: E402

SOURCES = [
    "MIDI Manufacturers Association, MIDI Show Control 1.0 (RP-002, 1991) as restated in ETC's Eos Family Show "
    "Control User Guide and MA Lighting's grandMA2 and grandMA3 help on MSC: the SysEx frame F0 7F <device> 02 "
    "<format> <command> <data> F7, the device and format numbers, the eleven commands, the cue number, list and "
    "path as ASCII with 00 between them",
    "MA Lighting, grandMA3 help, MSC: MSC over Ethernet in UDP datagrams, ports 6000 to 6100; cross-read, not copied: "
    "bitfocus/companion-module-malighting-msc (MIT)",
]

COMMANDS = [
    ("go", 0x01, "Go: the cue by number, and the list and path where given.", "sss", "go"),
    ("stop", 0x02, "Stop: the cue, or everything with no cue.", "sss", "stop"),
    ("resume", 0x03, "Resume: the cue, or everything with no cue.", "sss", None),
    ("timedGo", 0x04, "Timed Go: the time as five numbers, hours, minutes, seconds, frames and fractions, then the cue.", "iiiiis", None),
    ("load", 0x05, "Load: the cue, ready to go.", "sss", None),
    ("set", 0x06, "Set: a generic control by number and value.", "ii", None),
    ("fire", 0x07, "Fire: a macro by number.", "i", None),
    ("allOff", 0x08, "All Off.", None, "stop"),
    ("restore", 0x09, "Restore.", None, None),
    ("reset", 0x0A, "Reset.", None, None),
    ("goOff", 0x0B, "Go Off: the cue.", "sss", None),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {}
    for name, code, what, types, role in COMMANDS:
        contents[name] = midi(command("/msc/" + name, what + " (MSC command %02X)." % code, types, role), kind="msc", command=code)

    tree = root("/msc", "MIDI Show Control: Go, Stop, Resume, Timed Go, Load, Set, Fire, All Off, Restore, Reset and Go Off, "
                        "each a SysEx under the device and format the device's mscDevice and mscFormat rows name. A cue is its "
                        "number as text (\"12\", \"3.5\"), then the list and the path where the console wants them. Over a MIDI "
                        "port as declared, or in a datagram to an MA desk.",
                "msc", "MIDI Show Control", "MSC 1.0 (MIDI port or UDP)", SOURCES, "scripts/presets/msc.py",
                transport="midi", wire="midi", port=0, contents=contents)
    write(tree, "msc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
