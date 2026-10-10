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

"""Writes presets/devices/malighting-grandma2-line.json: an MA Lighting
grandMA2 (and a dot2) over its telnet remote, lines of the console's own
command line on TCP 30000 (namespace draft §57, AFB, AFJ; DP.8).

The source is MA Lighting's grandMA2 help, "Remote control via telnet": the
console listens on port 30000 once telnet is enabled in its network settings,
takes `login <user> <password>` first - a user with the rights to run the
command - and then any command line as it would be typed: `Go+ Executor 1.1`,
`Go- Executor 1.1`, `Pause Executor 1.1`, `Off Executor 1.1`, `Toggle Executor
1.1`, `Fader 1.1 At 50`, `Goto Cue 12`, `Go+ Executor 1.1 Cue 5`; port 30001
is the read-only system monitor. The line wire renders each node's GODOT.LINE
template with {x} and {y} the address's first two whole-number segments - the
page and the executor - and {1} the message's first atom. /cmd takes a whole
command line as its one string. --pages (2) and --executors (90) bound the
executors described; --cues (200) the cues of the main sequence.

Run from anywhere: `python3 scripts/presets/malighting_grandma2.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, container, leaf, numbered, root, span, write  # noqa: E402

SOURCES = [
    "MA Lighting, grandMA2 help, Remote control via telnet: port 30000, telnet enabled in the network settings, "
    "login <user> <password> first, then the command line as typed; 30001 the read-only system monitor",
    "MA Lighting, grandMA2 help, the command syntax of Go+, Go-, Pause, Off, Toggle, Fader ... At, Goto Cue",
    "cross-read, not copied: bitfocus/companion-module-malighting-grandma2 (MIT), its help page and actions: "
    "the login, the executor and cue commands it offers",
]


def lined(node, template):
    """The node's line template: {x} and {y} the address's page and executor, {1} the atom."""
    node.setdefault("GODOT", {})["LINE"] = template
    return node


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pages", type=int, default=2, help="executor pages described (2)")
    parser.add_argument("--executors", type=int, default=90, help="executors per page (90)")
    parser.add_argument("--cues", type=int, default=200, help="cues of the main sequence (200)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    def executor(path, page, exec_):
        who = "executor %d.%d" % (page, exec_)
        return container(path, "Executor %d of page %d." % (exec_, page), {
            "go": lined(command(path + "/go", "Go+ on %s: the next cue." % who, None, "go"), "Go+ Executor {x}.{y}"),
            "goback": lined(command(path + "/goback", "Go- on %s: the cue before." % who), "Go- Executor {x}.{y}"),
            "pause": lined(command(path + "/pause", "Pause %s." % who, None, "stop"), "Pause Executor {x}.{y}"),
            "off": lined(command(path + "/off", "Off %s: released." % who, None, "stop"), "Off Executor {x}.{y}"),
            "toggle": lined(command(path + "/toggle", "Toggle %s on or off." % who), "Toggle Executor {x}.{y}"),
            "fader": lined(leaf(path + "/fader", "i", 3, [0], "The fader of %s, 0 to 100." % who, span(0, 100), None, "strip.level"),
                           "Fader {x}.{y} At {1}"),
            "cue": lined(command(path + "/cue", "Go+ on %s to a cue by number: Go+ Executor {x}.{y} Cue <n>." % who, "i", "scene.recall",
                                 span(1, args.cues)),
                         "Go+ Executor {x}.{y} Cue {1}"),
        })

    pages = numbered("/exec", "The executor pages.", args.pages,
                     lambda pp, p: numbered(pp, "Page %d." % p, args.executors, lambda ep, e: executor(ep, p, e)))

    cues = numbered("/cue", "The cues of the main sequence, by number.", args.cues, lambda cp, c: container(cp, "Cue %d." % c, {
        "goto": lined(command(cp + "/goto", "Goto Cue %d on the main executor." % c, None, "scene.recall"), "Goto Cue {x}"),
    }))

    contents = {
        "cmd": lined(command("/cmd", "A whole command line, as typed on the console.", "s"), "{1}"),
        "exec": pages,
        "cue": cues,
    }

    tree = root("/", "An MA Lighting grandMA2 over its telnet remote on TCP 30000: the executors' Go+, Go-, Pause, Off, "
                     "Toggle and fader by page and number, a cue by number on the main executor, and a whole command "
                     "line. The device's login row holds the line sent first - login <user> <password>. %d pages of %d "
                     "executors, %d cues." % (args.pages, args.executors, args.cues),
                "malighting-grandma2-line", "MA Lighting", "grandMA2, dot2 (telnet)", SOURCES,
                "scripts/presets/malighting_grandma2.py", transport="tcp", wire="line", port=30000, contents=contents)
    write(tree, "malighting-grandma2-line", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
