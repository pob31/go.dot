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

"""Writes presets/devices/adamson-fletcher-osc.json: the Adamson Fletcher Machine over
its own OSC (namespace draft §57, §151).

The source is the OSC page of the Fletcher Machine's quick start, manual version
2.0.261002: every address under /fm, the number of the thing addressed LAST
(/fm/obj/pos/x/3 is object 3's x), positions 0-1 with 0.5 the centre, levels
-84 to +12 dB, angles in degrees. The page gives no port: a device made from
this preset is given the one in the Fletcher Machine's settings / OSC. Its
ADM-OSC grammar (/adm/obj/N/...) is the adm-osc preset.

What the page leaves open, and what this file does about it:
- how many objects, groups and aux sends a unit has: --objects, --groups and
  --aux say (64, 8 and 8 by default); layers and VCAs are written for the
  largest unit (Stage Extended, 12 and 16);
- the angle ranges are written high to low ([180..-180]); taken here as the
  same span written the other way;
- /fm/obj/curve/snapshot has no type: taken as an integer, 0 to 128;
- /fm/snapshot/rewrite's type is given as `t` with the value 1: sent here as the
  integer 1, as every other one-shot on the page is;
- the page's first column, headed "sent by remote", marks some addresses and
  not others; it is not read into the file - every address is sent alike.
/fm/requestall and /fm/obj/request/N are described, but how the answers come
back is not, so the device is not heard back (no READBACK) until a unit shows it.

Run from anywhere: `python3 scripts/presets/adamson_fletcher.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "Adamson, Fletcher Machine manual v2, Quick start - OSC (manual version 2.0.261002, read 28/11/2026): "
    "https://adamson-fletcher-machine.com/manuals/v2/en/quickstart/osc - the OSC table and its four footnotes",
]

UNIT = span(0.0, 1.0)
AZIM = span(-180.0, 180.0)
ELEV = span(-90.0, 90.0)
DB = span(-84.0, 12.0)
MS = span(20.0, 500000.0)
ONOFF = vals(0, 1)
ONCE = vals(1)


def per(path, description, count, make):
    """A parameter written for each of `count` things, the number last: /fm/obj/pos/x/1..count."""
    return numbered(path, description, count, make)


def objects_of(path, description, count, types, value, what, ranges=None, unit=None, role=None):
    """A value leaf per object: `what` is a sentence with %d for the object's number."""
    return per(path, description, count,
               lambda p, n: leaf(p, types, 3, value, what % n, ranges, unit, role))


def once_per(path, description, count, what):
    """A one-shot per thing, sent with the integer 1."""
    return per(path, description, count, lambda p, n: command(p, what % n, "i", None, ONCE))


def position(objects):
    base = "/fm/obj/pos"
    return container(base, "Object positions, by object.", {
        "x": objects_of(base + "/x", "X by object.", objects, "f", [0.5], "Object %d's X, 0 to 1, 0.5 the centre.", UNIT),
        "y": objects_of(base + "/y", "Y by object.", objects, "f", [0.5], "Object %d's Y, 0 to 1, 0.5 the centre.", UNIT),
        "z": objects_of(base + "/z", "Z by object.", objects, "f", [0.5], "Object %d's Z, 0 to 1, 0.5 the centre.", UNIT),
        "xyz": objects_of(base + "/xyz", "X, Y and Z by object.", objects, "fff", [0.5, 0.5, 0.5],
                          "Object %d's X, Y and Z in one message, each 0 to 1.", [UNIT, UNIT, UNIT], None, "object.position"),
        "xy": objects_of(base + "/xy", "X and Y by object.", objects, "ff", [0.5, 0.5],
                         "Object %d's X and Y in one message, each 0 to 1.", [UNIT, UNIT]),
        "azim": objects_of(base + "/azim", "Azimuth by object.", objects, "f", [0.0],
                           "Object %d's azimuth, -180 to 180 degrees.", AZIM, "deg"),
        "elev": objects_of(base + "/elev", "Elevation by object.", objects, "f", [0.0],
                           "Object %d's elevation, -90 to 90 degrees.", ELEV, "deg"),
        "dist": objects_of(base + "/dist", "Distance by object.", objects, "f", [0.0],
                           "Object %d's distance, 0 to 1.", UNIT),
        "aed": objects_of(base + "/aed", "Azimuth, elevation and distance by object.", objects, "fff", [0.0, 0.0, 0.0],
                          "Object %d's azimuth (degrees), elevation (degrees) and distance (0 to 1) in one message.",
                          [AZIM, ELEV, UNIT], None, "object.position"),
    })


def curve(objects):
    base = "/fm/obj/curve"
    return container(base, "Object trajectories drawn as curves, by object.", {
        "snapshot": objects_of(base + "/snapshot", "The curve's snapshot by object.", objects, "i", None,
                               "The snapshot object %d's curve is taken from, 0 to 128 (the page gives no type; an integer here).",
                               span(0, 128)),
        "on": objects_of(base + "/on", "Curve on by object.", objects, "i", [0], "Object %d follows its curve, 1, or not, 0.", ONOFF),
        "pos": objects_of(base + "/pos", "Place on the curve by object.", objects, "f", [0.0],
                          "Where object %d is along its curve, 0 to 1.", UNIT),
        "play": once_per(base + "/play", "Play by object.", objects, "Play object %d's curve."),
        "loop": objects_of(base + "/loop", "Loop by object.", objects, "i", [1], "Object %d's curve loops, 1, or not, 0.", ONOFF),
        "restartloop": once_per(base + "/restartloop", "Restart the loop by object.", objects,
                                "Restart object %d's curve loop from its beginning."),
        "duration": objects_of(base + "/duration", "Curve duration by object.", objects, "f", [15000.0],
                               "How long object %d's curve takes, 20 to 500 000 milliseconds.", MS, "ms"),
        "direction": objects_of(base + "/direction", "Curve direction by object.", objects, "i", [1],
                                "Object %d's curve direction: 0 forward, 1 backward, 2 round trip.", vals(0, 1, 2)),
        "sync": objects_of(base + "/sync", "Curve timing by object.", objects, "i", [0],
                           "What times object %d's curve: 0 its duration, 1 the tempo, 2 timecode.", vals(0, 1, 2)),
        "spline": objects_of(base + "/spline", "Curve shape by object.", objects, "i", [10],
                             "How object %d's curve joins its points: 2 linear, 10 spline, 1 point to point.", vals(1, 2, 10)),
        "pause": objects_of(base + "/pause", "Curve paused by object.", objects, "i", [0],
                            "Object %d's curve paused, 1, or running, 0.", ONOFF),
        "beat": objects_of(base + "/beat", "Curve length in beats by object.", objects, "i", [6],
                           "Object %d's curve length when timed by the tempo: 0 a 16th, 1 a triplet, 2 an 8th, 3 a beat, "
                           "4 two beats, 5 three beats, and so on to 18, sixteen beats.", span(0, 18)),
    })


def jitter(objects):
    base = "/fm/obj/jitter"
    return container(base, "Random movement about the position, by object.", {
        "on": objects_of(base + "/on", "Jitter on by object.", objects, "i", [0], "Object %d's jitter on, 1, or off, 0.", ONOFF),
        "interval": objects_of(base + "/interval", "Jitter interval by object.", objects, "f", [100.0],
                               "Time between object %d's jitter moves, 20 to 500 000 milliseconds.", MS, "ms"),
        "amplitude": objects_of(base + "/amplitude", "Jitter amplitude by object.", objects, "f", [0.0],
                                "How far object %d's jitter moves it, 0 off to 1, 100 m (the stage size).", UNIT),
        "3dmode": objects_of(base + "/3dmode", "Jitter in two or three dimensions by object.", objects, "i", [0],
                             "Object %d's jitter in 2D, 0, or 3D, 1.", ONOFF),
        "transition": objects_of(base + "/transition", "Jitter transition by object.", objects, "i", [0],
                                 "Object %d's jitter jumps, 0, or glides, 1.", ONOFF),
        "density": objects_of(base + "/density", "Jitter density by object.", objects, "f", [1.0],
                              "Object %d's jitter density, 0 to 1.", UNIT),
        "sync": objects_of(base + "/sync", "Jitter timing by object.", objects, "i", [0],
                           "What times object %d's jitter: 0 its interval, 1 the tempo.", ONOFF),
        "beat": objects_of(base + "/beat", "Jitter beat by object.", objects, "i", [2],
                           "Object %d's jitter step when timed by the tempo: 0 a 32nd, 1 a 24th, 2 a 16th, 3 a triplet, "
                           "4 an 8th, 5 a beat.", span(0, 5)),
    })


def render(objects):
    base = "/fm/obj/render"
    return container(base, "Rendering, by object.", {
        "mode": objects_of(base + "/mode", "Delay rendering by object.", objects, "i", [0],
                           "Object %d's rendering: 0 minimum delay, 1 full delay, 2 no delay.", vals(0, 1, 2)),
        "width": objects_of(base + "/width", "Rendering width by object.", objects, "f", [1.0],
                            "Object %d's rendering width, 0 to 2.", span(0.0, 2.0)),
        "distatt": container(base + "/distatt", "Distance attenuation, by object.", {
            "on": objects_of(base + "/distatt/on", "Distance attenuation on by object.", objects, "i", [0],
                             "Object %d's distance attenuation on, 1, or off, 0.", ONOFF),
            "coeff": objects_of(base + "/distatt/coeff", "Distance attenuation amount by object.", objects, "f", [1.0],
                                "Object %d's distance attenuation, 0 to 1 for 0 to 6 dB.", UNIT),
        }),
        "atmosabs": container(base + "/atmosabs", "Atmospheric absorption, by object.", {
            "on": objects_of(base + "/atmosabs/on", "Atmospheric absorption on by object.", objects, "i", [0],
                             "Object %d's atmospheric absorption on, 1, or off, 0.", ONOFF),
            "coeff": objects_of(base + "/atmosabs/coeff", "Atmospheric absorption amount by object.", objects, "f", [1.0],
                                "Object %d's atmospheric absorption, 0 to 1 for 0 to 100 %%.", UNIT),
        }),
    })


def sends(path, description, objects, default_level, what, role=None, master=True, master_on=0):
    """A reverb's, an aux's or a layer's sends by object, and (but for a layer) its master level and on."""
    contents = {
        "obj": container(path + "/obj", description + " sends, by object.", {
            "level": objects_of(path + "/obj/level", "Send levels by object.", objects, "f", [default_level],
                                "Object %%d's send to %s, -84 to +12 dB." % what, DB, "dB", role),
            "on": objects_of(path + "/obj/on", "Sends on by object.", objects, "i", [0],
                             "Object %%d's send to %s on, 1, or off, 0." % what, ONOFF),
        }),
    }
    if master:
        contents["level"] = leaf(path + "/level", "f", 3, [0.0], "%s's master level, -84 to +12 dB." % what.capitalize(), DB, "dB")
        contents["on"] = leaf(path + "/on", "i", 3, [master_on], "%s on, 1, or off, 0." % what.capitalize(), ONOFF)
    return container(path, description + ".", contents)


def group(groups):
    base = "/fm/group"
    return container(base, "Group trajectories, by group.", {
        "pos": container(base + "/pos", "Group positions.", {
            "xyz": objects_of(base + "/pos/xyz", "X, Y and Z by group.", groups, "fff", [0.5, 0.5, 0.5],
                              "Group %d's X, Y and Z in one message, each 0 to 1.", [UNIT, UNIT, UNIT]),
        }),
        "curve": container(base + "/curve", "Group curves.", {
            "on": objects_of(base + "/curve/on", "Curve on by group.", groups, "i", [0], "Group %d follows its curve, 1, or not, 0.", ONOFF),
            "pos": objects_of(base + "/curve/pos", "Place on the curve by group.", groups, "f", [0.0],
                              "Where group %d is along its curve, 0 to 1.", UNIT),
            "play": once_per(base + "/curve/play", "Play by group.", groups, "Play group %d's curve."),
            #  The page writes this one /fm/group/curve/loop/1 where its siblings are 1..N: taken as the same run.
            "loop": objects_of(base + "/curve/loop", "Loop by group.", groups, "i", [0], "Group %d's curve loops, 1, or not, 0.", ONOFF),
            "duration": objects_of(base + "/curve/duration", "Curve duration by group.", groups, "f", [15000.0],
                                   "How long group %d's curve takes, 20 to 500 000 milliseconds.", MS, "ms"),
            "direction": objects_of(base + "/curve/direction", "Curve direction by group.", groups, "i", [0],
                                    "Group %d's curve direction: 0 forward, 1 backward, 2 round trip.", vals(0, 1, 2)),
        }),
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--objects", type=int, default=64, help="how many objects to describe (64)")
    parser.add_argument("--groups", type=int, default=8, help="how many groups to describe (8)")
    parser.add_argument("--aux", type=int, default=8, help="how many aux sends to describe (8)")
    parser.add_argument("--layers", type=int, default=12, help="how many layers (12, Stage Extended's)")
    parser.add_argument("--vcas", type=int, default=16, help="how many VCAs (16, Stage Extended's)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    n = args.objects

    obj = container("/fm/obj", "The objects.", {
        "pos": position(n),
        "space": objects_of("/fm/obj/space", "Space by object.", n, "i", [0], "The space object %d is in, 0 to 4.", span(0, 4)),
        "tracking": objects_of("/fm/obj/tracking", "Tracking by object.", n, "i", [1], "Object %d follows tracking, 1, or not, 0.", ONOFF),
        "pair": container("/fm/obj/pair", "Stereo pairs, by object.", {
            "spread": objects_of("/fm/obj/pair/spread", "Pair spread by object.", n, "f", [1.0], "Object %d's pair spread, 0 to 2.", span(0.0, 2.0)),
            "on": objects_of("/fm/obj/pair/on", "Paired by object.", n, "i", [0], "Object %d paired, 1, or not, 0.", ONOFF),
        }),
        "level": objects_of("/fm/obj/level", "Level by object.", n, "f", [-84.0], "Object %d's level, -84 to +12 dB.", DB, "dB", "object.gain"),
        "on": objects_of("/fm/obj/on", "On by object.", n, "i", [0], "Object %d on, 1, or off, 0.", ONOFF),
        "traj": container("/fm/obj/traj", "Trajectories, by object.", {
            "on": objects_of("/fm/obj/traj/on", "Trajectory on by object.", n, "i", [0], "Object %d's trajectory on, 1, or off, 0.", ONOFF),
        }),
        "curve": curve(n),
        "jitter": jitter(n),
        "render": render(n),
        "name": objects_of("/fm/obj/name", "Name by object.", n, "s", [""], "Object %d's name, in ASCII."),
        "request": once_per("/fm/obj/request", "Ask for an object's parameters.", n, "Ask for object %d's parameters."),
    })

    contents = {
        "obj": obj,
        "coordmode": leaf("/fm/coordmode", "i", 3, [0], "The coordinates positions are read in: 0 cartesian, 1 spherical.", ONOFF),
        "group": group(args.groups),
        "tempo": leaf("/fm/tempo", "f", 3, [120.0], "The tempo, 20 to 400 beats a minute.", span(20.0, 400.0)),
        "taptempo": command("/fm/taptempo", "Tap the tempo.", "i", None, ONCE),
        "restarttraj": command("/fm/restarttraj", "Restart every trajectory.", "i", None, ONCE),
        "restartsync": command("/fm/restartsync", "Restart the synchronisation.", "i", None, ONCE),
        "global": container("/fm/global", "Global settings.", {
            "speedtraj": leaf("/fm/global/speedtraj", "f", 3, [1.0], "The speed of every trajectory, 0 to 1.", UNIT),
        }),
        "snapshot": container("/fm/snapshot", "Snapshots.", {
            "num": command("/fm/snapshot/num", "Recall snapshot 1 to 256; a fraction such as 5.4 goes part of the way between two.",
                           "f", "scene.recall", span(1.0, 256.0)),
            "next": command("/fm/snapshot/next", "Recall the next snapshot.", "i", "scene.next", ONCE),
            "previous": command("/fm/snapshot/previous", "Recall the previous snapshot.", "i", "scene.previous", ONCE),
            "first": command("/fm/snapshot/first", "Recall the first snapshot.", "i", None, ONCE),
            "name": leaf("/fm/snapshot/name", "s", 3, [""], "The snapshot's name, in ASCII."),
            "rewrite": command("/fm/snapshot/rewrite", "Write the current state over the snapshot (the page gives the type t; sent as the integer 1).",
                               "i", None, ONCE),
        }),
        "rev": numbered("/fm/rev", "The reverbs, 1 to 3; 2 and 3 on the Stage engine only.", 3,
                        lambda p, k: sends(p, "Reverb %d" % k + ("" if k == 1 else ", Stage engine only"), n, -84.0,
                                           "reverb %d" % k, "send.level")),
        "aux": numbered("/fm/aux", "The aux sends.", args.aux,
                        lambda p, k: sends(p, "Aux %d" % k, n, -84.0, "aux %d" % k, "send.level", master_on=1)),
        "layer": numbered("/fm/layer", "The layers: 4 on a Traveler Entry, 6 Full, 8 Stage Standard, 12 Stage Extended.", args.layers,
                          lambda p, k: sends(p, "Layer %d" % k, n, 0.0, "layer %d" % k, None, master=False)),
        "vca": numbered("/fm/vca", "The VCAs: 8 on the Travelers and Stage Standard, 16 Stage Extended.", args.vcas,
                        lambda p, k: container(p, "VCA %d." % k, {
                            "level": leaf(p + "/level", "f", 3, [0.0], "VCA %d's level, -84 to +12 dB." % k, DB, "dB", "dca.level"),
                            "on": leaf(p + "/on", "i", 3, [1], "VCA %d on, 1, or off, 0." % k, ONOFF),
                        })),
        "master": container("/fm/master", "The master.", {
            "level": leaf("/fm/master/level", "f", 3, [-84.0], "The master level, -84 to +12 dB.", DB, "dB", "master.level"),
            "on": leaf("/fm/master/on", "i", 3, [1], "The master on, 1, or off, 0.", ONOFF),
        }),
        "listener": container("/fm/listener", "The listener of the binaural mixdown.", {
            "xyz": leaf("/fm/listener/xyz", "fff", 3, [0.5, 0.5, 0.5], "The listener's X, Y and Z, each 0 to 1.", [UNIT, UNIT, UNIT]),
            "ypr": leaf("/fm/listener/ypr", "fff", 3, [0.0, 0.0, 0.0], "The listener's yaw, pitch and roll, each -180 to 180 degrees.",
                        [AZIM, AZIM, AZIM], "deg"),
        }),
        "osc": container("/fm/osc", "The OSC grammar.", {
            "format": leaf("/fm/osc/format", "i", 3, [0], "The OSC format the Fletcher Machine speaks: 0 AFM, 1 ADM, 2 DiGiCo, 3 User.",
                           vals(0, 1, 2, 3)),
        }),
        "requestall": command("/fm/requestall", "Ask for every selected parameter.", "i", None, ONCE),
    }

    tree = root("/fm", "The Adamson Fletcher Machine over its own OSC (the AFM format): %d objects' positions, levels, trajectories, "
                       "jitter, rendering and sends, %d groups, the reverbs, %d aux sends, %d layers, %d VCAs, the master, "
                       "snapshots and the binaural listener. The page names no port: give the one in its settings / OSC. "
                       "Its ADM-OSC grammar is the adm-osc preset." % (n, args.groups, args.aux, args.layers, args.vcas),
                "adamson-fletcher-osc", "Adamson", "Fletcher Machine (OSC)", SOURCES, "scripts/presets/adamson_fletcher.py",
                transport="udp", port=0, contents=contents)
    write(tree, "adamson-fletcher-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
