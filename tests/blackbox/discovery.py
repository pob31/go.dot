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

"""A device found on the network, made, saved as a preset, made again, replayed.

Namespace draft 76, UD.10: the shipped `wfg serve` against `mock_target.py
--describe --mdns-port`, a device written from the specifications in another
language. Serve asks the mock's mDNS port directly (`--mdns-port=0
--mdns-peer=...`) - no part of the network's 5353 is touched - and:

 1. lists the mock in network/found, by its name, address and query port;
 2. makes a device of it with mount.describeAt - the description fetched, its
    roots the description's own, its OSC port from HOST_INFO, read back over
    OSCQuery since HOST_INFO offers VALUE;
 3. saves it as a preset with preset.save, into a folder of this driver's own,
    which is then offered as yours;
 4. makes a second device from that preset, asked at the same query port;
 5. and `wfg replay` of the session's log makes the same show with no socket.
"""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

import common
from common import HarnessError, Report


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "minimal"
MOCK = Path(__file__).resolve().parent / "mock_target.py"

FOUND = "MCKDSC01"          # Crockford base32: no I, L, O or U
MADE_AGAIN = "MCKDSC02"
NAME = "Mock Desk"

DESCRIPTION = {
    "FULL_PATH": "/",
    "CONTENTS": {
        "mockdesk": {
            "FULL_PATH": "/mockdesk",
            "CONTENTS": {
                "fader": {"FULL_PATH": "/mockdesk/fader", "TYPE": "f", "ACCESS": 3,
                          "RANGE": [{"MIN": 0.0, "MAX": 1.0}], "DESCRIPTION": "the desk's fader"},
                "go": {"FULL_PATH": "/mockdesk/go", "TYPE": "N", "ACCESS": 2},
            },
        },
    },
}


class Mock:
    """`mock_target.py --describe <file> --mdns-port 0`, on ports it chose."""

    def __init__(self, description: Path):
        self.process = subprocess.Popen(
            [sys.executable, str(MOCK), "--behaviour=agree", f"--describe={description}",
             f"--name={NAME}", "--mdns-port=0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        line = self.process.stdout.readline().split()

        if len(line) != 6 or line[0] != "osc" or line[2] != "query" or line[4] != "mdns":
            self.stop()
            raise HarnessError(f"the mock did not report its ports: {line!r}")

        self.osc_port = int(line[1])
        self.query_port = int(line[3])
        self.mdns_port = int(line[5])

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=10)

    def __enter__(self) -> "Mock":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()


def run(locale: str) -> int:
    report = Report(f"OSCQuery devices found, saved as presets ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-discovery-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "show")
        yours = room / "Device presets"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        description = room / "mockdesk.json"
        description.write_text(json.dumps(DESCRIPTION), encoding="utf-8")

        with Mock(description) as mock, common.Server(
                bundle, log=log, locale=locale,
                extra_args=["--mdns-port=0", f"--mdns-peer=127.0.0.1:{mock.mdns_port}",
                            f"--user-presets={yours}"]) as server:

            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

            def command(name: str, args: list) -> None:
                sender.sendto(common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args),
                              ("127.0.0.1", server.osc_port))

            def value(address: str):
                reply = common.http_json(server.http_port, address + "?VALUE")
                values = (reply or {}).get("VALUE", [])
                return values[0] if values else None

            def settled(address: str, wanted, timeout: float = 10.0):
                common.wait_until(lambda: str(value(address)) == str(wanted), timeout=timeout)
                return value(address)

            # ---------------------------------------------------------- found
            report.equal(settled("/godot/network/discovery", "unicast"), "unicast",
                         "asking the mock directly, from a port of its own: unicast")
            line = f"{NAME}\t127.0.0.1\t{mock.query_port}"
            listed = common.wait_until(lambda: line in str(value("/godot/network/found") or ""), timeout=10.0)
            report.check(bool(listed), "the mock is listed in network/found by its name, address and query port",
                         str(value("/godot/network/found")))

            # ---------------------------------------------------------- made
            command("mount.describeAt", ["127.0.0.1", mock.query_port, NAME, FOUND])
            base = f"/godot/mount/{FOUND}/"
            report.equal(settled(base + "loaded", True), True, "picked, it is a device, loaded")
            report.equal(value(base + "prefix"), "/mockdesk", "at the description's own root")
            report.equal(str(value(base + "port")), str(mock.osc_port), "its OSC port from HOST_INFO")
            report.equal(str(value(base + "queryPort")), str(mock.query_port), "asked at its query port")
            report.equal(value(base + "readback"), "oscquery", "read back over OSCQuery, as HOST_INFO offers VALUE")
            report.equal(value(base + "name"), NAME, "named as it was found")
            report.equal(value(base + "namespace"), f"namespaces/{FOUND}.json", "its description kept in the show")
            report.check((bundle / "namespaces" / f"{FOUND}.json").is_file(), "the file is in the bundle")
            report.check(bool(common.http_json(server.http_port, "/mockdesk/fader")),
                         "its fader is a node of the tree")

            # ---------------------------------------------------------- saved
            command("preset.save", [FOUND])
            report.equal(settled("/godot/network/presetsSaved", "mock-desk@1"), "mock-desk@1",
                         "saved as a preset, under the device's name made one")
            report.check((yours / "mock-desk.json").is_file(), "into the folder of yours")
            report.equal(settled("/godot/preset/mock-desk/origin", "yours"), "yours",
                         "and offered as yours once the folder is read again")

            saved = json.loads((yours / "mock-desk.json").read_text(encoding="utf-8"))
            header = saved.get("GODOT", {})
            report.equal(header.get("QUERY_PORT"), mock.query_port, "the preset keeps the query port")
            report.check(any(f"captured from {NAME} at 127.0.0.1:{mock.query_port}" in source
                             for source in header.get("SOURCES", [])),
                         "and says where it was captured", str(header.get("SOURCES")))

            # ---------------------------------------------------------- made again
            command("mount.createFromPreset", ["mock-desk", MADE_AGAIN])
            again = f"/godot/mount/{MADE_AGAIN}/"
            report.equal(settled(again + "preset", "mock-desk@1"), "mock-desk@1", "a device made from the preset")
            report.equal(str(value(again + "queryPort")), str(mock.query_port), "asked where the first one was")
            report.equal(str(value(again + "port")), str(mock.osc_port), "at the same OSC port")

            sender.close()

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` makes the same show with no socket", (out + err).strip()[-2000:])
        report.check("reproduced exactly" in out, "saying so in as many words", out.strip()[-500:])

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
