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

"""Writes presets/devices/allenheath-dlive-midi.json (and, with --model
avantis, allenheath-avantis-midi.json): an Allen & Heath dLive over MIDI on
TCP 51325 (namespace draft §57, AFJ; DP.9).

The source is Allen & Heath's "dLive MIDI Over TCP Protocol V1.9": a stream
of MIDI bytes on TCP 51325 (51327 with TLS, not offered here); the base MIDI
channel N is the console's (Utility / Control / MIDI, 1 to 12) and the
device's midiChannel row; an audio channel is picked by the MIDI channel
offset from N and the note number - inputs on N (00-7F), groups on N+1,
auxes on N+2, matrices on N+3, FX sends, FX returns, mains, DCAs and mute
groups on N+4 at their own note ranges. A mute is a Note On at velocity 7F
(on) or 3F (off) followed by a Note Off; a fader is NRPN 17 with the level
00 to 7F on the console's own law; a send level a SysEx under the A&H
header; a scene a Bank Select and a Program Change, 128 scenes a bank. The
MIDI wire renders each node's GODOT.MIDI shape to those bytes, the console's
base channel added.

--model avantis writes the Avantis file: A&H's external control document says
the product lines share the format and the Avantis article names the same
port and base channel; the article itself could not be read when this was
written, so that file is marked unverified until a console has answered it.

Run from anywhere: `python3 scripts/presets/allenheath_dlive.py [--model avantis]`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, midi, numbered, put, root, span, vals, write  # noqa: E402

SOURCES = {
    "dlive": [
        "Allen & Heath, dLive MIDI Over TCP Protocol V1.9 (allen-heath.com, 2023): TCP 51325, the base MIDI "
        "channel N, the channel selection table, the SysEx header F0 00 00 1A 50 10 01 00, Mute ON/OFF, Fader "
        "Level NRPN 17, Channel Assignment to Main NRPN 18, AUX/FX/Matrix Send Level SysEx 0E, DCA and Mute Group "
        "Assignment NRPN 40, Channel Name SysEx 03, Channel Colour SysEx 06, Scene Recall by bank and program",
    ],
    "avantis": [
        "Allen & Heath, Avantis MIDI TCP/IP Protocol (support.allen-heath.com article 45146889174801): TCP 51325, "
        "the base MIDI channel N defaulting to 12; the article could not be read by the generator's author, so the "
        "dLive V1.9 message formats are assumed, as Allen & Heath's AH External control document says the product "
        "lines share them - unverified until a console has answered",
        "Allen & Heath, dLive MIDI Over TCP Protocol V1.9: the formats this file is written to",
    ],
}

HEADER = ["F0", "00", "00", "1A", "50", "10", "01", "00"]


def strip(path, who, offset, ch, role_level="strip.level", with_main=False, dcas=0, mute_groups=0, auxes=0):
    contents = {
        "mute": midi(leaf(path + "/mute", "T", 3, [False], "%s muted: a Note On at 7F, 3F to unmute, then its Note Off." % who,
                          None, None, "strip.mute"),
                     kind="note", note=ch, on=0x7F, off=0x3F, release=True, offset=offset),
        "fader": midi(leaf(path + "/fader", "i", 3, [0], "%s's fader, 00 to 7F on the console's own law (NRPN 17)." % who,
                           span(0, 127), None, role_level),
                      kind="nrpn", msb=ch, lsb=0x17, bits=7, offset=offset),
    }
    if with_main:
        contents["main"] = midi(leaf(path + "/main", "T", 3, [False], "%s assigned to the main mix (NRPN 18)." % who),
                                kind="nrpn", msb=ch, lsb=0x18, bits=7, on=0x7F, off=0x3F, offset=offset)
    if dcas:
        contents["dca"] = numbered(path + "/dca", "%s's DCA assignments." % who, dcas, lambda dp, d: midi(
            leaf(dp, "T", 3, [False], "%s assigned to DCA %d (NRPN 40)." % (who, d)),
            kind="nrpn", msb=ch, lsb=0x40, bits=7, on=0x40 + d - 1, off=d - 1, offset=offset))
    if mute_groups:
        contents["mutegroup"] = numbered(path + "/mutegroup", "%s's mute group assignments." % who, mute_groups, lambda mp, g: midi(
            leaf(mp, "T", 3, [False], "%s assigned to mute group %d (NRPN 40)." % (who, g)),
            kind="nrpn", msb=ch, lsb=0x40, bits=7, on=0x58 + g - 1, off=0x18 + g - 1, offset=offset))
    if auxes:
        contents["send"] = {"FULL_PATH": path + "/send", "DESCRIPTION": "%s's sends." % who, "CONTENTS": {
            "aux": numbered(path + "/send/aux", "To the mono auxes.", auxes, lambda ap, a: midi(
                leaf(ap, "i", 3, [0], "%s's send to aux %d, 00 to 7F (SysEx 0E)." % (who, a), span(0, 127), None, "send.level"),
                kind="sysex", bytes=HEADER + ["N", "0E", "%02X" % ch, "N+2", "%02X" % (a - 1), "V", "F7"], offset=offset))}}
    contents["name"] = midi(leaf(path + "/name", "s", 3, [""], "%s's name (SysEx 03)." % who),
                            kind="sysex", bytes=HEADER + ["N", "03", "%02X" % ch, "S", "F7"], offset=offset)
    contents["colour"] = midi(leaf(path + "/colour", "i", 3, [0], "%s's colour, 0 to 7 (SysEx 06)." % who, span(0, 7)),
                              kind="sysex", bytes=HEADER + ["N", "06", "%02X" % ch, "V", "F7"], offset=offset)
    return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--model", default="dlive", choices=("dlive", "avantis"))
    parser.add_argument("--inputs", type=int, default=64, help="input channels described (64; the console has 128)")
    parser.add_argument("--groups", type=int, default=16, help="mono groups (16; up to 62)")
    parser.add_argument("--auxes", type=int, default=16, help="mono auxes (16; up to 62)")
    parser.add_argument("--matrices", type=int, default=8, help="mono matrices (8; up to 62)")
    parser.add_argument("--fx", type=int, default=8, help="mono FX sends and FX returns (8; up to 16)")
    parser.add_argument("--mains", type=int, default=2, help="mains (2; up to 6)")
    parser.add_argument("--dcas", type=int, default=24)
    parser.add_argument("--mute-groups", type=int, default=8)
    parser.add_argument("--scenes", type=int, default=500)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    slug = "allenheath-%s-midi" % args.model
    model = "dLive" if args.model == "dlive" else "Avantis"

    contents = {
        "input": numbered("/input", "The input channels, on the base channel N, notes 00 to 7F.", args.inputs,
                          lambda p, n: strip(p, "Input %d" % n, 0, n - 1, with_main=True, dcas=args.dcas,
                                             mute_groups=args.mute_groups, auxes=args.auxes)),
        "group": numbered("/group", "The mono groups, on N+1.", args.groups,
                          lambda p, n: strip(p, "Group %d" % n, 1, n - 1, dcas=args.dcas, mute_groups=args.mute_groups)),
        "aux": numbered("/aux", "The mono auxes, on N+2.", args.auxes,
                        lambda p, n: strip(p, "Aux %d" % n, 2, n - 1, dcas=args.dcas, mute_groups=args.mute_groups)),
        "matrix": numbered("/matrix", "The mono matrices, on N+3.", args.matrices,
                           lambda p, n: strip(p, "Matrix %d" % n, 3, n - 1, dcas=args.dcas, mute_groups=args.mute_groups)),
        "fxsend": numbered("/fxsend", "The mono FX sends, on N+4, notes 00 to 0F.", args.fx,
                           lambda p, n: strip(p, "FX send %d" % n, 4, n - 1)),
        "fxreturn": numbered("/fxreturn", "The FX returns, on N+4, notes 20 to 2F.", args.fx,
                             lambda p, n: strip(p, "FX return %d" % n, 4, 0x20 + n - 1, dcas=args.dcas, mute_groups=args.mute_groups)),
        "main": numbered("/main", "The mains, on N+4, notes 30 to 35.", args.mains,
                         lambda p, n: strip(p, "Main %d" % n, 4, 0x30 + n - 1, role_level="master.level")),
        "dca": numbered("/dca", "The DCAs, on N+4, notes 36 to 4D.", args.dcas,
                        lambda p, n: strip(p, "DCA %d" % n, 4, 0x36 + n - 1, role_level="dca.level")),
        "mutegroup": numbered("/mutegroup", "The mute groups' masters, on N+4, notes 4E to 55.", args.mute_groups,
                              lambda p, n: {"FULL_PATH": p, "DESCRIPTION": "Mute group %d." % n, "CONTENTS": {
                                  "mute": midi(leaf(p + "/mute", "T", 3, [False], "Mute group %d engaged." % n, None, None, "strip.mute"),
                                               kind="note", note=0x4E + n - 1, on=0x7F, off=0x3F, release=True, offset=4)}}),
        "scene": {"FULL_PATH": "/scene", "DESCRIPTION": "The scenes.", "CONTENTS": {
            "recall": midi(command("/scene/recall", "Recall a scene by number, 1 to %d: Bank Select for each 128, then the "
                                                    "Program Change, on the base channel." % args.scenes, "i", "scene.recall",
                                   span(1, args.scenes)),
                           kind="pc", banked=True, start=1, offset=0)}},
    }

    tree = root("/", "An Allen & Heath %s over MIDI on TCP 51325: every strip's mute, fader, main assign, DCA and mute group "
                     "assignments, name and colour, the inputs' aux sends, the mute group masters and the scene recall. The "
                     "device's midiChannel row is the console's base channel N. %d inputs, %d groups, %d auxes, %d matrices, "
                     "%d DCAs." % (model, args.inputs, args.groups, args.auxes, args.matrices, args.dcas),
                slug, "Allen & Heath", "%s (MIDI over TCP)" % model, SOURCES[args.model], "scripts/presets/allenheath_dlive.py",
                transport="tcp", wire="midi", port=51325, contents=contents, readback="midi")
    write(tree, slug, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
