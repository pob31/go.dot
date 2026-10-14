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

"""A backup following its main, driven from outside (namespace draft 58, RD.6;
AFT, AGD, AGF, AGG).

WHAT THIS FILE IS FOR. The unit suite joins a main and a backup in one process
by a function, with a link that jitters, and holds the gate, the fixed delay and
the comparison each on its own. What none of it can prove is two shipped
binaries doing it over the port every client uses: that a backup linked to its
main says `following` and the main `leading`, that the backup holds, that the
delay it fixes is a tick or two between two processes on one machine, that a GO
at the main plays on both and reaches the device once - the main's send, the
backup's held - that a GO sent to the backup's WebSocket is passed on to the
main, played on both and heard once, that a refusal of something passed on
reaches the backup's client as the main's, and that a GO sent to the backup
over UDP is ignored, as a device's is (AGD): neither machine fires.

TWO SERVES ON ONE MACHINE on COPIES of the devices fixture, each with its own
temporary engine folder, and one device between them: mock_target.py over UDP,
silent. The desk's port is retyped at the main, which is an edit of the show,
and so reaches the backup down the link: both aim at the mock, and only the
main's datagram leaves.
"""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import common
from common import HarnessError, Report, Server, WSClient

# ON A SHARED CI RUNNER THE BEATS ARRIVE LESS EVENLY (lane_level's rule): the delay a backup
# fixes is the latest arrival it saw plus a tick, and macOS's Debug runner measured 3 on RD.6's
# first run (38174899610) where this bench reads 1 or 2. Off CI the claim is held tight.
ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"
MOST_DELAY = 4 if ON_CI else 2

MOCK = Path(__file__).resolve().parent / "mock_target.py"
FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "devices"

LIST = "7K2QM9X4"
DESK = "K4QW8YSC"          # the fixture's opaque lighting desk, over UDP
CUE = "B3N8R5TW"           # /light/go i:1, wait sent - at the desk

TIMEOUT = 1.0

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

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def received(self) -> int:
        return int(common.http_json(self.query_port, "/_mock/received").get("VALUE", [0])[0])


def wait_for(predicate, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def reader(server):
    def read(address):
        """A node's first value as text, "" when it cannot be read; a flag as true or false."""
        try:
            values = common.http_json(server.http_port, address + "?VALUE").get("VALUE", [])
        except (HarnessError, OSError, ValueError):
            return ""
        if not values:
            return ""
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return str(values[0])
    return read


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def runs_of(read, cue):
    """The states of every run of a cue, oldest first."""
    states = []
    for run in read("/godot/run/order").split():
        if read(f"/godot/run/{run}/cue") == cue:
            states.append(read(f"/godot/run/{run}/state"))
    return states


def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def run() -> int:
    report = Report(f"redundancy: a backup following its main ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pair-follow-") as scratch:
        scratch = Path(scratch)
        main_bundle = common.copy_bundle(FIXTURE, scratch / "main" / "show")
        backup_bundle = common.copy_bundle(FIXTURE, scratch / "backup" / "show")
        main_engine = scratch / "main" / "machine" / "engine"
        backup_engine = scratch / "backup" / "machine" / "engine"
        main_engine.mkdir(parents=True)
        backup_engine.mkdir(parents=True)

        desk = Mock("--transport=udp", "--behaviour=silent")
        main = backup = client = None

        try:
            main = Server(main_bundle, locale=locale, engine_folder=main_engine, role="main",
                          extra_args=[f"--link-timeout={TIMEOUT:g}"])
            backup = Server(backup_bundle, locale=locale, engine_folder=backup_engine, role="backup",
                            extra_args=[f"--peer=127.0.0.1:{main.http_port}", f"--link-timeout={TIMEOUT:g}"])
            read_main, read_backup = reader(main), reader(backup)
            send_main, send_backup_udp = sender(main), sender(backup)

            # --- following, and leading -------------------------------------------
            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", 5.0),
                         "the backup follows its main within 5 s",
                         f"state {read_backup('/godot/pair/state')!r}, linked {read_backup('/godot/pair/linked')!r}")
            report.check(wait_for(lambda: read_main("/godot/pair/state") == "leading", 3.0),
                         "and the main leads", f"state {read_main('/godot/pair/state')!r}")
            report.equal(read_backup("/godot/pair/sending"), "false", "the backup holds: it sends nothing to devices")
            report.equal(read_main("/godot/pair/sending"), "true", "the main sends")

            report.check(wait_for(lambda: as_int(read_backup("/godot/pair/delay")) > 0, 3.0),
                         "the backup fixes its delay", f"delay {read_backup('/godot/pair/delay')!r}")
            delay = as_int(read_backup("/godot/pair/delay"))
            print(f"  note delay {delay} tick(s), round trip {read_backup('/godot/pair/roundTrip')} s")
            report.check(1 <= delay <= MOST_DELAY,
                         f"the delay is between 1 and {MOST_DELAY} ticks between two serves on one machine",
                         f"delay {delay}")
            # FROM RD.9 THE MAIN'S pair/delay IS THE EXTRA LEAD its launches take
            # (AFS): the backup's delay as the backup said it - one until it has.
            report.check(wait_for(lambda: as_int(read_main("/godot/pair/delay")) == delay, 3.0),
                         "the main's launches take the backup's delay as their extra lead",
                         f"main {read_main('/godot/pair/delay')!r}, backup {delay}")

            # --- an edit at the main reaches the backup's show ---------------------
            send_main("node.set", [f"/godot/mount/{DESK}/port", str(desk.port)])
            report.check(wait_for(lambda: read_main(f"/godot/mount/{DESK}/port") == str(desk.port)
                                  and read_backup(f"/godot/mount/{DESK}/port") == str(desk.port), 3.0),
                         "the desk's port retyped at the main is retyped on the backup",
                         f"main {read_main(f'/godot/mount/{DESK}/port')!r}, "
                         f"backup {read_backup(f'/godot/mount/{DESK}/port')!r}")

            # --- a GO at the main ------------------------------------------------
            base_heard = desk.received()
            send_main("go")
            report.check(wait_for(lambda: runs_of(read_main, CUE) == ["done"]
                                  and runs_of(read_backup, CUE) == ["done"], 4.0),
                         "a GO at the main: the cue's run reaches done on both machines",
                         f"main {runs_of(read_main, CUE)}, backup {runs_of(read_backup, CUE)}")
            time.sleep(0.5)
            report.equal(desk.received() - base_heard, 1, "and the device hears it once - the main's")

            # --- a GO at the backup's WebSocket ----------------------------------
            send_main("node.set", [f"/godot/list/{LIST}/standby", CUE])
            report.check(wait_for(lambda: read_main(f"/godot/list/{LIST}/standby") == CUE
                                  and read_backup(f"/godot/list/{LIST}/standby") == CUE, 3.0),
                         "the standby put back at the main, on both")

            relayed_before = as_int(read_backup("/godot/pair/relayed"))
            client = WSClient(backup.http_port, "backup-page")
            client.send_osc("/godot/cmd/go")

            report.check(wait_for(lambda: runs_of(read_main, CUE) == ["done", "done"]
                                  and runs_of(read_backup, CUE) == ["done", "done"], 4.0),
                         "a GO sent to the backup's WebSocket is passed on and plays on both",
                         f"main {runs_of(read_main, CUE)}, backup {runs_of(read_backup, CUE)}")
            report.equal(as_int(read_backup("/godot/pair/relayed")) - relayed_before, 1,
                         "the backup counts the one command it passed on")
            time.sleep(0.5)
            report.equal(desk.received() - base_heard, 2, "and the device hears it once more - again the main's")

            # --- a refusal of what was passed on comes back as the main's ----------
            client.send_osc("/godot/cmd/cue/fire", ["ZZZZZZZZ"])
            report.check(wait_for(lambda: "unknown-id" in read_backup("/godot/engine/lastError")
                                  and "main:peer:ws:" in read_backup("/godot/engine/lastError"), 3.0),
                         "a command the main refuses reaches the backup's lastError with the main's reason",
                         f"lastError {read_backup('/godot/engine/lastError')!r}")
            report.check("unknown-id" in read_main("/godot/engine/lastError"),
                         "it was the main that refused it", f"main lastError {read_main('/godot/engine/lastError')!r}")

            # --- a GO at the backup over UDP: a device's, ignored -------------------
            send_main("node.set", [f"/godot/list/{LIST}/standby", CUE])
            report.check(wait_for(lambda: read_backup(f"/godot/list/{LIST}/standby") == CUE, 3.0),
                         "the standby put back again")
            dropped_before = as_int(read_backup("/godot/pair/dropped"))
            time.sleep(0.6)                         # past the GO debounce

            send_backup_udp("go")
            report.check(wait_for(lambda: as_int(read_backup("/godot/pair/dropped")) > dropped_before, 3.0),
                         "a GO sent to the backup over UDP is ignored, and counted",
                         f"dropped {read_backup('/godot/pair/dropped')!r} from {dropped_before}")
            time.sleep(0.8)
            report.equal(runs_of(read_main, CUE), ["done", "done"], "the main did not fire")
            report.equal(runs_of(read_backup, CUE), ["done", "done"], "nor did the backup")
            report.equal(desk.received() - base_heard, 2, "and the device heard nothing more")

            # --- in step ------------------------------------------------------------
            report.equal(read_backup("/godot/pair/differs"), "", "every record came out on the backup as on the main")
            # A RECORD THAT MISSES ITS SLOT still goes in, at once, and is only
            # counted (pair/Follow.h): what this counts is whether every beat
            # arrived within the margin the backup measured at link-up, which
            # is the two tick threads' punctuality - timing, so held to nought
            # off CI and only there (lane_level's rule, as MOST_DELAY is). With
            # ctest running entries side by side (2026-10-13), one record came
            # late in one run under a runner-like load, twenty entries at once
            # with GITHUB_ACTIONS set; on CI a nonzero count is said and
            # voided, never counted as a pass.
            late = read_backup("/godot/pair/late")
            if ON_CI and late != "0":
                report.void("none came late", f"{late} came late, on a shared runner whose tick threads"
                                              f" are held back; the records themselves went in, and"
                                              f" `differs` above says they came out the same")
            else:
                report.equal(late, "0", "none came late")
            report.equal(read_backup("/godot/pair/state"), "following", "and the backup is following still")
        finally:
            if client is not None:
                client.close()
            for server in (backup, main):
                if server is not None:
                    try:
                        server.stop()
                    except HarnessError as error:
                        report.check(False, "a serve stopped cleanly", str(error))
            desk.stop()

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
