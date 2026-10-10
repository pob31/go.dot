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

"""Writes presets/devices/dbaudio-ds100-osc.json: the d&b Soundscape DS100 and
DS100M over OSC (namespace draft §57, AFN).

The source is d&b's "OSC Protocol document for DS100", DOC05325, version
1.3.10: every address under /dbaudio1 - the coordinate mapping (a sound
object's position in one of four mapping areas, normalised), the positioning
(spread, delay mode), the matrix inputs and outputs (gain, mute, name), the
sound object routing per function group, the En-Space room settings, the
scenes, and the device's own settings, status and errors. The DS100 listens on
UDP 50010 and answers on 50011. The matrix is 64 in and 64 out (S and L), 128
in on the XL: --objects and --outputs write a file for the one at hand,
because a described device refuses an address its file lacks.

Run from anywhere: `python3 scripts/presets/dbaudio_ds100.py [--objects 128]`.
"""

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOURCES = [
    "d&b audiotechnik, OSC Protocol document for DS100, specification DOC05325, version 1.3.10 - "
    "the sections coordinatemapping, positioning, matrixinput, matrixoutput, soundobjectrouting, "
    "matrixsettings, scene, settings, fixed, status, error and device; ports 50010 in, 50011 out",
    "cross-read, not copied: madees/dbaudio-DS100-Chataigne-Module (MIT), for the argument ranges "
    "the module sends",
]


def leaf(path, types, access, value, description, ranges=None, unit=None, role=None, kind=None):
    node = {"FULL_PATH": path, "TYPE": types, "ACCESS": access}
    if value is not None:
        node["VALUE"] = value
    if ranges:
        node["RANGE"] = ranges
    if unit:
        node["UNIT"] = [unit] * len(types)
    node["DESCRIPTION"] = description
    godot = {}
    if role:
        godot["ROLE"] = role
    if kind:
        godot["KIND"] = kind
    if godot:
        node["GODOT"] = godot
    return node


def command(path, description, types=None, role=None):
    node = {"FULL_PATH": path}
    if types:
        node["TYPE"] = types
    node["ACCESS"] = 2
    node["DESCRIPTION"] = description
    if role:
        node["GODOT"] = {"ROLE": role}
    return node


def container(path, description, contents):
    return {"FULL_PATH": path, "DESCRIPTION": description, "CONTENTS": contents}


def unit_range():
    return [{"MIN": 0.0, "MAX": 1.0}]


def db_range():
    return [{"MIN": -120.0, "MAX": 24.0}]


def numbered(path, description, count, make):
    return container(path, description, {str(n): make(path + "/" + str(n), n) for n in range(1, count + 1)})


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--objects", type=int, default=64, help="matrix inputs, the sound objects (64; 128 on an XL)")
    parser.add_argument("--outputs", type=int, default=64, help="matrix outputs (64)")
    parser.add_argument("--areas", type=int, default=4, help="coordinate mapping areas (4)")
    parser.add_argument("--groups", type=int, default=32, help="function groups (32)")
    parser.add_argument("--out", default=os.path.join(REPO, "presets", "devices"), help="the folder to write into")
    args = parser.parse_args()

    def mapping(kind, types, value, description, role=None):
        unit = [{"MIN": 0.0, "MAX": 1.0}] * len(types)
        return numbered("/dbaudio1/coordinatemapping/" + kind, description + " By mapping area, then sound object.", args.areas,
                        lambda area_path, area: numbered(area_path, "Mapping area %d." % area, args.objects,
                                                         lambda p, obj: leaf(p, types, 3, value, description + " Area %d, object %d." % (area, obj),
                                                                             unit, None, role)))

    coordinatemapping = container("/dbaudio1/coordinatemapping", "Sound object positions relative to a mapping area, normalised nought to one.", {
        "source_position_xy": mapping("source_position_xy", "ff", [0.5, 0.5], "The object's X and Y in the area.", "object.position"),
        "source_position_x":  mapping("source_position_x", "f", [0.5], "The object's X in the area."),
        "source_position_y":  mapping("source_position_y", "f", [0.5], "The object's Y in the area."),
        "source_position_z":  mapping("source_position_z", "f", [0.0], "The object's Z in the area."),
        "source_position":    mapping("source_position", "fff", [0.5, 0.5, 0.0], "The object's X, Y and Z in the area."),
    })

    positioning = container("/dbaudio1/positioning", "En-Scene positioning of each sound object.", {
        "source_spread":    numbered("/dbaudio1/positioning/source_spread", "Spread, nought to one, by object.", args.objects,
                                     lambda p, n: leaf(p, "f", 3, [0.0], "Object %d's spread." % n, unit_range())),
        "source_delaymode": numbered("/dbaudio1/positioning/source_delaymode", "Delay mode by object: off, tight, full.", args.objects,
                                     lambda p, n: leaf(p, "i", 3, [0], "Object %d's delay mode: 0 off, 1 tight, 2 full." % n, [{"VALS": [0, 1, 2]}])),
        "source_position_xy": numbered("/dbaudio1/positioning/source_position_xy", "The object's X and Y in the project's own coordinates, in metres.", args.objects,
                                       lambda p, n: leaf(p, "ff", 3, [0.0, 0.0], "Object %d's X and Y in metres." % n, None, "m")),
        "source_position":  numbered("/dbaudio1/positioning/source_position", "The object's X, Y and Z in the project's own coordinates, in metres.", args.objects,
                                     lambda p, n: leaf(p, "fff", 3, [0.0, 0.0, 0.0], "Object %d's X, Y and Z in metres." % n, None, "m")),
    })

    matrixinput = container("/dbaudio1/matrixinput", "The matrix inputs, which are the sound objects.", {
        "gain":              numbered("/dbaudio1/matrixinput/gain", "Input gain in decibels, by input.", args.objects,
                                      lambda p, n: leaf(p, "f", 3, [0.0], "Input %d's gain." % n, db_range(), "dB", "object.gain")),
        "reverbsendgain":    numbered("/dbaudio1/matrixinput/reverbsendgain", "En-Space send in decibels, by input.", args.objects,
                                      lambda p, n: leaf(p, "f", 3, [-120.0], "Input %d's send to En-Space." % n, db_range(), "dB")),
        "mute":              numbered("/dbaudio1/matrixinput/mute", "Mute by input: one or nought.", args.objects,
                                      lambda p, n: leaf(p, "i", 3, [0], "Input %d muted." % n, [{"VALS": [0, 1]}], None, "object.mute")),
        "channelname":       numbered("/dbaudio1/matrixinput/channelname", "The input's name.", args.objects,
                                      lambda p, n: leaf(p, "s", 3, [""], "Input %d's name." % n)),
        "levelmeterpremute": numbered("/dbaudio1/matrixinput/levelmeterpremute", "The input's level before its mute, read back.", args.objects,
                                      lambda p, n: leaf(p, "f", 1, [-120.0], "Input %d's level before the mute, in decibels." % n, None, "dB")),
    })

    matrixoutput = container("/dbaudio1/matrixoutput", "The matrix outputs.", {
        "gain":        numbered("/dbaudio1/matrixoutput/gain", "Output gain in decibels, by output.", args.outputs,
                                lambda p, n: leaf(p, "f", 3, [0.0], "Output %d's gain." % n, db_range(), "dB")),
        "mute":        numbered("/dbaudio1/matrixoutput/mute", "Mute by output: one or nought.", args.outputs,
                                lambda p, n: leaf(p, "i", 3, [0], "Output %d muted." % n, [{"VALS": [0, 1]}])),
        "channelname": numbered("/dbaudio1/matrixoutput/channelname", "The output's name.", args.outputs,
                                lambda p, n: leaf(p, "s", 3, [""], "Output %d's name." % n)),
    })

    soundobjectrouting = container("/dbaudio1/soundobjectrouting", "Each sound object's send into each function group.", {
        "gain": numbered("/dbaudio1/soundobjectrouting/gain", "Gain in decibels, by function group then object.", args.groups,
                         lambda gp, g: numbered(gp, "Function group %d." % g, args.objects,
                                                lambda p, n: leaf(p, "f", 3, [0.0], "Object %d into function group %d, in decibels." % (n, g), db_range(), "dB"))),
        "mute": numbered("/dbaudio1/soundobjectrouting/mute", "Mute, by function group then object.", args.groups,
                         lambda gp, g: numbered(gp, "Function group %d." % g, args.objects,
                                                lambda p, n: leaf(p, "i", 3, [0], "Object %d muted into function group %d." % (n, g), [{"VALS": [0, 1]}]))),
    })

    matrixsettings = container("/dbaudio1/matrixsettings", "En-Space, the room.", {
        "reverbroomid":          leaf("/dbaudio1/matrixsettings/reverbroomid", "i", 3, [0], "The room: nought is off, one to nine the rooms.", [{"VALS": list(range(0, 10))}]),
        "reverbpredelayfactor":  leaf("/dbaudio1/matrixsettings/reverbpredelayfactor", "f", 3, [1.0], "The pre-delay factor.", [{"MIN": 0.2, "MAX": 2.0}]),
        "reverbrearlevel":       leaf("/dbaudio1/matrixsettings/reverbrearlevel", "f", 3, [0.0], "The rear level, in decibels.", [{"MIN": -24.0, "MAX": 24.0}], "dB"),
    })

    scene = container("/dbaudio1/scene", "The scenes.", {
        "recall":       command("/dbaudio1/scene/recall", "Recall a scene by its major and minor index.", "ii", "scene.recall"),
        "next":         command("/dbaudio1/scene/next", "Recall the next scene.", None, "scene.next"),
        "previous":     command("/dbaudio1/scene/previous", "Recall the previous scene.", None, "scene.previous"),
        "sceneindex":   leaf("/dbaudio1/scene/sceneindex", "s", 1, [""], "The scene now recalled, as major.minor; read back."),
        "scenename":    leaf("/dbaudio1/scene/scenename", "s", 1, [""], "The scene's name; read back."),
        "scenecomment": leaf("/dbaudio1/scene/scenecomment", "s", 1, [""], "The scene's comment; read back."),
    })

    device = {
        "settings": container("/dbaudio1/settings", "The device's settings.", {
            "devicename": leaf("/dbaudio1/settings/devicename", "s", 3, [""], "The device's name."),
        }),
        "fixed": container("/dbaudio1/fixed", "What the device is.", {
            "sernr":           leaf("/dbaudio1/fixed/sernr", "s", 1, [""], "The serial number; read back."),
            "firmwareversion": leaf("/dbaudio1/fixed/firmwareversion", "s", 1, [""], "The firmware version; read back."),
        }),
        "status": container("/dbaudio1/status", "How the device is.", {
            "matrixinputcount":         leaf("/dbaudio1/status/matrixinputcount", "i", 1, [64], "How many matrix inputs; read back."),
            "matrixoutputcount":        leaf("/dbaudio1/status/matrixoutputcount", "i", 1, [64], "How many matrix outputs; read back."),
            "audionetworksamplestatus": leaf("/dbaudio1/status/audionetworksamplestatus", "i", 1, [4], "The audio network's sample rate status: 4 is 48 kHz, 6 is 96 kHz; read back."),
        }),
        "error": container("/dbaudio1/error", "The device's errors.", {
            "gnrlerr":   leaf("/dbaudio1/error/gnrlerr", "i", 1, [0], "One when the device has an error; read back."),
            "errortext": leaf("/dbaudio1/error/errortext", "s", 1, [""], "The error, in words; read back."),
        }),
        "device": container("/dbaudio1/device", "The device itself.", {
            "clear": command("/dbaudio1/device/clear", "Clear the whole matrix: every input, output and object. Not undone."),
        }),
    }

    contents = {
        "coordinatemapping": coordinatemapping,
        "positioning": positioning,
        "matrixinput": matrixinput,
        "matrixoutput": matrixoutput,
        "soundobjectrouting": soundobjectrouting,
        "matrixsettings": matrixsettings,
        "scene": scene,
    }
    contents.update(device)

    tree = {
        "FULL_PATH": "/dbaudio1",
        "DESCRIPTION": "d&b Soundscape: a DS100 or DS100M over OSC, %d sound objects, %d outputs, %d mapping areas, %d function groups. "
                       "It listens on UDP 50010 and answers on 50011." % (args.objects, args.outputs, args.areas, args.groups),
        "GODOT": {
            "PRESET": "dbaudio-ds100-osc",
            "VERSION": 1,
            "VENDOR": "d&b audiotechnik",
            "MODEL": "DS100, DS100M (OSC)",
            "TRANSPORT": "udp",
            "WIRE": "osc",
            "PORT": 50010,
            "SOURCES": SOURCES,
            "GENERATED": "scripts/presets/dbaudio_ds100.py",
        },
        "CONTENTS": contents,
    }

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "dbaudio-ds100-osc.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(tree, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    print("wrote %s: %d objects, %d outputs" % (path, args.objects, args.outputs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
