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

"""Writes presets/devices/dbaudio-ds100-osc.json: the d&b Soundscape DS100
and DS100M over OSC (namespace draft §57, AFN).

The source is d&b's "OSC Protocol document for DS100", DOC05325, version
1.3.0 (26.03.2020), read whole: the DS100 listens on UDP 50010 and answers on
50011; under /dbaudio1 the general settings, the error and status rows, the
matrix inputs (mute, gain, delay, EQ and polarity switches, name, the two
level meters), the matrix nodes (the crosspoints: enable, gain, delay), the
matrix outputs, En-Scene positioning (spread, delay mode, the absolute
position in metres and the position in each of four mapping areas), the
En-Space room (room, pre-delay factor, rear level), the En-Space sends (the
input's reverb send gain, the input matrix by zone, the zone processing),
the device clear, and the scenes. Version 2 of this preset: version 1 was
written from the module's reading and had rows the document does not.

--objects (64) and --outputs (64) write a file for the matrix at hand (128
inputs on an XL); --matrix-nodes adds the 64 by 64 crosspoints, which are
left out by default because they are 16,384 nodes most shows never cue.

Run from anywhere: `python3 scripts/presets/dbaudio_ds100.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "d&b audiotechnik, OSC Protocol document for DS100, specification DOC05325, version 1.3.0 (26.03.2020), "
    "read whole: sections 2.1 (the path), 3.1 to 3.13 (settings, error, status, matrix input, matrix node, "
    "matrix output, En-Scene positioning, En-Space room settings, En-Space input, input matrix, input "
    "processing, device clear, scenes); ports 50010 in and 50011 out",
]


def db(lo, hi):
    return span(float(lo), float(hi))


def switch(path, description):
    return leaf(path, "i", 3, [0], description, vals(0, 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--objects", type=int, default=64, help="matrix inputs, the sound objects (64; 128 on an XL)")
    parser.add_argument("--outputs", type=int, default=64, help="matrix outputs (64)")
    parser.add_argument("--matrix-nodes", action="store_true", help="describe the crosspoints too (inputs x outputs x 4 nodes)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    objects, outputs = args.objects, args.outputs

    def per_input(name, make):
        return numbered("/dbaudio1/matrixinput/" + name, "By matrix input.", objects, make)

    def per_output(name, make):
        return numbered("/dbaudio1/matrixoutput/" + name, "By matrix output.", outputs, make)

    matrixinput = container("/dbaudio1/matrixinput", "The matrix inputs, which are the sound objects.", {
        "mute": per_input("mute", lambda p, n: leaf(p, "i", 3, [0], "Input %d muted, 1, or not, 0." % n, vals(0, 1), None, "object.mute")),
        "gain": per_input("gain", lambda p, n: leaf(p, "f", 3, [0.0], "Input %d's gain, in decibels." % n, db(-120, 24), "dB", "object.gain")),
        "delay": per_input("delay", lambda p, n: leaf(p, "f", 3, [0.0], "Input %d's delay, in milliseconds." % n, span(0.0, 500.0), "ms")),
        "delayenable": per_input("delayenable", lambda p, n: switch(p, "Input %d's delay on, 1, or off, 0." % n)),
        "eqenable": per_input("eqenable", lambda p, n: switch(p, "Input %d's EQ on, 1, or off, 0." % n)),
        "polarity": per_input("polarity", lambda p, n: switch(p, "Input %d's polarity inverted, 1, or not, 0." % n)),
        "channelname": per_input("channelname", lambda p, n: leaf(p, "s", 3, [""], "Input %d's name, up to 31 characters; R1 overwrites it." % n)),
        "levelmeterpremute": per_input("levelmeterpremute", lambda p, n: leaf(p, "f", 1, [-120.0], "Input %d's level before the mute, in decibels; read back." % n, db(-120, 0), "dB")),
        "levelmeterpostmute": per_input("levelmeterpostmute", lambda p, n: leaf(p, "f", 1, [-120.0], "Input %d's level after the mute, in decibels; read back." % n, db(-120, 0), "dB")),
        "reverbsendgain": per_input("reverbsendgain", lambda p, n: leaf(p, "f", 3, [-120.0], "Input %d's send to En-Space, in decibels." % n, db(-120, 24), "dB", "send.level")),
    })

    matrixoutput = container("/dbaudio1/matrixoutput", "The matrix outputs.", {
        "mute": per_output("mute", lambda p, n: switch(p, "Output %d muted, 1, or not, 0." % n)),
        "gain": per_output("gain", lambda p, n: leaf(p, "f", 3, [0.0], "Output %d's gain, in decibels." % n, db(-120, 10), "dB")),
        "delay": per_output("delay", lambda p, n: leaf(p, "f", 3, [0.0], "Output %d's delay, in milliseconds." % n, span(0.0, 500.0), "ms")),
        "delayenable": per_output("delayenable", lambda p, n: switch(p, "Output %d's delay on, 1, or off, 0." % n)),
        "eqenable": per_output("eqenable", lambda p, n: switch(p, "Output %d's EQ on, 1, or off, 0." % n)),
        "polarity": per_output("polarity", lambda p, n: switch(p, "Output %d's polarity inverted, 1, or not, 0." % n)),
        "channelname": per_output("channelname", lambda p, n: leaf(p, "s", 3, [""], "Output %d's name, up to 31 characters; R1 overwrites it." % n)),
        "levelmeterpremute": per_output("levelmeterpremute", lambda p, n: leaf(p, "f", 1, [-120.0], "Output %d's level before the mute; read back." % n, db(-120, 0), "dB")),
        "levelmeterpostmute": per_output("levelmeterpostmute", lambda p, n: leaf(p, "f", 1, [-120.0], "Output %d's level after the mute; read back." % n, db(-120, 0), "dB")),
    })

    def per_input_per_area(name, kind, types, value, description, ranges=None, unit=None, role=None):
        return numbered("/dbaudio1/" + kind + "/" + name, description + " By mapping area, then sound object.", 4,
                        lambda ap, area: numbered(ap, "Mapping area %d." % area, objects,
                                                  lambda p, n: leaf(p, types, 3, value, description + " Area %d, object %d." % (area, n), ranges, unit, role)))

    coordinatemapping = container("/dbaudio1/coordinatemapping", "Sound object positions relative to a mapping area, in the area's own scaling.", {
        "source_position": per_input_per_area("source_position", "coordinatemapping", "fff", [0.5, 0.5, 0.0], "The object's X, Y and Z in the area."),
        "source_position_xy": per_input_per_area("source_position_xy", "coordinatemapping", "ff", [0.5, 0.5], "The object's X and Y in the area.", None, None, "object.position"),
        "source_position_x": per_input_per_area("source_position_x", "coordinatemapping", "f", [0.5], "The object's X in the area."),
        "source_position_y": per_input_per_area("source_position_y", "coordinatemapping", "f", [0.5], "The object's Y in the area."),
    })

    positioning = container("/dbaudio1/positioning", "En-Scene positioning of each sound object.", {
        "source_spread": numbered("/dbaudio1/positioning/source_spread", "Spread by object, 0 to 1 in steps of 0.001.", objects,
                                  lambda p, n: leaf(p, "f", 3, [0.5], "Object %d's spread." % n, span(0.0, 1.0))),
        "source_delaymode": numbered("/dbaudio1/positioning/source_delaymode", "Delay mode by object: 0 off, 1 tight, 2 full.", objects,
                                     lambda p, n: leaf(p, "i", 3, [0], "Object %d's delay mode: 0 off, 1 tight, 2 full." % n, vals(0, 1, 2))),
        "source_position": numbered("/dbaudio1/positioning/source_position", "The object's X, Y and Z from the project origin, in metres.", objects,
                                    lambda p, n: leaf(p, "fff", 3, [0.0, 0.0, 0.0], "Object %d's X, Y and Z in metres." % n, None, "m", "object.position")),
        "source_position_xy": numbered("/dbaudio1/positioning/source_position_xy", "The object's X and Y from the project origin, in metres.", objects,
                                       lambda p, n: leaf(p, "ff", 3, [0.0, 0.0], "Object %d's X and Y in metres." % n, None, "m")),
        "source_position_x": numbered("/dbaudio1/positioning/source_position_x", "The object's X from the project origin, in metres.", objects,
                                      lambda p, n: leaf(p, "f", 3, [0.0], "Object %d's X in metres." % n, None, "m")),
        "source_position_y": numbered("/dbaudio1/positioning/source_position_y", "The object's Y from the project origin, in metres.", objects,
                                      lambda p, n: leaf(p, "f", 3, [0.0], "Object %d's Y in metres." % n, None, "m")),
    })

    matrixsettings = container("/dbaudio1/matrixsettings", "En-Space, the room.", {
        "reverbroomid": leaf("/dbaudio1/matrixsettings/reverbroomid", "i", 3, [0], "The room: 0 is off, 1 to 9 the rooms.", {"VALS": list(range(0, 10))}),
        "reverbpredelayfactor": leaf("/dbaudio1/matrixsettings/reverbpredelayfactor", "f", 3, [1.0], "The pre-delay factor.", span(0.2, 2.0)),
        "reverbrearlevel": leaf("/dbaudio1/matrixsettings/reverbrearlevel", "f", 3, [0.0], "The rear level, in decibels.", db(-24, 24), "dB"),
    })

    reverbinput = container("/dbaudio1/reverbinput", "The En-Space input matrix: each input's send into each zone (1 left, 2 centre, 3 right, 4 the audience).", {
        "gain": numbered("/dbaudio1/reverbinput/gain", "By input, then zone.", objects,
                         lambda ip, n: numbered(ip, "Input %d." % n, 4,
                                                lambda p, z: leaf(p, "f", 3, [-120.0], "Input %d into zone %d, in decibels." % (n, z), db(-120, 24), "dB"))),
    })

    reverbinputprocessing = container("/dbaudio1/reverbinputprocessing", "The En-Space zones' processing (1 left, 2 centre, 3 right, 4 the audience).", {
        "mute": numbered("/dbaudio1/reverbinputprocessing/mute", "By zone.", 4, lambda p, z: switch(p, "Zone %d muted, 1, or not, 0." % z)),
        "gain": numbered("/dbaudio1/reverbinputprocessing/gain", "By zone.", 4,
                         lambda p, z: leaf(p, "f", 3, [0.0], "Zone %d's gain, in decibels." % z, db(-120, 24), "dB")),
        "levelmeter": numbered("/dbaudio1/reverbinputprocessing/levelmeter", "By zone; read back.", 4,
                               lambda p, z: leaf(p, "f", 1, [-120.0], "Zone %d's level, in decibels; read back." % z, db(-120, 0), "dB")),
        "eqenable": numbered("/dbaudio1/reverbinputprocessing/eqenable", "By zone.", 4, lambda p, z: switch(p, "Zone %d's EQ on, 1, or off, 0." % z)),
    })

    scene = container("/dbaudio1/scene", "The scenes, made in R1.", {
        "previous": command("/dbaudio1/scene/previous", "Recall the previous scene.", None, "scene.previous"),
        "next": command("/dbaudio1/scene/next", "Recall the next scene.", None, "scene.next"),
        "recall": command("/dbaudio1/scene/recall", "Recall a scene by its major and minor index (major 0 to 999, minor 0 to 99; the smallest is 0.1). One integer recalls the major alone.",
                          "ii", "scene.recall", [span(0, 999), span(0, 99)]),
        "sceneindex": leaf("/dbaudio1/scene/sceneindex", "s", 1, [""], "The scene recalled, as major.minor; read back."),
        "scenename": leaf("/dbaudio1/scene/scenename", "s", 1, [""], "The scene's name; read back."),
        "scenecomment": leaf("/dbaudio1/scene/scenecomment", "s", 1, [""], "The scene's comment; read back."),
    })

    contents = {
        "settings": container("/dbaudio1/settings", "The device's settings.", {
            "devicename": leaf("/dbaudio1/settings/devicename", "s", 3, [""], "The device's name, up to 15 characters; an R1 project overwrites it."),
        }),
        "error": container("/dbaudio1/error", "The device's error.", {
            "gnrlerr": leaf("/dbaudio1/error/gnrlerr", "i", 1, [0], "One when the device has an error; read back.", vals(0, 1)),
            "errortext": leaf("/dbaudio1/error/errortext", "s", 1, [""], "The error, in words; read back."),
        }),
        "status": container("/dbaudio1/status", "The device's status.", {
            "statustext": leaf("/dbaudio1/status/statustext", "s", 1, [""], "The status, in words; read back."),
        }),
        "matrixinput": matrixinput,
        "matrixoutput": matrixoutput,
        "positioning": positioning,
        "coordinatemapping": coordinatemapping,
        "matrixsettings": matrixsettings,
        "reverbinput": reverbinput,
        "reverbinputprocessing": reverbinputprocessing,
        "device": container("/dbaudio1/device", "The device itself.", {
            "clear": command("/dbaudio1/device/clear", "Reset the device to its factory defaults, except the remote settings. Not undone."),
        }),
        "scene": scene,
    }

    if args.matrix_nodes:
        contents["matrixnode"] = container("/dbaudio1/matrixnode", "The matrix crosspoints, by input then output; a crosspoint used for positioning is disabled.", {
            "enable": numbered("/dbaudio1/matrixnode/enable", "By input, then output.", objects,
                               lambda ip, n: numbered(ip, "Input %d." % n, outputs, lambda p, o: switch(p, "Input %d to output %d enabled." % (n, o)))),
            "gain": numbered("/dbaudio1/matrixnode/gain", "By input, then output.", objects,
                             lambda ip, n: numbered(ip, "Input %d." % n, outputs,
                                                    lambda p, o: leaf(p, "f", 3, [0.0], "Input %d to output %d, in decibels." % (n, o), db(-120, 10), "dB"))),
            "delayenable": numbered("/dbaudio1/matrixnode/delayenable", "By input, then output.", objects,
                                    lambda ip, n: numbered(ip, "Input %d." % n, outputs, lambda p, o: switch(p, "Input %d to output %d: the delay on." % (n, o)))),
            "delay": numbered("/dbaudio1/matrixnode/delay", "By input, then output.", objects,
                              lambda ip, n: numbered(ip, "Input %d." % n, outputs,
                                                     lambda p, o: leaf(p, "f", 3, [0.0], "Input %d to output %d, delay in milliseconds." % (n, o), span(0.0, 500.0), "ms"))),
        })

    crosspoints = "with" if args.matrix_nodes else "without"
    tree = root("/dbaudio1", "d&b Soundscape: a DS100 or DS100M over OSC, %d sound objects and %d outputs, four mapping areas, En-Space, "
                             "the scenes, %s the matrix crosspoints. It listens on UDP 50010 and answers on 50011." % (objects, outputs, crosspoints),
                "dbaudio-ds100-osc", "d&b audiotechnik", "DS100, DS100M (OSC)", SOURCES, "scripts/presets/dbaudio_ds100.py",
                transport="udp", port=50010, version=2, contents=contents)
    write(tree, "dbaudio-ds100-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
