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

"""Writes presets/devices/etc-eos-osc.json: an ETC Eos family console over
OSC on TCP (namespace draft §57, AFN; DP.6).

The source is ETC's Eos Family Operations Manual, its Open Sound Control
chapter: the console takes OSC over TCP on port 3032 framed as OSC 1.0, a
four-byte length before each packet (or SLIP, OSC 1.1, on 3037), and over UDP
on the ports set in its show control settings. Under /eos: a cue fired by its
list and number (/eos/cue/<list>/<cue>/fire), the keys by their names
(/eos/key/go_0, /eos/key/stop...), a submaster's level as a float 0 to 1
and its bump, a macro, a preset or a palette fired by number, a channel's or
a group's level, the command line whole (/eos/cmd) or in parts, the OSC user
this connection acts as, the OSC faders, and /eos/ping. Many take a float as
a button edge, 1 down and 0 up. --lists (4) cue lists with --cues (200) each,
--subs (40), --macros (100), --channels (200), --groups (100), --presets
(50) and --faders (10) banks of 10 bound what is described.

Run from anywhere: `python3 scripts/presets/etc_eos.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "ETC, Eos Family Operations Manual (the Eos family v3 manual, 4wall's copy of ETC's PDF), the Open Sound "
    "Control chapter: OSC TCP format 1.0 with packet-length headers on 3032 and 1.1 with SLIP on 3037, the "
    "implicit and explicit OSC commands (chan, group, sub, cue, key, macro, preset, palettes, fader, user, "
    "cmd, newcmd, ping), the button-edge convention",
    "cross-read, not copied: bitfocus/companion-module-etc-eos (MIT), companion/HELP.md: the two framings "
    "and their ports, the user id, the actions it offers",
]

EDGE = span(0.0, 1.0)

KEYS = ["go_0", "stop", "blackout", "select_active", "clear_cmdline", "enter", "at", "full", "out", "last", "next",
        "chan", "group", "sub", "cue", "macro", "home", "label", "sneak", "release", "goto_cue", "assert", "park",
        "escape", "shift", "data", "level", "time", "live", "blind", "undo", "update", "record"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--lists", type=int, default=4)
    parser.add_argument("--cues", type=int, default=200)
    parser.add_argument("--subs", type=int, default=40)
    parser.add_argument("--macros", type=int, default=100)
    parser.add_argument("--channels", type=int, default=200)
    parser.add_argument("--groups", type=int, default=100)
    parser.add_argument("--presets", type=int, default=50)
    parser.add_argument("--faders", type=int, default=10, help="OSC fader banks, each of 10 faders")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    def fired(path, what, role=None):
        return command(path, "Fire %s; a float is the button edge, 1 down and 0 up, and no argument presses and releases." % what, "f", role, EDGE)

    cue_lists = numbered("/eos/cue", "The cue lists.", args.lists, lambda lp, l: numbered(
        lp, "Cue list %d." % l, args.cues,
        lambda cp, c: container(cp, "Cue %d of list %d." % (c, l), {
            "fire": fired(cp + "/fire", "cue %d of list %d" % (c, l), "scene.recall"),
        })))
    cue_lists["CONTENTS"]["fire"] = command("/eos/cue/fire", "Fire a cue of the active list by number.", "f", "scene.recall", span(0.0, 9999.999))

    keys = container("/eos/key", "The console's keys, by name; a float is the button edge, 1 down and 0 up.", {
        name: command("/eos/key/" + name, "Press the %s key." % name.replace("_", " "), "f",
                      "go" if name == "go_0" else "stop" if name == "stop" else None, EDGE)
        for name in KEYS
    })

    subs = numbered("/eos/sub", "The submasters.", args.subs, lambda p, n: container(p, "Submaster %d." % n, {
        "fire": fired(p + "/fire", "submaster %d's bump" % n),
    }))
    for n in range(1, args.subs + 1):
        sub = subs["CONTENTS"][str(n)]
        sub.update({"TYPE": "f", "ACCESS": 3, "VALUE": [0.0], "RANGE": [span(0.0, 1.0)], "GODOT": {"ROLE": "strip.level"}})
        sub["DESCRIPTION"] = "Submaster %d's level, 0 to 1; or, with /fire under it, its bump." % n
    subs["CONTENTS"]["fire"] = command("/eos/sub/fire", "Bump a submaster by number.", "i", None, span(1, args.subs))

    macros = numbered("/eos/macro", "The macros.", args.macros, lambda p, n: container(p, "Macro %d." % n, {
        "fire": fired(p + "/fire", "macro %d" % n),
    }))
    macros["CONTENTS"]["fire"] = command("/eos/macro/fire", "Run a macro by number.", "i", None, span(1, args.macros))

    def palette(path, word, count):
        node = numbered(path, "The %ss." % word, count, lambda p, n: container(p, "%s %d." % (word.capitalize(), n), {
            "fire": fired(p + "/fire", "%s %d" % (word, n)),
        }))
        node["CONTENTS"]["fire"] = command(path + "/fire", "Recall a %s by number." % word, "i", None, span(1, count))
        return node

    channels = numbered("/eos/chan", "The channels.", args.channels, lambda p, n: leaf(
        p, "f", 3, [0.0], "Channel %d's level, 0 to 100." % n, span(0.0, 100.0), None, "strip.level"))
    groups = numbered("/eos/group", "The groups.", args.groups, lambda p, n: leaf(
        p, "f", 3, [0.0], "Group %d's level, 0 to 100." % n, span(0.0, 100.0)))

    faders = numbered("/eos/fader", "The OSC fader banks, each of ten faders once configured.", args.faders,
                      lambda bp, b: numbered(bp, "Fader bank %d." % b, 10, lambda p, f: leaf(
                          p, "f", 3, [0.0], "Fader %d of bank %d, 0 to 1." % (f, b), span(0.0, 1.0))))

    contents = {
        "cue": cue_lists,
        "key": keys,
        "sub": subs,
        "macro": macros,
        "preset": palette("/eos/preset", "preset", args.presets),
        "ip": palette("/eos/ip", "intensity palette", args.presets),
        "fp": palette("/eos/fp", "focus palette", args.presets),
        "cp": palette("/eos/cp", "color palette", args.presets),
        "bp": palette("/eos/bp", "beam palette", args.presets),
        "chan": channels,
        "group": groups,
        "fader": faders,
        "cmd": command("/eos/cmd", "The command line, whole: \"Chan 1 At 75#\" runs it, without the # it is left typed.", "s"),
        "newcmd": command("/eos/newcmd", "Clear the command line, then this.", "s"),
        "user": leaf("/eos/user", "i", 3, [1], "The OSC user this connection acts as; -1 is the console's current user, 0 the background user.", span(-1, 99)),
        "snap": command("/eos/snap", "Recall a snapshot by number.", "i"),
        "ping": command("/eos/ping", "Ask the console to answer /eos/out/ping with the same arguments."),
    }

    tree = root("/eos", "An ETC Eos family console over OSC on TCP 3032 (OSC 1.0, length-framed; 3037 for SLIP): cues fired by list and "
                        "number, the keys, submasters, macros, presets and palettes, channel and group levels, the command line, the OSC "
                        "user and faders. %d lists of %d cues, %d submasters, %d macros, %d channels." % (args.lists, args.cues, args.subs, args.macros, args.channels),
                "etc-eos-osc", "ETC", "Eos family (OSC over TCP)", SOURCES, "scripts/presets/etc_eos.py",
                transport="tcp", wire="osc", framing="length", port=3032, contents=contents)
    write(tree, "etc-eos-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
