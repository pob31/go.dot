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

"""The link between a main and its backup, driven from outside (namespace
draft 58, RD.5; AFV, AGB, AGD).

WHAT THIS FILE IS FOR. The unit suite holds the stream, the receiver and the
dialler each on its own, and the two halves joined in one process. What none
of it can prove is two shipped binaries finding each other: that
`wfg serve --role=backup --peer=...` dials a `wfg serve --role=main` on the
port every client uses, that both say they are linked, that what an operator
sends the main reaches the backup - and only that, the engine's own records
staying home - that the round trip is measured, that a main killed is a link
lost within the timeout, and that a new main started on the same port is
found again.

TWO SERVES ON ONE MACHINE, each on its own COPY of one fixture and its own
temporary engine folder, so neither reads the developer's pairing.xml nor
writes anything into tests/fixtures. The main starts first: the backup is told
its port, which is only known once it has started.
"""
import sys
import tempfile
import time
from pathlib import Path

import common
from common import HarnessError, Report, Server

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "minimal"
CUE = "B3N8R5TW"        # "House to half", first in the minimal fixture's list

# Seconds of silence that lose the link, given to both on the command line.
TIMEOUT = 1.0

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


def wait_for(predicate, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def reader(server):
    def read(address):
        """A node's first value, None when it cannot be read; a flag as true or false."""
        try:
            values = common.http_json(server.http_port, address + "?VALUE").get("VALUE", [])
        except (HarnessError, OSError, ValueError):
            return None
        if not values:
            return None
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return values[0]
    return read


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return -1.0


def settled(read, address, seconds=0.4, limit=5.0):
    """A number that has stopped moving: the same reading for `seconds`."""
    until = time.monotonic() + limit
    last = read(address)
    since = time.monotonic()
    while time.monotonic() < until:
        time.sleep(0.05)
        now = read(address)
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= seconds:
            break
    return last


def start_main(bundle, engine_folder, port=0, attempts_for=0.0):
    """A main, on a port the OS chooses or the one named - tried again for a
    while when that port will not bind yet, and said so."""
    until = time.monotonic() + attempts_for
    tries = 0
    while True:
        tries += 1
        try:
            server = Server(bundle, locale=locale, engine_folder=engine_folder, role="main",
                            http_port=port, extra_args=[f"--link-timeout={TIMEOUT:g}"])
            if tries > 1:
                print(f"  note the port bound on try {tries}")
            return server
        except HarnessError as error:
            if time.monotonic() >= until:
                raise
            print(f"  note try {tries} on port {port} did not start: {str(error).splitlines()[0]}")
            time.sleep(0.25)


def run() -> int:
    report = Report(f"redundancy: the link between a main and its backup ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pair-link-") as scratch:
        scratch = Path(scratch)
        main_bundle = common.copy_bundle(FIXTURE, scratch / "main" / "show")
        backup_bundle = common.copy_bundle(FIXTURE, scratch / "backup" / "show")
        main_engine = scratch / "main" / "machine" / "engine"
        backup_engine = scratch / "backup" / "machine" / "engine"
        main_engine.mkdir(parents=True)
        backup_engine.mkdir(parents=True)
        log = scratch / "main.wfglog"

        main = Server(main_bundle, log=log, locale=locale, engine_folder=main_engine, role="main",
                      extra_args=[f"--link-timeout={TIMEOUT:g}"])
        backup = None
        second_main = None

        try:
            port = main.http_port
            backup = Server(backup_bundle, locale=locale, engine_folder=backup_engine, role="backup",
                            extra_args=[f"--peer=127.0.0.1:{port}", f"--link-timeout={TIMEOUT:g}"])
            started = time.monotonic()
            read_main, read_backup, send = reader(main), reader(backup), sender(main)

            # --- found ------------------------------------------------------------
            report.check(wait_for(lambda: read_backup("/godot/pair/linked") == "true", 3.0),
                         "the backup is linked to its main within 3 s",
                         f"linked {read_backup('/godot/pair/linked')!r} after {time.monotonic() - started:.2f} s")
            report.check(wait_for(lambda: read_main("/godot/pair/linked") == "true",
                                  max(0.1, 3.0 - (time.monotonic() - started))),
                         "and the main to its backup, within the same 3 s",
                         f"linked {read_main('/godot/pair/linked')!r} after {time.monotonic() - started:.2f} s")
            report.equal(read_backup("/godot/pair/peer"), f"127.0.0.1:{port}",
                         "the backup's peer is the main's address as it dialled it")
            main_peer = str(read_main("/godot/pair/peer"))
            report.check(main_peer.startswith("ws:") and "127.0.0.1:" in main_peer,
                         "the main's peer is the backup's connection", f"peer {main_peer!r}")
            report.equal(as_int(read_main("/godot/engine/errorCount")), 0,
                         "a hello and a stream of pings refused nothing: they never became writes")

            # --- what an operator sends travels, and only that --------------------
            base_backup = as_int(settled(read_backup, "/godot/pair/received"))
            base_main = as_int(settled(read_main, "/godot/pair/received"))
            report.check(base_backup >= 0 and base_main >= 0, "both count records this link",
                         f"backup {base_backup}, main {base_main}")

            send("cue.fire", [CUE])
            send("standby.next")
            send("go")

            report.check(wait_for(lambda: as_int(read_backup("/godot/pair/received")) >= base_backup + 3, 3.0),
                         "a cue.fire, a standby.next and a go sent to the main reach the backup",
                         f"received {read_backup('/godot/pair/received')!r}, from {base_backup}")
            time.sleep(0.6)
            report.equal(as_int(read_backup("/godot/pair/received")), base_backup + 3,
                         "exactly three: what the engine made of them stays on each machine")
            report.equal(as_int(read_main("/godot/pair/received")), base_main + 3,
                         "and the main counts the three it sent")
            report.equal(as_int(read_backup("/godot/pair/gaps")), 0, "no gap on the backup")
            report.equal(as_int(read_main("/godot/pair/gaps")), 0, "nor on the main")

            backup_trip = as_float(read_backup("/godot/pair/roundTrip"))
            main_trip = as_float(read_main("/godot/pair/roundTrip"))
            report.check(0.0 < backup_trip < 0.1, "the backup's round trip is there, and under a tenth of a second",
                         f"roundTrip {backup_trip}")
            report.check(0.0 < main_trip < 0.1, "and the main reads it too, from the backup's ping",
                         f"roundTrip {main_trip}")

            # The main's log: the three records, and nothing of the link's own.
            main._let_the_log_catch_up()
            text = log.read_text(encoding="utf-8") if log.exists() else ""
            report.equal(common.logged_before(log, "cue.fire", containing=CUE), 1, "the main logged the cue.fire")
            report.check("/godot/pair/" not in text, "no pairing frame is anywhere in the main's log",
                         next((line for line in text.splitlines() if "/godot/pair/" in line), ""))

            # --- the main goes ---------------------------------------------------
            main.process.terminate()
            killed = time.monotonic()
            lost = wait_for(lambda: read_backup("/godot/pair/linked") == "false", TIMEOUT + 3.0)
            elapsed = time.monotonic() - killed
            report.check(lost and elapsed <= TIMEOUT + 0.5,
                         f"the main killed, the backup is unlinked within the timeout and half a second "
                         f"({elapsed:.2f} s)",
                         f"linked {read_backup('/godot/pair/linked')!r}")
            report.equal(as_float(read_backup("/godot/pair/roundTrip")), 0.0, "and has no round trip to say")
            report.equal(read_backup("/godot/pair/peer"), f"127.0.0.1:{port}", "it still says who was lost")
            main.stop()

            # --- and a new main on the same port is found again ------------------
            second_main = start_main(main_bundle, main_engine, port=port, attempts_for=10.0)
            again = time.monotonic()
            read_second, send_second = reader(second_main), sender(second_main)
            report.check(wait_for(lambda: read_backup("/godot/pair/linked") == "true", 10.0),
                         "a new main started on the same port is found again",
                         f"linked {read_backup('/godot/pair/linked')!r} after {time.monotonic() - again:.2f} s")
            report.check(wait_for(lambda: read_second("/godot/pair/linked") == "true", 5.0),
                         "and links back", f"linked {read_second('/godot/pair/linked')!r}")
            print(f"  note found again {time.monotonic() - again:.2f} s after the new main had started")

            base_again = as_int(settled(read_backup, "/godot/pair/received"))
            report.equal(as_int(read_backup("/godot/pair/gaps")), 0,
                         "a new main is a new stream: nothing of the old one is a gap")
            send_second("standby.next")
            report.check(wait_for(lambda: as_int(read_backup("/godot/pair/received")) == base_again + 1, 3.0),
                         "and what is sent to it reaches the backup, counted on the new link",
                         f"received {read_backup('/godot/pair/received')!r}, from {base_again}")
        finally:
            for server in (second_main, backup, main):
                if server is not None:
                    try:
                        server.stop()
                    except HarnessError as error:
                        report.check(False, "a serve stopped cleanly", str(error))

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
