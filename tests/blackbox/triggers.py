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

"""A datagram from outside fires a cue, and the operator does not lose their place.

WHAT THIS IS FOR. The matching is three pure functions and the unit suite
exercises them exhaustively; what it cannot reach is the wiring — that a real
datagram, arriving on a real socket, on a thread that may not read the document,
reaches the right cue. That is four lines of code and it is the four lines that
would silently do nothing.

AND THE PROPERTY THE WHOLE FEATURE RESTS ON: the standby does not move. Section
3.5 and section 3.7 both say it, and it is the reason a background list can be
driven by something other than a person without the person losing their place.
An operator who found their pointer somewhere else after a lighting desk sent a
message would never trust the show file again.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "triggers"


def value_at(port: int, address: str):
    status, body = common.http_get(port, address + "?VALUE")

    if status == 204:
        return None
    if status != 200:
        raise HarnessError(f"GET {address}?VALUE returned {status}")

    return json.loads(body)["VALUE"][0]


def settles(port: int, address: str, wanted, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if value_at(port, address) == wanted:
            return True

        time.sleep(0.02)

    return value_at(port, address) == wanted


def run(locale: "str | None") -> int:
    report = Report(f"a trigger fires a cue and moves nothing ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-triggers-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "triggers")
        log = Path(scratch) / "session.wfglog"

        with Server(bundle, locale=locale, log=log) as server:
            port = server.http_port

            report.equal(value_at(port, "/godot/trigger/TRG00012/address"), "/desk/chime",
                         "the trigger is published where a client can read it")
            report.equal(value_at(port, "/godot/trigger/TRG00012/cue"), "TRG00011",
                         "and says which cue it fires, derived from where it sits")

            standby = value_at(port, "/godot/list/TRG00001/standby")

            """THE DATAGRAM. No arguments at all, which is the case that would
            have been swallowed: a bare address is what a foot switch sends, and
            the namespace's "a message with no arguments is not a write" return
            sits directly after the trigger match for exactly that reason."""
            common.send_udp(server.osc_port, common.osc_encode("/desk/chime"))

            deadline = time.monotonic() + 5.0
            fired = ""

            while time.monotonic() < deadline and not fired:
                fired = value_at(port, "/godot/run/order") or ""
                time.sleep(0.02)

            report.check(bool(fired), "the cue ran", f"/godot/run/order is {fired!r}")

            if fired:
                run_id = fired.split()[0]
                report.equal(value_at(port, f"/godot/run/{run_id}/cue"), "TRG00011",
                             "and it is the cue the trigger names")

            report.equal(value_at(port, "/godot/list/TRG00001/standby"), standby,
                         "and the operator's pointer did not move")

            """AN ADDRESS NOBODY LISTENS TO takes the road it always took, which
            for an argument-less message is no road at all: there is nothing to
            write and nothing to fire, so nothing is submitted and nothing is
            logged. A message that quietly became something would be worse than
            one that does nothing."""
            before = value_at(port, "/godot/engine/errorCount")
            common.send_udp(server.osc_port, common.osc_encode("/desk/nothing"))
            time.sleep(0.4)

            report.equal(value_at(port, "/godot/engine/errorCount"), before,
                         "an address nobody listens to is not an error either")

            """A DISABLED TRIGGER IS STILL WRITTEN DOWN AND FIRES NOTHING, which
            is the whole difference between turning one off during tech and
            deleting it."""
            common.WSClient(port).send_osc("/godot/trigger/TRG00012/enabled", ["false"])
            report.check(settles(port, "/godot/trigger/TRG00012/enabled", False),
                         "the trigger can be turned off")

            runs_before = value_at(port, "/godot/run/order")
            common.send_udp(server.osc_port, common.osc_encode("/desk/chime"))
            time.sleep(0.5)

            report.equal(value_at(port, "/godot/run/order"), runs_before,
                         "and then it fires nothing")

        """AND THE RECORD SAYS WHAT FIRED. The origin says where the message
        came from - which is not enforced anywhere, by design - so the trigger's
        identity travels as the argument, where a replay can be handed it."""
        records = [line for line in log.read_text(encoding="utf-8").splitlines()
                   if line.startswith("A ") and "trigger.fire" in line]

        report.equal(len(records), 1, "one trigger.fire in the log", "\n".join(records))

        if records:
            report.check("TRG00012" in records[0],
                         "naming the trigger that fired", records[0])
            report.check("udp:" in records[0],
                         "and the origin it came from", records[0])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"triggers: {problem}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"triggers: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
