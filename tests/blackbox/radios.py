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

"""Radio microphones, driven from outside the process (namespace draft §124,
BER; WR.2).

THE FAR END IS mock_shure.py, an Axient Digital receiver written from Shure's
command-string document in another language with no shared code. No receiver
is on the bench before JTSE (BEP), so this is what says the shipped binary
speaks the document.

THE SEQUENCE: a device made from the Axient Digital preset and pointed at the
mock with Rx on; the link opening, and its greeting - everything asked for, the
meters once a second - the first frames down it; what the receiver answered
heard at the preset's nodes, the gain in decibels, the model's padding gone; a
cue setting a channel's gain arriving as the receiver's own SET, the receiver's
REP heard back; a name in braces; a mute as ON; a pack's battery dropping on
the receiver's side heard; the meters' RF level heard in dBm; a REP ERR kept as
the device's last reply.

WHAT WAS HEARD IS READ FROM THE SESSION'S LOG, where every report is a
`mount.heard` record with its address and typed values: a mounted node's HTTP
answer is what Go.dot wrote, and what the device said reaches no client yet
(WR.6's panel is what publishes it).
"""

import os
import shutil
import sys
import tempfile
import time
import urllib.parse
import subprocess
from pathlib import Path

import common
from common import HarnessError, Report, Server

MOCK = Path(__file__).resolve().parent / "mock_shure.py"
FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "devices"

RADIO = "AD4QRAD1"         # the device made from the Axient Digital preset
CUE = "B3N8R5TW"           # the fixture's /light/go, re-aimed at the receiver

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


class MockReceiver:
    """`mock_shure.py`, on ports it chose, in a process of its own."""

    def __init__(self):
        self.process = subprocess.Popen([sys.executable, str(MOCK)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        line = self.process.stdout.readline().split()

        if len(line) != 4 or line[0] != "osc" or line[2] != "query":
            self.stop()
            raise HarnessError(f"the mock did not report its ports: {line!r}")

        self.port = int(line[1])
        self.query_port = int(line[3])

    def __enter__(self) -> "MockReceiver":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def values(self, door: str, query: str = ""):
        target = "/_mock/" + door + ("?" + urllib.parse.quote(query, safe="/=") if query else "")
        return common.http_json(self.query_port, target).get("VALUE", [])

    def frames(self):
        return self.values("frames")


def run() -> int:
    report = Report(f"radio microphones: a Shure receiver's command strings ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-radios-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "radios")

        log = Path(scratch) / "radios.wfglog"

        def heard(address, typed):
            """Whether the log holds the device's report of `address` as `typed` - `i:12`, `s:"AD4Q-A"`."""
            text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
            needle = f's:"{address}" {typed}'
            return any(" mount.heard " in line and line.rstrip().endswith(needle) for line in text.splitlines())

        with MockReceiver() as ad4, Server(bundle, log=log, locale=locale) as server:
            def send(command, args=None):
                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))

            def read(address):
                try:
                    values = common.http_json(server.http_port, address).get("VALUE", [])
                except HarnessError:
                    return ""
                if not values:
                    return ""
                if isinstance(values[0], bool):
                    return "true" if values[0] else "false"
                return str(values[0])

            def wait_for(predicate, seconds=5.0):
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    if predicate():
                        return True
                    time.sleep(0.05)
                return predicate()

            def settle(address, wanted, seconds=5.0):
                return wait_for(lambda: read(address) == wanted, seconds)

            def fire(address, value):
                send("node.set", [f"/godot/cue/{CUE}/address", address])
                send("node.set", [f"/godot/cue/{CUE}/value", value])
                ok = settle(f"/godot/cue/{CUE}/address", address) and settle(f"/godot/cue/{CUE}/value", value)
                send("cue.fire", [CUE])
                return ok

            base = f"/godot/mount/{RADIO}/"

            # --- the device, from its preset ----------------------------------
            send("mount.createFromPreset", ["shure-axient-digital", RADIO])
            report.check(settle(base + "wire", "shure"),
                         "a device made from the Axient Digital preset is on the shure wire",
                         f"wire {read(base + 'wire')!r}, problem {read(base + 'problem')!r}")
            report.equal(read(base + "transport"), "tcp", "over a connection")
            report.equal(read(base + "port"), "2202", "at the receiver's own port")
            report.equal(read(base + "readback"), "notify", "and it tells: readback notify")
            report.equal(read(base + "prefix"), "/ad4", "under the preset's root")

            send("node.set", [base + "port", str(ad4.port)])
            send("node.set", [base + "rx", "true"])
            report.check(settle(base + "link", "open"),
                         "pointed at the mock with Rx on, the link opens",
                         f"link {read(base + 'link')!r}, problem {read(base + 'linkProblem')!r}")

            # --- the greeting -----------------------------------------------------
            report.check(wait_for(lambda: len(ad4.frames()) >= 2),
                         "the first frames down it are the greeting", f"{ad4.frames()}")
            report.equal(ad4.frames()[:2], ["< GET 0 ALL >", "< SET 0 METER_RATE 01000 >"],
                         "everything asked for, then the meters once a second, nothing after either frame")

            # --- what the receiver answered, heard --------------------------------
            report.check(wait_for(lambda: heard("/ad4/1/AUDIO_GAIN", "i:12")),
                         "the gain heard in decibels: the receiver's 030, less its 18")
            report.check(wait_for(lambda: heard("/ad4/MODEL", 's:"AD4Q-A"')),
                         "the model heard without the receiver's padding")
            report.check(wait_for(lambda: heard("/ad4/1/FREQUENCY", "i:578350")), "the frequency heard in kHz")
            report.check(wait_for(lambda: heard("/ad4/4/AUDIO_MUTE", "F")), "the mute heard as a bool")

            # --- a cue changes the receiver -----------------------------------------
            report.check(fire("/ad4/1/AUDIO_GAIN", "i:22"), "the fixture's cue re-aimed at channel 1's gain, 22 dB")
            report.check(wait_for(lambda: "< SET 1 AUDIO_GAIN 40 >" in ad4.frames()),
                         "fired, it arrives as the receiver's own SET, 22 dB as its 40", f"{ad4.frames()[-3:]}")
            report.equal(ad4.values("state", "1/AUDIO_GAIN"), ["40"], "and the receiver holds it")
            report.check(wait_for(lambda: heard("/ad4/1/AUDIO_GAIN", "i:22")), "its REP heard back at the node, in decibels")

            report.check(fire("/ad4/2/CHAN_NAME", 's:"Lead Vox"'), "re-aimed at channel 2's name")
            report.check(wait_for(lambda: "< SET 2 CHAN_NAME {Lead Vox} >" in ad4.frames()),
                         "fired, the name goes in braces", f"{ad4.frames()[-3:]}")
            report.check(wait_for(lambda: heard("/ad4/2/CHAN_NAME", 's:"Lead Vox"')), "and comes back trimmed")

            report.check(fire("/ad4/3/AUDIO_MUTE", "T"), "re-aimed at channel 3's mute, on")
            report.check(wait_for(lambda: "< SET 3 AUDIO_MUTE ON >" in ad4.frames()),
                         "fired, a bool goes as ON", f"{ad4.frames()[-3:]}")
            report.check(wait_for(lambda: heard("/ad4/3/AUDIO_MUTE", "T")), "and the receiver's ON comes back a bool")

            # --- the receiver tells ------------------------------------------------
            ad4.values("hand", "1/TX_BATT_BARS=001")
            report.check(wait_for(lambda: heard("/ad4/1/TX_BATT_BARS", "i:1")),
                         "a pack's battery dropping on the receiver's side is heard")

            report.check(wait_for(lambda: heard("/ad4/1/RSSI/1", "i:-34"), seconds=4.0),
                         "the meters, once a second, heard field by field: antenna A in dBm")
            report.check(wait_for(lambda: heard("/ad4/1/AUDIO_LEVEL_PEAK", "i:-18")), "and the audio peak in dBFS")

            ad4.values("say", "< REP ERR >")
            report.check(settle(base + "lastReply", "< REP ERR >"),
                         "a REP ERR is the device's last reply", f"{read(base + 'lastReply')!r}")

        #  WFG_KEEP_RADIOS_LOG=<file> keeps the session's log for a look after a red run.
        if os.environ.get("WFG_KEEP_RADIOS_LOG") and log.is_file():
            shutil.copyfile(log, os.environ["WFG_KEEP_RADIOS_LOG"])

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
