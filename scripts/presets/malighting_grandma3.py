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

"""Writes presets/devices/malighting-grandma3-osc.json: a grandMA3 over OSC
(namespace draft §57, AFN).

The source is MA Lighting's grandMA3 help, "OSC" under Remote Inputs: the
console takes OSC 1.1 messages over UDP or TCP on a port set in its OSC
configuration line, with an optional prefix that this preset leaves empty -
set the same prefix on both ends or none. `/cmd` with a string runs the whole
command line ("Go+ Exec 402", "FaderMaster Page 1.201 At 50") when Receive
Command is on; `/Page<p>/Fader<e>` moves executor e of page p as an integer
over the console's FaderRange (0-100 by default); `/Page<p>/Key<e>` presses
and releases an executor's key with 1 and 0. The pages and executors written
are --pages (4) by the four rows of ninety.

Run from anywhere: `python3 scripts/presets/malighting_grandma3.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, root, span, write  # noqa: E402

SOURCES = [
    "MA Lighting, grandMA3 User Manual 2.3, Remote Inputs > OSC (help.malighting.com/grandMA3/2.3/HTML/"
    "remote_inputs_osc.html): the message pattern, the optional prefix, /cmd with a string, "
    "/Page1/Fader201 as an integer over FaderRange, Receive and Receive Command, UDP or TCP",
    "MA Lighting, grandMA3 User Manual, OSC with TouchOSC (osc_touchosc.html): the SendOSC examples "
    "and the executor numbering by rows of 100",
    "cross-read, not copied: bitfocus/companion-module-malighting-grandma3 (MIT) for the actions it offers",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pages", type=int, default=4, help="how many pages to describe (4)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    pages = {}
    for p in range(1, args.pages + 1):
        page = "/Page%d" % p
        contents = {}
        for row in (100, 200, 300, 400):
            for e in range(row + 1, row + 91):
                if row == 200:
                    contents["Fader%d" % e] = leaf(page + "/Fader%d" % e, "i", 3, [0],
                                                   "Executor %d of page %d: its fader, over the console's FaderRange, 0 to 100 by default." % (e, p),
                                                   span(0, 100), None, "strip.level")
                contents["Key%d" % e] = leaf(page + "/Key%d" % e, "i", 3, [0],
                                             "Executor %d of page %d: its key, 1 pressed and 0 released." % (e, p),
                                             {"VALS": [0, 1]})
        pages["Page%d" % p] = container(page, "Page %d's executors." % p, contents)

    contents = {
        "cmd": command("/cmd", "The command line, whole: \"Go+ Exec 402\", \"FaderMaster Page 1.201 At 50\", \"Fixture 1 At 75\". Needs Receive Command on.", "s"),
    }
    contents.update(pages)

    tree = root("/", "A grandMA3 over OSC: the command line at /cmd, and the executors' faders and keys by page. "
                     "No prefix: set none on the console's OSC line, or add the same one to every address here.",
                "malighting-grandma3-osc", "MA Lighting", "grandMA3 (OSC)", SOURCES, "scripts/presets/malighting_grandma3.py",
                transport="udp", port=0, contents=contents)

    # The file is rooted at "/" so that /cmd and /Page1 are the device's roots (AFK).
    write(tree, "malighting-grandma3-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
