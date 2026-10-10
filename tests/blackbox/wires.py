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

"""A device over a connection, driven from outside the process (namespace
draft 57, AFJ; DP.6).

WHAT THIS FILE IS FOR. The unit suite proves the link table against a fake
cable and the sender against a fake sink. What neither can prove is that the
shipped binary opens a real TCP connection to something that is not itself,
frames a cue's bytes the way the far end cuts them, notices the far end going
away, comes back when it does, and says so in the rows a client reads. Each of
those crosses the document, the tick thread, the links table's own thread, the
sender and a socket; a test that held any of them still would be testing
something else.

THE FAR END IS mock_target.py LISTENING FOR CONNECTIONS, written from the
specification in another language with no shared code: cut by length first,
as an Eos on 3032 is, then by SLIP, as one on 3037. Its ports are the operating
system's, reported on its first line, so two of these can run at once.

THE SEQUENCE: a device made over a connection, its link seen opening; a cue
aimed under it arriving down the stream; the cable pulled by the mock and the
link seen to come back on its own; the console gone for good and the link
saying so, a cue fired meanwhile leaving nothing; the same device moved to a
SLIP console and the cue arriving framed the other way.
"""
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import common
from common import HarnessError, Report, Server

MOCK = Path(__file__).resolve().parent / "mock_target.py"
FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "devices"

DEVICE = "E0SW1RE1"        # the device this file makes, over a connection
CUE = "B3N8R5TW"           # the fixture's /light/go, re-aimed at the console

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


class MockWire:
    """`mock_target.py --transport tcp`, on ports it chose, in a process of its own."""

    def __init__(self, framing: str):
        self.process = subprocess.Popen(
            [sys.executable, str(MOCK), "--transport=tcp", f"--framing={framing}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        line = self.process.stdout.readline().split()

        if len(line) != 4 or line[0] != "osc" or line[2] != "query":
            self.stop()
            raise HarnessError(f"the mock did not report its ports: {line!r}")

        self.port = int(line[1])
        self.query_port = int(line[3])

    def __enter__(self) -> "MockWire":
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

    def ask(self, door: str):
        """`/_mock/<door>`'s first value: a count, or the first message."""
        return common.http_json(self.query_port, "/_mock/" + door).get("VALUE", [None])[0]


def run() -> int:
    report = Report(f"device integration presets: OSC over a connection ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-wires-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "wires")

        with MockWire("length") as eos, Server(bundle, locale=locale) as server:
            def send(command, args=None):
                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))

            def read(address):
                """A node's value as the document spells it, and nothing for a
                node that is not there yet: a device made by a command this
                driver just sent is a tick away, which is what settle is for."""
                try:
                    values = common.http_json(server.http_port, address).get("VALUE", [])
                except HarnessError:
                    return ""
                if not values:
                    return ""
                if isinstance(values[0], bool):
                    return "true" if values[0] else "false"
                return str(values[0])

            def settle(address, wanted, tries=60):
                """The tick is 50 Hz and a link opens on its own thread: a
                driver that read straight back would be timing the machine."""
                for _ in range(tries):
                    if read(address) == wanted:
                        return True
                    time.sleep(0.05)
                return False

            def wait_for(predicate, seconds=5.0):
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    if predicate():
                        return True
                    time.sleep(0.05)
                return predicate()

            base = f"/godot/mount/{DEVICE}/"

            # --- a device over a connection -------------------------------
            send("mount.create", ["/eos", "", DEVICE])
            report.check(settle(base + "prefix", "/eos"), "a device is made at the console's root")
            report.equal(read(base + "link"), "off", "and reached by datagram, its link reads off")

            send("node.set", [base + "transport", "tcp"])
            send("node.set", [base + "port", str(eos.port)])
            report.check(settle(base + "transport", "tcp"), "put over a connection from a client")
            report.equal(read(base + "framing"), "length",
                         "with the framing a device typed by hand gets: a size before each packet")
            report.check(settle(base + "link", "open", tries=100),
                         "the link opens on its own thread, and the row says open",
                         f"link {read(base + 'link')!r}, problem {read(base + 'linkProblem')!r}")
            report.equal(read(base + "problem"), "", "nothing wrong with the declaration itself")
            report.check(wait_for(lambda: eos.ask("connections") == 1),
                         "the console took one connection", f"{eos.ask('connections')}")

            # --- a cue aimed under it goes down the stream -----------------
            send("node.set", [f"/godot/cue/{CUE}/address", "/eos/key/go_0"])
            send("node.set", [f"/godot/cue/{CUE}/value", "f:1"])
            report.check(settle(f"/godot/cue/{CUE}/address", "/eos/key/go_0"),
                         "the fixture's cue re-aimed at the console")

            send("cue.fire", [CUE])
            report.check(wait_for(lambda: eos.ask("received") == 1),
                         "fired, its message arrives down the connection, cut by length",
                         f"received {eos.ask('received')}")
            report.equal(eos.ask("messages"), ["/eos/key/go_0", [1.0]],
                         "with the address as the cue spells it and the value as typed")
            report.check(settle(base + "sent", "1"), "and the device's sent count says one")

            # --- the cable pulled: the link comes back on its own ----------
            common.http_get(eos.query_port, "/_mock/drop")
            report.check(wait_for(lambda: eos.ask("connections") == 2, seconds=8.0),
                         "the console closing the connection, the link opens it again after its pause",
                         f"connections {eos.ask('connections')}, link {read(base + 'link')!r}")
            report.check(settle(base + "link", "open", tries=100), "and the row says open again")

            send("cue.fire", [CUE])
            report.check(wait_for(lambda: eos.ask("received") == 2),
                         "a cue fired after that arrives down the new connection")

            # --- the console gone for good -----------------------------------
            eos.stop()
            report.check(settle(base + "link", "retrying", tries=160),
                         "with the console gone the link says retrying",
                         f"link {read(base + 'link')!r}")
            report.check(wait_for(lambda: read(base + "linkProblem") != ""),
                         "and why, in a sentence", f"{read(base + 'linkProblem')!r}")

            sent_before = read(base + "sent")
            send("cue.fire", [CUE])
            time.sleep(0.6)
            report.equal(read(base + "sent"), sent_before,
                         "a cue fired meanwhile leaves nothing: the count does not move")

            # --- the same device moved to a SLIP console ----------------------
            with MockWire("slip") as ion:
                send("node.set", [base + "port", str(ion.port)])
                send("node.set", [base + "framing", "slip"])
                report.check(settle(base + "framing", "slip"), "the framing retyped to slip")
                report.check(settle(base + "link", "open", tries=100),
                             "the link opens to the new port, cut the new way",
                             f"link {read(base + 'link')!r}, problem {read(base + 'linkProblem')!r}")
                report.equal(read(base + "linkProblem"), "", "with nothing left to say")

                send("cue.fire", [CUE])
                report.check(wait_for(lambda: ion.ask("received") == 1),
                             "and the cue arrives framed in SLIP", f"received {ion.ask('received')}")
                report.equal(ion.ask("messages"), ["/eos/key/go_0", [1.0]], "whole")

            # --- back by datagram: the link let go ---------------------------
            send("node.set", [base + "transport", "udp"])
            report.check(settle(base + "link", "off"), "back by datagram, the link reads off")

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
