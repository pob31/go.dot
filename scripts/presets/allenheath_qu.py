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

"""Writes presets/devices/allenheath-qu-midi.json: an Allen & Heath Qu-16,
Qu-24, Qu-32, Qu-Pac or Qu-SB over MIDI on TCP 51325 (namespace draft §57,
AFJ; DP.9).

The source is Allen & Heath's "Qu MIDI Protocol V1.9+": the Qu's own MIDI
channel N (the device's midiChannel row); an audio channel is a note number
- inputs 20 to 3F, stereo channels 40 to 42, FX sends 00 to 03, FX returns 08
to 0B, mixes 60 to 66 (mix 5-6, 7-8 and 9-10 one each), LR 67, groups 68 to
6B, matrices 6C and 6D, DCAs 10 to 13, mute groups 50 to 53. A mute is a Note
On at 7F (on) or 3F (off) then a Note Off; a fader NRPN 17 with the level 00
to 7F (62 is 0 dB) and 07 as the data LSB; a pan NRPN 16, 00 to 4A with 25
the centre and the mix as the data LSB; a send level NRPN 20 with the mix
index as the data LSB; an LR assign NRPN 18; a scene a Bank Select and a
Program Change, bank 0 for the Qu's 100.

Run from anywhere: `python3 scripts/presets/allenheath_qu.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "Allen & Heath, Qu MIDI Protocol V1.9+ ISS.2 (allen-heath.com, 2023): the MIDI channel N, the channel numbers "
    "table, Mute control, NRPN Parameter control - Fader 17, Pan 16, LR Assign 18, Mix Assign 55, Mute Group Assign "
    "5C, DCA Assign 40, Mix Pre/Post 50, Send Level 20 - and Scene Recall; Active Sensing over TCP",
]

#  The data LSB a mix is named by in a pan or a send level: mixes 1 to 4
#  singly, then 5-6, 7-8, 9-10 and LR.
MIX_INDEX = {1: 0x00, 2: 0x01, 3: 0x02, 4: 0x03, 5: 0x04, 6: 0x05, 7: 0x06}
MIX_NAMES = {1: "Mix 1", 2: "Mix 2", 3: "Mix 3", 4: "Mix 4", 5: "Mix 5-6", 6: "Mix 7-8", 7: "Mix 9-10"}


def strip(path, who, ch, role_level="strip.level", sends=False, lr_assign=False, dcas=0, mute_groups=0):
    contents = {
        "mute": midi(leaf(path + "/mute", "T", 3, [False], "%s muted: a Note On at 7F, 3F to unmute, then its Note Off." % who,
                          None, None, "strip.mute"),
                     kind="note", note=ch, on=0x7F, off=0x3F, release=True),
        "fader": midi(leaf(path + "/fader", "i", 3, [0x62], "%s's fader, 00 to 7F on the console's law, 62 is 0 dB (NRPN 17)." % who,
                           span(0, 127), None, role_level),
                      kind="nrpn", msb=ch, lsb=0x17, bits=7, fine=0x07),
    }
    if lr_assign:
        contents["lr"] = midi(leaf(path + "/lr", "T", 3, [True], "%s assigned to LR (NRPN 18)." % who),
                              kind="nrpn", msb=ch, lsb=0x18, bits=7, fine=0x07, on=1, off=0)
        contents["pan"] = midi(leaf(path + "/pan", "i", 3, [0x25], "%s's pan to LR, 00 left, 25 centre, 4A right (NRPN 16)." % who,
                                    span(0, 0x4A)),
                               kind="nrpn", msb=ch, lsb=0x16, bits=7, fine=0x07)
    if sends:
        contents["send"] = numbered(path + "/send", "%s's sends to the mixes." % who, 7, lambda sp, m: {
            "FULL_PATH": sp, "DESCRIPTION": "%s to %s." % (who, MIX_NAMES[m]), "CONTENTS": {
                "level": midi(leaf(sp + "/level", "i", 3, [0], "%s's send level to %s, 00 to 7F (NRPN 20)." % (who, MIX_NAMES[m]),
                                   span(0, 127), None, "send.level"),
                              kind="nrpn", msb=ch, lsb=0x20, bits=7, fine=MIX_INDEX[m]),
                "assign": midi(leaf(sp + "/assign", "T", 3, [False], "%s assigned to %s (NRPN 55)." % (who, MIX_NAMES[m])),
                               kind="nrpn", msb=ch, lsb=0x55, bits=7, fine=MIX_INDEX[m], on=1, off=0),
                "pre": midi(leaf(sp + "/pre", "T", 3, [False], "%s's send to %s pre fader, 1, or post, 0 (NRPN 50)." % (who, MIX_NAMES[m])),
                            kind="nrpn", msb=ch, lsb=0x50, bits=7, fine=MIX_INDEX[m], on=1, off=0),
            }})
    if dcas:
        contents["dca"] = numbered(path + "/dca", "%s's DCA assignments." % who, dcas, lambda dp, d: midi(
            leaf(dp, "T", 3, [False], "%s assigned to DCA %d (NRPN 40)." % (who, d)),
            kind="nrpn", msb=ch, lsb=0x40, bits=7, fine=0x07, on=0x40 + d - 1, off=d - 1))
    if mute_groups:
        contents["mutegroup"] = numbered(path + "/mutegroup", "%s's mute group assignments." % who, mute_groups, lambda mp, g: midi(
            leaf(mp, "T", 3, [False], "%s assigned to mute group %d (NRPN 5C)." % (who, g)),
            kind="nrpn", msb=ch, lsb=0x5C, bits=7, fine=0x07, on=0x40 + g - 1, off=g - 1))
    return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inputs", type=int, default=32)
    parser.add_argument("--scenes", type=int, default=100)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    contents = {
        "input": numbered("/input", "The input channels, notes 20 to 3F.", args.inputs,
                          lambda p, n: strip(p, "Input %d" % n, 0x20 + n - 1, sends=True, lr_assign=True, dcas=4, mute_groups=4)),
        "stereo": numbered("/stereo", "The stereo channels, notes 40 to 42.", 3,
                           lambda p, n: strip(p, "Stereo %d" % n, 0x40 + n - 1, sends=True, lr_assign=True, dcas=4, mute_groups=4)),
        "fxreturn": numbered("/fxreturn", "The FX returns, notes 08 to 0B.", 4,
                             lambda p, n: strip(p, "FX return %d" % n, 0x08 + n - 1, sends=True, lr_assign=True, dcas=4, mute_groups=4)),
        "fxsend": numbered("/fxsend", "The FX sends' masters, notes 00 to 03.", 4,
                           lambda p, n: strip(p, "FX send %d" % n, n - 1)),
        "mix": numbered("/mix", "The mix masters: 1 to 4, then 5-6, 7-8 and 9-10, notes 60 to 66.", 7,
                        lambda p, n: strip(p, MIX_NAMES[n], 0x60 + n - 1, dcas=4, mute_groups=4)),
        "lr": strip("/lr", "LR", 0x67, "master.level", dcas=4, mute_groups=4),
        "group": numbered("/group", "The stereo groups 1-2 to 7-8, notes 68 to 6B.", 4,
                          lambda p, n: strip(p, "Group %d-%d" % (2 * n - 1, 2 * n), 0x68 + n - 1, dcas=4, mute_groups=4)),
        "matrix": numbered("/matrix", "The matrices 1-2 and 3-4, notes 6C and 6D.", 2,
                           lambda p, n: strip(p, "Matrix %d-%d" % (2 * n - 1, 2 * n), 0x6C + n - 1)),
        "dca": numbered("/dca", "The DCAs, notes 10 to 13.", 4,
                        lambda p, n: strip(p, "DCA %d" % n, 0x10 + n - 1, "dca.level")),
        "mutegroup": numbered("/mutegroup", "The mute groups' masters, notes 50 to 53.", 4,
                              lambda p, n: {"FULL_PATH": p, "DESCRIPTION": "Mute group %d." % n, "CONTENTS": {
                                  "mute": midi(leaf(p + "/mute", "T", 3, [False], "Mute group %d engaged." % n, None, None, "strip.mute"),
                                               kind="note", note=0x50 + n - 1, on=0x7F, off=0x3F, release=True)}}),
        "scene": {"FULL_PATH": "/scene", "DESCRIPTION": "The scenes.", "CONTENTS": {
            "recall": midi(command("/scene/recall", "Recall a scene by number, 1 to %d: Bank Select 0 then the Program Change." % args.scenes,
                                   "i", "scene.recall", span(1, args.scenes)),
                           kind="pc", banked=True, start=1)}},
    }

    tree = root("/", "An Allen & Heath Qu over MIDI on TCP 51325: every strip's mute and fader, the inputs' pan, LR assign, "
                     "sends with their level, assign and pre/post, DCA and mute group assignments, the mix, group, matrix and "
                     "DCA masters, the mute groups and the scene recall; the device's midiChannel row is the Qu's MIDI "
                     "channel. %d inputs." % args.inputs,
                "allenheath-qu-midi", "Allen & Heath", "Qu (MIDI over TCP)", SOURCES, "scripts/presets/allenheath_qu.py",
                transport="tcp", wire="midi", port=51325, contents=contents)
    write(tree, "allenheath-qu-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
