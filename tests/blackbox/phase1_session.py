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

"""Phase 1's done-when clause, driven from outside the process.

The devplan's criterion, word for word: *an external OSCQuery client can load a
document, read and write nodes, move standby, and the event log replays the
session bit-for-bit.* This is that sentence as a program.

It launches the shipped binary on a throwaway copy of a bundle, talks to it over
all three surfaces a real client has — HTTP, WebSocket and UDP — changes the
show, saves it, and then requires the engine's own log to reproduce what it just
did. Nothing here links the library or includes a header.

WRITES ARE SERIALISED, WITH A READ-BACK BETWEEN THEM, and that is a correctness
requirement rather than caution. Three transports are three independent
producers; the order two of them reach the queue in is decided by the operating
system, not by this file. A driver that fired a UDP write and a WebSocket write
without waiting would be asserting on an order nobody promised, and it would
pass or fail by the weather. So: one write, one read-back confirming it landed,
then the next.

THE ENGINE'S OWN LOG IS THE ORACLE at the end. Replay is not "did it crash" — it
re-executes every record against a fresh engine and requires the same log out.
That is the property the whole phase was shaped around: no wall-clock reads in
the apply path, no unordered containers, generated identifiers recorded as
applied, one number formatter.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run
(no binary, no fixture, a server that would not start). The third is separate on
purpose — "Go.dot is broken" and "this machine has no build" want different
people to look.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "minimal"


def settle(server: Server, address: str, expected, report: Report,
           description: str, timeout: float = 10.0) -> bool:
    """Waits for a write to become visible over HTTP, then asserts it.

    The wait is the serialisation point. A write crosses a socket, joins a
    queue, and is applied by the tick thread up to 20 ms later; reading straight
    back would be reading before the engine had been asked.
    """
    deadline = time.monotonic() + timeout
    actual = None

    while time.monotonic() < deadline:
        status, body = common.http_get(server.http_port, f"{address}?VALUE")

        if status == 200:
            try:
                actual = json.loads(body).get("VALUE", [None])[0]
            except (json.JSONDecodeError, IndexError):
                actual = None

            if actual == expected:
                break

        time.sleep(0.02)

    return report.equal(actual, expected, description)


def run(locale: "str | None") -> int:
    report = Report(f"phase1 session ({locale or 'C'})")

    if not FIXTURE.is_dir():
        raise HarnessError(f"no fixture bundle at {FIXTURE}")

    workspace = Path(tempfile.mkdtemp(prefix="wfg-blackbox-"))

    try:
        bundle = common.copy_bundle(FIXTURE, workspace / "MyShow")
        log = workspace / "session.wfglog"

        with Server(bundle, log=log, locale=locale) as server:
            # -- 1. the document loaded, and a client can read it ------------
            root = common.http_json(server.http_port, "/")
            report.check("CONTENTS" in root, "GET / returns a tree with CONTENTS")

            host = common.http_json(server.http_port, "/?HOST_INFO")
            report.equal(host.get("NAME"), "Go.dot", "HOST_INFO names the product")
            report.equal(host.get("WS_PORT"), server.http_port,
                         "HOST_INFO's WS_PORT is the port we are talking to")
            report.equal(host.get("OSC_PORT"), server.osc_port,
                         "HOST_INFO's OSC_PORT is the UDP port serve reported")

            # The clock numbers, which PRD 6.2 wants distinguishable from "no
            # clock". Three zeroes here would mean a stopped engine.
            rate = common.http_json(server.http_port, "/godot/engine/sampleRate?VALUE")
            report.equal(rate.get("VALUE", [None])[0], 48000,
                         "the engine reports the sample rate it was given")

            deadline = time.monotonic() + 10.0
            ticking = False

            while time.monotonic() < deadline:
                tick = common.http_json(server.http_port, "/godot/engine/tick?VALUE")
                if tick.get("VALUE", [0])[0] > 0:
                    ticking = True
                    break
                time.sleep(0.05)

            report.check(ticking, "the tick advances without anybody asking it to")

            # -- 2. a cue to work with ---------------------------------------
            lists = common.http_json(server.http_port, "/godot/list")
            list_ids = sorted((lists.get("CONTENTS") or {}).keys())
            report.check(bool(list_ids), "the fixture publishes at least one list")

            if not list_ids:
                return report.finish()

            cues = common.http_json(server.http_port, "/godot/cue")
            cue_ids = sorted((cues.get("CONTENTS") or {}).keys())
            report.check(bool(cue_ids), "the fixture publishes at least one cue")

            if not cue_ids:
                return report.finish()

            cue = cue_ids[0]
            name_address = f"/godot/cue/{cue}/name"

            # -- 3. a write over UDP, read back over HTTP --------------------
            #
            # READ FIRST, AND WRITE SOMETHING ELSE. An earlier version of this
            # wrote "House to half" — which is what the fixture already says —
            # so it passed for months of nothing working: writes were being
            # dropped entirely and the check could not tell. Every value below
            # is one no fixture contains, and each is asserted to DIFFER from
            # what was there before.
            status, body = common.http_get(server.http_port, f"{name_address}?VALUE")
            report.equal(status, 200, "the cue's name is readable before anything writes")

            original = json.loads(body).get("VALUE", [None])[0] if status == 200 else None

            report.check(original != "wrote-over-udp",
                         "the fixture does not already hold the value about to be written",
                         "otherwise the next check passes without a write happening")

            common.send_udp(server.osc_port,
                            common.osc_encode(name_address, ["wrote-over-udp"]))

            settle(server, name_address, "wrote-over-udp", report,
                   "a UDP write reaches the document and is readable over HTTP")

            # -- 4. a write over the WebSocket, with a subscription ----------
            client = common.WSClient(server.http_port, "driver")

            try:
                client.listen(name_address)
                time.sleep(0.3)         # let the LISTEN land before writing

                client.send_osc(name_address, ["wrote-over-websocket"])

                settle(server, name_address, "wrote-over-websocket", report,
                       "a WebSocket write reaches the document too")

                # Echo suppression: this client caused it, so it is not told.
                own = client.pushes_for(name_address)
                report.check(not own,
                             "the client that caused a change is not told about it",
                             f"got {own!r}")

                # But somebody else's change does reach it.
                common.send_udp(server.osc_port,
                                common.osc_encode(name_address, ["wrote-by-somebody-else"]))

                pushed = client.wait_for_push(name_address, timeout=10.0)
                report.check(pushed == ["wrote-by-somebody-else"],
                             "a subscriber is told when somebody else changes a node",
                             f"got {pushed!r}")

                # -- 5. standby, which is the one piece of engine state ------
                standby_address = f"/godot/list/{list_ids[0]}/standby"

                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/standby/set", [cue]))

                settle(server, standby_address, cue, report,
                       "standby.set over UDP moves the standby pointer")

                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/standby/next"))
                time.sleep(0.5)

                after, _ = common.http_get(server.http_port,
                                           f"{standby_address}?VALUE")
                report.equal(after, 200, "standby is still readable after next")

                # -- 6. a rejection is reportable ----------------------------
                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/no/such/command"))
                time.sleep(0.5)

                errors = common.http_json(server.http_port,
                                          "/godot/engine/errorCount?VALUE")
                report.check(errors.get("VALUE", [0])[0] > 0,
                             "an unknown command is counted as an error",
                             "OSC has no reply channel, so errorCount and "
                             "lastError are the only way a client can find out")

                # -- 7. a malformed datagram is dropped, not applied ---------
                common.send_udp(server.osc_port, b"/x\0\0,fff")
                time.sleep(0.5)

                still, _ = common.http_get(server.http_port, "/")
                report.equal(still, 200,
                             "a malformed datagram does not take the server with it")

                # -- 8. save -------------------------------------------------
                common.send_udp(server.osc_port,
                                common.osc_encode("/godot/cmd/document/save"))
                time.sleep(1.0)
            finally:
                client.close()

        # -- 9. the show on disk carries what the session decided ------------
        show = (bundle / "show.xml").read_text(encoding="utf-8")
        report.check("wrote-by-somebody-else" in show,
                     "the saved show.xml holds the last name written",
                     "a session that changed a show and saved it must leave "
                     "that change on disk")

        # -- 10. the log replays, record for record --------------------------
        report.check(log.is_file(), "the session wrote an event log")

        if log.is_file():
            # --bundle, because a replay without the show has no commands
            # registered and reports every applied record as unknown-command.
            # --out so the save in the log writes somewhere harmless rather
            # than over the bundle being checked.
            code, out, err = common.run_wfg(
                "replay", str(log),
                f"--bundle={bundle}",
                f"--out={workspace / 'replayed'}",
                f"--wfg-locale={locale or 'C'}")

            report.equal(code, 0,
                         "wfg replay reproduces the session's own log exactly",
                         (out + err).strip()[:2000])

            text = log.read_text(encoding="utf-8")

            report.check(any(line.startswith("X ") for line in text.splitlines()),
                         "the malformed datagram is in the log as an X record",
                         "a dropped packet with no record is one nobody can act on")

            report.check(any(line.startswith("R ") for line in text.splitlines()),
                         "the unknown command is in the log as an R record")

            report.check(any("document.save" in line
                             for line in text.splitlines()),
                         "the save is in the log as a command like any other")

        return report.finish()
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


def main() -> int:
    locale = None

    for arg in sys.argv[1:]:
        if arg.startswith("--wfg-locale="):
            locale = arg.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as exc:
        print(f"\nphase1 session: CANNOT RUN: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
