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
"""Phase 5's document half, as a program. Started by PR 5.3 with the edit lock.

WHAT THE LOCK PROMISES, in the words of decision W: show mode is a lock on the
SHOW, kept by the engine, and not a mode in a client. The tablet in the house,
an MCP client and somebody's renumbering script all reach the same commands
through the same sockets, and a client that hides its own buttons has locked
exactly one of them. So everything below is asked of the shipped binary over
UDP and HTTP by a program that shares no code with it, which is the only way to
ask "does the engine refuse" rather than "does a client decline to send".

THE FOUR THINGS IT MUST DO, in the order the PR states them: lock, and see an
edit refused with `/godot/engine/lastError` naming `locked` and the command;
GO, and see the standby move; write to a MOUNTED address, and see the device
receive it; unlock, and see the same edit applied. Around them, the claims only
a running engine can make: that a bundle saved locked opens locked, that the
lock and its release do not light the dirty dot, and that the session's own log
replays - refusals included - record for record.

THE BUNDLE is `fixtures/bundles/locked/`, which carries `<Show locked="true"/>`
in its state.xml. Two lists of memos, so a GO has somewhere to move the pointer
and a second list has a pointer that must not move; and a mount on a desk with
one writable fader, pointed by the driver at `mock_target.py`.

WAITED FOR, NEVER SLEPT. Every check waits for the thing it is about -
`common.wait_until` - because a datagram crosses a socket, joins a queue and is
applied on the next tick, and how long that takes on a loaded runner is not a
number anybody has measured. A refusal is waited for as the error COUNT moving,
which is the one reading that cannot be satisfied by an earlier refusal.

Exit codes as the rest of the suite: 0 everything held, 1 something did not,
2 the harness could not run.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "locked"

LIST = "K5ACT001"
FOYER = "K5FYR001"
BEGINNERS = "K5MEM001"
HOUSE = "K5MEM002"

LOCK = "/godot/document/locked"
DIRTY = "/godot/document/dirty"
FADER = "/desk/fader"


# =============================================================================
# Reading the engine
# =============================================================================

def value_of(server: Server, address: str):
    """The node's value as the engine last published it, or None."""
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return common.json.loads(body)["VALUE"][0]
    except (ValueError, KeyError, IndexError):
        return None


def reads(server: Server, address: str, wanted, timeout: float = common.REPLY_TIMEOUT) -> bool:
    """Waits until a node reads `wanted`. Compared with ==, never by
    truthiness: a node that is not there reads None, and None is not False."""
    return common.wait_until(lambda: value_of(server, address) == wanted,
                             timeout=timeout) is not None


def error_count(server: Server) -> int:
    value = value_of(server, "/godot/engine/errorCount")
    return value if isinstance(value, int) else -1


def order_of(server: Server, list_id: str) -> "list[str]":
    return str(value_of(server, f"/godot/list/{list_id}/order") or "").split()


# =============================================================================
# Writing to it
# =============================================================================

def command(server: Server, name: str, args: "list | None" = None) -> None:
    """A named command (PRD 4.11), published at /godot/cmd/<dotted/as/slashes>."""
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def write(server: Server, address: str, value) -> None:
    """A value to a node, as a plain datagram: the engine makes it `node.set`.

    The lock is written this way and not by a command of its own, which is
    the design (namespace draft §14.7): a `document.lock` would be a second
    door onto one attribute, and the second door is the one that forgets."""
    common.send_udp(server.osc_port, common.osc_encode(address, [value]))


def refusal_of(server: Server, send) -> str:
    """Sends one gesture and waits for the engine to COUNT a refusal; answers
    with `/godot/engine/lastError` then, or "" when nothing was refused.

    The count and not the text, because a text that already ends the way the
    check wants could be the previous refusal's."""
    before = error_count(server)
    send()

    counted = common.wait_until(lambda: error_count(server) > before)

    if not counted:
        return ""

    return str(value_of(server, "/godot/engine/lastError") or "")


# =============================================================================
# Reading the desk
# =============================================================================

def desk_received(target) -> int:
    """How many datagrams the mock has taken, ever. Counted rather than read
    back, because a value read back cannot tell a write from its absence when
    the desk already held it."""
    try:
        reply = common.http_json(target.query_port, "/_mock/received")
        return int(reply["VALUE"][0])
    except (HarnessError, OSError, ValueError, KeyError, IndexError):
        return -1


def desk_value(target, address: str):
    try:
        reply = common.http_json(target.query_port, f"{address}?VALUE")
        return reply["VALUE"][0]
    except (HarnessError, OSError, ValueError, KeyError, IndexError):
        return None


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 5: the document, locked and unlocked ({locale or 'C'})")

    if not FIXTURE.is_dir():
        raise HarnessError(f"no fixture bundle at {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="wfg-phase5-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "locked")
        log = room / "session.wfglog"
        replayed = room / "replayed"

        with first_sound.MockTarget("agree") as target:
            first_sound.point_mount_at(bundle, target)

            with Server(bundle, log=log, locale=locale) as server:

                # --- the lock comes back with the show -----------------------
                #  Plan decision 6: a show locked at 20:40 whose engine was
                #  restarted at 20:44 comes back locked, or the first thing a
                #  rebooted engine does is un-protect a running performance.
                #  The unit suite proves the bytes round-trip; only a process
                #  opening a folder proves the binary honours them.
                report.check(reads(server, LOCK, True),
                             "a bundle saved locked opens locked",
                             f"{LOCK} reads {value_of(server, LOCK)!r}")

                access = common.http_json(server.http_port, f"{LOCK}?ACCESS").get("ACCESS")
                report.equal(access, 3,
                             "and the lock is published writable, so any client can find it")

                report.equal(value_of(server, DIRTY), False,
                             "a show that has just been opened has nothing to save")

                # --- the release gets through the lock it releases ----------
                write(server, LOCK, False)
                report.check(reads(server, LOCK, False),
                             "unlocking is a plain write, and the lock lets it through",
                             f"{LOCK} reads {value_of(server, LOCK)!r}")

                # --- and setting it ------------------------------------------
                write(server, LOCK, True)
                report.check(reads(server, LOCK, True),
                             "locking is a plain write too",
                             f"{LOCK} reads {value_of(server, LOCK)!r}")

                report.equal(value_of(server, DIRTY), False,
                             "and neither is an edit to the show: the dot stays out")

                # --- an edit, refused, and said so --------------------------
                order_before = order_of(server, LIST)

                said = refusal_of(server, lambda: command(
                    server, "cue.create", [LIST, 0, "memo", "Added under the lock"]))

                report.check(said.endswith(" locked cue.create"),
                             "cue.create on a locked show is refused, and lastError says "
                             "locked and names the command",
                             f"lastError reads {said!r}")

                #  `<tick> <seq> <origin> <reason> <command>`: the origin is
                #  what tells the tablet in the house from the booth.
                fields = said.split(" ")
                report.check(len(fields) == 5 and fields[2].startswith("udp:"),
                             "and names the machine that sent it",
                             f"lastError reads {said!r}")

                report.equal(order_of(server, LIST), order_before,
                             "the refusal changed nothing: the list is what it was")
                report.equal(value_of(server, DIRTY), False,
                             "and the dot is still out")

                # A write to a value show.xml records: the fourth door.
                said = refusal_of(server, lambda: write(
                    server, f"/godot/cue/{HOUSE}/name", "Renamed under the lock"))

                report.check(said.endswith(" locked node.set"),
                             "a write to a show value is refused the same way",
                             f"lastError reads {said!r}")
                report.equal(value_of(server, f"/godot/cue/{HOUSE}/name"), "House to half",
                             "and the name is what it was")

                # --- GO -------------------------------------------------------
                #  The reason show mode exists at all: an operator must be able
                #  to GO while nothing else can edit (PRD 4.1). And only GO
                #  moves the standby - the other list's pointer is read before
                #  and after, as the phase 4 driver reads its foyer's.
                report.check(reads(server, f"/godot/list/{LIST}/standby", BEGINNERS),
                             "the standby is where state.xml left it")

                foyer_before = value_of(server, f"/godot/list/{FOYER}/standby")
                errors_before = error_count(server)

                command(server, "go")

                report.check(reads(server, f"/godot/list/{LIST}/standby", HOUSE),
                             "GO on a locked show is applied, and the standby moves",
                             f"standby reads {value_of(server, f'/godot/list/{LIST}/standby')!r}")
                report.equal(value_of(server, f"/godot/list/{FOYER}/standby"), foyer_before,
                             "and moves nobody else's pointer")
                report.equal(error_count(server), errors_before,
                             "and nothing was refused on the way")
                report.equal(value_of(server, DIRTY), False,
                             "and a GO is not an edit either")

                # --- a hand on a level from the back of the room ------------
                #  PRD 3.17's third role for the tablet: "parameter adjustment
                #  while walking the house". A mounted write never reaches the
                #  document, so no door is on its way to refuse it - locking
                #  that would be locking the mixing, which is the opposite of
                #  the point.
                received_before = desk_received(target)
                write(server, FADER, 0.5)

                arrived = common.wait_until(lambda: desk_received(target) > received_before)
                report.check(bool(arrived),
                             "a write to a mounted address reaches the device through the lock",
                             f"the desk has taken {desk_received(target)} datagrams, "
                             f"{received_before} before the write")

                held = common.wait_until(
                    lambda: abs(float(desk_value(target, FADER)) - 0.5) < 1e-6)
                report.check(bool(held), "and the desk holds what was written",
                             f"the desk holds {desk_value(target, FADER)!r}")

                # --- unlocked, the same edit is applied ---------------------
                write(server, LOCK, False)
                report.check(reads(server, LOCK, False), "the lock lifts")

                command(server, "cue.create", [LIST, 0, "memo", "Added once it was lifted"])

                grew = common.wait_until(
                    lambda: len(order_of(server, LIST)) == len(order_before) + 1)
                report.check(bool(grew),
                             "unlocked, the same cue.create is applied",
                             f"the list holds {order_of(server, LIST)}")

                report.check(reads(server, DIRTY, True),
                             "and that one IS an edit: the dot comes on",
                             f"{DIRTY} reads {value_of(server, DIRTY)!r}")

                #  The log is read by the replay below, so the create's record
                #  has to be in it before the server is stopped - the record is
                #  written after the command is applied, and a snapshot showing
                #  the new cue is not the log line that says so.
                common.wait_until(
                    lambda: "Added once it was lifted" in log.read_text(encoding="utf-8"))

        # --- and it reproduces, refusals and all -------------------------------
        #  A REFUSAL REPLAYS AS A REFUSAL, which is what putting `locked` in the
        #  log's reason vocabulary promised: the replay opens the same bundle -
        #  locked, from its state.xml - applies the same datagrams in the same
        #  ticks, and has to be refused by the same doors.
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))

        report.equal(code, 0, "and `wfg replay` reproduces the session record for record",
                     (out + err).strip()[:2000])

        text = log.read_text(encoding="utf-8") if log.is_file() else ""
        refusals = [line for line in text.splitlines()
                    if line.startswith("R ") and " locked " in line]

        report.equal(len(refusals), 2,
                     "the log holds both refusals, each with the reason locked",
                     "\n".join(refusals))

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"phase5: {problem}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"phase5: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
