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

"""Writes presets/devices/shure-axient-digital.json, shure-ulxd.json and
shure-slxd.json: Shure's wireless receivers over their command strings on TCP
2202 (namespace draft §124, BER).

The sources are Shure's third-party command-string documents, one per range:
every string is `< GET x PARAM >`, `< SET x PARAM value >` or the receiver's
`< REP x PARAM value >`, told on every change, and `< SAMPLE x ALL ... >` its
meters at METER_RATE. The address under the device's prefix spells the string
in its own order - `/1/AUDIO_GAIN`, a slot or an antenna after the parameter,
`/1/SLOT_RF_OUTPUT/3`, `/1/RSSI/2` - and a node's GODOT.SHURE says how its value
is spelled: OFFSET added before a number is sent and taken off one heard (the
gain's 18, so a cue says decibels), BRACES a string in braces (names), PAD the
digits a number is sent in. Where a document gives a value an offset and a
sentinel both (a battery temperature less 40, 255 unknown) the raw number is
kept and the description says how to read it. The root's GODOT.SAMPLE lists a
SAMPLE line's fields per layout, picked by their count; an empty name is a
field no node keeps.

Run from anywhere: `python3 scripts/presets/shure_wireless.py`.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _preset import command, container, leaf, root, span, vals, write  # noqa: E402

COMPANION = ("cross-read, not copied: bitfocus/companion-module-shure-wireless (MIT), its help page: the actions and "
             "feedbacks it offers per range, and that it sends `< GET 0 ALL >` and `< SET 0 METER_RATE ... >` with no "
             "line end and cuts what it reads at `>`")

AD4_SOURCES = [
    "Shure, Axient Digital Command Strings, version 4 (2019-L): TCP 2202; GET, SET, REP, SAMPLE; the device, channel, "
    "metering, side channel and transmitter slot strings, their formats, offsets and unknown values",
    COMPANION,
]
ULXD_SOURCES = [
    "Shure, ULX-D Command Strings, version 1.0 (2022-L): TCP 2202; the device, channel, metering and side channel "
    "strings; RX_RF_LVL less 128, AUDIO_LVL less 50; SAMPLE x ALL nn aaa eee",
    COMPANION,
]
SLXD_SOURCES = [
    "Shure, SLX-D Command Strings, version 2 (2020-G): TCP 2202; the device, channel, metering and side channel "
    "strings; SAMPLE x ALL audPeak audRms rfRssi",
    COMPANION,
]


def shure(node, **spec):
    """The node's spelling on the shure wire: OFFSET, BRACES, PAD."""
    node.setdefault("GODOT", {})["SHURE"] = {key.upper(): value for key, value in spec.items()}
    return node


def node(path, row):
    """One row: (name, type, access, description, ranges, unit, spec, role). Access 2 is a command."""
    _, types, access, description, ranges, unit, spec, role = row
    made = command(path, description, types, role, ranges) if access == 2 \
        else leaf(path, types, access, None, description, ranges, unit, role)
    return shure(made, **spec) if spec else made


def rows_under(prefix, rows, indexed):
    """The rows as nodes under `prefix`; a row named in `indexed` is a container of its indexes."""
    contents = {}
    for row in rows:
        name = row[0]
        path = prefix + "/" + name
        if name in indexed:
            count, what = indexed[name]
            contents[name] = container(path, row[3] + " By %s, from one." % what,
                                       {str(i): node(path + "/%d" % i, row) for i in range(1, count + 1)})
        else:
            contents[name] = node(path, row)
    return contents


def receiver(prefix, description, slug, model, sources, channels, device_rows, channel_rows, indexed, layouts):
    contents = rows_under(prefix, device_rows, {})
    for ch in range(1, channels + 1):
        contents[str(ch)] = container(prefix + "/%d" % ch, "Channel %d." % ch,
                                      rows_under(prefix + "/%d" % ch, channel_rows, indexed))
    tree = root(prefix, description, slug, "Shure", model, sources, "scripts/presets/shure_wireless.py",
                transport="tcp", wire="shure", port=2202, readback="notify", contents=contents)
    tree["GODOT"]["SAMPLE"] = layouts
    return tree


GAIN = ("AUDIO_GAIN", "i", 3, "The channel's audio gain, -18 to +42 dB in steps of one; the receiver counts it from "
        "nought, 18 below, and the wire adds and takes off the 18.", span(-18, 42), "dB", {"offset": 18}, None)
MUTE = ("AUDIO_MUTE", "T", 3, "The channel's audio output muted.", None, None, None, "strip.mute")
FLASH = ("FLASH", "T", 2, "Flash the lights to find it: on, and the receiver stops by itself.", None, None, None, None)
METER = ("METER_RATE", "i", 3, "How often the receiver sends its meters, in milliseconds: nought off, else 100 to "
         "65535. Go.dot asks for 1000 on every opening while the device's Rx is on.", span(0, 65535), "ms",
         {"pad": 5}, None)
BARS = "Battery bars, 0 to 5; 255 unknown - no pack, or one that does not say."
MINS = "Battery minutes left, 0 to 65532; 65533 a battery warning, 65534 still calculating, 65535 unknown."


def axient():
    device = [
        ("DEVICE_ID", "s", 3, "The receiver's name: eight characters set, thirty-one heard.", None, None, {"braces": True}, None),
        ("MODEL", "s", 1, "The model, AD4D or AD4Q and its band.", None, None, {"braces": True}, None),
        ("FW_VER", "s", 1, "The firmware, Maj.Min.Pack.Build; a star after it, a self test failed.", None, None, {"braces": True}, None),
        ("RF_BAND", "s", 1, "The RF band, G55 and the like.", None, None, {"braces": True}, None),
        ("TRANSMISSION_MODE", "s", 1, "STANDARD or HIGH_DENSITY.", vals("STANDARD", "HIGH_DENSITY"), None, None, None),
        ("ENCRYPTION_MODE", "s", 1, "Encryption on or off, ON or OFF.", vals("ON", "OFF"), None, None, None),
        ("QUADVERSITY_MODE", "s", 1, "Quadversity on or off (an AD4Q only), ON or OFF.", vals("ON", "OFF"), None, None, None),
        FLASH,
    ]
    channel = [
        GAIN, MUTE,
        ("CHAN_NAME", "s", 3, "The channel's name: eight characters set, thirty-one heard.", None, None, {"braces": True}, None),
        ("FREQUENCY", "i", 3, "The receive frequency in kHz, 578350 for 578.350 MHz, within the band. Setting it "
         "makes the group and channel --,--.", None, "kHz", None, None),
        ("GROUP_CHANNEL", "s", 3, "The group and channel, 6,100; --,-- when the frequency was set directly, and never "
         "set to that.", None, None, {"braces": True}, None),
        ("FREQUENCY2", "i", 3, "The second frequency of a channel in FD-C, in kHz.", None, "kHz", None, None),
        ("GROUP_CHANNEL2", "s", 3, "The second group and channel of a channel in FD-C.", None, None, {"braces": True}, None),
        ("FD_MODE", "s", 1, "Frequency diversity: OFF, FD-C combining, FD-S selection.", vals("OFF", "FD-C", "FD-S"), None, None, None),
        ("INTERFERENCE_STATUS", "s", 1, "NONE, or DETECTED while interference is.", vals("NONE", "DETECTED"), None, None, None),
        ("INTERFERENCE_STATUS2", "s", 1, "The same for an FD-C channel's second frequency.", vals("NONE", "DETECTED"), None, None, None),
        ("ENCRYPTION_STATUS", "s", 1, "OK, or ERROR for a mismatched transmitter.", vals("OK", "ERROR"), None, None, None),
        ("UNREGISTERED_TX_STATUS", "s", 1, "OK, or ERROR while an unregistered transmitter is heard.", vals("OK", "ERROR"), None, None, None),
        FLASH, METER,
        ("CHAN_QUALITY", "i", 1, "The channel quality, 0 to 5; 255 unknown. Metered.", None, None, None, None),
        ("AUDIO_LED_BITMAP", "i", 1, "The audio LEDs as a bitmap, bottom to top: four green, three amber, one red. Metered.", None, None, None, None),
        ("AUDIO_LEVEL_PEAK", "i", 1, "The peak audio level, -120 to 0 dBFS. Metered.", span(-120, 0), "dBFS", {"offset": 120}, None),
        ("AUDIO_LEVEL_RMS", "i", 1, "The RMS audio level, -120 to 0 dBFS. Metered.", span(-120, 0), "dBFS", {"offset": 120}, None),
        ("ANTENNA_STATUS", "s", 1, "A letter per antenna, A to D: B blue, R red, X off. Metered.", None, None, None, None),
        ("RSSI", "i", 1, "The RF level at an antenna, -120 to 0 dBm. Metered.", span(-120, 0), "dBm", {"offset": 120}, None),
        ("RSSI_LED_BITMAP", "i", 1, "An antenna's RF LEDs as a bitmap, bottom to top: five amber, one red. Metered.", None, None, None, None),
        ("TX_BATT_BARS", "i", 1, BARS, None, None, None, None),
        ("TX_BATT_CHARGE_PERCENT", "i", 1, "Battery charge, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("TX_BATT_MINS", "i", 1, MINS, None, "min", None, None),
        ("TX_BATT_HEALTH_PERCENT", "i", 1, "Battery health, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("TX_BATT_CYCLE_COUNT", "i", 1, "Battery charge cycles; 65535 unknown.", None, None, None, None),
        ("TX_BATT_TEMP_C", "i", 1, "Battery temperature as sent: less 40 is degrees Celsius; 255 unknown.", None, None, None, None),
        ("TX_BATT_TYPE", "s", 1, "LION, ALKA, NIMH, LITH, or UNKN.", vals("LION", "ALKA", "NIMH", "LITH", "UNKN"), None, None, None),
        ("TX_DEVICE_ID", "s", 1, "The transmitter's name; blank with none.", None, None, {"braces": True}, None),
        ("TX_MODEL", "s", 1, "The transmitter: AD1, AD2, ADX1, ADX1M, ADX2, ADX2FD, or UNKNOWN.", None, None, None, None),
        ("TX_MUTE_MODE_STATUS", "s", 1, "ON transmitting audio, MUTE muted at the pack and still on air, UNKNOWN.",
         vals("ON", "MUTE", "UNKNOWN"), None, None, None),
        ("TX_LOCK", "s", 1, "The pack's lock: NONE, POWER, MENU, ALL, UNKNOWN.", None, None, None, None),
        ("TX_OFFSET", "i", 1, "The pack's offset as sent: less 12 is decibels, -12 to +21; 255 unknown.", None, None, None, None),
        ("TX_POWER_LEVEL", "i", 1, "The pack's RF power in mW; 255 unknown.", None, "mW", None, None),
        ("TX_TALK_SWITCH", "s", 1, "The talk switch: ON, OFF, UNKNOWN.", vals("ON", "OFF", "UNKNOWN"), None, None, None),
        ("TX_INPUT_PAD", "i", 1, "The pack's input pad as sent: 0 on (-12 dB), 12 off; 255 unknown.", None, None, None, None),
        ("TX_POLARITY", "s", 1, "POSITIVE, NEGATIVE, UNKNOWN.", vals("POSITIVE", "NEGATIVE", "UNKNOWN"), None, None, None),
        ("SLOT_STATUS", "s", 1, "A slot: EMPTY, STANDARD (an AD pack), LINKED.INACTIVE or LINKED.ACTIVE (an ADX pack off "
         "or on air).", vals("EMPTY", "STANDARD", "LINKED.INACTIVE", "LINKED.ACTIVE"), None, None, None),
        ("SLOT_TX_MODEL", "s", 1, "A slot's transmitter model, or UNKNOWN.", None, None, None, None),
        ("SLOT_TX_DEVICE_ID", "s", 3, "A slot's transmitter name, eight characters set; an ADX on air only.", None, None, {"braces": True}, None),
        ("SLOT_BATT_BARS", "i", 1, BARS, None, None, None, None),
        ("SLOT_BATT_CHARGE_PERCENT", "i", 1, "A slot's battery charge, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("SLOT_BATT_MINS", "i", 1, MINS, None, "min", None, None),
        ("SLOT_BATT_HEALTH_PERCENT", "i", 1, "A slot's battery health, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("SLOT_BATT_CYCLE_COUNT", "i", 1, "A slot's battery charge cycles; 65535 unknown.", None, None, None, None),
        ("SLOT_BATT_TYPE", "s", 1, "A slot's battery: LION, ALKA, NIMH, LITH, UNKN.", None, None, None, None),
        ("SLOT_RF_OUTPUT", "s", 3, "A slot's pack on air or RF-muted; an ADX on air only.", vals("RF_ON", "RF_MUTE"), None, None, None),
        ("SLOT_RF_POWER_MODE", "s", 3, "A slot's power mode: LOW, NORMAL, HIGH (not every band takes HIGH).",
         vals("LOW", "NORMAL", "HIGH"), None, None, None),
        ("SLOT_RF_POWER", "i", 1, "A slot's RF power in mW; 255 unknown.", None, "mW", None, None),
        ("SLOT_OFFSET", "i", 3, "A slot's pack offset as sent, 0 to 33: less 12 is decibels; 255 unknown.", span(0, 33), None, None, None),
        ("SLOT_INPUT_PAD", "i", 3, "A slot's pack input pad: 0 on (-12 dB), 12 off; an ADX1 only.", None, None, None, None),
        ("SLOT_POLARITY", "s", 3, "A slot's pack polarity; an ADX1 or ADX1M only.", vals("POSITIVE", "NEGATIVE"), None, None, None),
        ("SLOT_SHOWLINK_STATUS", "i", 1, "A slot's ShowLink quality, 1 to 5; 255 unknown.", None, None, None, None),
    ]
    indexed = {"RSSI": (4, "antenna"), "RSSI_LED_BITMAP": (4, "antenna")}
    indexed.update({row[0]: (8, "slot") for row in channel if row[0].startswith("SLOT_")})
    plain = ["CHAN_QUALITY", "AUDIO_LED_BITMAP", "AUDIO_LEVEL_PEAK", "AUDIO_LEVEL_RMS", "ANTENNA_STATUS",
             "RSSI_LED_BITMAP/1", "RSSI/1", "RSSI_LED_BITMAP/2", "RSSI/2"]
    quad = plain + ["RSSI_LED_BITMAP/3", "RSSI/3", "RSSI_LED_BITMAP/4", "RSSI/4"]
    layouts = [plain, quad, plain + [""] * 5, quad + [""] * 10]
    return receiver("/ad4", "A Shure Axient Digital receiver, an AD4D or AD4Q, over its command strings on TCP 2202: "
                            "each channel's gain, mute, name, frequency and identify; what its pack says - battery, "
                            "model, mute, lock, power; its eight transmitter slots; and its meters once a second while "
                            "Rx is on. Four channels described; an AD4D has two.",
                    "shure-axient-digital", "Axient Digital AD4D, AD4Q", AD4_SOURCES, 4, device, channel, indexed, layouts)


def ulxd():
    device = [
        ("DEVICE_ID", "s", 3, "The receiver's name, eight characters.", None, None, {"braces": True}, None),
        ("MODEL", "s", 1, "The model, ULXD4, ULXD4D or ULXD4Q.", None, None, {"braces": True}, None),
        ("FW_VER", "s", 1, "The firmware, Maj.Min.Pack.Build; a star after it, an update left incomplete.", None, None, {"braces": True}, None),
        ("ENCRYPTION", "s", 1, "Encryption, ON or OFF.", vals("ON", "OFF"), None, None, None),
        ("HIGH_DENSITY", "s", 3, "High-density mode, ON or OFF.", vals("ON", "OFF"), None, None, None),
        ("AUDIO_SUMMING_MODE", "s", 3, "Audio summing: OFF, 1+2, 3+4, 1+2/3+4, 1+2+3+4 (the last two a quad only).",
         vals("OFF", "1+2", "3+4", "1+2/3+4", "1+2+3+4"), None, None, None),
        ("NA_DEVICE_NAME", "s", 1, "The receiver's Dante device name.", None, None, {"braces": True}, None),
        FLASH,
    ]
    channel = [
        GAIN, MUTE,
        ("CHAN_NAME", "s", 3, "The channel's name, eight characters.", None, None, {"braces": True}, None),
        ("FREQUENCY", "i", 3, "The receive frequency in kHz, 578350 for 578.350 MHz.", None, "kHz", None, None),
        ("GROUP_CHAN", "s", 3, "The group and channel, gg,cc; -- where invalid.", None, None, None, None),
        ("RF_INT_DET", "s", 1, "NONE, or CRITICAL while interference is detected.", vals("NONE", "CRITICAL"), None, None, None),
        ("ENCRYPTION_WARNING", "s", 1, "An encryption mismatch, as the receiver words it.", None, None, None, None),
        ("NA_CHAN_NAME", "s", 1, "The channel's Dante name.", None, None, {"braces": True}, None),
        ("FREQUENCY_DIVERSITY_MODE", "s", 1, "Frequency diversity, as the receiver words it.", None, None, None, None),
        FLASH, METER,
        ("RF_ANTENNA", "s", 1, "The antennas: AX, XB, AB or XX, a letter on and X off. Metered.", None, None, None, None),
        ("RX_RF_LVL", "i", 1, "The RF level on the antenna in use, in dBm. Metered.", span(-128, 127), "dBm", {"offset": 128}, None),
        ("AUDIO_LVL", "i", 1, "The RMS audio level, in dBFS. Metered.", None, "dBFS", {"offset": 50}, None),
        ("BATT_BARS", "i", 1, BARS, None, None, None, None),
        ("BATT_CHARGE", "i", 1, "Battery charge of a rechargeable, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("BATT_RUN_TIME", "i", 1, MINS, None, "min", None, None),
        ("BATT_HEALTH", "i", 1, "Battery health, 0 to 100 per cent; 255 unknown.", None, "%", None, None),
        ("BATT_CYCLE", "i", 1, "Battery charge cycles.", None, None, None, None),
        ("BATT_TEMP_C", "i", 1, "Battery temperature as sent: less 40 is degrees Celsius; 255 unknown.", None, None, None, None),
        ("BATT_TYPE", "s", 1, "LION, ALKA, NIMH, LITH, WARN (charging on a dock or USB), UNKN.",
         vals("LION", "ALKA", "NIMH", "LITH", "WARN", "UNKN"), None, None, None),
        ("TX_TYPE", "s", 1, "The transmitter's model.", None, None, None, None),
        ("TX_DEVICE_ID", "s", 1, "The transmitter's name.", None, None, {"braces": True}, None),
        ("TX_FW_VER", "s", 1, "The transmitter's firmware.", None, None, {"braces": True}, None),
        ("TX_MUTE_STATUS", "s", 1, "A ULXD6 or ULXD8 muted: ON muted, OFF not, UNKN.", vals("ON", "OFF", "UNKN"), None, None, None),
        ("TX_MUTE_BUTTON_STATUS", "s", 1, "The mute button, as the receiver words it.", None, None, None, None),
        ("TX_OFFSET", "i", 1, "The pack's offset as sent.", None, None, None, None),
        ("TX_RF_PWR", "s", 1, "The pack's RF power: LOW, NORMAL, HIGH, UNKN.", vals("LOW", "NORMAL", "HIGH", "UNKN"), None, None, None),
        ("TX_PWR_LOCK", "s", 1, "The pack's power lock: ON, OFF, UNKN.", vals("ON", "OFF", "UNKN"), None, None, None),
        ("TX_MENU_LOCK", "s", 1, "The pack's menu lock, as the receiver words it.", None, None, None, None),
        ("TX_POWER_SOURCE", "s", 1, "A ULXD6 or ULXD8's power: BATTERY and the like.", None, None, None, None),
    ]
    layouts = [["RF_ANTENNA", "RX_RF_LVL", "AUDIO_LVL"]]
    return receiver("/ulxd", "A Shure ULX-D receiver, a ULXD4, ULXD4D or ULXD4Q, over its command strings on TCP 2202: "
                             "each channel's gain, mute, name, frequency and identify; its pack's battery and state; "
                             "and its meters once a second while Rx is on. Firmware 1.4.8 takes five controllers at "
                             "once. Four channels described.",
                    "shure-ulxd", "ULX-D ULXD4, ULXD4D, ULXD4Q", ULXD_SOURCES, 4, device, channel, {}, layouts)


def slxd():
    device = [
        ("DEVICE_ID", "s", 3, "The receiver's name, eight characters.", None, None, {"braces": True}, None),
        ("MODEL", "s", 1, "The model, SLXD4 or SLXD4D.", None, None, {"braces": True}, None),
        ("FW_VER", "s", 1, "The firmware, Maj.Min.Pack.Build; a star after it, a self test failed.", None, None, {"braces": True}, None),
        ("RF_BAND", "s", 1, "The RF band.", None, None, {"braces": True}, None),
        ("LOCK_STATUS", "s", 1, "The front panel's lock: OFF, MENU, ALL.", vals("OFF", "MENU", "ALL"), None, None, None),
        FLASH,
    ]
    channel = [
        GAIN,
        ("CHAN_NAME", "s", 3, "The channel's name: eight characters set, thirty-one heard.", None, None, {"braces": True}, None),
        ("AUDIO_OUT_LVL_SWITCH", "s", 1, "The output's switch, MIC or LINE.", vals("MIC", "LINE"), None, None, None),
        ("FREQUENCY", "i", 3, "The receive frequency in kHz, 578350 for 578.350 MHz.", None, "kHz", None, None),
        ("GROUP_CHANNEL", "s", 3, "The group and channel, 6,100; --,-- when the frequency was set directly.", None, None, {"braces": True}, None),
        ("FREQUENCY2", "i", 3, "The second frequency of a channel in FD-C, in kHz.", None, "kHz", None, None),
        ("FD_MODE", "s", 1, "Frequency diversity, as the receiver words it.", None, None, None, None),
        FLASH, METER,
        ("AUDIO_LEVEL_PEAK", "i", 1, "The peak audio level, -120 to 0 dBFS. Metered.", span(-120, 0), "dBFS", {"offset": 120}, None),
        ("AUDIO_LEVEL_RMS", "i", 1, "The RMS audio level, -120 to 0 dBFS. Metered.", span(-120, 0), "dBFS", {"offset": 120}, None),
        ("RSSI", "i", 1, "The RF level at an antenna, -120 to 0 dBm; the meters' one value is kept as the first. "
         "Metered.", span(-120, 0), "dBm", {"offset": 120}, None),
        ("TX_MODEL", "s", 1, "The transmitter: SLXD1, SLXD2, or UNKNOWN.", vals("SLXD1", "SLXD2", "UNKNOWN"), None, None, None),
        ("TX_BATT_BARS", "i", 1, BARS, None, None, None, None),
        ("TX_BATT_MINS", "i", 1, MINS, None, "min", None, None),
    ]
    layouts = [["AUDIO_LEVEL_PEAK", "AUDIO_LEVEL_RMS", "RSSI/1"]]
    return receiver("/slxd", "A Shure SLX-D receiver, an SLXD4 or SLXD4D, over its command strings on TCP 2202: each "
                             "channel's gain, name, frequency and identify (SLX-D has no mute command); its pack's "
                             "battery; and its meters once a second while Rx is on. Two channels described.",
                    "shure-slxd", "SLX-D SLXD4, SLXD4D", SLXD_SOURCES, 2, device, channel, {"RSSI": (2, "antenna")}, layouts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    write(axient(), "shure-axient-digital", args.out)
    write(ulxd(), "shure-ulxd", args.out)
    write(slxd(), "shure-slxd", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
