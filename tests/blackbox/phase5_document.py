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
"""Phase 5's document half, as a program. The lock (5.3), undo (5.4), a crash (5.5).

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

AND WHAT AUTOSAVE PROMISES, which is PR 5.5's half and the one PRD §4.3 calls a
reason anybody trusts show software at all. That the engine decides on its own
to write, unasked, once the show is dirty and has gone quiet. That what it
writes goes to `recovery/` and NEVER to the file somebody last saved, because a
designer who spent an afternoon getting it wrong has to be able to throw the
afternoon away by not saving, and an autosave into `show.xml` would take that
gesture away from the person who most wanted it. That a process which dies
leaves the folder behind, and that the next process to open the bundle SAYS SO
rather than adopting it - the operator decides whether the afternoon was worth
keeping, because the engine is headless and has nobody to ask. That the question
can WAIT without costing either afternoon, which is the author's decision of
2026-09-11: the next process goes on autosaving while nobody answers, and its
first autosave moves the earlier afternoon aside to `recovery.previous.1/`
rather than writing over it - so `document.recover` adopts the OFFER, wherever
it was moved, and not the work of the session asking. Recovering is an answer,
so it consumes the offer - once the recovered work is safe elsewhere, at the
next autosave or save, so that a crash straight after the recover still loses
nothing - and the process after that, finding nothing, offers nothing. An offer
nobody answers is kept until a discard, and offered again at the next open.
And that the four verbs around it mean exactly what §14.10 says:
`document.recover` leaves the dot LIT, because the recovered work is not on disk
as the show; `document.save` puts it out and takes this session's own
`recovery/` with it; `document.revert` puts the disk back and re-stamps what
`adopt` would otherwise have left looking unsaved; and `document.saveAs` writes
somewhere else without quietly making somewhere else the live document.

AND THAT A SESSION WHICH ADOPTED A RECOVERY DOES NOT REPLAY, and says so. The
bytes it adopted are in none of its records and outside the header's hash, so
a replay that went ahead would build a different show from the one the session
had and check every later record against it. `wfg replay` reproduces such a log
up to the recovery and refuses it there, with a sentence saying why; and the
session before it, which recovered nothing, still reproduces record for record.

THE PROCESS IS KILLED AND NOT STOPPED, which is the whole of why that half can
only be asked here. `Server.stop()` calls `terminate()`, and on POSIX that is
a SIGTERM `wfg serve` catches and turns into the ordinary clean shutdown; only
on Windows is it an end no handler sees. A driver that used it would be testing
shutdown on two platforms and crash recovery on one - and the one it tested
would be the platform the author is sitting at. What `BundleTests` cannot do is
die, and this is the file that can.

THE BUNDLE is `fixtures/bundles/locked/`, which carries `<Show locked="true"/>`
in its state.xml. Two lists of memos, so a GO has somewhere to move the pointer
and a second list has a pointer that must not move; and a mount on a desk with
one writable fader, pointed by the driver at `mock_target.py`. COPIED TWICE:
the crash half wants a folder no earlier session has left unsaved work in, or
`recovery` reading false on a fresh open would be a claim about the session
before it rather than about the bundle.

WAITED FOR, NEVER SLEPT. Every check waits for the thing it is about -
`common.wait_until` - because a datagram crosses a socket, joins a queue and is
applied on the next tick, and how long that takes on a loaded runner is not a
number anybody has measured. A refusal is waited for as the error COUNT moving,
which is the one reading that cannot be satisfied by an earlier refusal. And a
file is waited for as ITSELF: the bytes of a save, an autosave and a saveAs land
on a writer thread a tick or two after the command is applied and recorded, so
nothing the engine says about them - a record, the dot going out - is a look at
the disk.

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

#  WHAT PR 5.5 CALLS ONE CUE, five times over, because each name is a claim
#  about a different file and a driver that reused one could not tell them
#  apart. `AUTHORED` is what the fixture holds and what show.xml must still say
#  while an autosave is on disk; `CRASHED` is the afternoon nobody saved;
#  `MEANWHILE` is what the next process does before anybody has answered for
#  that afternoon, whose autosave has to move the afternoon aside rather than
#  over it, and which a recovery must NOT be mistaken for; `AFTER_SAVE` is the
#  edit a revert throws away; `ARCHIVED` is the one that proves saveAs wrote
#  the copy and the next save wrote the original.
AUTHORED = "Curtain up"
CRASHED = "Curtain up, and the process died"
MEANWHILE = "Curtain up, edited before anybody answered"
AFTER_SAVE = "Curtain up, edited after the save"
ARCHIVED = "Curtain up, in the archive"

LOCK = "/godot/document/locked"
DIRTY = "/godot/document/dirty"
RECOVERY = "/godot/document/recovery"
CAN_UNDO = "/godot/document/canUndo"
CAN_REDO = "/godot/document/canRedo"
UNDO_NAME = "/godot/document/undoName"
REDO_NAME = "/godot/document/redoName"
FADER = "/desk/fader"

#  AUTOSAVE IS NOT ASKED FOR, so the wait for it is longer than every other
#  wait in this file. §14.10 gives it two conditions - a hundred ticks since
#  the last change, which is two seconds of quiet, or fifteen hundred
#  regardless, which is thirty seconds - and the driver goes quiet the moment
#  it stops editing, so the first is the one that should fire. A wait of the
#  suite's usual fifteen seconds would pass on the quiet rule and be red on the
#  ceiling for something that is not a fault, so this is the ceiling with a
#  loaded runner's margin on top. It is a deadline and not a sleep: a working
#  engine reaches it in about two seconds.
AUTOSAVE_TIMEOUT = 45.0


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
# The crash, and the process that finds what it left
# =============================================================================

def crash(server: Server) -> None:
    """Ends a process the way a power cut ends one, and NOT the way `stop()`
    does.

    `Server.stop()` calls `terminate()`. On POSIX that is a SIGTERM, which
    `wfg serve` catches and turns into the ordinary clean shutdown - footers,
    the tick thread joined, and the tidy-up that deletes `recovery/` when the
    document is not dirty. Only on Windows is `terminate()` a `TerminateProcess`
    that no handler sees. So a driver that reached for the harness's own stop
    here would be testing shutdown on two platforms and crash recovery on one,
    and would go green on all three without two of them having crashed at all,
    which is the worst kind of green. `kill()` is SIGKILL and
    `TerminateProcess`: on every platform the process stops between one
    instruction and the next, which is the event autosave exists for.

    CALLED INSIDE THE `with`, so that the `stop()` which `__exit__` runs
    afterwards finds a process that has already gone and does nothing. That
    keeps the harness untouched: this is the only thing PR 5.5 needed of it that
    it did not already have, and a helper here is cheaper than a second way to
    end a server in `common.py` that every other driver would then have to
    choose between.
    """
    server.process.kill()
    server.process.wait(timeout=10)


def after_a_crash(report: Report, room: Path, locale: "str | None") -> None:
    """Three processes on one folder: one that dies with unsaved work, one
    that finds it and goes on working before anybody answers for it, and one
    that finds what the second was not allowed to delete.

    ITS OWN COPY OF THE BUNDLE, and not the one the session above worked in.
    That session ends dirty and is stopped rather than saved, so whatever
    `recovery/` it leaves behind is a fact about how far it got before the
    harness pulled it - and `recovery` reading false on a fresh open has to be
    asked of a folder nobody has crashed in yet, or it is a claim about the
    previous test rather than about this one.

    UNDER THE SAME LOCALE AS THE REST, every process. An autosave is a
    serialisation nobody asked for, written by a process whose numeric locale
    is whatever the machine says, and read back by another; a `recovery/`
    written with decimal commas is precisely the file the fr_FR rule exists to
    catch, and the one nobody would look at until the night it was needed.

    EVERY FILE IS WAITED FOR AS ITSELF. Since PR 5.5's second half the bytes
    of `document.save`, `document.autosave` and `document.saveAs` land on a
    writer thread a tick or two AFTER the command is applied and recorded, so
    nothing the engine says is a look at the disk: not the record, and not
    `dirty` going out - which now waits for the writer to confirm the save, and
    is therefore a good wait for "the save landed" and still not a reading of
    the file a check goes on to open. The Phase 4 handoff's third trap is the
    rule, and this half of the driver broke it twice before the writer thread
    existed.
    """
    folder = common.copy_bundle(FIXTURE, room / "crashed")
    archive = room / "archive"
    log = room / "crashed.wfglog"
    second_log = room / "recovered.wfglog"

    show = folder / "show.xml"
    autosaved = folder / "recovery" / "show.xml"
    autosaved_state = folder / "recovery" / "state.xml"
    cue = f"/godot/cue/{CURTAIN}/name"

    #  WHERE THE CRASHED AFTERNOON GOES when a later session autosaves before
    #  anybody has answered for it: the first free `recovery.previous.N/`,
    #  counted from one - and this folder has never held another.
    moved_aside = folder / "recovery.previous.1"

    def text_of(path: Path) -> str:
        """A file's contents, or "" for one that is not there - so a check
        about what a file holds reads FAIL rather than taking the whole driver
        down with a traceback when the file the engine should have written is
        missing. The absence is what the check is about.

        And "" for one that could not be read at the instant of asking, which
        since the writer thread can be a file being replaced under the reader:
        a poll should ask again rather than fall over."""
        try:
            return path.read_text(encoding="utf-8") if path.is_file() else ""
        except OSError:
            return ""

    def holds(path: Path, wanted: str, timeout: float = common.REPLY_TIMEOUT) -> bool:
        """Waits until a file holds `wanted`, and says whether it came to.

        THE FILE, AND NOTHING THAT VOUCHES FOR IT. A write lands on the writer
        thread after its command's record and after whatever the engine
        publishes about it, so the only reading that says what a file holds
        is a reading of the file."""
        return common.wait_until(lambda: wanted in text_of(path),
                                 timeout=timeout) is not None

    def applied(path: Path, name: str) -> int:
        """How many APPLIED records of the command `name` a log holds.

        Read off the command field - `A <tick> <seq> <origin> <command> ...` -
        rather than found anywhere in the line, so that a refusal of the same
        command, which is an `R` with its reason in front of the name, cannot
        answer for it. A count rather than a yes, so that "one more autosave
        than before the recovery" can be asked of a log that already has one."""
        return sum(1 for fields in (line.split() for line in text_of(path).splitlines())
                   if fields[:1] == ["A"] and fields[4:5] == [name])

    # --- the afternoon nobody saved ----------------------------------------
    with Server(folder, log=log, locale=locale) as dying:
        report.check(reads(dying, RECOVERY, False),
                     "a bundle nobody has crashed in opens with nothing to recover",
                     f"{RECOVERY} reads {value_of(dying, RECOVERY)!r}")

        write(dying, LOCK, False)
        report.check(reads(dying, LOCK, False), "the show is opened for editing")

        write(dying, cue, CRASHED)

        report.check(reads(dying, cue, CRASHED), "a cue is renamed",
                     f"the cue reads {value_of(dying, cue)!r}")
        report.check(reads(dying, DIRTY, True),
                     "and the dot comes on: the file on disk is behind this document",
                     f"{DIRTY} reads {value_of(dying, DIRTY)!r}")

        #  TWO CLAIMS, AND THEY ARE NOT THE SAME CLAIM. The record says the
        #  engine DECIDED to autosave - unasked, on its own arithmetic, and
        #  logged as a command like any other so that a replay re-applies it.
        #  The file says the disk AGREED. An engine that submitted the command
        #  and wrote nothing would satisfy the first; one that wrote bytes
        #  without a record would satisfy the second and be unreplayable.
        #
        #  AND SINCE THE WRITER THREAD THEY ARE NOT EVEN ONE MOMENT. The handler
        #  takes the snapshot and hands the bytes on; the record is written when
        #  it returns, and the file lands a tick or two after that. So each is
        #  waited for, in that order, and only the first gets the autosave's
        #  long deadline: it is the one waiting on the engine's arithmetic. Once
        #  the record is in, the bytes are milliseconds away, and if they never
        #  came, a second forty-five seconds of ctest's three hundred would
        #  learn nothing the ordinary deadline had not.
        wrote = common.wait_until(lambda: applied(log, "document.autosave"),
                                  timeout=AUTOSAVE_TIMEOUT)

        report.check(bool(wrote),
                     "the engine decides on its own to autosave, and records it as an "
                     "applied command like any other",
                     f"no applied document.autosave record after {AUTOSAVE_TIMEOUT:.0f}s")

        landed = common.wait_until(lambda: autosaved.is_file())

        report.check(bool(landed), "and recovery/show.xml is on the disk to prove it",
                     f"{autosaved} is not a file")

        #  WAITED FOR ITSELF, and not taken on the strength of the show beside
        #  it having arrived: it is a second file and a second replace, and the
        #  writer puts them down one after the other.
        placed = common.wait_until(lambda: autosaved_state.is_file())

        report.check(bool(placed),
                     "with the operator's position beside it in recovery/state.xml",
                     f"{autosaved_state} is not a file")

        #  AND THE AUTHORED FILE IS EXACTLY AS IT WAS, which is the whole of
        #  §14.10's argument for the folder: not saving is a gesture, and an
        #  autosave that wrote show.xml would take it away silently from the
        #  person who most wanted it. Nothing to wait for in a claim that
        #  something did NOT happen, so it is read once the autosave's own
        #  files have both landed - the moment by which a writer that got it
        #  wrong would have had every chance to.
        authored = text_of(show)

        report.check(AUTHORED in authored and CRASHED not in authored,
                     "and show.xml still holds what somebody last SAVED, which is what "
                     "keeps not saving a gesture somebody can make",
                     f"show.xml holds the authored name: {AUTHORED in authored}, "
                     f"and the unsaved one: {CRASHED in authored}")
        report.check(holds(autosaved, CRASHED),
                     "while recovery/show.xml holds the work nobody saved",
                     f"recovery/show.xml is {len(text_of(autosaved))} bytes")

        crash(dying)

    #  THE FOLDER AS THE NEXT PROCESS WILL FIND IT, kept for the replay of that
    #  process's log. The process saves over show.xml and state.xml twice
    #  before it ends, so a replay handed the folder afterwards would open a
    #  different show - unlocked, renamed - and diverge at its first record for
    #  that reason alone, which is not the reason the replay below is asked
    #  about. Copied between two processes, so nothing is writing it.
    as_found = common.copy_bundle(folder, room / "as-found")

    # --- and the process that finds it -------------------------------------
    with Server(folder, log=second_log, locale=locale) as second:
        #  READ OFF THE NOTICES `Server` ALREADY COLLECTS, which are the `wfg:`
        #  lines it reads from stdout WHILE STARTING - it stops reading at the
        #  second of the two port lines. So this assertion is also a claim about
        #  where the notice is printed: at the open, with the bundle, and not
        #  after the sockets are bound. A person running `wfg serve` in a
        #  terminal is the other reader, and they are reading the same line.
        told = [line for line in second.notices if "recovery available" in line]

        report.check(bool(told),
                     "the next process to open that folder says so on stdout, where "
                     "somebody who started it by hand will read it",
                     "\n".join(second.notices) or "it printed no notices at all")

        report.check(reads(second, RECOVERY, True),
                     "and publishes it, because a headless engine has nobody to ask",
                     f"{RECOVERY} reads {value_of(second, RECOVERY)!r}")

        #  AND HAS ADOPTED NOTHING. Three things would be wrong with a silent
        #  adopt and §14.10 names them: the show on screen would differ from the
        #  file the operator opened with no gesture in between; the decision
        #  autosave exists to leave open would have been taken for them; and
        #  `adopt` replaces the root, lock included, so a show could come back
        #  UNLOCKED during a performance because a file on disk said so.
        report.check(reads(second, cue, AUTHORED),
                     "and adopts nothing on its own: the show on screen is the show that "
                     "was opened",
                     f"the cue reads {value_of(second, cue)!r}")
        report.equal(value_of(second, DIRTY), False,
                     "with the dot out, because nothing has been changed since it opened")

        #  §14.7: the lock refuses `recover` as it refuses `undo`, and for the
        #  same reason read the other way round - `adopt` replaces the root and
        #  the lock with it, so a recovery on a locked show is the one gesture
        #  that could unlock it without anybody deciding to.
        said = refusal_of(second, lambda: command(second, "document.recover"))

        report.check(said.endswith(" locked document.recover"),
                     "a recovery on a locked show is refused, and says locked",
                     f"lastError reads {said!r}")

        write(second, LOCK, False)
        report.check(reads(second, LOCK, False), "the lock lifts")

        # --- this session's own work, before anybody has answered ----------
        #  THE SCENARIO THE AUTHOR'S DECISION OF 2026-09-11 EXISTS FOR. As
        #  §14.10 drew it, this session's first autosave - two seconds after
        #  its first edit - wrote recovery/show.xml straight over the afternoon
        #  the banner was offering back, before anybody had read the banner.
        #  The first build closed that by suspending autosave until somebody
        #  answered, which kept the old work safe by leaving the new work with
        #  no copy at all. The decision is that neither pays: `recovery/` is
        #  always THIS session's autosave, and the first time it writes while
        #  the offer still sits there, the offer is moved aside to the first
        #  free `recovery.previous.N/` - which nothing deletes while it is
        #  unanswered, and which an answer consumes.
        #
        #  ASKED BEFORE THE EDIT, so that what follows is a claim about the
        #  autosave rather than about the open: a process that has only opened
        #  the folder, and has nothing dirty to write, has moved nothing.
        report.check(CRASHED in text_of(autosaved) and not moved_aside.exists(),
                     "the offer sits in recovery/ where the dead process left it, and "
                     "opening the folder has moved nothing aside",
                     f"recovery/show.xml holds the crashed name: "
                     f"{CRASHED in text_of(autosaved)}; "
                     f"{moved_aside.name}/ exists: {moved_aside.exists()}")

        write(second, cue, MEANWHILE)

        report.check(reads(second, cue, MEANWHILE),
                     "this session renames the cue before anybody has answered the offer",
                     f"the cue reads {value_of(second, cue)!r}")
        report.check(reads(second, DIRTY, True), "and the dot comes on",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        wrote = common.wait_until(lambda: applied(second_log, "document.autosave"),
                                  timeout=AUTOSAVE_TIMEOUT)

        report.check(bool(wrote),
                     "and it AUTOSAVES with the offer unanswered, which the first build "
                     "suspended and the author's decision keeps running",
                     f"no applied document.autosave record after {AUTOSAVE_TIMEOUT:.0f}s")

        #  THE TWO FOLDERS, EACH WAITED FOR AS ITSELF: the move and the write
        #  happen after the record, and neither folder says when the other's
        #  part has happened. The NAME is compared rather than the folder's
        #  presence, because a folder holding the wrong afternoon would pass a
        #  check for the directory.
        report.check(holds(moved_aside / "show.xml", CRASHED),
                     "the earlier afternoon is MOVED ASIDE to recovery.previous.1/ and not "
                     "destroyed: its show.xml still holds the crashed process's name",
                     f"{moved_aside.name}/show.xml is "
                     f"{len(text_of(moved_aside / 'show.xml'))} bytes")
        report.check(holds(autosaved, MEANWHILE),
                     "and recovery/ holds this session's own autosave, which is all that "
                     "folder is ever for",
                     f"recovery/show.xml holds this session's name: "
                     f"{MEANWHILE in text_of(autosaved)}")

        #  AND THE OFFER STILL STANDS. Moving it is not answering it: the
        #  banner stays up while this session's own work is kept safe beside
        #  it, which is the whole of what the decision buys.
        report.check(reads(second, RECOVERY, True),
                     "and the offer still stands, wherever it was moved to: moving it aside "
                     "is not answering it",
                     f"{RECOVERY} reads {value_of(second, RECOVERY)!r}")

        # --- the answer, and it is the offer that comes back ----------------
        #  WITH BOTH FOLDERS ON THE DISK, which is the one moment the question
        #  has teeth: `recovery/` holds this session's edit and
        #  `recovery.previous.1/` the crashed afternoon, and an engine that
        #  took "the recovery" to mean the folder of that name would adopt the
        #  work of the very session asking.
        autosaves_before = applied(second_log, "document.autosave")

        command(second, "document.recover")

        report.check(reads(second, cue, CRASHED),
                     "document.recover adopts the EARLIER afternoon - the offer, from where "
                     "it was moved - and not this session's own autosave in recovery/",
                     f"the cue reads {value_of(second, cue)!r}")

        #  THE DOT WAS ALREADY LIT, by this session's own edit, so what is
        #  asked is that the recovery does not put it OUT: `savedRevision` is
        #  not re-stamped, which is `document.revert`'s rule read the other way
        #  round. Read after the recovered name has been published, so the
        #  answer comes from a snapshot that includes the recovery.
        report.check(reads(second, DIRTY, True),
                     "with the dot still LIT, deliberately: the recovered work is not on "
                     "disk as the show, and the dot is telling the truth",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")
        report.check(reads(second, RECOVERY, False),
                     "and nothing left to recover, because it has been",
                     f"{RECOVERY} reads {value_of(second, RECOVERY)!r}")

        #  RECOVERING IS AN ANSWER, AND IT CONSUMES THE OFFER - but not at the
        #  recover. The rule the author's decision comes to (2026-09-11): the
        #  folder the offer came from is deleted by the first write that puts
        #  the recovered work somewhere safe, so a crash straight after the
        #  recover still loses nothing. Whether it is still on the disk at THIS
        #  instant is therefore a race against the catch-up autosave's two
        #  seconds of quiet, and is not asked here; `DocumentWriterTests` asks
        #  it where there is no clock. What is asked below is the end of the
        #  story, which is not a race: once the catch-up has landed, it is gone.

        #  AND THIS SESSION'S OWN COPY CATCHES UP. `recovery/` still holds the
        #  edit the recovery has just replaced, and it is this session's crash
        #  copy of what is on screen - which is now the recovered afternoon,
        #  dirty. So the next autosave writes that. Were it skipped because the
        #  recovery "already holds this revision" - true while an offer could
        #  only ever be read out of `recovery/` itself - a crash here would
        #  offer back the edit the operator had just chosen to replace.
        #
        #  THE RECORD FIRST AND THEN THE FILE, as for the first autosave, and
        #  for one more reason: the file is about to be REPLACED rather than
        #  created, and a reader that polled it for the two seconds of quiet
        #  would hold it open, fifty times a second, across the very replace it
        #  was waiting for - which on Windows is a sharing violation for the
        #  engine. After the record, the poll overlaps the replace for
        #  milliseconds rather than seconds.
        caught_up = common.wait_until(
            lambda: applied(second_log, "document.autosave") > autosaves_before,
            timeout=AUTOSAVE_TIMEOUT)

        report.check(bool(caught_up) and holds(autosaved, CRASHED),
                     "and this session's next autosave writes the recovered afternoon into "
                     "recovery/, over the edit the recovery replaced",
                     f"autosaves since the recovery: "
                     f"{applied(second_log, 'document.autosave') - autosaves_before}; "
                     f"recovery/show.xml holds the crashed name: "
                     f"{CRASHED in text_of(autosaved)}, and this session's earlier one: "
                     f"{MEANWHILE in text_of(autosaved)}")

        #  AND THAT WRITE CONSUMED THE OFFER. The recovered afternoon is now in
        #  recovery/ as this session's crash copy, so the folder it was read
        #  out of has done its job, and the writer deletes it in queue order
        #  behind the bytes that made it redundant. Waited for, because the
        #  deletion is the writer's and lands after the record.
        consumed = common.wait_until(lambda: not moved_aside.exists())

        report.check(bool(consumed),
                     "and the folder the recovery came from goes with it: recovering was an "
                     "answer, and once the work is safe elsewhere there is nothing to offer",
                     f"{moved_aside.name}/ is still on the disk")

        # --- a save, which is the work becoming the show --------------------
        command(second, "document.save")

        report.check(reads(second, DIRTY, False), "a save puts the dot out",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        #  `dirty` GOING OUT IS THE WRITER'S CONFIRMATION, which makes it a
        #  good wait for "the save landed" and still not a look at a file - so
        #  each file below is waited for itself.
        gone = common.wait_until(lambda: not (folder / "recovery").exists())

        report.check(bool(gone),
                     "and takes this session's own recovery/ with it: the work has become "
                     "the show",
                     f"{folder / 'recovery'} is still there")
        report.check(holds(show, CRASHED),
                     "which show.xml now holds, on the authored path this time",
                     f"show.xml is {len(text_of(show))} bytes")

        #  AND THE AFTERNOON IT CAME FROM STAYS GONE. Recovered and now saved, it
        #  is the show; keeping its folder would only mean offering it again at
        #  every start until somebody discarded work that is already on screen.
        report.check(not moved_aside.exists(),
                     "and the recovered afternoon's folder is not kept to be offered again: "
                     "it is the show now",
                     f"{moved_aside.name}/ is back on the disk")

        # --- revert, which is the disk winning ------------------------------
        write(second, cue, AFTER_SAVE)

        report.check(reads(second, cue, AFTER_SAVE), "an edit after the save lands",
                     f"the cue reads {value_of(second, cue)!r}")
        report.check(reads(second, DIRTY, True), "and lights the dot again")

        command(second, "document.revert")

        report.check(reads(second, cue, CRASHED),
                     "document.revert puts the show on disk back",
                     f"the cue reads {value_of(second, cue)!r}")

        #  AND THE DOT GOES OUT, which a revert has to re-stamp for itself:
        #  `adopt` ends by counting a load as the largest change there is, and
        #  the restored positions count again on top, so a revert that did not
        #  re-stamp `savedRevision` would report unsaved changes at the instant
        #  the document matched the disk exactly.
        report.check(reads(second, DIRTY, False),
                     "with the dot OUT, because this document IS the file now",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        # --- saveAs, which writes elsewhere and stays here ------------------
        write(second, cue, ARCHIVED)
        report.check(reads(second, cue, ARCHIVED), "one more edit, for the archive")

        command(second, "document.saveAs", [str(archive)])

        #  WAITED FOR ON THE MANIFEST, which is the last file saveAs writes and
        #  so the one arrival that vouches for all the others: the descriptions
        #  are copied first, then show.xml, then state.xml, then the manifest
        #  that makes the folder a bundle. The first build of this driver
        #  waited on show.xml - the FIRST of the show's files - and then
        #  checked the manifest, the last, without waiting, and three runs in
        #  six caught it mid-write as `archive.wfg.tmp-<pid>`. A wait must see
        #  what its check reads, and here the thing to see is the one whose
        #  arrival means the copy is finished - all of it the writer's since
        #  PR 5.5's second half, after the command's record, so there is no
        #  earlier moment to lean on even in principle.
        common.wait_until(lambda: (archive / "archive.wfg").is_file())

        copied = common.wait_until(lambda: ARCHIVED in text_of(archive / "show.xml"))

        report.check(bool(copied), "document.saveAs writes the show into the named folder",
                     f"{archive / 'show.xml'} is {len(text_of(archive / 'show.xml'))} bytes")
        #  THE FILE, NOT THE FOLDER: an empty `namespaces/` would pass a check
        #  for the directory, and a copy whose mount points at a namespace that
        #  did not come with it opens with a warning instead of a desk.
        #
        #  WAITED FOR ITSELF, and not taken on the strength of show.xml having
        #  arrived. The engine now copies the descriptions BEFORE it writes the
        #  show, so show.xml's arrival already implies them - but a check that
        #  leans on the order of another file's writes is one refactor away
        #  from reading a description mid-copy, which is exactly what the first
        #  build of this PR did, at nought bytes. A wait must see what its check
        #  reads (the Phase 4 handoff's third trap).
        source_namespace = text_of(folder / "namespaces" / "desk.json")
        namespace = common.wait_until(
            lambda: (lambda text: text if text and text == source_namespace else None)(
                text_of(archive / "namespaces" / "desk.json"))) or ""

        report.check(namespace != "" and namespace == source_namespace,
                     "with the namespaces beside it, which Bundle::save refuses to write "
                     "and saveAs copies rather than pretending the copy is a save",
                     f"archive/namespaces/desk.json is {len(namespace)} bytes")

        listing = sorted(p.name for p in archive.iterdir()) if archive.is_dir() else []

        report.check((archive / "archive.wfg").is_file(),
                     "and a manifest named after the folder it landed in, which is where "
                     "a copy stops being a bundle if nobody writes it",
                     f"{archive} holds {listing}")

        #  THE HALF MOST LIKELY TO BE ARGUED ABOUT (§14.10), asked twice. A
        #  *save a copy for the archive* that silently made the archive the live
        #  document is a trap with a delay fuse: the operator's next ctrl-S goes
        #  somewhere they did not name, and they find out at the next load.
        report.check(CRASHED in text_of(show),
                     "and it did not write the bundle this session opened",
                     f"show.xml holds the archived name: {ARCHIVED in text_of(show)}")

        #  READ SOME PUBLISHES LATER, AND NOT THE INSTANT THE COPY APPEARED. The
        #  copy is the writer's, and the engine hears that it landed on a later
        #  tick than the one that queued it - so a reading taken as soon as the
        #  file exists could come from a snapshot taken before the writer's
        #  confirmation was accounted for at all: `true` for the old reason,
        #  and passing for the wrong one, since a saveAs whose confirmation
        #  stamped the session would put the dot out a tick afterwards. Waiting
        #  for the tick counter to move on is waiting for snapshots that have
        #  to include it - five ticks rather than the two the first build
        #  waited, because the writer can be descheduled between putting the
        #  manifest down and saying so.
        def tick() -> int:
            value = value_of(second, "/godot/engine/tick")
            return value if isinstance(value, int) else -1

        copied_at = tick()
        common.wait_until(lambda: tick() > copied_at + 5)

        report.check(reads(second, DIRTY, True),
                     "which is why the dot stays lit: those bytes are not this session's "
                     "file, and this session's file is still behind",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        command(second, "document.save")

        report.check(reads(second, DIRTY, False), "the next save puts the dot out")
        report.check(holds(show, ARCHIVED),
                     "and writes the folder the session opened, not the one saveAs was "
                     "handed: saveAs does not re-point the session",
                     f"show.xml holds the archived name: {ARCHIVED in text_of(show)}")

        #  AND THE FOLDER IS LEFT AS THE NEXT PROCESS WILL FIND IT: none of
        #  this session's `recovery/` - the save takes it, if an autosave wrote
        #  one after the revert - and `recovery.previous.1/`, which nothing
        #  here was allowed to delete. Waited for rather than assumed, because
        #  what the next open offers depends on exactly which of the two is
        #  there.
        report.check(bool(common.wait_until(lambda: not (folder / "recovery").exists())),
                     "and the last save leaves no recovery/ of this session's behind it",
                     f"{folder / 'recovery'} is still there")

    # --- and a replay that will not pretend --------------------------------
    #  §14.10'S KNOWN-NOT-BUILT, answered by the author on 2026-09-11. A
    #  session that adopted a recovery cannot be replayed, because the bytes it
    #  adopted are in none of its records and outside the header's hash, which
    #  covers show.xml, state.xml and namespaces/ and nothing else. A replay
    #  that went ahead would build a different show from the one the session
    #  had and check every later record against it - silently, for a session
    #  started with `--recover`, whose adoption is in no record at all, and as
    #  a divergence nobody could explain for one like this, which pressed
    #  `document.recover`. So `wfg replay` refuses the log, with a sentence
    #  saying why.
    #
    #  AT THE RECORD, AND NOT BEFORE IT. Everything the session did up to the
    #  applied `document.recover` is a session like any other, and replays -
    #  the refusal refused by the lock among it, since a REJECTED recovery
    #  adopted nothing. So the replay is handed the folder as this process
    #  found it (`as_found`, above), with the first session's two flags, and
    #  what is asked is that it reproduces everything before the recovery and
    #  then stops and says why.
    #
    #  THREE CHECKS, because a divergence is also a non-zero exit, and its
    #  report quotes `document.recover` in the record it fails on - so "exits
    #  non-zero and mentions a recovery" is satisfied by the very failure the
    #  refusal exists to replace. The third asks that nothing diverged: a
    #  replay's divergence is the only thing that prints "but replay
    #  produced", and a replay that stopped where it had to has nothing of the
    #  kind to print.
    naming = [line for line in text_of(second_log).splitlines() if "document.recover" in line]

    report.check(applied(second_log, "document.recover") > 0,
                 "the second process's log records the recovery it adopted, as an applied "
                 "command like any other",
                 f"the lines naming it: {naming}")

    code, out, err = common.run_wfg("replay", str(second_log), f"--bundle={as_found}",
                                    f"--out={room / 'recovered-replayed'}",
                                    *([f"--wfg-locale={locale}"] if locale else []))
    said = (out + err).strip()

    report.check(code != 0,
                 "and `wfg replay` refuses that log rather than exiting 0 over a show it "
                 "could not have rebuilt",
                 f"exit {code}: {said[:2000]}")
    report.check("recover" in said.lower(),
                 "with a sentence that names the recovery as the reason",
                 said[:2000])
    report.check("but replay produced" not in said,
                 "and reports no divergence: what came before the recovery reproduces, "
                 "and the recovery is where it stops",
                 said[:2000])

    # --- and the process after that, which has nothing to be offered ------
    #  THE AFTERNOON THAT WAS RECOVERED AND SAVED IS NOT OFFERED AGAIN, which
    #  is the whole of why recovering consumes its offer. Had it not, every
    #  later start would offer back work that is already the show, until
    #  somebody discarded it - and an offer that is always there is an offer
    #  people learn to dismiss without reading, which is the day it matters.
    #  That an UNANSWERED `recovery.previous.N/` survives a save, a revert and
    #  a clean exit, and is offered again at the next open, is asked where
    #  there is no clock, in `DocumentWriterTests`; here the folder holds no
    #  recovery of any kind, and the open must say nothing.
    with Server(folder, locale=locale) as third:
        told = [line for line in third.notices if "recovery available" in line]

        report.check(not told,
                     "the next process to open the folder offers nothing: the recovered "
                     "afternoon was answered, and saved",
                     "\n".join(told))
        report.check(reads(third, RECOVERY, False),
                     "and publishes that, rather than an offer nobody has left",
                     f"{RECOVERY} reads {value_of(third, RECOVERY)!r}")
        report.check(value_of(third, cue) == ARCHIVED and value_of(third, DIRTY) is False,
                     "and opens on the show last saved, with nothing unsaved",
                     f"the cue reads {value_of(third, cue)!r}, "
                     f"{DIRTY} reads {value_of(third, DIRTY)!r}")

        #  UNLOCKED FOR THE REFUSALS BELOW, not because a discard needs it - a
        #  discard deletes bytes and touches the document not at all (§14.7) -
        #  but because a locked show answers a RECOVERY `locked` before it
        #  answers `no-recovery`, and the question here is the second.
        write(third, LOCK, False)
        report.check(reads(third, LOCK, False), "the show is open for editing")

        # --- and an empty gesture is refused rather than pretended ----------
        #  Moved here from the process above, where a folder a discard could
        #  have been aimed at was still on the disk. Here there is nothing left
        #  anywhere, so both answers are about an empty gesture and nothing
        #  else.
        said = refusal_of(third, lambda: command(third, "document.recover"))

        report.check(said.endswith(" no-recovery document.recover"),
                     "a recovery with nothing to adopt is refused with no-recovery",
                     f"lastError reads {said!r}")

        said = refusal_of(third, lambda: command(third, "document.discardRecovery"))

        report.check(said.endswith(" no-recovery document.discardRecovery"),
                     "and so is a discard with nothing to delete: refusing an empty "
                     "gesture is cheaper than pretending it worked",
                     f"lastError reads {said!r}")


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 5: the document - locked, undone, recovered ({locale or 'C'})")

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

        # --- and the one thing no unit test can do: die ----------------------
        #  ITS OWN BUNDLE AND ITS OWN LOGS, so that nothing above reaches it:
        #  the session that has just ended left this room with a bundle it
        #  edited and never saved, and the questions below are about a folder
        #  whose history the driver knows completely.
        after_a_crash(report, room, locale)

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
