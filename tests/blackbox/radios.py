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

THEN SENNHEISER (BES), against mock_ssc.py, an EW-DX written from its SSC
guide: a device from the EW-DX preset on the ssc wire over a datagram socket of
its own; the subscription the first message, everything it covers heard; a
cue's mute and gain as JSON, the gain adapted by the receiver to its step and
the adapted value heard; a pack's battery dropping heard through the
subscription; an address the receiver lacks kept as the last reply; and the
subscription renewed before its lifetime runs out.

THEN SPECTERA (BET), against mock_spectera.py, a base station from SSCv2 2.3
over real TLS with a self-signed certificate: a device from the Spectera preset
reached, its certificate shown and NOTHING sent - no request, no password -
until it is trusted; a pin that is not its fingerprint refused and said; the
right pin, then the subscription opened and the resources added, their values
heard, a pack under the identifier the base station gave it; a cue renaming a
pack as a PUT of that pack with the one field, heard back; a battery dropping
heard; a field the base station refuses kept as the last reply; the base
station closing the subscription, opened again; and a wrong password said.

WHAT WAS HEARD IS READ FROM THE SESSION'S LOG, where every report is a
`mount.heard` record with its address and typed values: a mounted node's HTTP
answer is what Go.dot wrote, and what the device said reaches no client yet
(WR.6's panel is what publishes it).
"""

import json
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
MOCK_SSC = Path(__file__).resolve().parent / "mock_ssc.py"
MOCK_SPECTERA = Path(__file__).resolve().parent / "mock_spectera.py"
FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "devices"

RADIO = "AD4QRAD1"         # the device made from the Axient Digital preset
EWDX = "EWDXRAD1"          # and the one from the EW-DX preset, on SSC
SPECTERA = "SPECTRA1"      # and the one from the Spectera preset, over HTTPS
CUE = "B3N8R5TW"           # the fixture's /light/go, re-aimed at the receiver

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


class MockReceiver:
    """`mock_shure.py` or `mock_ssc.py`, on ports it chose, in a process of its own."""

    def __init__(self, script=MOCK):
        self.process = subprocess.Popen([sys.executable, str(script)],
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
    report = Report(f"radio microphones: Shure and Sennheiser receivers ({locale})")

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

            # --- a Sennheiser EW-DX on SSC (BES) -------------------------------------
            with MockReceiver(MOCK_SSC) as em:
                sen = f"/godot/mount/{EWDX}/"
                send("mount.createFromPreset", ["sennheiser-ewdx-ssc", EWDX])
                report.check(settle(sen + "wire", "ssc"),
                             "a device made from the EW-DX preset is on the ssc wire",
                             f"wire {read(sen + 'wire')!r}, problem {read(sen + 'problem')!r}")
                report.equal(read(sen + "transport"), "udp", "by datagram")
                report.equal(read(sen + "port"), "45", "at the receiver's own port")
                report.equal(read(sen + "readback"), "subscribe", "and it subscribes")

                send("node.set", [sen + "port", str(em.port)])
                send("node.set", [sen + "rx", "true"])
                report.check(settle(sen + "link", "open"),
                             "pointed at the mock with Rx on, its own socket opens",
                             f"link {read(sen + 'link')!r}, problem {read(sen + 'linkProblem')!r}")

                def first_message():
                    messages = em.values("messages")
                    return json.loads(messages[0]) if messages else {}

                report.check(wait_for(lambda: bool(em.values("messages"))), "a first datagram arrives")
                report.check("subscribe" in first_message().get("osc", {}).get("state", {}),
                             "and it is the file's subscription", f"{em.values('messages')[:1]}")
                report.check(wait_for(lambda: heard("/ewdx/rx2/name", 's:"RX2"')),
                             "what the subscription covers is heard: a channel's name")
                report.check(wait_for(lambda: heard("/ewdx/mates/tx1/battery/gauge", "i:90")),
                             "a pack's battery gauge")
                report.check(wait_for(lambda: heard("/ewdx/m/rx1/af", "f:-24")), "a meter, as a float")

                report.check(fire("/ewdx/rx1/mute", "T"), "the fixture's cue re-aimed at channel 1's mute")
                report.check(wait_for(lambda: '{"rx1":{"mute":true}}' in em.values("messages")),
                             "fired, it arrives as the JSON's path, true at its end", f"{em.values('messages')[-2:]}")
                report.equal(em.values("state", "rx1/mute"), [True], "and the receiver holds it")

                report.check(fire("/ewdx/rx1/gain", "i:20"), "re-aimed at channel 1's gain, 20 dB")
                report.check(wait_for(lambda: '{"rx1":{"gain":20}}' in em.values("messages")), "sent as asked")
                report.check(wait_for(lambda: heard("/ewdx/rx1/gain", "i:21")),
                             "the receiver adapts it to its step of three, and the adapted value is heard")

                em.values("hand", "mates/tx3/battery/gauge=20")
                report.check(wait_for(lambda: heard("/ewdx/mates/tx3/battery/gauge", "i:20")),
                             "a pack's battery dropping, told through the subscription, is heard")

                report.check(fire("/ewdx/rx1/presets/active", "i:1"), "re-aimed at a property this receiver lacks")
                report.check(wait_for(lambda: "404" in read(sen + "lastReply")),
                             "its error is the device's last reply", f"{read(sen + 'lastReply')!r}")

                subscribed = lambda: sum(1 for m in em.values("messages") if '"subscribe"' in m)
                report.check(wait_for(lambda: subscribed() >= 2, seconds=12.0),
                             "the subscription asked again before its fifteen seconds run out",
                             f"{subscribed()} subscriptions")
                report.equal(em.values("subscribers"), [1], "and the receiver still has it")

            # --- a Sennheiser Spectera over HTTPS (BET) ---------------------------------
            with MockReceiver(MOCK_SPECTERA) as bs:
                spec = f"/godot/mount/{SPECTERA}/"
                pack = "/spectera/api/mts/paired/all/7/"
                send("mount.createFromPreset", ["sennheiser-spectera", SPECTERA])
                report.check(settle(spec + "wire", "sscv2"),
                             "a device made from the Spectera preset is on the sscv2 wire",
                             f"wire {read(spec + 'wire')!r}, problem {read(spec + 'problem')!r}")
                report.equal(read(spec + "transport"), "https", "over TLS")
                report.equal(read(spec + "port"), "443", "at the base station's own port")

                send("node.set", [spec + "host", "127.0.0.1"])
                send("node.set", [spec + "port", str(bs.port)])
                send("node.set", [spec + "login", "controlSennheiser mockpass"])
                send("node.set", [spec + "rx", "true"])

                fingerprint = bs.values("fingerprint")[0]
                report.check(settle(spec + "certificate", fingerprint, seconds=8.0),
                             "reached, the certificate it presented is shown", f"{read(spec + 'certificate')!r}")
                report.check(wait_for(lambda: "not trusted yet" in read(spec + "linkProblem")),
                             "and the link says it is not trusted yet", f"{read(spec + 'linkProblem')!r}")
                report.equal(bs.values("requests"), [], "nothing was sent: no request, no password")

                send("node.set", [spec + "pin", "00:11:22"])
                report.check(wait_for(lambda: "not the one trusted" in read(spec + "linkProblem"), seconds=10.0),
                             "a pin that is not its fingerprint is refused, and said", f"{read(spec + 'linkProblem')!r}")
                report.equal(bs.values("requests"), [], "and still nothing was sent")

                send("node.set", [spec + "pin", fingerprint])
                report.check(settle(spec + "link", "open", seconds=12.0),
                             "trusted, the link opens", f"link {read(spec + 'link')!r}, {read(spec + 'linkProblem')!r}")
                report.check(wait_for(lambda: any(r.startswith("PUT /api/ssc/state/subscriptions/")
                                                  and r.split(" ")[1].endswith("/add")
                                                  for r in bs.values("requests"))),
                             "the subscription opened and the preset's resources added", f"{bs.values('requests')}")
                report.check(wait_for(lambda: heard(pack + "name", 's:"Lead"')),
                             "a pack's name heard, under the identifier the base station gave it")
                report.check(wait_for(lambda: heard(pack + "batteryFillLevel", "i:90")), "its battery")
                report.check(wait_for(lambda: heard("/spectera/api/rf/channels/0/frequency", "i:602000")),
                             "and an RF channel's frequency")

                report.check(fire(pack + "name", 's:"Lead Vox"'), "the fixture's cue re-aimed at the pack's name")
                report.check(wait_for(lambda: 'PUT /api/mts/paired/all/7 {"name":"Lead Vox"}' in bs.values("requests")),
                             "fired, it is a PUT of that pack with the one field", f"{bs.values('requests')[-2:]}")
                report.equal(bs.values("state", "/api/mts/paired/all/7")[0].get("name"), "Lead Vox",
                             "and the base station holds it")
                report.check(wait_for(lambda: heard(pack + "name", 's:"Lead Vox"')), "its change heard back")

                bs.values("hand", "/api/mts/paired/all/9/batteryFillLevel=20")
                report.check(wait_for(lambda: heard("/spectera/api/mts/paired/all/9/batteryFillLevel", "i:20")),
                             "a pack's battery dropping, told as an event, is heard")

                report.check(fire(pack + "bogus", "i:1"), "re-aimed at a field the base station does not have")
                report.check(wait_for(lambda: "HTTP 400" in read(spec + "lastReply")),
                             "its refusal is the device's last reply", f"{read(spec + 'lastReply')!r}")

                bs.values("close")
                report.check(wait_for(lambda: bs.values("subscribers") == [1]
                                      and sum(1 for r in bs.values("requests")
                                              if r == "GET /api/ssc/state/subscriptions") >= 2,
                                      seconds=12.0),
                             "the base station closing the subscription, it is opened again",
                             f"{bs.values('subscribers')}")

                send("node.set", [spec + "login", "controlSennheiser wrong"])
                report.check(wait_for(lambda: "refused the login" in read(spec + "linkProblem"), seconds=12.0),
                             "a wrong password is said", f"{read(spec + 'linkProblem')!r}")

        #  WFG_KEEP_RADIOS_LOG=<file> keeps the session's log for a look after a red run.
        if os.environ.get("WFG_KEEP_RADIOS_LOG") and log.is_file():
            shutil.copyfile(log, os.environ["WFG_KEEP_RADIOS_LOG"])

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
