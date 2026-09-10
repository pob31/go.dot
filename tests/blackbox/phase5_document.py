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
keeping, because the engine is headless and has nobody to ask. And that the
four verbs around it mean exactly what §14.10 says: `document.recover` leaves
the dot LIT, because the recovered work is not on disk as the show;
`document.save` puts it out and takes the folder with it; `document.revert`
puts the disk back and re-stamps what `adopt` would otherwise have left looking
unsaved; and `document.saveAs` writes somewhere else without quietly making
somewhere else the live document.

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

#  WHAT PR 5.5 CALLS ONE CUE, four times over, because each name is a claim
#  about a different file and a driver that reused one could not tell them
#  apart. `AUTHORED` is what the fixture holds and what show.xml must still say
#  while an autosave is on disk; `CRASHED` is the afternoon nobody saved;
#  `AFTER_SAVE` is the edit a revert throws away; `ARCHIVED` is the one that
#  proves saveAs wrote the copy and the next save wrote the original.
AUTHORED = "Curtain up"
CRASHED = "Curtain up, and the process died"
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
    """Two processes on one folder: one that dies with unsaved work, and one
    that finds it.

    ITS OWN COPY OF THE BUNDLE, and not the one the session above worked in.
    That session ends dirty and is stopped rather than saved, so whatever
    `recovery/` it leaves behind is a fact about how far it got before the
    harness pulled it - and `recovery` reading false on a fresh open has to be
    asked of a folder nobody has crashed in yet, or it is a claim about the
    previous test rather than about this one.

    UNDER THE SAME LOCALE AS THE REST, both processes. An autosave is a
    serialisation nobody asked for, written by a process whose numeric locale
    is whatever the machine says, and read back by another; a `recovery/`
    written with decimal commas is precisely the file the fr_FR rule exists to
    catch, and the one nobody would look at until the night it was needed.
    """
    folder = common.copy_bundle(FIXTURE, room / "crashed")
    archive = room / "archive"
    log = room / "crashed.wfglog"

    show = folder / "show.xml"
    autosaved = folder / "recovery" / "show.xml"
    autosaved_state = folder / "recovery" / "state.xml"
    cue = f"/godot/cue/{CURTAIN}/name"

    def text_of(path: Path) -> str:
        """A file's contents, or "" for one that is not there - so a check
        about what a file holds reads FAIL rather than taking the whole driver
        down with a traceback when the file the engine should have written is
        missing. The absence is what the check is about."""
        return path.read_text(encoding="utf-8") if path.is_file() else ""

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
        #  without a record would satisfy the second and be unreplayable. Both
        #  are waited for, in that order, because the record is written after
        #  the handler returns and the handler is what writes the file - which
        #  is also why only the first wait gets the autosave's long deadline.
        #  Once the record is in, the file is already there; if the record never
        #  came, a second long wait would spend another forty-five seconds of
        #  ctest's three hundred learning nothing the first did not say.
        wrote = common.wait_until(
            lambda: any(line.startswith("A ") and "document.autosave" in line
                        for line in text_of(log).splitlines()),
            timeout=AUTOSAVE_TIMEOUT)

        report.check(bool(wrote),
                     "the engine decides on its own to autosave, and records it as an "
                     "applied command like any other",
                     f"no applied document.autosave record after {AUTOSAVE_TIMEOUT:.0f}s")

        landed = common.wait_until(lambda: autosaved.is_file())

        report.check(bool(landed), "and recovery/show.xml is on the disk to prove it",
                     f"{autosaved} is not a file")
        report.check(autosaved_state.is_file(),
                     "with the operator's position beside it in recovery/state.xml",
                     f"{autosaved_state} is not a file")

        #  AND THE AUTHORED FILE IS EXACTLY AS IT WAS, which is the whole of
        #  §14.10's argument for the folder: not saving is a gesture, and an
        #  autosave that wrote show.xml would take it away silently from the
        #  person who most wanted it.
        authored = text_of(show)

        report.check(AUTHORED in authored and CRASHED not in authored,
                     "and show.xml still holds what somebody last SAVED, which is what "
                     "keeps not saving a gesture somebody can make",
                     f"show.xml holds the authored name: {AUTHORED in authored}, "
                     f"and the unsaved one: {CRASHED in authored}")
        report.check(CRASHED in text_of(autosaved),
                     "while recovery/show.xml holds the work nobody saved",
                     f"recovery/show.xml is {len(text_of(autosaved))} bytes")

        crash(dying)

    # --- and the process that finds it -------------------------------------
    with Server(folder, log=room / "recovered.wfglog", locale=locale) as second:
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

        command(second, "document.recover")

        report.check(reads(second, cue, CRASHED),
                     "and then the afternoon comes back",
                     f"the cue reads {value_of(second, cue)!r}")
        report.check(reads(second, DIRTY, True),
                     "with the dot LIT, deliberately: the recovered work is not on disk "
                     "as the show, and the dot is telling the truth",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")
        report.check(reads(second, RECOVERY, False),
                     "and nothing left to recover, because it has been",
                     f"{RECOVERY} reads {value_of(second, RECOVERY)!r}")

        # --- a save, which is the work becoming the show --------------------
        command(second, "document.save")

        report.check(reads(second, DIRTY, False), "a save puts the dot out",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        gone = common.wait_until(lambda: not (folder / "recovery").exists())

        report.check(bool(gone),
                     "and takes the recovery folder with it: the work has become the show",
                     f"{folder / 'recovery'} is still there")
        report.check(CRASHED in text_of(show),
                     "which show.xml now holds, on the authored path this time",
                     "the dot went out before this was read, and the bytes are written "
                     "before the dot goes out")

        # --- and an empty gesture is refused rather than pretended ----------
        said = refusal_of(second, lambda: command(second, "document.recover"))

        report.check(said.endswith(" no-recovery document.recover"),
                     "a recovery with nothing to adopt is refused with no-recovery",
                     f"lastError reads {said!r}")

        said = refusal_of(second, lambda: command(second, "document.discardRecovery"))

        report.check(said.endswith(" no-recovery document.discardRecovery"),
                     "and so is a discard with nothing to delete: refusing an empty "
                     "gesture is cheaper than pretending it worked",
                     f"lastError reads {said!r}")

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
        #  arrival means the copy is finished.
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

        #  READ ONE PUBLISH LATER, AND NOT THE INSTANT THE COPY APPEARED. The
        #  handler writes the copy and the after hook assigns `dirty` at the end
        #  of the same tick, so a reading taken as soon as the file exists could
        #  be the snapshot from before saveAs was accounted for at all - `true`
        #  for the old reason, passing for the wrong one. Waiting for the tick
        #  counter to move is waiting for a snapshot that has to include it.
        def tick() -> int:
            value = value_of(second, "/godot/engine/tick")
            return value if isinstance(value, int) else -1

        copied_at = tick()
        common.wait_until(lambda: tick() > copied_at + 2)

        report.check(reads(second, DIRTY, True),
                     "which is why the dot stays lit: those bytes are not this session's "
                     "file, and this session's file is still behind",
                     f"{DIRTY} reads {value_of(second, DIRTY)!r}")

        command(second, "document.save")

        report.check(reads(second, DIRTY, False), "the next save puts the dot out")
        report.check(ARCHIVED in text_of(show),
                     "and writes the folder the session opened, not the one saveAs was "
                     "handed: saveAs does not re-point the session")


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
