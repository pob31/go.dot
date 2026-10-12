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

"""The link lost, a backup taking over, a main coming back, and two machines
both sending, driven from outside (namespace draft 58, RD.7; AFQ, AFR, AFV,
AGJ, AGK).

WHAT THIS FILE IS FOR. The unit suite lays the state machine out as a table
and joins two engines in one process. What none of it can prove is two shipped
binaries doing it over the port every client uses, with a device between them
that counts what it hears:

  - a main and its backup, a GO and a GO at the main: the device hears each
    once, the main's;
  - the main KILLED mid-show: within the timeout the backup leads and sends,
    and says why; a GO sent to the backup's WebSocket is heard once each;
  - the main started again on its port: having run nothing, it follows the
    backup that took over (AFR) - the stream running from the machine that
    dials to the machine it dialled - and the device still hears each GO once;
  - two machines that each lead: a TCP relay between a backup's dialler and
    its main, paused past the timeout, so the backup takes over and the main
    goes on alone; one GO at each, each heard once - two different cues; the
    relay resumed, and BOTH SEND (AFQ): both say so, and a GO at either is sent
    by both, heard twice - the author's rule, which the alarm is there for -
    until `pair.keep` at the main: the backup follows, and the next GO is
    heard once.

SERVES ON ONE MACHINE, each on its own COPY of the devices fixture and its own
temporary engine folder, and one device: mock_target.py over UDP. The desk's
port and the S21's are retyped to the mock at a main, an edit of the show
which reaches its backup down the link: both aim at the mock, two cues to two
addresses, so the mock can tell one GO from another.
"""
import socket
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import common
from common import HarnessError, Report, Server, WSClient

MOCK = Path(__file__).resolve().parent / "mock_target.py"
FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "devices"

LIST = "7K2QM9X4"
DESK = "K4QW8YSC"          # the fixture's opaque lighting desk, over UDP
S21 = "K5RX9ZTD"           # the fixture's S21, over UDP
DESK_CUE = "B3N8R5TW"      # /light/go i:1, at the desk
S21_CUE = "N6VRA3WB"       # /digico/snapshots/fire i:4, at the S21
DESK_SAYS = "/light/go"
S21_SAYS = "/digico/snapshots/fire"

TIMEOUT = 1.0

# ON A SHARED CI RUNNER A TICK RUNS LATE (lane_level's rule, as pair_follow.py since c44ec75): the
# takeover is judged within a second of the timeout here and within three on CI, where macOS's
# Debug runner can hold the backup's tick back that long.
ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"
TAKEOVER_SLACK = 3.0 if ON_CI else 1.0
DEBOUNCE = 0.7             # past the fixture's half-second GO debounce

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

    def heard(self, address: str) -> int:
        """How many messages to that address arrived, since the mock started."""
        messages = common.http_json(self.query_port, "/_mock/messages").get("VALUE", [])
        return sum(1 for message in messages if message and message[0] == address)


class Relay:
    """A TCP relay between a backup's dialler and its main: what one side
    writes the other reads, until it is PAUSED - nothing passes either way,
    what is written waits - and RESUMED, when what waited goes on. A network
    switch that stops forwarding and starts again, the connections never
    closed."""

    def __init__(self, target_port: int):
        self.target_port = target_port
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.open = threading.Event()
        self.open.set()
        self.closing = False
        self.sockets = []
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self) -> None:
        while not self.closing:
            try:
                near, _ = self.listener.accept()
            except OSError:
                return
            try:
                far = socket.create_connection(("127.0.0.1", self.target_port))
            except OSError:
                near.close()
                continue
            self.sockets += [near, far]
            threading.Thread(target=self._pump, args=(near, far), daemon=True).start()
            threading.Thread(target=self._pump, args=(far, near), daemon=True).start()

    def _pump(self, source, sink) -> None:
        while not self.closing:
            try:
                data = source.recv(65536)
            except OSError:
                break
            if not data:
                break
            self.open.wait()
            try:
                sink.sendall(data)
            except OSError:
                break
        for end in (source, sink):
            try:
                end.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def pause(self) -> None:
        self.open.clear()

    def resume(self) -> None:
        self.open.set()

    def close(self) -> None:
        self.closing = True
        self.open.set()
        for each in [self.listener] + self.sockets:
            try:
                each.close()
            except OSError:
                pass


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


PAIR_ROWS = ("state", "linked", "sending", "peer", "received", "relayed", "dropped", "gaps",
             "delay", "late", "unconfirmed", "differs", "alarm")


def pair_rows(read) -> str:
    """One machine's pairing rows on one line, for a failure's detail: what each
    side believed when a check did not hold (the Linux-only failure of RD.7's
    first CI run, 38183715084, printed nothing to go on)."""
    return ", ".join(f"{name}={read('/godot/pair/' + name)!r}" for name in PAIR_ROWS)


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def start_main(bundle, engine_folder, port=0, attempts_for=0.0, peer=None):
    """A main, on a port the OS chooses or the one named - tried again for a
    while when that port will not bind yet."""
    until = time.monotonic() + attempts_for
    extra = [f"--link-timeout={TIMEOUT:g}"]
    while True:
        try:
            return Server(bundle, locale=locale, engine_folder=engine_folder, role="main",
                          http_port=port, extra_args=extra)
        except HarnessError as error:
            if time.monotonic() >= until:
                raise
            print(f"  note port {port} did not bind yet: {str(error).splitlines()[0]}")
            time.sleep(0.25)


def start_backup(bundle, engine_folder, peer_port):
    return Server(bundle, locale=locale, engine_folder=engine_folder, role="backup",
                  extra_args=[f"--peer=127.0.0.1:{peer_port}", f"--link-timeout={TIMEOUT:g}"])


def machine(scratch: Path, name: str):
    bundle = common.copy_bundle(FIXTURE, scratch / name / "show")
    engine = scratch / name / "machine" / "engine"
    engine.mkdir(parents=True)
    return bundle, engine


def run() -> int:
    report = Report(f"redundancy: the link lost, taking over, both sending ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-failover-") as scratch:
        scratch = Path(scratch)
        main_bundle, main_engine = machine(scratch, "main")
        backup_bundle, backup_engine = machine(scratch, "backup")

        device = Mock("--transport=udp", "--behaviour=silent")
        servers = []
        clients = []
        relay = None

        def heard_since(base):
            return (device.heard(DESK_SAYS) - base[0], device.heard(S21_SAYS) - base[1])

        def counts():
            return (device.heard(DESK_SAYS), device.heard(S21_SAYS))

        def aim_at_the_device(send, read):
            send("node.set", [f"/godot/mount/{DESK}/port", str(device.port)])
            send("node.set", [f"/godot/mount/{S21}/port", str(device.port)])
            return wait_for(lambda: read(f"/godot/mount/{DESK}/port") == str(device.port)
                            and read(f"/godot/mount/{S21}/port") == str(device.port), 3.0)

        try:
            # === A MAIN AND ITS BACKUP ==============================================
            main = start_main(main_bundle, main_engine)
            servers.append(main)
            port = main.http_port
            backup = start_backup(backup_bundle, backup_engine, port)
            servers.append(backup)
            read_main, read_backup = reader(main), reader(backup)
            send_main = sender(main)

            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following"
                                  and read_main("/godot/pair/state") == "leading", 6.0),
                         "the backup follows its main, the main leads",
                         f"backup {read_backup('/godot/pair/state')!r}, main {read_main('/godot/pair/state')!r}")
            report.check(aim_at_the_device(send_main, read_backup),
                         "the two devices retyped at the main are retyped on the backup")

            base = counts()
            send_main("node.set", [f"/godot/list/{LIST}/standby", DESK_CUE])
            time.sleep(0.3)
            send_main("go")
            time.sleep(DEBOUNCE)
            send_main("node.set", [f"/godot/list/{LIST}/standby", S21_CUE])
            time.sleep(0.3)
            send_main("go")
            report.check(wait_for(lambda: heard_since(base) == (1, 1), 3.0),
                         "a GO and a GO at the main: the device hears each once", f"heard {heard_since(base)}")
            time.sleep(0.5)
            report.equal(heard_since(base), (1, 1), "and no more: the backup held both")

            # === THE MAIN KILLED MID-SHOW ===========================================
            main.process.terminate()
            killed = time.monotonic()
            took_over = wait_for(lambda: read_backup("/godot/pair/state") == "leading"
                                 and read_backup("/godot/pair/sending") == "true", TIMEOUT + 3.0)
            elapsed = time.monotonic() - killed
            report.check(took_over and elapsed <= TIMEOUT + TAKEOVER_SLACK,
                         f"the main killed, the backup leads and sends within the timeout ({elapsed:.2f} s)",
                         f"state {read_backup('/godot/pair/state')!r}, sending {read_backup('/godot/pair/sending')!r}")
            report.check("took over" in read_backup("/godot/pair/alarm"),
                         "and says the main was lost and it took over", f"alarm {read_backup('/godot/pair/alarm')!r}")
            main.stop()
            servers.remove(main)

            page = WSClient(backup.http_port, "backup-page")
            clients.append(page)
            base = counts()
            page.send_osc(f"/godot/list/{LIST}/standby", [DESK_CUE])
            time.sleep(0.3)
            page.send_osc("/godot/cmd/go")
            time.sleep(DEBOUNCE)
            page.send_osc(f"/godot/list/{LIST}/standby", [S21_CUE])
            time.sleep(0.3)
            page.send_osc("/godot/cmd/go")
            report.check(wait_for(lambda: heard_since(base) == (1, 1), 3.0),
                         "GOs sent to the backup's WebSocket are heard once each", f"heard {heard_since(base)}")

            # === THE MAIN STARTED AGAIN ON ITS PORT ==================================
            main = start_main(main_bundle, main_engine, port=port, attempts_for=10.0)
            servers.append(main)
            read_main = reader(main)
            report.check(wait_for(lambda: read_main("/godot/pair/state") == "following", 10.0),
                         "the main started again, having run nothing, follows the backup (AFR)",
                         f"main {read_main('/godot/pair/state')!r}, linked {read_main('/godot/pair/linked')!r}")
            report.equal(read_backup("/godot/pair/state"), "leading", "and the backup stays leading")
            report.equal(read_main("/godot/pair/sending"), "false", "the main holds")
            report.check(wait_for(lambda: read_backup("/godot/pair/alarm") == "", 3.0),
                         "the backup's alarm is gone once the main follows it",
                         f"alarm {read_backup('/godot/pair/alarm')!r}")
            report.check(wait_for(lambda: int(read_main("/godot/pair/delay") or 0) > 0, 3.0),
                         "the main fixes its delay behind the backup", f"delay {read_main('/godot/pair/delay')!r}")

            base = counts()
            time.sleep(DEBOUNCE)
            page.send_osc(f"/godot/list/{LIST}/standby", [DESK_CUE])
            time.sleep(0.3)
            page.send_osc("/godot/cmd/go")
            report.check(wait_for(lambda: heard_since(base) == (1, 0), 3.0),
                         "a GO at the backup that leads: heard once", f"heard {heard_since(base)}")
            report.check(wait_for(lambda: read_main(f"/godot/list/{LIST}/standby") != DESK_CUE, 3.0),
                         "and the main, following, played it too - its standby moved on",
                         f"standby {read_main(f'/godot/list/{LIST}/standby')!r}")
            time.sleep(0.5)
            report.equal(heard_since(base), (1, 0), "and the device heard it once: the main held its copy")

            for client in clients:
                client.close()
            clients.clear()
            for server in list(servers):
                server.stop()
                servers.remove(server)

            # === TWO MACHINES THAT EACH LEAD ========================================
            second_bundle, second_engine = machine(scratch, "second-main")
            spare_bundle, spare_engine = machine(scratch, "second-backup")

            main = start_main(second_bundle, second_engine)
            servers.append(main)
            relay = Relay(main.http_port)
            backup = start_backup(spare_bundle, spare_engine, relay.port)
            servers.append(backup)
            read_main, read_backup = reader(main), reader(backup)
            send_main, send_backup = sender(main), sender(backup)

            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", 6.0),
                         "a second pair, the backup's dialler through a relay: it follows",
                         f"backup {read_backup('/godot/pair/state')!r}")
            report.check(aim_at_the_device(send_main, read_backup),
                         "the devices retyped at the main, on both",
                         f"main: {pair_rows(read_main)} | backup: {pair_rows(read_backup)}"
                         f" | backup ports {read_backup(f'/godot/mount/{DESK}/port')!r}"
                         f" {read_backup(f'/godot/mount/{S21}/port')!r}"
                         f" against {device.port}")

            relay.pause()
            paused = time.monotonic()
            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "leading"
                                  and read_main("/godot/pair/state") == "alone", TIMEOUT + 3.0),
                         f"the relay paused past the timeout: the backup took over, the main goes on alone "
                         f"({time.monotonic() - paused:.2f} s)",
                         f"backup {read_backup('/godot/pair/state')!r}, main {read_main('/godot/pair/state')!r}")
            report.equal(read_main("/godot/pair/sending"), "true", "both send: the main")
            report.equal(read_backup("/godot/pair/sending"), "true", "and the backup")

            base = counts()
            send_main("node.set", [f"/godot/list/{LIST}/standby", DESK_CUE])
            send_backup("node.set", [f"/godot/list/{LIST}/standby", S21_CUE])
            time.sleep(0.3)
            send_main("go")
            send_backup("go")
            report.check(wait_for(lambda: heard_since(base) == (1, 1), 3.0),
                         "one GO at each, two different cues: each heard once", f"heard {heard_since(base)}")

            relay.resume()
            report.check(wait_for(lambda: read_main("/godot/pair/state") == "bothSending"
                                  and read_backup("/godot/pair/state") == "bothSending", 6.0),
                         "the relay resumed: both read bothSending (AFQ)",
                         f"main {read_main('/godot/pair/state')!r}, backup {read_backup('/godot/pair/state')!r}")
            for read, who in ((read_main, "the main"), (read_backup, "the backup")):
                report.check(read("/godot/pair/alarm").startswith("Both machines are sending"),
                             f"{who} says both machines are sending", f"alarm {read('/godot/pair/alarm')!r}")

            # Each applies the other's operators' commands: a page at either is heard from both.
            main_page = WSClient(main.http_port, "main-page")
            backup_page = WSClient(backup.http_port, "backup-page")
            clients += [main_page, backup_page]
            time.sleep(1.0)                 # each follow measured, and past the debounce

            base = counts()
            main_page.send_osc(f"/godot/list/{LIST}/standby", [DESK_CUE])
            report.check(wait_for(lambda: read_backup(f"/godot/list/{LIST}/standby") == DESK_CUE, 3.0),
                         "a standby moved at the main's page is moved on the backup too",
                         f"backup standby {read_backup(f'/godot/list/{LIST}/standby')!r}")
            main_page.send_osc("/godot/cmd/go")
            report.check(wait_for(lambda: heard_since(base) == (2, 0), 3.0),
                         "a GO at the main is sent by both: heard twice - the author's rule (AFQ)",
                         f"heard {heard_since(base)}")

            time.sleep(DEBOUNCE)
            base = counts()
            backup_page.send_osc(f"/godot/list/{LIST}/standby", [S21_CUE])
            report.check(wait_for(lambda: read_main(f"/godot/list/{LIST}/standby") == S21_CUE, 3.0),
                         "and one moved at the backup's page is moved on the main",
                         f"main standby {read_main(f'/godot/list/{LIST}/standby')!r}")
            backup_page.send_osc("/godot/cmd/go")
            report.check(wait_for(lambda: heard_since(base) == (0, 2), 3.0),
                         "a GO at the backup is sent by both too: heard twice", f"heard {heard_since(base)}")

            # An operator picks: the main keeps the show, the backup follows.
            send_main("pair.keep")
            report.check(wait_for(lambda: read_main("/godot/pair/state") == "leading"
                                  and read_backup("/godot/pair/state") == "following", 3.0),
                         "pair.keep at the main: the main leads, the backup follows",
                         f"main {read_main('/godot/pair/state')!r}, backup {read_backup('/godot/pair/state')!r}")
            report.equal(read_backup("/godot/pair/sending"), "false", "the backup holds again")
            report.check(wait_for(lambda: read_main("/godot/pair/alarm") == ""
                                  and read_backup("/godot/pair/alarm") == "", 3.0),
                         "and neither says both are sending",
                         f"main {read_main('/godot/pair/alarm')!r}, backup {read_backup('/godot/pair/alarm')!r}")

            time.sleep(DEBOUNCE)
            base = counts()
            main_page.send_osc(f"/godot/list/{LIST}/standby", [DESK_CUE])
            time.sleep(0.3)
            main_page.send_osc("/godot/cmd/go")
            report.check(wait_for(lambda: heard_since(base) == (1, 0), 3.0),
                         "the next GO is heard once", f"heard {heard_since(base)}")
            time.sleep(0.6)
            report.equal(heard_since(base), (1, 0), "and only once")
        finally:
            for client in clients:
                try:
                    client.close()
                except Exception:
                    pass
            for server in reversed(servers):
                try:
                    server.stop()
                except HarnessError as error:
                    report.check(False, "a serve stopped cleanly", str(error))
            if relay is not None:
                relay.close()
            device.stop()

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
