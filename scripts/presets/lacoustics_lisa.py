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

"""Writes presets/devices/lacoustics-lisa-osc.json: the L-ISA Controller over
its own OSC API (namespace draft §57, AFN).

The source is L-Acoustics' "OSC API" page of the L-ISA Controller
documentation (the one attached to bitfocus/companion-module-requests #1343):
the Controller listens on UDP 8880; a source k (1-96) takes pan, width,
distance, elevation and aux send as floats 0-1 at /ext/src/k/p, /w, /d, /e,
/s, the five at once at /pwdes, relative moves at /rp, /rw, /rd, /re, /rs,
the pan spread of a stereo pair at /v and /rv, an FX n's intensity and state
at /fx/n/intensity and /active, solo at /ext/solo/src/k, a static delay at
/ext/delayms/src/k, a snap to the nearest speaker at /ext/spksnap/src/k, the
processing flags under /ext/config/src/k, and the current selection's
relative moves at /ext/selsrc. ADM-OSC is the Controller's other grammar:
the adm-osc preset covers it.

Run from anywhere: `python3 scripts/presets/lacoustics_lisa.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import leaf, command, container, numbered, root, span, vals, write  # noqa: E402

SOURCES = [
    "L-Acoustics, L-ISA Controller documentation, OSC API (references_concepts/osc_api_reference.html, "
    "read 15/12/2023 as attached to bitfocus/companion-module-requests issue 1343): the Parameters table, "
    "Messages from External to Controller - Source parameters, Source Solo, Snap, Delay, Source processing",
]

UNIT = span(0.0, 1.0)
RELATIVE = span(-1.0, 1.0)


def source(path, k):
    fx = numbered(path + "/fx", "The source's effects, 1 to 32.", 32,
                  lambda p, n: container(p, "Effect %d." % n, {
                      "intensity": leaf(p + "/intensity", "f", 3, [0.0], "Effect %d's intensity on source %d, 0 to 1." % (n, k), UNIT),
                      "active": leaf(p + "/active", "i", 3, [0], "Effect %d on or off for source %d." % (n, k), vals(0, 1)),
                  }))
    return container(path, "Source %d." % k, {
        "p": leaf(path + "/p", "f", 3, [0.5], "Pan, 0 the minimum pan to 1 the maximum.", UNIT, None, "object.position"),
        "w": leaf(path + "/w", "f", 3, [0.0], "Width, 0 degrees to the maximum width.", UNIT),
        "d": leaf(path + "/d", "f", 3, [0.15], "Distance, 0 metres to the maximum distance.", UNIT),
        "e": leaf(path + "/e", "f", 3, [0.0], "Elevation, the minimum to the maximum.", UNIT),
        "s": leaf(path + "/s", "f", 3, [0.0], "Aux send level: 0 off, 0.5 is -6 dBFS, 0.707 is -3 dBFS, 1 is 0 dBFS.", UNIT, None, "send.level"),
        "v": leaf(path + "/v", "f", 3, [0.0], "Pan spread, for the left source of a stereo pair.", UNIT),
        "pwdes": leaf(path + "/pwdes", "fffff", 3, [0.5, 0.0, 0.15, 0.0, 0.0], "Pan, width, distance, elevation and aux send in one message.",
                      [UNIT, UNIT, UNIT, UNIT, UNIT], None, "object.position"),
        "rp": command(path + "/rp", "Move the pan by this much, -1 to 1.", "f", None, RELATIVE),
        "rw": command(path + "/rw", "Move the width by this much, -1 to 1.", "f", None, RELATIVE),
        "rd": command(path + "/rd", "Move the distance by this much, -1 to 1.", "f", None, RELATIVE),
        "re": command(path + "/re", "Move the elevation by this much, -1 to 1.", "f", None, RELATIVE),
        "rs": command(path + "/rs", "Move the aux send by this much, -1 to 1.", "f", None, RELATIVE),
        "rv": command(path + "/rv", "Move the pan spread by this much, 0 to 1, for a stereo pair.", "f", None, UNIT),
        "fx": fx,
    })


def config(path, k):
    return container(path, "Source %d's processing." % k, {
        "distatt": container(path + "/distatt", "Distance attenuation.", {
            "gain": leaf(path + "/distatt/gain", "i", 3, [1], "Gain attenuation with distance, on or off.", vals(0, 1)),
            "hf": leaf(path + "/distatt/hf", "i", 3, [1], "HF attenuation with distance, on or off.", vals(0, 1)),
        }),
        "delay": container(path + "/delay", "The input delay.", {
            "enable": leaf(path + "/delay/enable", "i", 3, [0], "The input delay on or off.", vals(0, 1)),
            "mode": leaf(path + "/delay/mode", "s", 3, ["static"], "The delay mode: static or dynamic.", {"VALS": ["static", "dynamic"]}),
        }),
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--sources", type=int, default=96, help="how many sources (96, the Controller's)")
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    ext = container("/ext", "The L-ISA Controller's external control.", {
        "src": numbered("/ext/src", "The sources, 1 to %d." % args.sources, args.sources, source),
        "solo": container("/ext/solo", "Solos.", {
            "src": numbered("/ext/solo/src", "Solo by source.", args.sources,
                            lambda p, k: leaf(p, "i", 3, [0], "Source %d soloed, 1, or not, 0." % k, vals(0, 1))),
        }),
        "delayms": container("/ext/delayms", "Static input delays.", {
            "src": numbered("/ext/delayms/src", "Delay by source, in milliseconds.", args.sources,
                            lambda p, k: leaf(p, "f", 3, [0.0], "Source %d's static input delay, 0 to 200 ms; applied when the delay is static and on." % k, span(0.0, 200.0), "ms")),
        }),
        "spksnap": container("/ext/spksnap", "Snaps to speakers.", {
            "src": numbered("/ext/spksnap/src", "Snap by source.", args.sources,
                            lambda p, k: command(p, "Snap source %d to the nearest speaker." % k)),
        }),
        "config": container("/ext/config", "Source processing.", {
            "src": numbered("/ext/config/src", "Processing by source.", args.sources, config),
        }),
        "selsrc": container("/ext/selsrc", "The sources selected in the Controller, moved together.", {
            "rp": command("/ext/selsrc/rp", "Move the selection's pan by this much, -1 to 1 of the pan range.", "f", None, RELATIVE),
            "rv": command("/ext/selsrc/rv", "Move the selection's pan spread by this much.", "f", None, RELATIVE),
            "rw": command("/ext/selsrc/rw", "Move the selection's width by this much.", "f", None, RELATIVE),
            "rd": command("/ext/selsrc/rd", "Move the selection's distance by this much.", "f", None, RELATIVE),
            "re": command("/ext/selsrc/re", "Move the selection's elevation by this much.", "f", None, RELATIVE),
        }),
    })

    tree = root("/ext", "The L-ISA Controller over its own OSC API, on UDP 8880: %d sources' pan, width, distance, elevation and aux send, "
                        "solos, delays, processing, and the selection. Its ADM-OSC grammar is the adm-osc preset." % args.sources,
                "lacoustics-lisa-osc", "L-Acoustics", "L-ISA Controller (OSC)", SOURCES, "scripts/presets/lacoustics_lisa.py",
                transport="udp", port=8880, contents=ext["CONTENTS"])
    write(tree, "lacoustics-lisa-osc", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
