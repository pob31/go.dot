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

"""A machine that holds, driven from outside the process (namespace draft 58,
RD.1; AGG).

WHAT THIS FILE IS FOR. The unit suite holds each door on its own - the device
sender, the MIDI sender, the serial table, the answers, the questions - against
fakes. What none of it can prove is that `wfg serve --hold` holds them all at
once in the shipped binary: that a cue aimed at a real device on a real socket
puts nothing on it, that a device over a connection is never even connected to
(AGW), that a verified cue aimed at a device that will never answer still ends
at once, and that every one of those cues ends `done`, unwarned and unfailed -
read back over HTTP as a client reads it - so that the backup's run table says
what the main's does. And `/godot/pair/sending` says false.

THE SAME SCENARIO TWICE, held and then not, each against devices of its own:
mock_target.py over UDP (silent: it takes datagrams and answers no question)
and over TCP, cut by length as an Eos on 3032 is. Not held, the same cues reach
both mocks and the verified cue times out against the silent one - which is
what makes "the mock heard nothing" mean something rather than prove a broken
wire. devices.py and wires.py prove the rest of sending; this proves holding.

A WRITABLE COPY of the devices fixture each time, in a temporary folder: the
scenario retypes ports and re-aims cues, and the autosave writes recovery/
beside the show it is serving.
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

DESK = "K4QW8YSC"          # the fixture's opaque lighting desk, over UDP
DESCRIBED = "K3PV7WRB"     # the fixture's described desk, read back over OSCQuery
LINKED = "E0HQDX01"        # the device this file makes, over a connection (Crockford: no L)
CUE = "B3N8R5TW"           # /light/go i:1, wait sent - at the desk
VERIFIED = "E4GP6QSC"      # /desk/fader f:0.75 - made verified here
OVER_LINK = "M5TQ7XVA"     # aimed at no device in the fixture; re-aimed at the link

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


class Mock:
    """`mock_target.py`, on ports it chose, in a process of its own."""

    def __init__(self, *args: str):
        self.process = subprocess.Popen([sys.executable, str(MOCK), *args],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        line = self.process.stdout.readline().split()

        if len(line) != 4 or line[0] != "osc" or line[2] != "query":
            self.stop()
            raise HarnessError(f"the mock did not report its ports: {line!r}")

        self.port = int(line[1])
        self.query_port = int(line[3])

    def __enter__(self) -> "Mock":
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
        """`/_mock/<door>`'s first value."""
        return common.http_json(self.query_port, "/_mock/" + door).get("VALUE", [None])[0]


def scenario(report: Report, scratch: Path, held: bool) -> None:
    word = "held" if held else "not held"
    bundle = common.copy_bundle(FIXTURE, scratch / ("held" if held else "sending"))

    with Mock("--transport=udp", "--behaviour=silent") as desk, \
            Mock("--transport=tcp", "--framing=length") as eos, \
            Server(bundle, locale=locale, hold=held) as server:

        def send(command, args=None):
            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))

        def read(address):
            """A node's value as the document spells it, and nothing for a node
            that is not there yet - a device made a tick ago, a run not yet made."""
            try:
                values = common.http_json(server.http_port, address).get("VALUE", [])
            except HarnessError:
                return ""
            if not values:
                return ""
            if isinstance(values[0], bool):
                return "true" if values[0] else "false"
            return str(values[0])

        def settle(address, wanted, tries=100):
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

        def run_of(cue):
            """The newest run of a cue, once it has finished: its state, error and warning."""
            def finished():
                for run in reversed(read("/godot/run/order").split()):
                    if read(f"/godot/run/{run}/cue") == cue:
                        return read(f"/godot/run/{run}/state") in ("done", "failed")
                return False

            wait_for(finished, seconds=4.0)

            for run in reversed(read("/godot/run/order").split()):
                if read(f"/godot/run/{run}/cue") == cue:
                    return (read(f"/godot/run/{run}/state"), read(f"/godot/run/{run}/error"),
                            read(f"/godot/run/{run}/warning"))
            return ("", "", "")

        # --- the row --------------------------------------------------------
        report.equal(read("/godot/pair/sending"), "false" if held else "true",
                     f"{word}: /godot/pair/sending says whether this machine sends")
        if held:
            report.check(any("hold" in notice for notice in server.notices),
                         "held: serve says so as it starts", f"notices {server.notices}")

        # --- a device over UDP ------------------------------------------------
        send("node.set", [f"/godot/mount/{DESK}/port", str(desk.port)])
        report.check(settle(f"/godot/mount/{DESK}/port", str(desk.port)), f"{word}: the desk's port retyped")

        send("cue.fire", [CUE])
        state, error, warning = run_of(CUE)
        report.equal(state, "done", f"{word}: a cue at a device over UDP ends done")
        report.equal(error, "", f"{word}: and did not fail")
        report.equal(warning, "", f"{word}: and carries no warning - nothing was switched off")

        if held:
            time.sleep(0.5)
            report.equal(desk.ask("received"), 0, "held: and the device heard nothing")
        else:
            report.check(wait_for(lambda: desk.ask("received") == 1),
                         "not held: the device heard it", f"received {desk.ask('received')}")

        # --- a device over a connection ---------------------------------------
        base = f"/godot/mount/{LINKED}/"
        send("mount.create", ["/eos", "", LINKED])
        report.check(settle(base + "prefix", "/eos"), f"{word}: a device is made at the console's root")
        send("node.set", [base + "transport", "tcp"])
        send("node.set", [base + "port", str(eos.port)])
        report.check(settle(base + "port", str(eos.port)) and settle(base + "transport", "tcp"),
                     f"{word}: put over a connection to the console")

        if held:
            #  AGW: a held machine's connections stay closed - a console that
            #  takes one session gives it to the main.
            time.sleep(1.5)
            report.equal(eos.ask("connections"), 0, "held: the console is never connected to")
            report.check(read(base + "link") != "open", "held: and the link does not read open",
                         f"link {read(base + 'link')!r}")
        else:
            report.check(wait_for(lambda: eos.ask("connections") == 1),
                         "not held: the console took a connection", f"{eos.ask('connections')}")
            report.check(settle(base + "link", "open"), "not held: and the link reads open")

        send("node.set", [f"/godot/cue/{OVER_LINK}/address", "/eos/key/go_0"])
        send("node.set", [f"/godot/cue/{OVER_LINK}/value", "f:1"])
        send("node.set", [f"/godot/cue/{OVER_LINK}/wait", "sent"])
        report.check(settle(f"/godot/cue/{OVER_LINK}/wait", "sent"), f"{word}: a cue re-aimed at the console")

        send("cue.fire", [OVER_LINK])
        state, error, warning = run_of(OVER_LINK)
        report.equal(state, "done", f"{word}: a cue at a device over a connection ends done")
        report.equal(error, "", f"{word}: never send-failed")
        report.equal(warning, "", f"{word}: and unwarned")

        if held:
            time.sleep(0.5)
            report.equal(eos.ask("received"), 0, "held: and the console heard nothing")
        else:
            report.check(wait_for(lambda: eos.ask("received") == 1),
                         "not held: the console heard it down the connection", f"received {eos.ask('received')}")

        # --- a verified cue, at a device that never answers --------------------
        send("node.set", [f"/godot/mount/{DESCRIBED}/port", str(desk.port)])
        send("node.set", [f"/godot/mount/{DESCRIBED}/queryPort", str(desk.query_port)])
        send("node.set", [f"/godot/cue/{VERIFIED}/wait", "verified"])
        send("node.set", [f"/godot/cue/{VERIFIED}/timeout", "1"])
        report.check(settle(f"/godot/cue/{VERIFIED}/wait", "verified")
                     and settle(f"/godot/mount/{DESCRIBED}/queryPort", str(desk.query_port)),
                     f"{word}: a verified cue aimed at a device that answers nothing")

        received_before = desk.ask("received")
        fired_at = time.monotonic()
        send("cue.fire", [VERIFIED])
        state, error, warning = run_of(VERIFIED)
        took = time.monotonic() - fired_at

        if held:
            report.equal(state, "done", "held: the verified cue ends done - nobody to hear an answer from")
            report.equal(warning, "", "held: unwarned")
            report.check(took < 0.9, "held: without waiting out its second", f"took {took:.2f} s")
            time.sleep(0.3)
            report.equal(desk.ask("received"), received_before, "held: and nothing reached the device")
        else:
            report.equal(state, "failed", "not held: the same cue waits for the silent device")
            report.equal(error, "timeout", "not held: and times out, which is what holding spares the backup")


def run() -> int:
    report = Report(f"redundancy: a machine that holds ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-hold-") as scratch:
        scenario(report, Path(scratch), held=True)
        scenario(report, Path(scratch), held=False)

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
