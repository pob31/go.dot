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
"""Phase 5's document half, as a program. The edit lock (5.3), then undo (5.4).

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

AND WHAT UNDO PROMISES, which is the half PR 5.4 adds and which namespace draft
§14.9 lists as the six things only a running engine can be asked. That
`undoName` says which COMMAND a press would unmake, so a client writes "Undo
object.delete" without a lookup table. That ten drags on one address come back
with ONE press - asserted as the undo STEP and never as a count of actions,
because JUCE merges one action fewer than the parenthetical promises and a
driver written to the parenthetical would be red for something that is not a
bug. That the lock refuses `undo` in its handler, where the doors cannot see
it. That a group deleted, undone and read back holds the same identifiers,
which is the only assertion that proves the identifier registry was rebuilt.
That an empty stack answers `nothing-to-undo`. And that deleting a cue whose
run is live neither disturbs the run nor, on the undo, starts a second one -
the seam no unit test reaches, because it wants a live run, a document edit and
a publish in one process at one moment.

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

import socket
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
CURTAIN = "K5MEM003"

#  WHAT PR 5.4 MAKES, with identifiers the driver chooses rather than reads
#  back. "The same identifiers came back" is only a claim if it is a comparison
#  against something written down before the delete; against whatever the last
#  read happened to say it would hold even if every one of them had changed.
#  `cue.create` takes one as an optional last argument - the convention that
#  makes replay work without randomness - and Crockford's alphabet is what makes
#  these legal: eight digits, and no I, L, O or U.
SCENE = "K5GRP001"
FIRST = "K5GRP002"
SECOND = "K5GRP003"
SOUNDING = "K5MED001"

#  The last thing this session writes, waited for in the log so that every
#  record before it has been written too.
LAST_WORD = "The last thing this session wrote"

LOCK = "/godot/document/locked"
DIRTY = "/godot/document/dirty"
CAN_UNDO = "/godot/document/canUndo"
CAN_REDO = "/godot/document/canRedo"
UNDO_NAME = "/godot/document/undoName"
REDO_NAME = "/godot/document/redoName"
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


def children_of(server: Server, cue_id: str) -> "list[str]":
    """A group's members, as its own order node spells them. This is where a
    colliding identifier shows itself and nowhere else: two objects wearing one
    name are one identifier written twice in the order of whatever contains
    them."""
    return str(value_of(server, f"/godot/cue/{cue_id}/order") or "").split()


def runs_for(server: Server, cue_id: str) -> "list[str]":
    """Every run instantiating a cue.

    A run says which cue it is and nothing says the other way round, so asking
    every run is the only way to ask. A LIST rather than one answer, because
    "the run that was live, and not a second one beside it" is precisely
    what the undo of a delete has to be held to."""
    return [run for run in str(value_of(server, "/godot/run/order") or "").split()
            if value_of(server, f"/godot/run/{run}/cue") == cue_id]


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


def drag(server: Server, address: str, values: "list") -> None:
    """Several writes to one address from ONE socket, which is what makes them
    one drag rather than several edits.

    §14.9 coalesces consecutive `node.set` on the same address, from the same
    ORIGIN, within twenty-five ticks - and the origin of a datagram is
    `udp:<ip>:<port>`, the sender's own port. `common.send_udp` opens a fresh
    socket for every call, so ten writes sent that way carry ten origins and
    are ten operators as far as the engine can tell; it would honestly give
    them ten undo steps, and the check below would fail for a reason that is
    the driver's rather than the engine's. A slider under one finger sends from
    one socket, so this does too."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for value in values:
            sock.sendto(common.osc_encode(address, [value]),
                        (common.HOST, server.osc_port))


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

                # --- a stack that begins empty, and says so -------------------
                #  §14.4: the four undo rows persist nowhere, because they read
                #  a stack that begins empty at every open and a restored
                #  `undoName` would name a transaction no UndoManager holds.
                #
                #  ASKED HERE, and the position is the whole of why it is
                #  trustworthy: the show is unlocked and nothing has been edited
                #  yet, so `nothing-to-undo` is the only answer either order of
                #  checks could give. Under the lock the same press is refused
                #  `locked` - which is asserted further down, where there is
                #  something on the stack for the two answers to disagree about.
                report.equal(value_of(server, CAN_UNDO), False,
                             "a document nobody has edited has nothing to take back")
                report.equal(value_of(server, CAN_REDO), False,
                             "and nothing to put back")
                report.equal(str(value_of(server, UNDO_NAME) or ""), "",
                             "and no transaction to name")

                said = refusal_of(server, lambda: command(server, "undo"))

                report.check(said.endswith(" nothing-to-undo undo"),
                             "and an undo against it is refused with nothing-to-undo",
                             f"lastError reads {said!r}")

                #  AND THE WRITE THAT GOT US HERE LEFT NOTHING BEHIND. `locked`
                #  is a `persist=state` row and state rows are off the stack
                #  (plan decision 3), so unlocking a show is not an edit anybody
                #  could take back by pressing Ctrl-Z - which matters because
                #  the press that took the lock off would be the one an operator
                #  used next, on the edit they had actually come to undo.
                report.equal(value_of(server, CAN_UNDO), False,
                             "and writing the lock itself put nothing on the stack")

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

                # --- the transaction is named after the command --------------
                #  §14.9: each transaction is named for the command that opened
                #  it, and `undoName` publishes the name of the one an undo
                #  WOULD UNMAKE - which is the last that actually performed an
                #  action rather than the one the hook has just opened, since
                #  beginning a transaction allocates nothing. So the word a
                #  client shows is the command's own, and "Undo cue.create" is a
                #  sentence written without a lookup table that could go stale.
                report.check(reads(server, UNDO_NAME, "cue.create"),
                             "the create went on the stack under the command's own name",
                             f"{UNDO_NAME} reads {value_of(server, UNDO_NAME)!r}")
                report.equal(value_of(server, CAN_UNDO), True,
                             "and the document says there is something to take back")

                # --- a rename, taken back, and put back ----------------------
                write(server, f"/godot/cue/{CURTAIN}/name", "Curtain up, renamed")
                report.check(reads(server, f"/godot/cue/{CURTAIN}/name", "Curtain up, renamed"),
                             "a rename is applied")
                report.check(reads(server, UNDO_NAME, "node.set"),
                             "and undoName after a rename is node.set",
                             f"{UNDO_NAME} reads {value_of(server, UNDO_NAME)!r}")

                command(server, "undo")

                report.check(reads(server, f"/godot/cue/{CURTAIN}/name", "Curtain up"),
                             "one undo puts the name back",
                             f"the cue reads "
                             f"{value_of(server, f'/godot/cue/{CURTAIN}/name')!r}")
                report.check(reads(server, REDO_NAME, "node.set"),
                             "and the same word is now on the redo half",
                             f"{REDO_NAME} reads {value_of(server, REDO_NAME)!r}")

                command(server, "redo")

                report.check(reads(server, f"/godot/cue/{CURTAIN}/name", "Curtain up, renamed"),
                             "and redo puts the rename back")

                # --- ten drags, and ONE press to take them back --------------
                #  §14.9's coalescing rule is the address, the origin and
                #  twenty-five ticks. A number field dragged in an inspector
                #  sends one `node.set` per change event and a slider under a
                #  finger one per frame, so an undo that took back one of them
                #  would be a keystroke somebody had to hold down - which is how
                #  an operator overshoots into the edit before the one they
                #  meant.
                #
                #  THE UNDO STEP IS ASSERTED AND THE ACTIONS ARE NEVER COUNTED.
                #  JUCE refuses to merge an action that ADDS a property, and
                #  this document omits defaults - an absent attribute IS its
                #  default - so the first write to an attribute a cue does not
                #  yet carry makes an adding action that never merges with the
                #  next, and ten writes are one transaction of TWO actions. The
                #  design is unharmed, because the step is the transaction; what
                #  would be harmed is a check written to the parenthetical.
                pre_wait = f"/godot/cue/{CURTAIN}/preWait"

                report.equal(value_of(server, pre_wait), 0,
                             "the address the drag is aimed at is at its default")

                drag(server, pre_wait, [str(n) for n in range(1, 11)])

                report.check(reads(server, pre_wait, 10),
                             "ten writes to one address from one sender all land",
                             f"{pre_wait} reads {value_of(server, pre_wait)!r}")
                report.check(reads(server, UNDO_NAME, "node.set"),
                             "and they are one transaction, named node.set")

                command(server, "undo")

                report.check(reads(server, pre_wait, 0),
                             "and ONE undo takes the whole drag back rather than the last "
                             "write of it",
                             f"{pre_wait} reads {value_of(server, pre_wait)!r}, which is the "
                             "ninth write if the ten did not coalesce")

                # --- the lock refuses undo in undo's own handler -------------
                #  §14.11: `undo` and `redo` knock at none of the four doors.
                #  JUCE's actions write through the tree rather than through
                #  ShowDocument, so a predicate at the doors would see nothing
                #  of an undo - and half a transaction undone is worse than
                #  none. Asked with something on the stack, so `locked` is the
                #  only answer available: the empty stack was asked about at the
                #  top of this session, where `nothing-to-undo` was the only one.
                write(server, LOCK, True)
                report.check(reads(server, LOCK, True), "the show is locked again")

                said = refusal_of(server, lambda: command(server, "undo"))

                report.check(said.endswith(" locked undo"),
                             "an undo on a locked show is refused, and lastError says locked "
                             "and names the command",
                             f"lastError reads {said!r}")
                report.equal(value_of(server, f"/godot/cue/{CURTAIN}/name"),
                             "Curtain up, renamed",
                             "and the edit it would have taken back is still there")

                write(server, LOCK, False)
                report.check(reads(server, LOCK, False), "the lock lifts again")

                # --- a group deleted, undone, and read back ------------------
                #  THE ONE ASSERTION THAT PROVES THE REGISTRY WAS REBUILT
                #  (§14.9). Undo touches IdRegistry not at all, and does not
                #  need to: removing a child with a manager builds one action
                #  holding a ref-counted handle to it, and undoing that action
                #  re-adds the same node with its `id` property still on it. So
                #  the document is self-consistent under undo. What is not is
                #  the REGISTRY, which released every identifier under the node
                #  on the way out - and the failure is not this gesture but the
                #  next create, which can be handed an identifier a restored cue
                #  is already using.
                command(server, "cue.create", [LIST, 0, "group", "The scene that goes", SCENE])

                report.check(bool(common.wait_until(lambda: SCENE in order_of(server, LIST))),
                             "a group is created at the top of the list",
                             f"the list holds {order_of(server, LIST)}")

                command(server, "cue.create", [SCENE, 0, "memo", "First", FIRST])
                command(server, "cue.create", [SCENE, 1, "memo", "Second", SECOND])

                report.check(bool(common.wait_until(
                                 lambda: children_of(server, SCENE) == [FIRST, SECOND])),
                             "with two cues inside it",
                             f"the group holds {children_of(server, SCENE)}")

                command(server, "object.delete", [SCENE])

                report.check(bool(common.wait_until(lambda: SCENE not in order_of(server, LIST))),
                             "and object.delete takes the group and its contents with it",
                             f"the list holds {order_of(server, LIST)}")

                command(server, "undo")

                report.check(bool(common.wait_until(lambda: SCENE in order_of(server, LIST))),
                             "one undo brings the whole subtree back",
                             f"the list holds {order_of(server, LIST)}")
                report.equal(children_of(server, SCENE), [FIRST, SECOND],
                             "under the identifiers it had, in the order it had them in")

                order = order_of(server, LIST)
                report.equal(len(set(order)), len(order),
                             "and no identifier appears twice in the list",
                             f"the list holds {order}")

                #  AND THE NEXT CREATE, which is where a registry that had
                #  forgotten those identifiers would show it: `generate()` draws
                #  from 2^40 and inserts whatever it finds free, so an
                #  identifier it has released is free to be handed out a second
                #  time - two objects with one identity, every reference to
                #  either pointing at a coin toss.
                before_create = order_of(server, LIST)
                command(server, "cue.create", [LIST, 0, "memo", "Created after the undo"])

                grew = common.wait_until(
                    lambda: len(order_of(server, LIST)) == len(before_create) + 1)
                report.check(bool(grew), "a cue created afterwards is created",
                             f"the list holds {order_of(server, LIST)}")

                fresh = [one for one in order_of(server, LIST) if one not in before_create]

                report.equal(len(fresh), 1, "and takes exactly one new identifier",
                             f"the list gained {fresh}")
                report.check(all(one not in (SCENE, FIRST, SECOND) for one in fresh),
                             "which is not one a restored cue is already using",
                             f"the list gained {fresh}")

                #  A COLLISION OUT OF 2^40 CANNOT BE FORCED, and a driver that
                #  pretended otherwise would be asserting a coin toss. So the
                #  registry is asked the same question directly, through the one
                #  door that answers it: a create may NAME its identifier - the
                #  convention that lets a replay re-supply what a session drew -
                #  and refuses `unknown-id` when the name is malformed or
                #  already taken. A registry that had not been rebuilt would
                #  have taken this one, and the duplicate would have shown up in
                #  the group's order node, which is where §14.9 says a collision
                #  shows up and nowhere else.
                said = refusal_of(server, lambda: command(
                    server, "cue.create", [SCENE, 0, "memo", "A second First", FIRST]))

                report.check(said.endswith(" unknown-id cue.create"),
                             "an identifier a restored cue holds is refused to the next caller "
                             "that asks for it by name",
                             f"lastError reads {said!r}")
                report.equal(children_of(server, SCENE), [FIRST, SECOND],
                             "so the group still holds two cues, and neither of them twice")

                # --- a cue deleted while its run is playing ------------------
                #  THE SEAM NO UNIT TEST REACHES (§14.9). It wants a live run, a
                #  document edit removing the cue that run points at, and a
                #  publish afterwards, in one process at one moment; a unit test
                #  that assembled a runner, a run table, a tick loop and a
                #  document would be this driver with the transport taken off.
                #  The design's answer is that the run table holds its own
                #  COPIES - `Run::cue`, `Run::kind` - and no handle into the
                #  tree, so the delete cannot stop the sound and the undo cannot
                #  start it again.
                #
                #  SAID HONESTLY: this session has no `--hosted` graph, so what
                #  is live here is a run and not a sound, and the run does not
                #  reach `playing` at all - `launchIfDue` returns before it does
                #  anything when there is no audio side to ask, so the run sits
                #  at `armed` for as long as the process does. This driver
                #  therefore reads whatever live state GO leaves and asserts
                #  that the delete and the undo do not DISTURB it, which is the
                #  claim: a run is not in the document, so a document edit
                #  cannot reach it. Whether that run is armed or sounding is the
                #  audio side's business, and `first_sound.py` is where the
                #  sound itself is asserted. What is real here is the run table,
                #  the document edit and the publish - the three the claim is
                #  about.
                order_before_media = order_of(server, LIST)

                command(server, "cue.create",
                        [LIST, len(order_before_media), "media",
                         "The one that is playing", SOUNDING])

                report.check(bool(common.wait_until(lambda: SOUNDING in order_of(server, LIST))),
                             "a media cue is created at the foot of the list",
                             f"the list holds {order_of(server, LIST)}")

                command(server, "standby.set", [SOUNDING])
                report.check(reads(server, f"/godot/list/{LIST}/standby", SOUNDING),
                             "the standby is parked on it")

                command(server, "go")

                found = common.wait_until(lambda: runs_for(server, SOUNDING))
                sounding_run = found[0] if found else ""

                report.check(bool(sounding_run), "GO gives the media cue a run of its own")

                state = f"/godot/run/{sounding_run}/state"

                live_state = value_of(server, state)

                report.check(live_state not in ("", "done", "failed"),
                             "and GO leaves that run live",
                             f"the run reads {live_state!r}")

                command(server, "object.delete", [SOUNDING])

                report.check(bool(common.wait_until(
                                 lambda: SOUNDING not in order_of(server, LIST))),
                             "the cue is deleted while its run is playing",
                             f"the list holds {order_of(server, LIST)}")
                report.equal(value_of(server, state), live_state,
                             "and the delete did not disturb the run: a run is not in the document")
                report.equal(runs_for(server, SOUNDING), [sounding_run],
                             "which is still in the run table, still naming the cue that has "
                             "gone, out of the copy it took")

                #  READ AFTER THE DELETE AND NOT BEFORE IT, because the delete
                #  is allowed to move this: `remove` repairs the containing
                #  list's standby when the cue the pointer was on goes. What the
                #  undo may not do is move it back.
                standby_after_delete = value_of(server, f"/godot/list/{LIST}/standby")

                command(server, "undo")

                report.check(bool(common.wait_until(lambda: SOUNDING in order_of(server, LIST))),
                             "and one undo brings the cue back",
                             f"the list holds {order_of(server, LIST)}")
                report.equal(runs_for(server, SOUNDING), [sounding_run],
                             "without starting a second one: undoing a delete is not a GO")
                report.equal(value_of(server, state), live_state,
                             "and the run that was live is the one that still is, in the state "
                             "it was already in")

                #  AND THE POINTER STAYS WHERE THE DELETE LEFT IT, which §14.9
                #  decides rather than overlooks: undo restores what somebody
                #  DECIDED, and where the operator is standing is not among
                #  those things. A standby that jumped backwards on Ctrl-Z would
                #  be the machine moving the pointer, which §3.5 forbids for the
                #  same reason it forbids a trigger doing it.
                report.equal(value_of(server, f"/godot/list/{LIST}/standby"),
                             standby_after_delete,
                             "and the pointer is where the delete left it, not where it was "
                             "before somebody deleted anything")

                # --- one last edit, and the log caught up ---------------------
                #  The log is read by the replay below, so this session's last
                #  record has to be in it before the server is stopped: a record
                #  is written after its command is applied, and a snapshot
                #  showing the edit is not the log line that says so. The log is
                #  appended to in order, so waiting for the last record is
                #  waiting for all of them.
                write(server, f"/godot/cue/{CURTAIN}/notes", LAST_WORD)

                report.check(reads(server, f"/godot/cue/{CURTAIN}/notes", LAST_WORD),
                             "a last edit lands")
                report.check(reads(server, UNDO_NAME, "node.set"),
                             "and the readout names what one more press would unmake",
                             f"{UNDO_NAME} reads {value_of(server, UNDO_NAME)!r}")

                common.wait_until(
                    lambda: LAST_WORD in log.read_text(encoding="utf-8"))

        # --- and it reproduces, refusals and all -------------------------------
        #  A REFUSAL REPLAYS AS A REFUSAL, which is what putting `locked` in the
        #  log's reason vocabulary promised: the replay opens the same bundle -
        #  locked, from its state.xml - applies the same datagrams in the same
        #  ticks, and has to be refused by the same doors and the same handlers.
        #
        #  AND AN UNDO REPLAYS AS THE TRANSACTION IT POPPED. §14.9 makes the
        #  applied arguments of `undo` the domain and the transaction's NAME,
        #  precisely so that a replay whose stack had drifted writes a different
        #  line and fails on that record with both names on screen - the `go`
        #  pattern applied to a stack: log what was applied, not what was asked.
        #  §14.7 is why `--bundle` is not optional here: a log carrying an undo
        #  replayed without one registers no document commands at all, and every
        #  record in it would come back `unknown-command`.
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))

        report.equal(code, 0, "and `wfg replay` reproduces the session record for record",
                     (out + err).strip()[:2000])

        text = log.read_text(encoding="utf-8") if log.is_file() else ""
        refusals = [line for line in text.splitlines()
                    if line.startswith("R ") and " locked " in line]

        report.equal(len(refusals), 3,
                     "the log holds all three refusals the lock made, each with the reason "
                     "locked: two doors and undo's own handler",
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
