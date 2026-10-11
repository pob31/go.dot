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

"""Writes presets/devices/allenheath-sq-midi.json: an Allen & Heath SQ over
MIDI on TCP 51325 (namespace draft §57, AFJ; DP.9).

The source is Allen & Heath's "SQ MIDI Protocol, Issue 5": every mute, level,
pan and assignment is an NRPN whose 14-bit parameter number names the source
and the destination - BN 63 MSB, BN 62 LSB, then BN 06 and BN 26 the value
coarse and fine - on the SQ's own MIDI channel (the device's midiChannel
row). A mute or an assignment is 00 01 for on and 00 00 for off; a level is
a 14-bit value on the NRPN Fader Law the console is set to, 0 dB being 76 5C
on the linear taper (the document's example tables), -inf 00 00, +10 dB 7F
7F; a scene is a Bank Select then a Program Change, 128 scenes a bank. The
parameter numbers are read off the document's reference tables, which are
arithmetic: inputs to LR from 40 00 a channel apart, inputs to aux n from
40 44 twelve apart, groups and FX returns likewise, the master sends to
matrices three apart, the pan numbers the level numbers with 10 added to
the MSB, the assignment numbers with 20 added.

Run from anywhere: `python3 scripts/presets/allenheath_sq.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, leaf, midi, numbered, root, span, write  # noqa: E402

SOURCES = [
    "Allen & Heath, SQ MIDI Protocol Issue 5 (allen-heath.com, 2023): section 3.1 Scene change, 3.3 Mutes, 3.4 "
    "Levels and the NRPN Fader Law, 3.5 Panning/Balance, 3.6 Mix Assignments, 3.7 Getting values; section 4 the "
    "reference tables - Mute, Level, Panning/Balance and Assignment Parameter Numbers for inputs, groups, FX "
    "returns, FX sends, master sends and outputs - from which the numbers here are computed",
]


def pair(number):
    """A 14-bit parameter number as its MSB and LSB."""
    return (number >> 7) & 0x7F, number & 0x7F


def nrpn14(path, description, number, role=None, kind_text="i", default=None, rng=None, on=None, off=None):
    msb, lsb = pair(number)
    node = leaf(path, kind_text, 3, default if default is not None else ([False] if kind_text == "T" else [0]),
                description + " (NRPN %02X %02X)." % (msb, lsb), rng, None, role)
    shape = {"kind": "nrpn", "msb": msb, "lsb": lsb, "bits": 14}
    if on is not None:
        shape["on"] = on
        shape["off"] = off
    return midi(node, **shape)


LEVEL = span(0, 16383)
PAN = span(0, 16383)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--inputs", type=int, default=48)
    parser.add_argument("--groups", type=int, default=12)
    parser.add_argument("--auxes", type=int, default=12)
    parser.add_argument("--fx", type=int, default=4, help="FX sends (4) - the returns are 8")
    parser.add_argument("--matrices", type=int, default=3)
    parser.add_argument("--dcas", type=int, default=8)
    parser.add_argument("--mute-groups", type=int, default=8)
    parser.add_argument("--scenes", type=int, default=300)
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    inputs, groups, auxes, fx, matrices = args.inputs, args.groups, args.auxes, args.fx, args.matrices
    returns = 8

    def to_mixes(path, who, base_lr, base_aux, step, base_fx=None, fx_step=4, assign_base=None):
        """A source's level, pan and assignment to LR and each aux, its FX sends, with the table's bases."""
        out = {}
        out["lr"] = {"FULL_PATH": path + "/lr", "DESCRIPTION": "%s to LR." % who, "CONTENTS": {
            "level": nrpn14(path + "/lr/level", "%s's level to LR, 0 to 16383 on the NRPN Fader Law" % who, base_lr, "send.level", rng=LEVEL),
            "pan": nrpn14(path + "/lr/pan", "%s's pan to LR, 0 left to 16383 right, 3F 7F centre" % who, base_lr + (0x10 << 7), rng=PAN),
            "assign": nrpn14(path + "/lr/assign", "%s assigned to LR" % who, base_lr + (0x20 << 7), None, "T", on=1, off=0),
        }}
        if base_aux is not None:
            out["aux"] = numbered(path + "/aux", "%s to the auxes." % who, auxes, lambda ap, a: {
                "FULL_PATH": ap, "DESCRIPTION": "%s to aux %d." % (who, a), "CONTENTS": {
                    "level": nrpn14(ap + "/level", "%s's level to aux %d" % (who, a), base_aux + (a - 1), "send.level", rng=LEVEL),
                    "pan": nrpn14(ap + "/pan", "%s's pan to aux %d" % (who, a), base_aux + (a - 1) + (0x10 << 7), rng=PAN),
                    "assign": nrpn14(ap + "/assign", "%s assigned to aux %d" % (who, a), base_aux + (a - 1) + (0x20 << 7), None, "T", on=1, off=0),
                }})
        if base_fx is not None:
            out["fx"] = numbered(path + "/fx", "%s to the FX sends." % who, fx, lambda fp, f: {
                "FULL_PATH": fp, "DESCRIPTION": "%s to FX send %d." % (who, f), "CONTENTS": {
                    "level": nrpn14(fp + "/level", "%s's level to FX send %d" % (who, f), base_fx + (f - 1), "send.level", rng=LEVEL)}})
        return out

    def input_strip(path, n):
        who = "Input %d" % n
        contents = {"mute": nrpn14(path + "/mute", "%s muted" % who, 0x0000 + (n - 1), "strip.mute", "T", on=1, off=0)}
        contents.update(to_mixes(path, who, 0x2000 + (n - 1), 0x2044 + (n - 1) * 12, 12, 0x2614 + (n - 1) * 4))
        return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}

    def group_strip(path, n):
        who = "Group %d" % n
        contents = {
            "mute": nrpn14(path + "/mute", "%s muted" % who, 0x0030 + (n - 1), "strip.mute", "T", on=1, off=0),
            "level": nrpn14(path + "/level", "%s's output level" % who, 0x2000 + 0x30 + (n - 1), "strip.level", rng=LEVEL),
        }
        contents.update(to_mixes(path, who, 0x2000 + 0x30 + (n - 1), 0x2284 + (n - 1) * 12, 12, 0x26D4 + (n - 1) * 4))
        return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}

    def return_strip(path, n):
        who = "FX return %d" % n
        contents = {"mute": nrpn14(path + "/mute", "%s muted" % who, 0x003C + (n - 1), "strip.mute", "T", on=1, off=0)}
        contents.update(to_mixes(path, who, 0x2000 + 0x3C + (n - 1), 0x2314 + (n - 1) * 12, 12, 0x2704 + (n - 1) * 4))
        return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}

    def output(path, who, mute_number, level_number, role="strip.level", matrix_base=None):
        contents = {
            "mute": nrpn14(path + "/mute", "%s muted" % who, mute_number, "strip.mute", "T", on=1, off=0),
            "level": nrpn14(path + "/level", "%s's level, 0 to 16383" % who, level_number, role, rng=LEVEL),
            "balance": nrpn14(path + "/balance", "%s's balance, 3F 7F centre" % who, level_number + (0x10 << 7), rng=PAN),
        }
        if matrix_base is not None:
            contents["matrix"] = numbered(path + "/matrix", "%s to the matrices." % who, matrices, lambda mp, m: {
                "FULL_PATH": mp, "DESCRIPTION": "%s to matrix %d." % (who, m), "CONTENTS": {
                    "level": nrpn14(mp + "/level", "%s's level to matrix %d" % (who, m), matrix_base + (m - 1), "send.level", rng=LEVEL)}})
        return {"FULL_PATH": path, "DESCRIPTION": who + ".", "CONTENTS": contents}

    contents = {
        "input": numbered("/input", "The input channels.", inputs, input_strip),
        "group": numbered("/group", "The groups.", groups, group_strip),
        "fxreturn": numbered("/fxreturn", "The FX returns.", returns, return_strip),
        "lr": output("/lr", "LR", 0x0044, 0x2780, "master.level", 0x2724),
        "aux": numbered("/aux", "The auxes' outputs.", auxes,
                        lambda p, n: output(p, "Aux %d" % n, 0x0045 + (n - 1), 0x2781 + (n - 1), "strip.level", 0x2727 + (n - 1) * 3)),
        "fxsend": numbered("/fxsend", "The FX sends' masters.", fx,
                           lambda p, n: output(p, "FX send %d" % n, 0x0051 + (n - 1), 0x278D + (n - 1))),
        "matrix": numbered("/matrix", "The matrices' outputs.", matrices,
                           lambda p, n: output(p, "Matrix %d" % n, 0x0055 + (n - 1), 0x2791 + (n - 1))),
        "dca": numbered("/dca", "The DCAs.", args.dcas,
                        lambda p, n: {"FULL_PATH": p, "DESCRIPTION": "DCA %d." % n, "CONTENTS": {
                            "mute": nrpn14(p + "/mute", "DCA %d muted" % n, 0x0100 + (n - 1), "strip.mute", "T", on=1, off=0),
                            "level": nrpn14(p + "/level", "DCA %d's level" % n, 0x27A0 + (n - 1), "dca.level", rng=LEVEL)}}),
        "mutegroup": numbered("/mutegroup", "The mute groups.", args.mute_groups,
                              lambda p, n: {"FULL_PATH": p, "DESCRIPTION": "Mute group %d." % n, "CONTENTS": {
                                  "mute": nrpn14(p + "/mute", "Mute group %d engaged" % n, 0x0200 + (n - 1), "strip.mute", "T", on=1, off=0)}}),
        "scene": {"FULL_PATH": "/scene", "DESCRIPTION": "The scenes.", "CONTENTS": {
            "recall": midi(command("/scene/recall", "Recall a scene by number, 1 to %d: Bank Select for each 128, then the Program "
                                                    "Change; the scene has to exist." % args.scenes, "i", "scene.recall", span(1, args.scenes)),
                           kind="pc", banked=True, start=1)}},
    }

    tree = root("/", "An Allen & Heath SQ over MIDI on TCP 51325: mutes, levels, pans and assignments of the inputs, groups and "
                     "FX returns to LR, the auxes and the FX sends, the outputs and their matrix sends, the DCAs, the mute groups "
                     "and the scene recall, each an NRPN of the console's reference tables; the device's midiChannel row is the "
                     "SQ's MIDI channel. %d inputs, %d groups, %d auxes, %d matrices." % (inputs, groups, auxes, matrices),
                "allenheath-sq-midi", "Allen & Heath", "SQ (MIDI over TCP)", SOURCES, "scripts/presets/allenheath_sq.py",
                transport="tcp", wire="midi", port=51325, contents=contents, readback="midi")
    write(tree, "allenheath-sq-midi", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
