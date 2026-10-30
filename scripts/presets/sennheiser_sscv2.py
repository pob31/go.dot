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

"""Writes presets/devices/sennheiser-spectera.json: a Sennheiser Spectera base
station over the Sound Control Protocol, version 2 - REST and JSON over HTTPS,
its changes told as Server-Sent Events (namespace draft §124, BET).

The address under the device's prefix is the resource's path and the field:
`/api/mts/paired/all/<mtUid>/name` is the field `name` of one paired pack; a
write is a PUT of the resource with that field alone, which SSCv2 2.3 allows
("PUT {"gain": -10} changes only gain"). The base station numbers its packs,
inputs and outputs itself, so those parts are OPEN: an address under them is
sent as typed and heard as the JSON types it. The two RF channels, the site and
the state are described. The root's SUBSCRIBE lists the resources watched -
each answered with its values when the session adds it - and IDS names the
field each collection's elements are known by.

The login row holds `controlSennheiser <password>`, the password set in Control
Cockpit when third-party access is turned on; the pin row the certificate's
fingerprint, trusted once it is shown.

Run from anywhere: `python3 scripts/presets/sennheiser_sscv2.py`.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import container, leaf, root, span, vals, write  # noqa: E402

SOURCES = [
    "Sennheiser, Sennheiser Sound Control (SSCv2) Specification, version 2.3 (docs.cloud.sennheiser.com): HTTPS and "
    "HTTP/1.1 only, the subscription - GET /api/ssc/state/subscriptions as Server-Sent Events, the open event's "
    "sessionUUID, PUT .../{sessionUUID}/add with a JSON array of paths, the initial values reported on add, "
    "notifications keyed by resource path - and PUT's partial, transactional updates",
    "Sennheiser, 3rd party API documentation as docs.cloud.sennheiser.com lists it (Spectera OpenAPI 18.1, 07/2026; the "
    "instruction manual 08/2025): third-party access off until enabled in Control Cockpit with a password. The "
    "OpenAPI file itself was not read - the field names below are to be checked against it, or a base station, at JTSE",
    "cross-read, not copied: bitfocus/companion-module-sennheiser-spectera (MIT; Sennheiser's own), its API client and "
    "types: Basic authentication as controlSennheiser, the resource paths it watches, the fields of a pack, an RF "
    "channel, the site and the state",
]

WATCHED = [
    "/api/device/identity", "/api/device/state", "/api/device/site",
    "/api/rf/channels", "/api/rf/antennas", "/api/mts/paired/all",
    "/api/audio/inputs", "/api/audio/outputs",
    "/api/health/psu", "/api/health/tempstateoverall",
]

IDS = {
    "/api/rf/channels": "rfChannelId",
    "/api/rf/antennas": "antennaPortId",
    "/api/mts/paired/all": "mtUid",
    "/api/audio/inputs": "inputId",
    "/api/audio/outputs": "outputId",
    "/api/audio/links": "audiolinkId",
}

PACK_FIELDS = ("name, type (SEK or SKM), state (Connected, Disconnected, Syncing, Sleeping, Updating), "
               "batteryFillLevel (per cent), batteryRuntime (minutes), batteryLow, rssi, micLqi, interference/severity, "
               "sleep, identify, micPreampGain, micLowCutHz, headphoneVolume, headphoneBalance, rfChannelId")


def opened(path, description):
    node = container(path, description, {})
    node["GODOT"] = {"OPEN": True}
    return node


def spectera():
    P = "/spectera/api"

    def rf_channel(n):
        base = "%s/rf/channels/%d" % (P, n)
        return container(base, "RF channel %d (rfChannelId %d)." % (n + 1, n), {
            "frequency": leaf(base + "/frequency", "i", 3, None, "The channel's frequency in kHz, in steps of 1000.",
                              span(10000, 10000000), "kHz"),
            "txPower": leaf(base + "/txPower", "i", 3, None, "The transmit power in mW: 10, 20, 30, 50 or 100.",
                            span(10, 100), "mW"),
            "bandwidthMode": leaf(base + "/bandwidthMode", "i", 3, None, "The bandwidth in kHz: 6000, 8000 or 10000, as "
                                  "the country's licence allows.", span(6000, 10000), "kHz"),
            "rfState": leaf(base + "/rfState", "s", 3, None, "RF on air or muted.", vals("RfActive", "RfMuted")),
            "rfChannelId": leaf(base + "/rfChannelId", "i", 1, None, "The channel's identifier.", None),
        })

    contents = {
        "device": container(P + "/device", "The base station.", {
            "identity": container(P + "/device/identity", "What it is.", {
                name: leaf("%s/device/identity/%s" % (P, name), "s", 1, None, "Its %s." % what, None)
                for name, what in (("product", "product"), ("hardwareRevision", "hardware revision"),
                                   ("serial", "serial number"), ("vendor", "vendor"))
            }),
            "state": container(P + "/device/state", "How it is.", {
                "state": leaf(P + "/device/state/state", "s", 1, None, "Its state.",
                              vals("Initializing", "Normal", "FactoryReset", "Standby", "Cooldown", "Warmup",
                                   "CriticalFailure")),
                "warnings": leaf(P + "/device/state/warnings", "s", 1, None, "Its warnings now, one a value.", None),
            }),
            "site": container(P + "/device/site", "Where it is.", {
                name: leaf("%s/device/site/%s" % (P, name), "s", 3, None, "Its %s." % what, None)
                for name, what in (("deviceName", "name"), ("location", "location"), ("position", "position"))
            }),
        }),
        "rf": container(P + "/rf", "The radio.", {
            "channels": container(P + "/rf/channels", "The two RF channels, by rfChannelId.",
                                  {"0": rf_channel(0), "1": rf_channel(1)}),
            "antennas": opened(P + "/rf/antennas", "The antenna ports, by antennaPortId: state, interference."),
        }),
        "mts": container(P + "/mts", "The mobile devices.", {
            "paired": container(P + "/mts/paired", "Those paired.", {
                "all": opened(P + "/mts/paired/all", "Every paired pack - SEK bodypack or SKM handheld - by the mtUid "
                              "the base station gave it; its fields: " + PACK_FIELDS + "."),
            }),
        }),
        "audio": container(P + "/audio", "The audio interfaces.", {
            "inputs": opened(P + "/audio/inputs", "The audio inputs, by inputId: name, inputSource (AoIp, Madi1, Madi2)."),
            "outputs": opened(P + "/audio/outputs", "The audio outputs, by outputId: what is routed to each interface."),
        }),
        "health": opened(P + "/health", "The power supplies and the temperature: psu, tempstateoverall."),
    }

    tree = root("/spectera", "A Sennheiser Spectera base station over the Sound Control Protocol, version 2, HTTPS on 443: "
                             "its packs - name, state, battery, RF, mute and sleep - its two RF channels, its inputs and "
                             "outputs, its site and its state, all watched as they change. Its login row holds "
                             "controlSennheiser and the password set in Control Cockpit; its pin the certificate's "
                             "fingerprint, trusted once shown.",
                "sennheiser-spectera", "Sennheiser", "Spectera base station (SSC v2)", SOURCES,
                "scripts/presets/sennheiser_sscv2.py", transport="https", wire="sscv2", port=443, readback="subscribe",
                contents={"api": container(P, "The base station's REST API.", contents)})
    tree["GODOT"]["SUBSCRIBE"] = json.dumps(WATCHED, separators=(",", ":"))
    tree["GODOT"]["IDS"] = IDS
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    write(spectera(), "sennheiser-spectera", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
