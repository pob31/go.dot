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

"""Writes presets/devices/sennheiser-spectera.json and sennheiser-ewdx.json: a
Sennheiser Spectera base station, and an EW-DX receiver from firmware 4, over
the Sound Control Protocol, version 2 - REST and JSON over HTTPS, their changes
told as Server-Sent Events (namespace draft §124, BET).

THE EW-DX (its 3rd-party API 1.7) is small and fixed: four channels and four
transmitters numbered 0 to 3, described whole. A channel's frequency is read in
`/api/rf/channels/<n>` and written by a PUT of `/api/rf/channels/<n>/frequency`,
so the node says the resource it is put to (GODOT.SSCV2.RESOURCE) and one node
reads and writes. Its meters are resources of their own, each `{"value": ...}`.

The source is Sennheiser's Spectera OpenAPI 18.3 (SPECTERA API v18.3.0), the
YAML its documentation portal's Swagger page loads. The address under the
device's prefix is the resource's path and the field:
`/api/mts/paired/all/<mtUid>/name` is the field `name` of one paired pack; a
write is a PUT of that resource with the field, partial as SSCv2 2.3 allows -
but every PutSchema requires the element's identifier (inputId, rfChannelId,
mtUid...) and a pack's is one of two, SEK or SKM, picked by `type`. So the
root's IDS names each collection's identifier, which the wire reads from the
address, and CARRY the fields every PUT of an element carries besides - a
pack's `type`, as the base station last said it. The base station numbers its
packs, inputs and outputs itself, so those parts are OPEN; the two RF channels,
the site and the state are described. The root's SUBSCRIBE lists the
resources watched, each answered with its values when the session adds it.

Spectera has no audio mute per pack or per input or output (release notes
18.3, Known Issues): muting is the console's. A pack can be put to sleep
(`sleep`, preliminary in 18.3) and an RF channel muted (`rfState`).

The login row holds `api <password>`: the third-party user the OpenAPI names,
its password the one given when third-party access is allowed in Control
Cockpit, and "no rights to reconfigure vital configurations". The pin row holds
the certificate's fingerprint, trusted once it is shown.

Run from anywhere: `python3 scripts/presets/sennheiser_sscv2.py`.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import container, leaf, root, span, vals, write  # noqa: E402

SOURCES = [
    "Sennheiser, SPECTERA API v18.3.0 (OpenAPI 3.1, Spectera-openapi-api-18.3.yaml, loaded by "
    "docs.cloud.sennheiser.com/en-us/api-docs/spectera/open-api-specification-18-3.html): the paths, the PUT schemas "
    "and their required identifiers (SekPut and SkmPut by the discriminator type), RfChannelPut, the read-only site, "
    "and the three Basic users - initialSennheiser, controlSennheiser, api (third-party access)",
    "Sennheiser, Spectera API Release Notes 18.3: no audio mute per Mobile Device or Audio I/O; sleep preliminary; "
    "the 10 MHz bandwidth not certified",
    "Sennheiser, Sennheiser Sound Control (SSCv2) Specification, version 2.3 (docs.cloud.sennheiser.com): HTTPS and "
    "HTTP/1.1 only, the subscription - GET /api/ssc/state/subscriptions as Server-Sent Events, the open event's "
    "sessionUUID, PUT .../{sessionUUID}/add with a JSON array of paths, the initial values reported on add, "
    "notifications keyed by resource path - and PUT's partial, transactional updates",
    "cross-read, not copied: bitfocus/companion-module-sennheiser-spectera (MIT; Sennheiser's own), its API client: "
    "the resources it watches",
]

WATCHED = [
    "/api/device/identity", "/api/device/state", "/api/device/site",
    "/api/rf/channels", "/api/rf/antennas", "/api/mts/paired/all",
    "/api/audio/inputs", "/api/audio/outputs",
    "/api/health/psu", "/api/health/tempstateoverall",
]

CARRY = {"/api/mts/paired/all": ["type"]}

IDS = {
    "/api/rf/channels": "rfChannelId",
    "/api/rf/antennas": "antennaPortId",
    "/api/mts/paired/all": "mtUid",
    "/api/audio/inputs": "inputId",
    "/api/audio/outputs": "outputId",
    "/api/audio/links": "audiolinkId",
}

PACK_FIELDS = ("name, type (SEK or SKM), state (Connected, Disconnected, Syncing, Sleeping, Updating), "
               "batteryFillLevel (per cent), batteryRuntime (minutes), batteryLow, rssi (-138 to 30 dBm), micLqi (0 to 4), "
               "interference/severity, dominantAntenna, commandState, sleep and identify (writable), micPreampGain "
               "(writable: -6 to 42 dB on an SEK, -10 to 42 on an SKM), micLowCutHz, micTestToneEnabled, micTestToneLevel, "
               "an SEK's headphoneVolume (-100 to 27.5 in halves) and headphoneBalance, rfChannelId")


def opened(path, description):
    node = container(path, description, {})
    node["GODOT"] = {"OPEN": True}
    return node


def spectera():
    P = "/spectera/api"

    def rf_channel(n):
        base = "%s/rf/channels/%d" % (P, n)
        return container(base, "RF channel %d (rfChannelId %d)." % (n + 1, n), {
            "frequency": leaf(base + "/frequency", "i", 3, None, "The channel's frequency in kHz, in steps of 1000, within "
                              "what /api/rf/restrictions allows.", span(10000, 10000000), "kHz"),
            "txPower": leaf(base + "/txPower", "i", 3, None, "The transmit power in mW: 10, 20, 30, 50 or 100.",
                            span(10, 100), "mW"),
            "bandwidthMode": leaf(base + "/bandwidthMode", "i", 3, None, "The bandwidth in kHz: 6000 or 8000 as the "
                                  "licence allows; 10000 is taken and never transmits (18.3, Known Issues).",
                                  span(6000, 10000), "kHz"),
            "rfState": leaf(base + "/rfState", "s", 3, None, "RF on air or muted.", vals("RfActive", "RfMuted")),
            "rfStateOnStartup": leaf(base + "/rfStateOnStartup", "s", 3, None, "RF at power-up: on, muted, or as it was.",
                                     vals("RfActive", "RfMuted", "RfLastState")),
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
                name: leaf("%s/device/site/%s" % (P, name), "s", 1, None, "Its %s, as set in Control Cockpit." % what, None)
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
                             "its packs - name, state, battery, RF and sleep - its two RF channels, its inputs and "
                             "outputs, its site and its state, all watched as they change. It has no audio mute: muting is "
                             "the console's. Its login row holds api and the third-party password set in Control Cockpit; "
                             "its pin the certificate's fingerprint, trusted once shown.",
                "sennheiser-spectera", "Sennheiser", "Spectera base station (SSC v2, API 18.3)", SOURCES,
                "scripts/presets/sennheiser_sscv2.py", transport="https", wire="sscv2", port=443, readback="subscribe",
                contents={"api": container(P, "The base station's REST API.", contents)})
    tree["GODOT"]["SUBSCRIBE"] = json.dumps(WATCHED, separators=(",", ":"))
    tree["GODOT"]["IDS"] = IDS
    tree["GODOT"]["CARRY"] = CARRY
    return tree


EWDX_SOURCES = [
    "Sennheiser, EW-DX 3rd-party API 1.7 (OpenAPI 3.1, EW-DX_openapi_3rdparty_release_1.7.yaml): the paths, their GET "
    "and PUT bodies and ranges, channelId and transmitterId 0 to 3, the third-party user api",
    "Sennheiser, Sennheiser Sound Control (SSCv2) Specification, version 2.3: the subscription and its initial values",
]


def ewdx():
    P = "/ewdx/api"

    def rw(path, types, description, ranges=None, unit=None):
        return leaf(P + path, types, 3, None, description, ranges, unit)

    def ro(path, types, description, ranges=None, unit=None):
        return leaf(P + path, types, 1, None, description, ranges, unit)

    tree = root("/ewdx", "A Sennheiser EW-DX receiver - EM 2, EM 2 Dante, EM 4 Dante - from firmware 4, over the Sound "
                         "Control Protocol, version 2, HTTPS on 443: each channel's name, mute, gain, output level, "
                         "frequency and identify, its meters, its pack's battery and settings, the device's own. Its login "
                         "row holds api and the third-party password; its pin the certificate's fingerprint, trusted once "
                         "shown. Four channels described; an EM 2 has two.",
                "sennheiser-ewdx", "Sennheiser", "EW-DX EM 2, EM 2 Dante, EM 4 Dante (SSC v2, firmware 4, API 1.7)",
                EWDX_SOURCES, "scripts/presets/sennheiser_sscv2.py", transport="https", wire="sscv2", port=443,
                readback="subscribe", contents={"api": container(P, "The receiver's REST API.", {})})

    def place(path, node):
        put(tree, "/api" + path, node)

    for name, what in (("product", "product: EWDX2CHS, EWDX2CHDS or EWDX4CHDS"), ("hardwareRevision", "hardware revision"),
                       ("serial", "serial number"), ("vendor", "vendor")):
        place("/device/identity/" + name, ro("/device/identity/" + name, "s", "Its %s." % what))
    place("/device/state/state", ro("/device/state/state", "s", "Its state.", vals("Normal", "Identifying", "FirmwareUpdate", "Busy")))
    place("/device/state/warnings", ro("/device/state/warnings", "s", "Its warnings now, one a value."))
    for name in ("deviceName", "location", "position"):
        place("/device/site/" + name, ro("/device/site/" + name, "s", "Its %s." % name))
    place("/device/identification/visual", rw("/device/identification/visual", "T", "The device identifying itself."))
    place("/rf/code", ro("/rf/code", "s", "Its frequency range's code, Q1-9 and the like."))
    place("/rf/transmission/mode", rw("/rf/transmission/mode", "s", "Link density or standard.", vals("LinkDensity", "Standard")))
    place("/rf/encryption/enabled", rw("/rf/encryption/enabled", "T", "Encryption on."))

    for c in range(4):
        n = c + 1
        ch = "/channel/%d" % c
        place(ch + "/name", rw(ch + "/name", "s", "Channel %d's name, eight characters." % n))
        place(ch + "/mute", rw(ch + "/mute", "T", "Channel %d's audio output muted." % n))
        node = place(ch + "/gain", rw(ch + "/gain", "i", "Channel %d's gain, -3 to +42 dB in steps of 3." % n, span(-3, 42), "dB"))
        place(ch + "/outputLevel", rw(ch + "/outputLevel", "i", "Channel %d's AF output level, -24 to +18 dB in steps of 6." % n,
                                      span(-24, 18), "dB"))
        place(ch + "/mates", ro(ch + "/mates", "s", "The transmitter linked to channel %d: TX1 to TX4." % n))
        place(ch + "/audio", ro(ch + "/audio", "s", "The output channel %d plays on: Out1 to Out4." % n))
        place(ch + "/warnings", ro(ch + "/warnings", "s", "Channel %d's warnings now, one a value." % n))
        place(ch + "/identify/enabled", rw(ch + "/identify/enabled", "T", "Channel %d identifying itself." % n))
        place(ch + "/signalStrengthIndicator/value", ro(ch + "/signalStrengthIndicator/value", "f",
                                                        "Channel %d's RF level, -107 to 0 dBm." % n, span(-107.0, 0.0), "dBm"))
        place(ch + "/signalQualityIndicator/value", ro(ch + "/signalQualityIndicator/value", "i",
                                                       "Channel %d's RF quality, 0 to 100 per cent." % n, span(0, 100), "%"))
        place(ch + "/diversityIndicator/value", ro(ch + "/diversityIndicator/value", "i",
                                                   "Channel %d's antenna: 0 none, 1 A, 2 B." % n, span(0, 2)))
        place(ch + "/level/value", ro(ch + "/level/value", "f", "Channel %d's audio level, -138.5 to 0 dBFS." % n,
                                      span(-138.5, 0.0), "dBFS"))

        rf = "/rf/channels/%d" % c
        frequency = rw(rf + "/frequency", "i", "Channel %d's frequency in kHz, in steps of 25; put to its own resource." % n,
                       None, "kHz")
        frequency["GODOT"] = {"SSCV2": {"RESOURCE": "/api" + rf + "/frequency"}}
        place(rf + "/frequency", frequency)
        place(rf + "/presets/type", ro(rf + "/presets/type", "s", "The preset list the frequency came from.",
                                       vals("Factory", "User", "None")))
        place(rf + "/presets/channel", ro(rf + "/presets/channel", "i", "Its channel in that list."))

        sync = "/syncSettings/%d" % c
        place(sync + "/muteConfig", rw(sync + "/muteConfig", "s", "What the pack's mute switch does, synced to it.",
                                       vals("Off", "RfMute", "AfMute")))
        place(sync + "/lowcut", rw(sync + "/lowcut", "s", "The pack's low cut, synced to it.",
                                   vals("Off", "30Hz", "60Hz", "80Hz", "100Hz", "120Hz")))
        place(sync + "/trim", rw(sync + "/trim", "i", "The pack's trim, -12 to +6 dB, synced to it.", span(-12, 6), "dB"))
        place(sync + "/lock", rw(sync + "/lock", "T", "The pack's keys locked, synced to it."))

        tx = "/transmitters/%d" % c
        place(tx + "/battery/type", ro(tx + "/battery/type", "s", "Transmitter %d's power." % n,
                                       vals("Battery", "PrimaryCell", "NoBattery")))
        place(tx + "/battery/gauge", ro(tx + "/battery/gauge", "i", "Transmitter %d's battery, per cent." % n, span(0, 100), "%"))
        place(tx + "/battery/lifetime", ro(tx + "/battery/lifetime", "i", "Transmitter %d's battery, minutes left." % n,
                                           None, "min"))
        place(tx + "/warnings", ro(tx + "/warnings", "s", "Transmitter %d's warnings now, one a value." % n))
        for field, what in (("type", "model"), ("name", "name"), ("mute", "mute switch"), ("trim", "trim"),
                            ("lowcut", "low cut"), ("lock", "lock"), ("capsule", "capsule")):
            types = "T" if field in ("mute", "lock") else "i" if field == "trim" else "s"
            place(tx + "/" + field, ro(tx + "/" + field, types, "Transmitter %d's %s, as it says." % (n, what)))

    watched = ["/api/device/state", "/api/device/site", "/api/device/identity"]
    for c in range(4):
        watched += ["/api/channel/%d" % c, "/api/channel/%d/warnings" % c, "/api/rf/channels/%d" % c,
                    "/api/channel/%d/signalStrengthIndicator" % c, "/api/channel/%d/signalQualityIndicator" % c,
                    "/api/channel/%d/diversityIndicator" % c, "/api/channel/%d/level" % c,
                    "/api/transmitters/%d" % c, "/api/transmitters/%d/battery" % c, "/api/transmitters/%d/warnings" % c]
    tree["GODOT"]["SUBSCRIBE"] = json.dumps(watched, separators=(",", ":"))
    return tree


def put(tree, path, node):
    """Place `node` at `path` under the root `tree`, each container's FULL_PATH under the root's."""
    here = tree
    parts = path.strip("/").split("/")
    built = tree["FULL_PATH"]
    for name in parts[:-1]:
        built += "/" + name
        here = here.setdefault("CONTENTS", {}).setdefault(name, {"FULL_PATH": built})
    here.setdefault("CONTENTS", {})[parts[-1]] = node
    return node


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()
    write(spectera(), "sennheiser-spectera", args.out)
    write(ewdx(), "sennheiser-ewdx", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
