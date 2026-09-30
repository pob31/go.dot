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
"""A save killed halfway through a write, twenty times over: the show survives (H6).

THE CLAIM PUT TO THE TEST is DocumentWriter.h's, in its own words: a kill loses
what was queued, and "the atomic write leaves every file either old and whole or
new and whole". PRD §4.3 names crash-safe autosave as one of the three reasons
anybody trusts show software, and until this driver nothing had ever killed the
engine INSIDE a write. phase5_document.py's crash waits for the autosave's
record and for both of its files before it pulls the plug, which proves that a
finished autosave survives a crash and says nothing about one half done.

WHAT IT DOES. A fresh copy of `fixtures/bundles/crash-write/` is calibrated
once: about 24 KB of notes on three ballast cues, the fourth renamed
`H6-calibration`, and a save. The show.xml that save wrote is the TEMPLATE: the
bytes every later write of the show must be, with one hole where the name goes.
Then, twenty times, a victim process opens the folder and one UDP socket streams
a new name (a marker nobody else ever used) and a write command every five
milliseconds, every third a `document.save` and the others a
`document.autosave`, so the writer's queue never empties. Once the victim's
own log holds three applied write records, the process is killed with SIGKILL
or TerminateProcess at a seeded random instant up to half a second later.

WHAT IT ASKS AFTER EACH KILL, with no process alive, of the disk itself:
show.xml is byte for byte the template with a marker the victim started with,
adopted, or was sent - so nothing torn, nothing mixed, and nothing gone
backwards; state.xml and the manifest parse; `recovery/show.xml` and every
`recovery.previous.N/show.xml` are the template with some marker ever sent; the
folder the engine will offer - one holding a show no landed save has marked
`superseded` (H6b) - is worked out from the disk the engine's way, and no
recovery show.xml was left by the kill under no name; the offer is never OLDER
than a show.xml the same session saved, because a save retires that session's
`recovery/` and adopting what it left would take the show back past the save;
and, the other way round, a folder passed over for its mark is one a landed
save outgrew - a state of a session that put a show.xml no older on the disk,
in a folder that session's saves retire - and never a later afternoon, or an
earlier session's unanswered one, hidden; and the afternoon a plain victim
left standing is still on offer after it, moved aside or not, never hidden or
gone. Temps (`<name>.tmp-<pid>`) may be
anywhere, because nothing reads them, and are counted by whose they are; the
`<name>~RF<hex>.TMP` files Windows' own `ReplaceFile` stranded when a kill
landed inside it were counted too, until H6b took `ReplaceFile` out of the
engine - one a kill strands now fails the run.

AND OF THE NEXT PROCESS, twice. A plain start must open, announce the offer
exactly when the disk holds one (naming its folder), publish show.xml's marker
and not a temp's, with the dot out; then `document.recover` must be applied -
a record in its log, no refusal counted, nothing left on offer, the offer's
marker on screen and the dot lit - or, with nothing offered, be refused
`no-recovery`. And a `--recover` start must open rather than exit 2, say
`recovery adopted` or `nothing to recover`, and publish what it adopted. When
the offer is a stale one, what the adoption puts on screen is said and not
counted as a pass: the inspection has already failed the offer itself. Every
check session is killed, not stopped: `Server.stop()` is SIGTERM on POSIX and a
clean exit that drains the writer and tidies `recovery/` (phase5's `crash`
says why). After the twentieth kill, one more `--recover` start renames the cue,
saves, and a last plain open reads that save back.

A CLIENT ASKS FOR THE AUTOSAVES - the autosave nobody asks for, in §14.10's
title. It is the same handler, the same writer and the same files, and nothing
refuses it (EngineNamespace.cpp's `/godot/cmd/` door has no origin gate). The
engine's own autosave waits for a hundred ticks of quiet, which a stream of
edits never gives it, so without the client's there would be no `recovery/` in
flight at all - and twenty kills on the engine's two seconds of quiet would cost
minutes and find the writer idle almost every time. So the run also checks that
autosaves were applied and that some kill left an offer its own victim wrote:
without them every recovery check would pass on an empty folder.

THE CONTROLS, because a net that cannot go red is not a test. A show.xml cut in
half must be flagged by the inspection and must stop `serve` ("could not be
loaded"); a `recovery/show.xml` cut in half must stop `serve --recover` ("could
not be read"), which is also the proof that `recover=True` reaches the binary; a
displaced show (under a temp's name, or under ReplaceFile's), a recovery show
under no name and the `~RF` its kill stranded, a show gone backwards, and an
offer older than the show its own session saved - in `recovery/` or in the
folder that session adopted - must each be named by the inspection, while an
earlier session's unanswered offer, which a later save leaves standing on
purpose, must not; a `recovery.previous.N/` with no show in it, and a
`recovery/` whose `superseded` mark names the show.xml on the disk - on its
first line or a later one - must be offered by nothing - by the mirror, and by
the engine, whose `--recover` start must have nothing to recover - while a mark
naming any other show hides nothing, and the engine's `--recover` adopts that
folder; a mark over work newer than the show.xml it names, and one over an
earlier session's unanswered offer, must each be named as a hidden offer; and
two whole temps planted with process id 1 - which is never the engine's - must
be ignored by the first start, which must find nothing to recover and open on
`H6-calibration`.

WHAT IT FOUND, AND WHAT CLOSED IT. Two ENGINE findings, 2026-09-30, both closed
by H6b the same day (namespace draft §23.4-23.5), which is when ctest began to
run this driver, as `blackbox.crash-write.C` and `.fr_FR`.

The stale offer, on every platform, since the order is the writer's and not the
file system's. A save writes show.xml, state.xml and the manifest, and only then
deletes the session's `recovery/` (DocumentWriter.cpp's save job) or the
`recovery.previous.N/` it adopted. A kill in between left the last autosave -
older than the show.xml just written - where the next start offered it as
`recovery available`, and adopting it quietly took the show back past the save:
about one kill in five on the Windows box, so this driver was red on nearly
every run. Now the save marks those folders `superseded`, naming the show.xml it
is about to write, before it writes; a folder whose mark names the show.xml on
the disk is no offer, and the next session's first write deletes it.

The file under no name, on Windows. `ReplaceFile`, given no backup name, is
four steps and not one: it creates an empty `<file>~RF<hex>.TMP`, moves the old
file onto that name, moves the new one into place, and deletes the first.
Between its two moves the file was under no name `open` reads, and a kill there
left it so: the old bytes whole under the `~RF` name, the new ones whole under
the temp's. On the manifest `serve` then refused the folder as not a bundle; on
show.xml it refused a folder with no show; and on `recovery/show.xml` it offered
nothing at all. Now the engine renames in one step: by handle, with POSIX
semantics, and with `MoveFileExW` only where the volume cannot do that.

A red here is the engine's, never a flake to re-run. A pass says only that no
kill found a window this time, and there is one the random kills cannot reach:
a power cut rather than a kill, which on POSIX can undo a rename the directory
was never made to keep, since nothing fsyncs it. Nor does this driver hold a
file, so the folders something holds while a save runs - carried by every save
after, their marks a line longer each time - are the unit tests' to ask
(DocumentWriterTests.cpp). Either is an engine finding too.

Exit codes as the rest of the suite: 0 everything held, 1 something did not,
2 the harness could not run.
"""
from __future__ import annotations

import contextlib
import hashlib
import random
import re
import socket
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import phase5_document as doc
from common import HarnessError, Report, Server

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "crash-write"

#  THE COPY HAS THE FIXTURE'S NAME, and the manifest is named after it. A copy
#  under another name opens with a warning, and its first save writes a second
#  manifest named after the new folder (Bundle.cpp's `open`), which would be a
#  file this driver did not expect and a claim about renaming, not crashing.
BUNDLE = "crash-write"
MANIFEST = BUNDLE + ".wfg"
SHOW = "show.xml"
STATE = "state.xml"
RECOVERY_DIR = "recovery"

#  ONE CUE CARRIES THE MARKER, and nothing else in the show moves while the
#  stream runs - no standby, no lock, no other edit - so that the bytes of every
#  show.xml ever written differ from the template in that one attribute and
#  nowhere else. That is what makes "exactly one of the states written" a byte
#  comparison rather than a judgement.
RENAMED = "H6WRT001"
NAME = f"/godot/cue/{RENAMED}/name"
WRITE_ERROR = "/godot/document/writeError"

#  THE BALLAST: about eight thousand characters of notes on each of three cues,
#  so show.xml is some 25 KB. Each write of it is then several pages, one flush
#  and one scan long - a target a random instant can land inside - and on Linux
#  a SIGKILL can cut a write between two pages, which is how a torn temp would
#  arise there. Lower-case words, commas and spaces: nothing the canonical
#  writer escapes, and never the text of a marker.
BALLAST = ("H6WRT002", "H6WRT003", "H6WRT004")
BALLAST_LENGTH = 8000
BALLAST_PHRASE = ("written down so that every save of this show is several pages long "
                  "and a kill has more than an instant to land in, ")

#  THE MARKERS. ASCII letters, digits and hyphens only, so no marker is ever
#  escaped in the XML and each is one token in the log; unique per step, so a
#  file names the one moment it was snapshotted.
CALIBRATION = "H6-calibration"
PLANTED = "H6-planted"
FINAL = "H6-final"
MIDDLE = re.compile(r"H6-[a-z0-9-]+")


def marker(k: int, n: int) -> str:
    return f"H6-k{k:02d}-e{n:05d}"


#  THE PLAN. The suite's seeded-RNG convention (timbre_cache.py), with this
#  stage's date. The seed fixes each kill's delay after arming and how its
#  victim starts, and nothing more: what a kill interrupts depends on how far
#  the writer had got, which no seed controls. So a red is known by the disk
#  state it left, which the run prints, and not by its seed. `--seed=` draws
#  another plan, for a soak.
SEED = 20260930
KILLS = 20

#  ONE STEP EVERY FIVE MILLISECONDS: a node.set and one write command, which is
#  four write commands a tick. Each costs a snapshot on the tick thread (M23:
#  under 2 ms for a show nearly three times this size, in Release), and the
#  queue drops its oldest event beyond 4096 (EventQueue.h), so this is fast
#  enough to keep the writer - about twenty milliseconds an autosave on this box
#  with Defender on, M23 again - permanently behind, and slow enough to leave
#  the tick thread room.
PERIOD = 0.005
ARM_AFTER = 3
KILL_WINDOW = 0.5
LEAVE_OFFER = 0.25
SAVE_ATTEMPTS = 3

#  A file the engine has just let go of can still be held for a moment by an
#  antivirus scanner that noticed it being written. The inspection reads only
#  between processes, so a sharing violation then is somebody else's, and brief.
SETTLE_TIMEOUT = 3.0

SAVE = common.osc_encode("/godot/cmd/document/save")
AUTOSAVE = common.osc_encode("/godot/cmd/document/autosave")

#  Bundle::temporaryFor names a temp `<file>.tmp-<pid>`; Bundle::previousRecoveries
#  takes a folder `recovery.previous.<up to 18 digits>` by its name, and since
#  H6b offers it only when it holds a show no landed save has superseded.
TEMP = re.compile(r"(show\.xml|state\.xml|crash-write\.wfg|superseded)\.tmp-([0-9]+)")
PREVIOUS = re.compile(r"recovery\.previous\.([0-9]{1,18})")

#  THE MARK A SAVE LEAVES IN WHAT IT WILL RETIRE (H6b, namespace draft §23.5):
#  `superseded`, in `recovery/` or a `recovery.previous.N/`, written before the
#  save writes show.xml and naming the show.xml it is about to write -
#  `show.xml <bytes> sha256:<hex>` and a newline (Bundle::fingerprintOf) - and
#  a line more for each later save that found the folder outgrown and could not
#  delete it (Bundle::markSuperseded). A folder with a whole line naming the
#  show.xml on the disk was outgrown by a save that landed, and the engine
#  offers it no more.
SUPERSEDED = "superseded"
FINGERPRINT = re.compile(rb"(?:show\.xml [0-9]+ sha256:[0-9a-f]{64}\n)+")

#  AND WINDOWS' OWN. `ReplaceFile` is not one step: given no backup name -
#  and JUCE gives none - it creates an empty `<file>~RF<hex>.TMP` beside the
#  target, moves the target onto it, moves the replacement into place and
#  deletes it, and a kill inside the call strands it. The first runs of this
#  driver found all three states: an empty one beside an untouched target, a
#  whole one (the old bytes) beside the new target, and a whole one beside NO
#  target - the displaced file the inspection fails on. Go.dot never reads one
#  (`open` asks for its files by name, and the manifest search is for `*.wfg`)
#  and never deletes one outside `recovery/`, so beside a target each is litter
#  rather than damage; and a kill that strands one is a kill that landed INSIDE
#  the replace, which is the moment "old and whole or new and whole" is most in
#  doubt. So they are counted, and what they hold is said. Since H6b the engine
#  renames in one step and strands none, and only `ReplaceFile` makes them: one
#  a kill strands now is the old replace come back, and fails the run
#  (`stranded-rf`, fixer of H6b). One a move-aside carried into a
#  `recovery.previous.N/` is the same file, failed where it was stranded, and is
#  only counted.
SWAP = re.compile(r"(show\.xml|state\.xml|crash-write\.wfg)~RF[0-9A-Fa-f]+\.TMP", re.IGNORECASE)

#  Process ids that are never the engine's: 1 is init on POSIX and no process
#  on Windows, whose ids are multiples of four; 2 likewise. The planted temps
#  use the first, the displaced-show control the second.
PLANTED_PID = 1
DISPLACED_PID = 2


def ballast_for(cue: str) -> str:
    text = f"Ballast for {cue}, " + BALLAST_PHRASE * (BALLAST_LENGTH // len(BALLAST_PHRASE) + 1)
    return text[:BALLAST_LENGTH].rstrip(" ,")


# =============================================================================
# Reading the disk, only ever between processes
# =============================================================================

def read_settled(path: Path) -> "bytes | None":
    """A file's bytes, or None when there is no file.

    ONLY EVER CALLED WITH NO ENGINE ALIVE. Python's `open` on Windows leaves out
    FILE_SHARE_DELETE, so a read that overlapped the engine's `ReplaceFile` on
    that file - or its rename of `recovery/` - would make the ENGINE's write
    fail, and the driver would be reporting a fault it caused (phase5 met this).
    Between processes the one other holder can be a virus scanner, briefly."""
    deadline = time.monotonic() + SETTLE_TIMEOUT

    while True:
        try:
            return path.read_bytes()
        except FileNotFoundError:
            return None
        except PermissionError:
            if path.is_dir():
                return None

            if time.monotonic() >= deadline:
                raise HarnessError(f"{path} was still held by another process "
                                   f"{SETTLE_TIMEOUT:.0f} s after the engine had gone")

            time.sleep(0.05)


def parsed(data: bytes):
    try:
        return ET.fromstring(data)
    except (ET.ParseError, ValueError):
        return None


def parses_as(data: bytes, tag: str) -> bool:
    root = parsed(data)
    return root is not None and root.tag == tag


def parse_note(data: bytes, tag: str) -> str:
    root = parsed(data)

    if root is None:
        return "which does not parse as XML"

    if root.tag != tag:
        return f"which parses, with the root <{root.tag}> rather than <{tag}>"

    return f"which parses as <{tag}> but is not the template with one marker in it"


def digest(data: bytes) -> str:
    """A short name for some bytes, so that a problem's identity says WHICH
    bytes it was about - and a new file that is wrong the same way at the same
    path is a new finding, not the old one said again."""
    return hashlib.sha1(data).hexdigest()[:12]


def applied(log: Path, name: str) -> int:
    """How many APPLIED records of the command `name` a session's log holds.

    phase5's helper of the same name, which is nested there and cannot be
    imported. Read off the command field - `A <tick> <seq> <origin> <command>` -
    so a refusal of the same command cannot answer for it. Safe while the engine
    appends: the log is flushed record by record and opened for sharing."""
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0

    return sum(1 for fields in (line.split() for line in text.splitlines())
               if fields[:1] == ["A"] and fields[4:5] == [name])


class Template:
    """THE SHOW AS BYTES, WITH ONE HOLE IN IT.

    Taken from the calibration save, split around the one place `H6-calibration`
    occurs. A show.xml is "one of the states written" exactly when it is the
    prefix, a marker, and the suffix - byte for byte, so a file that is torn,
    mixed from two writes, or has any byte moved is not one, and the marker in
    the hole says which instant it was snapshotted at."""

    def __init__(self, calibrated: bytes):
        token = CALIBRATION.encode("ascii")
        at = calibrated.find(token)
        self.prefix = calibrated[:at]
        self.suffix = calibrated[at + len(token):]

    def bytes_for(self, name: str) -> bytes:
        return self.prefix + name.encode("ascii") + self.suffix

    def marker_of(self, data: "bytes | None") -> "str | None":
        if data is None or len(data) <= len(self.prefix) + len(self.suffix):
            return None

        if not (data.startswith(self.prefix) and data.endswith(self.suffix)):
            return None

        try:
            middle = data[len(self.prefix):len(data) - len(self.suffix)].decode("ascii")
        except UnicodeDecodeError:
            return None

        return middle if MIDDLE.fullmatch(middle) else None


def temp_state(template: Template, base: str, data: bytes) -> str:
    """'whole', 'partial' or 'empty', and for a whole show the marker in it."""
    if not data:
        return "empty"

    if base == SHOW:
        held = template.marker_of(data)
        return f"whole ({held})" if held else f"partial ({len(data)} bytes)"

    if base == SUPERSEDED:
        return "whole" if FINGERPRINT.fullmatch(data) else f"partial ({len(data)} bytes)"

    whole = parses_as(data, "State") if base == STATE else parses_as(data, "Bundle")
    return "whole" if whole else f"partial ({len(data)} bytes)"


def recovery_folders(bundle: Path) -> "list[Path]":
    """`recovery/`, then every `recovery.previous.N/` in the engine's order."""
    numbered = []

    for child in bundle.iterdir():
        match = PREVIOUS.fullmatch(child.name)

        if match and child.is_dir():
            numbered.append((int(match.group(1)), child))

    numbered.sort(key=lambda entry: entry[0])
    first = [bundle / RECOVERY_DIR] if (bundle / RECOVERY_DIR).is_dir() else []

    return first + [folder for _, folder in numbered]


def fingerprint(show: bytes) -> bytes:
    """What a `superseded` mark holds for these show.xml bytes, spelled here
    from the format and not asked of the engine (Bundle::fingerprintOf)."""
    return f"{SHOW} {len(show)} sha256:{hashlib.sha256(show).hexdigest()}\n".encode("ascii")


def superseded(bundle: Path, folder: Path) -> bool:
    """Bundle::isSuperseded: one whole line of the folder's mark names the
    show.xml on the disk, so a save that wrote it landed. No mark, or no
    show.xml, is not; a line cut short, with no newline, names nothing."""
    mark = read_settled(folder / SUPERSEDED) if (folder / SUPERSEDED).is_file() else None
    shown = read_settled(bundle / SHOW) if (bundle / SHOW).is_file() else None

    if mark is None or shown is None:
        return False

    #  Split on the newline alone, as the engine does (Bundle.cpp's `holdsLine`):
    #  what follows the last one is a line cut short.
    whole = [line + b"\n" for line in mark.split(b"\n")[:-1]]

    return fingerprint(shown) in whole


def holds_offer(bundle: Path, folder: Path) -> bool:
    """What makes a recovery folder an offer since H6b: a show.xml, and no mark
    saying a landed save has outgrown it."""
    return (folder / SHOW).is_file() and not superseded(bundle, folder)


def offered_folder(bundle: Path) -> "Path | None":
    """Bundle::offeredRecovery, read off the same disk the same way: `recovery/`
    when it is an offer, otherwise the `recovery.previous.N/` with the highest
    number that is one, otherwise nothing.

    Until H6b a `recovery.previous.N/` was offered by its NAME, whatever was
    inside, and a folder a landed save had outgrown was offered like any other:
    this driver found both, as the torn offer and the stale offer below, and
    the engine now passes over either - so this mirror does."""
    if holds_offer(bundle, bundle / RECOVERY_DIR):
        return bundle / RECOVERY_DIR

    previous = [folder for folder in recovery_folders(bundle)
                if folder.name != RECOVERY_DIR and holds_offer(bundle, folder)]

    return previous[-1] if previous else None


def temps_of(bundle: Path) -> "dict[str, bytes]":
    """Every temp in the bundle and its recovery folders, Go.dot's and
    ReplaceFile's, by relative path."""
    found = {}

    for folder in [bundle] + recovery_folders(bundle):
        for entry in folder.iterdir():
            if entry.is_file() and (TEMP.fullmatch(entry.name) or SWAP.fullmatch(entry.name)):
                key = entry.name if folder == bundle else f"{folder.name}/{entry.name}"
                found[key] = read_settled(entry) or b""

    return found


class Session:
    """ONE VICTIM'S OWN STATES, OLDEST FIRST: the offer it adopted at its start,
    when it adopted one, then every marker it was sent, in the order they were
    sent - and the folder it adopted from.

    The name only ever moves forward inside a session, and each write command
    snapshots the show as the rename just before it left it, so of two states
    one session held, the one earlier in this list is the older. That is what
    makes "older than the show it saved" a comparison and not a guess. Across
    sessions there is no such order to ask for, and none is needed: an earlier
    session's unanswered offer stands beside a later save on purpose (§14.10)."""

    def __init__(self, adopted: "str | None" = None, adopted_from: "str | None" = None):
        self.states: "list[str]" = [adopted] if adopted else []
        self.adopted_from = adopted_from

    def older(self, offered: str, shown: str) -> bool:
        if offered not in self.states or shown not in self.states:
            return False

        return self.states.index(offered) < self.states.index(shown)


class Finding:
    """What one look at the disk found. It never reports by itself, so the
    controls can ask it about folders that are wrong on purpose."""

    def __init__(self) -> None:
        self.problems: "list[str]" = []
        self.new_problems: "list[str]" = []
        self.new_kinds: "set[str]" = set()
        self.keys: "list[tuple[str, str]]" = []
        self.kinds: "set[str]" = set()
        self.fatal = False
        self.show_marker: "str | None" = None
        self.offer: "Path | None" = None
        self.offer_marker: "str | None" = None
        self.stale: "str | None" = None
        self.victim_temps: "dict[str, str]" = {}
        self.other_temps: "dict[str, str]" = {}
        self.stranded: "dict[str, str]" = {}
        self.new_stranded: "dict[str, str]" = {}
        self.unknown_names: "list[str]" = []
        self.notes: "list[str]" = []
        self.displaced: "list[str]" = []

    def problem(self, kind: str, text: str, fatal: bool = False, subject: str = "") -> None:
        """A problem, and what it is ABOUT - its kind, its file and the evidence
        (the bytes, or what stands beside a missing file) - which is what makes
        the same state seen by two looks one finding and not two, while the
        same fault struck again at the same file is a second one."""
        self.problems.append(text)
        self.keys.append((kind, subject or text))
        self.kinds.add(kind)
        self.fatal = self.fatal or fatal

    def offer_name(self) -> str:
        return f"{self.offer.name}/" if self.offer is not None else "none"

    def whole_temp_markers(self) -> "list[str]":
        """The markers of whole show temps beside show.xml, anybody's."""
        found = []

        for key, state in list(self.victim_temps.items()) + list(self.other_temps.items()):
            if "/" not in key and key.startswith(SHOW + ".tmp-") and state.startswith("whole ("):
                found.append(f"{key} {state}")

        return found


def inspect(bundle: Path, template: Template, allowed_show: "set[str]",
            allowed_any: "set[str]", victim_pid: "int | None" = None,
            before: "dict[str, bytes] | None" = None,
            owner: "dict[str, Session] | None" = None,
            landed: "set[str] | None" = None) -> Finding:
    """Looks at every file the engine could have been writing, and says what is
    wrong. `allowed_show` is what show.xml may hold, `allowed_any` what a
    recovery folder may, `owner` which session each marker was sent to, and
    `landed` every marker show.xml has been seen to hold, this look's included.
    A temp is the victim's when its process id is the victim's and it was not
    already there, byte for byte, before the victim started: Windows reuses
    process ids, and an earlier process's temp with the same number is not
    evidence of where this kill landed."""
    finding = Finding()
    before = before or {}
    temps = temps_of(bundle)

    #  REPLACEFILE'S OWN ARE KNOWN BY THEIR NAME ALONE, NOT THEIR PATH. The hex
    #  in `~RF<hex>` is new on every call, and a plain victim's first autosave
    #  moves `recovery/` - with any `~RF` an earlier kill stranded in it - to a
    #  `recovery.previous.N/`, where the path is new and the file is not: by
    #  its path it would be counted as this kill's.
    carried = {key.rsplit("/", 1)[-1]: data for key, data in before.items()
               if SWAP.fullmatch(key.rsplit("/", 1)[-1])}

    for entry in bundle.iterdir():
        if entry.name in (SHOW, STATE, MANIFEST) or TEMP.fullmatch(entry.name) \
                or SWAP.fullmatch(entry.name):
            continue

        if entry.is_dir() and (entry.name == RECOVERY_DIR or PREVIOUS.fullmatch(entry.name)):
            continue

        finding.unknown_names.append(entry.name)

    for key, data in sorted(temps.items()):
        name = key.rsplit("/", 1)[-1]
        swap = SWAP.fullmatch(name)

        if swap:
            state = temp_state(template, swap.group(1).lower(), data)
            finding.stranded[key] = state

            if victim_pid is not None and carried.get(name) != data:
                finding.new_stranded[key] = state

            continue

        base, pid = TEMP.fullmatch(name).groups()
        state = temp_state(template, base, data)

        if victim_pid is not None and int(pid) == victim_pid and before.get(key) != data:
            finding.victim_temps[key] = state
        else:
            finding.other_temps[key] = state

    #  A `~RF` THIS KILL STRANDED is `ReplaceFile` come back (fixer of H6b):
    #  nothing else makes one, and H6b took it out of `writeBytesAtomically`.
    #  Not fatal - the displaced file it can leave is failed on its own below -
    #  and failed once, where it was stranded; carried on by a move-aside it is
    #  the same file, and only counted.
    for key, state in sorted(finding.new_stranded.items()):
        finding.problem("stranded-rf",
                        f"{key} ({state}) was stranded by this kill: a temp only ReplaceFile makes, "
                        f"so ReplaceFile is back in writeBytesAtomically, where H6b took it out. "
                        f"An engine finding, not a flake", subject=f"{key} {state}")

    def of(base: str) -> "list[tuple[str, str]]":
        """Every temp of one file in the bundle's own folder, anybody's."""
        every = list(finding.victim_temps.items()) + list(finding.other_temps.items()) \
              + list(finding.stranded.items())

        return sorted((key, state) for key, state in every
                      if "/" not in key and (key.startswith(base + ".tmp-")
                                             or key.lower().startswith(base.lower() + "~rf")))

    def beside(base: str) -> str:
        listed = [f"{key} {state}" for key, state in of(base)]
        return "; ".join(listed) if listed else "no temp of it"

    def displaced(base: str) -> "tuple[bool, str, str]":
        """THE DISPLACED FILE: no target, and a whole copy of it under another
        name beside it. Until H6b there were two ways: ReplaceFile's
        ERROR_UNABLE_TO_MOVE_REPLACEMENT left the new bytes under the temp's
        name until the 20/60/200 ms retry moved them, and a kill between
        ReplaceFile's own two moves - seen on the manifest on 2026-09-30 - left
        the old bytes under its `~RF` name, the new under the temp's. The
        engine's rename is one step now and should leave neither. Either way
        `open` looks for neither. The third answer is the evidence - the whole
        copies by name - for the problem's identity."""
        whole = [key for key, state in of(base) if state.startswith("whole")]

        if not whole:
            return False, f"; beside it: {beside(base)}", ""

        return True, (f"; and beside it, WHOLE: {', '.join(whole)} - the DISPLACED {base}, "
                      f"a replace that took the old file away and was killed or refused before "
                      f"the new one took its name. An engine finding, not a flake: {beside(base)}"), \
            ", ".join(whole)

    # --- the show -----------------------------------------------------------
    shown = read_settled(bundle / SHOW) if (bundle / SHOW).is_file() else None

    if shown is None:
        moved, why, evidence = displaced(SHOW)
        finding.problem("displaced-show" if moved else "missing-show", f"{SHOW} is missing{why}",
                        fatal=True, subject=f"{SHOW} [{evidence}]")

        if moved:
            finding.displaced.append(SHOW)
    else:
        held = template.marker_of(shown)

        if held is None:
            finding.problem("torn-show",
                            f"{SHOW} is not one of the states written: {len(shown)} bytes, "
                            f"{parse_note(shown, 'Show')}; beside it: {beside(SHOW)}",
                            fatal=True, subject=f"{SHOW} {digest(shown)}")
        else:
            finding.show_marker = held

            if held not in allowed_show:
                finding.problem("backwards",
                                f"{SHOW} holds {held}, which the show neither started with, "
                                f"adopted nor was sent: it went backwards, or came from nowhere",
                                subject=f"{SHOW} {held}")

    # --- the operator's position, and the manifest ---------------------------
    state = read_settled(bundle / STATE) if (bundle / STATE).is_file() else None

    if state is None:
        moved, why, evidence = displaced(STATE)
        finding.problem("displaced-state" if moved else "missing-state", f"{STATE} is missing{why}",
                        subject=f"{STATE} [{evidence}]")

        if moved:
            finding.displaced.append(STATE)
    elif not parses_as(state, "State"):
        finding.problem("torn-state", f"{STATE} is {len(state)} bytes, {parse_note(state, 'State')}",
                        subject=f"{STATE} {digest(state)}")

    manifest = read_settled(bundle / MANIFEST) if (bundle / MANIFEST).is_file() else None

    if manifest is None:
        moved, why, evidence = displaced(MANIFEST)
        finding.problem("displaced-manifest" if moved else "missing-manifest",
                        f"{MANIFEST} is missing{why}", fatal=True, subject=f"{MANIFEST} [{evidence}]")

        if moved:
            finding.displaced.append(MANIFEST)
    else:
        root = parsed(manifest)

        if root is None or root.tag != "Bundle" or root.get("formatVersion") != "1":
            finding.problem("torn-manifest",
                            f"{MANIFEST} is not <Bundle formatVersion=\"1\"/>: {manifest[:120]!r}",
                            fatal=True, subject=f"{MANIFEST} {digest(manifest)}")

    # --- every recovery folder, and the one the engine will offer ------------
    finding.offer = offered_folder(bundle)

    for folder in recovery_folders(bundle):
        offered = folder == finding.offer

        for entry in folder.iterdir():
            if entry.name not in (SHOW, STATE, SUPERSEDED) and not TEMP.fullmatch(entry.name) \
                    and not SWAP.fullmatch(entry.name):
                finding.unknown_names.append(f"{folder.name}/{entry.name}")

        kept = read_settled(folder / SHOW) if (folder / SHOW).is_file() else None

        if kept is not None:
            held = template.marker_of(kept)

            if held is None or held not in allowed_any:
                finding.problem("torn-recovery",
                                f"{folder.name}/{SHOW} is not a state any session was sent: "
                                f"{len(kept)} bytes, "
                                + (f"holding {held}" if held else parse_note(kept, "Show")),
                                fatal=offered, subject=f"{folder.name}/{SHOW} {digest(kept)}")
            elif offered:
                finding.offer_marker = held

        position = read_settled(folder / STATE) if (folder / STATE).is_file() else None

        if position is not None and not parses_as(position, "State"):
            finding.problem("torn-recovery-state",
                            f"{folder.name}/{STATE} is {len(position)} bytes, "
                            f"{parse_note(position, 'State')}",
                            subject=f"{folder.name}/{STATE} {digest(position)}")

    #  A RECOVERY FILE UNDER NO NAME. A file missing from a recovery folder is
    #  often nothing: the folder was just made and its first write never
    #  landed, or a save was deleting it. So only the signature a kill between
    #  ReplaceFile's own two moves leaves counts here, and only for this kill:
    #  no file, its OLD bytes whole under a `~RF` name this kill stranded, and
    #  its NEW bytes whole under the victim's own temp. A show so displaced
    #  takes the offer it was with it - the autosave before it is on the disk
    #  and nothing offers it - so it fails. A state so displaced costs the
    #  recovered standby, which §3.20 keeps apart because losing it is not
    #  losing work, and the brief asks of a recovery's state.xml only that one
    #  present parses: it is said, and counted, and does not fail.
    for folder in recovery_folders(bundle):
        for base in (SHOW, STATE):
            if (folder / base).is_file():
                continue

            old = [key for key, state in finding.new_stranded.items()
                   if key.lower().startswith(f"{folder.name}/{base}~rf") and state.startswith("whole")]
            new = [key for key, state in finding.victim_temps.items()
                   if key.startswith(f"{folder.name}/{base}.tmp-") and state.startswith("whole")]

            if not (old and new):
                continue

            finding.displaced.append(f"{folder.name}/{base}")
            said = (f"{folder.name}/{base} is under no name: its old bytes whole under "
                    f"{old[0]}, its new ones whole under {new[0]} - a kill between "
                    f"ReplaceFile's own two moves")

            if base == SHOW:
                finding.problem("displaced-recovery-show",
                                f"{said}, so the offer this folder held is gone from the engine's "
                                f"view while its bytes sit on the disk. An engine finding, not a "
                                f"flake", subject=f"{folder.name}/{base} {old[0]}")
            else:
                finding.notes.append(f"{said}; the recovered standby would come back at its "
                                     f"default (the same engine finding, in the file §3.20 lets go)")

    #  A TORN OFFER - a `recovery.previous.N/` offered by its name with no show
    #  in it, left by a kill inside its deletion - was a check here until H6b.
    #  The engine offers only a folder that holds a show now, and so does
    #  `offered_folder`, so no offer read off the disk can be torn: an engine
    #  that offered one anyway disagrees with the mirror, and `announced` says
    #  so at the next start. The controls ask the engine directly.

    #  A STALE OFFER: the next start offers an autosave OLDER than a show.xml
    #  the same session saved. A save writes show.xml, state.xml and the
    #  manifest, and only then deletes the session's `recovery/` - or the
    #  `recovery.previous.N/` it adopted, which is consumed (DocumentWriter.cpp,
    #  the save job) - because the work has become the show (§14.10). A kill
    #  in between left the last autosave where `serve` offered it as the most
    #  recent afternoon there is, and `document.recover` or `--recover` then
    #  took the show back past the save, with the dot lit to invite saving the
    #  loss for good. Since H6b the save marks those folders `superseded` before
    #  it writes, so a folder it outgrew is no offer: one that is offered
    #  anyway is that mark missing or misread. "Older" is decided inside one
    #  session only (`Session`): an EARLIER session's unanswered offer is left
    #  standing by a later save on purpose, and is not this. Not fatal: every
    #  file is whole and every later start opens, so the loop goes on and
    #  counts. An engine finding, not a flake.
    if owner and finding.offer is not None and finding.offer_marker is not None \
            and finding.show_marker is not None:
        session = owner.get(finding.show_marker)

        if session is not None and finding.offer.name in (RECOVERY_DIR, session.adopted_from) \
                and session.older(finding.offer_marker, finding.show_marker):
            finding.stale = (f"{finding.offer.name}/ offers {finding.offer_marker}, older than the "
                             f"{finding.show_marker} the same session saved into {SHOW}")
            finding.problem("stale-offer",
                            f"{finding.stale}: the save's {SHOW} landed and the offer outlived "
                            f"it, so recovering it takes the show back past the save. An engine "
                            f"finding (DocumentWriter.cpp's save job retires the offer only after "
                            f"all three writes, and marks it {SUPERSEDED} before them - a mark "
                            f"missing or misread), not a flake",
                            subject=f"{finding.offer_marker} under {finding.show_marker}")

    #  A HIDDEN OFFER (fixer of H6b): the stale offer turned inside out. Since
    #  H6b the engine passes over a folder whose `superseded` mark names
    #  show.xml, and so does `offered_folder` - by the same rule, so every
    #  restart check above agrees with an engine that hides a folder it should
    #  not have, and cannot see it. A mark on an earlier session's unanswered
    #  offer, or one written over an afternoon newer than the save that landed,
    #  would silently lose the most recent work there is. So a folder passed
    #  over must be one a landed save OUTGREW: its show a state of some session
    #  that also put a show.xml on the disk no older than it - one seen by a
    #  look, this one included - and the folder that session's saves retire,
    #  its own `recovery/` or the `recovery.previous.N/` it adopted from.
    #  Anything else is an afternoon hidden that should stand. Not fatal.
    if owner is not None:
        sessions = list({id(each): each for each in owner.values()}.values())
        shows = set(landed or ()) | ({finding.show_marker} if finding.show_marker else set())

        for folder in recovery_folders(bundle):
            if not (folder / SHOW).is_file() or not superseded(bundle, folder):
                continue

            held = template.marker_of(read_settled(folder / SHOW))

            if held is None:
                continue  # not a state anybody wrote: failed as torn above

            outgrown = [shown for each in sessions
                        if held in each.states and folder.name in (RECOVERY_DIR, each.adopted_from)
                        for shown in shows
                        if shown in each.states and (shown == held or each.older(held, shown))]

            if not outgrown:
                finding.problem("hidden-offer",
                                f"{folder.name}/ holds {held} and is passed over for a {SUPERSEDED} "
                                f"mark naming {SHOW}, but no save that landed had outgrown it: no "
                                f"session that held {held} put a {SHOW} as new on the disk, from a "
                                f"folder its saves retire. An afternoon hidden that should stand - "
                                f"an engine finding, not a flake",
                                subject=f"{folder.name}/ {held} hidden")

    return finding


def describe(finding: Finding) -> str:
    """One line of what the disk holds, for the log of the run."""
    temps = ", ".join(f"{key} {state}" for key, state in finding.victim_temps.items()) or "none"
    others = len(finding.other_temps)
    offer = finding.offer_name() + (f" {finding.offer_marker}" if finding.offer_marker else "") \
        + (" (STALE: older than the show its session saved)" if finding.stale else "")
    stranded = ", ".join(f"{key} {state}" for key, state in finding.new_stranded.items())

    return (f"show.xml {finding.show_marker}; offer {offer}; the victim's temps: {temps}; "
            f"other temps: {others}"
            + (f"; ReplaceFile's own, stranded by this kill: {stranded}" if stranded else "")
            + (f"; ReplaceFile's own, from earlier: {len(finding.stranded) - len(finding.new_stranded)}"
               if len(finding.stranded) > len(finding.new_stranded) else "")
            + (f"; other names: {finding.unknown_names}" if finding.unknown_names else ""))


# =============================================================================
# The sessions
# =============================================================================

def start(report: Report, label: str, bundle: Path, **options) -> "Server | None":
    """A `wfg serve`, or a FAIL that says why not - never a HarnessError.

    serve exiting 2 on a folder a kill left is a finding about the engine, and
    `main` would otherwise report it as a harness that could not run."""
    try:
        return Server(bundle, **options)
    except HarnessError as problem:
        report.check(False, f"{label}: serve starts on the folder as the kill left it",
                     str(problem).strip())
        return None


@contextlib.contextmanager
def dying(server: Server):
    """Every session here ends in a kill, whatever happens inside it.

    `Server`'s own `with` would call `stop()`, which on POSIX is a SIGTERM and a
    clean exit - one that drains the writer, and may delete `recovery/` - and a
    check session that tidied the folder before the next one looked at it would
    be a check of shutdown rather than of the crash."""
    try:
        yield server
    finally:
        if server.process.poll() is None:
            doc.crash(server)


def save_until_clean(server: Server) -> "tuple[bool, str]":
    """A save, waited for as the dot going out, which `settle` does only when
    the writer says the bytes landed - asked again up to three times, because a
    virus scanner holding a fresh file can outlast the writer's own retries on a
    shared runner (be46383's run)."""
    for attempt in range(1, SAVE_ATTEMPTS + 1):
        doc.command(server, "document.save")

        if doc.reads(server, doc.DIRTY, False):
            return True, ""

        print(f"  note: save {attempt} of {SAVE_ATTEMPTS} did not put the dot out; "
              f"writeError reads {doc.value_of(server, WRITE_ERROR)!r}")

    return False, f"writeError reads {doc.value_of(server, WRITE_ERROR)!r}"


def announced(report: Report, label: str, server: Server, finding: Finding) -> None:
    """The start-up notice: `recovery available` exactly when the disk holds an
    offer, naming the folder when it is not `recovery/`. Printed before the port
    lines, so `Server` has it among the notices it collected."""
    told = [line for line in server.notices if "recovery available" in line]

    if finding.offer is None:
        report.check(not told, f"{label}: nothing is offered on the disk, and serve announces nothing",
                     " | ".join(told))
    elif finding.offer.name == RECOVERY_DIR:
        report.check(told == ["wfg: recovery available"],
                     f"{label}: serve announces the offer in recovery/",
                     " | ".join(server.notices))
    else:
        report.check(len(told) == 1 and told[0].endswith(f" in {finding.offer.name}/"),
                     f"{label}: serve announces the offer in {finding.offer.name}/",
                     " | ".join(server.notices))


def opened_plain(report: Report, label: str, server: Server, finding: Finding) -> None:
    """A plain start on the folder: the offer as the disk has it, and the show
    as show.xml has it - never as a temp beside it has it."""
    announced(report, label, server, finding)

    report.check(doc.reads(server, doc.RECOVERY, finding.offer is not None),
                 f"{label}: /godot/document/recovery reads {finding.offer is not None}, as the disk says",
                 f"it reads {doc.value_of(server, doc.RECOVERY)!r}")

    unread = [entry for entry in finding.whole_temp_markers()
              if f"({finding.show_marker})" not in entry]
    aside = f", not the whole temp beside it ({'; '.join(unread)})" if unread else ""

    report.check(doc.reads(server, NAME, finding.show_marker),
                 f"{label}: the show opens as show.xml holds it, {finding.show_marker}{aside}",
                 f"the name reads {doc.value_of(server, NAME)!r}")
    report.check(doc.reads(server, doc.DIRTY, False), f"{label}: with the dot out",
                 f"{doc.DIRTY} reads {doc.value_of(server, doc.DIRTY)!r}")


def went_back(label: str, server: Server, finding: Finding, how: str) -> None:
    """WHAT ADOPTING A STALE OFFER PUT ON SCREEN, said and not counted: the
    engine did as it was asked, and what it was asked to adopt is the fault,
    which the inspection has already failed once. A pass here would be the
    driver agreeing that the show may go back past a save."""
    doc.reads(server, NAME, finding.offer_marker)
    print(f"  note {label}: {how} the stale offer, and the show now reads "
          f"{doc.value_of(server, NAME)!r} - back past the save of {finding.show_marker} "
          f"(the stale offer is failed above, where the kill left it)")


def recovered_at_start(report: Report, label: str, server: Server, finding: Finding) -> None:
    """A `--recover` start: it opened, rather than exiting 2, said which of its
    two sentences it had to say, and publishes what it adopted - or show.xml,
    when there was nothing to adopt."""
    adopted = finding.offer is not None
    said = "wfg: recovery adopted" if adopted else "wfg: nothing to recover"
    wanted = finding.offer_marker if adopted else finding.show_marker

    announced(report, label, server, finding)
    report.check(said in server.notices, f"{label}: --recover says '{said[5:]}'",
                 " | ".join(server.notices))

    if adopted and finding.stale:
        went_back(label, server, finding, "--recover adopted")
    else:
        report.check(doc.reads(server, NAME, wanted),
                     f"{label}: and publishes {wanted}, "
                     + (f"the offer in {finding.offer_name()}" if adopted else "as show.xml holds it"),
                     f"the name reads {doc.value_of(server, NAME)!r}")
    report.check(doc.reads(server, doc.RECOVERY, False),
                 f"{label}: with nothing left on offer",
                 f"{doc.RECOVERY} reads {doc.value_of(server, doc.RECOVERY)!r}")
    report.check(doc.reads(server, doc.DIRTY, adopted),
                 f"{label}: and the dot {'lit: the recovered work is not the show on disk' if adopted else 'out'}",
                 f"{doc.DIRTY} reads {doc.value_of(server, doc.DIRTY)!r}")


def recover_or_refuse(report: Report, label: str, server: Server, log: Path,
                      finding: Finding) -> None:
    """`document.recover` on the folder the kill left. Applied when something is
    offered - and judged by five things at once, because the offer's marker can
    equal show.xml's and the name alone would prove nothing; by four when the
    offer is stale, whose adoption is said and not passed (`went_back`) - or
    refused `no-recovery` when nothing is."""
    if finding.offer is None:
        said = doc.refusal_of(server, lambda: doc.command(server, "document.recover"))

        report.check(said.endswith(" no-recovery document.recover"),
                     f"{label}: with nothing offered, document.recover is refused no-recovery",
                     f"lastError reads {said!r}")
        return

    errors = doc.error_count(server)
    doc.command(server, "document.recover")

    landed = common.wait_until(lambda: applied(log, "document.recover") >= 1)

    report.check(bool(landed), f"{label}: document.recover is applied (its record is in the log)",
                 f"no applied document.recover in {log.name}")
    report.check(doc.reads(server, doc.RECOVERY, False), f"{label}: and nothing is left on offer",
                 f"{doc.RECOVERY} reads {doc.value_of(server, doc.RECOVERY)!r}")

    if finding.stale:
        went_back(label, server, finding, "document.recover adopted")
    else:
        report.check(doc.reads(server, NAME, finding.offer_marker),
                     f"{label}: and the show is the offer's, {finding.offer_marker} from "
                     f"{finding.offer_name()}",
                     f"the name reads {doc.value_of(server, NAME)!r}")

    report.equal(doc.error_count(server), errors, f"{label}: with no refusal counted")
    report.check(doc.reads(server, doc.DIRTY, True),
                 f"{label}: and the dot lit, because the recovered work is not the show on disk",
                 f"{doc.DIRTY} reads {doc.value_of(server, doc.DIRTY)!r}")


def stream_until_kill(report: Report, label: str, victim: Server, log: Path, k: int,
                      delay: float, sent: "list[str]") -> dict:
    """Names and writes from ONE socket until the kill, and then the kill.

    ARMED ON THE VICTIM'S OWN LOG, once three write commands are applied: the
    writer then has work, and more behind it. An applied record means "handed
    to the writer" and never "landed" (Bundle.cpp's save handler), so nothing
    about the files is inferred from it - the disk is read after the kill."""
    address = (common.HOST, victim.osc_port)
    steps = 0
    armed_at = None
    began = time.monotonic()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        while True:
            now = time.monotonic()

            if armed_at is not None and now >= armed_at + delay:
                break

            if armed_at is None and (now - began > common.REPLY_TIMEOUT
                                     or victim.process.poll() is not None):
                break

            name = marker(k, steps)
            sent.append(name)

            #  Wrapped, because after an engine has gone a Windows socket can
            #  report the ICMP port-unreachable on a later call (WSAECONNRESET).
            for packet in (common.osc_encode(NAME, [name]), SAVE if steps % 3 == 0 else AUTOSAVE):
                try:
                    sock.sendto(packet, address)
                except OSError:
                    pass

            steps += 1

            if armed_at is None and steps % 8 == 0:
                if applied(log, "document.save") + applied(log, "document.autosave") >= ARM_AFTER:
                    armed_at = time.monotonic()

            time.sleep(PERIOD)

    alive = victim.process.poll() is None

    if armed_at is None and alive:
        report.check(False, f"{label}: the stream arms within {common.REPLY_TIMEOUT:.0f} s",
                     f"{applied(log, 'document.save')} saves and "
                     f"{applied(log, 'document.autosave')} autosaves applied after {steps} steps")

    report.check(alive, f"{label}: the engine is still running when the kill comes",
                 "" if alive else f"it exited {victim.process.returncode} on its own:\n"
                                  + (victim.process.stderr.read() or "")[-2000:])

    doc.crash(victim)

    return {"steps": steps,
            "armed": (armed_at - began) if armed_at is not None else None,
            "lived": time.monotonic() - began,
            "saves": applied(log, "document.save"),
            "autosaves": applied(log, "document.autosave"),
            "sets": applied(log, "node.set")}


# =============================================================================
# The run
# =============================================================================

class Run:
    def __init__(self, report: Report, room: Path, locale: "str | None", kills: int, seed: int):
        self.report = report
        self.room = room
        self.locale = locale
        self.kills = kills
        self.seed = seed
        self.bundle = common.copy_bundle(FIXTURE, room / BUNDLE)
        self.template: "Template | None" = None
        self.sent_ever: "set[str]" = set()
        self.owner: "dict[str, Session]" = {}
        self.landed: "set[str]" = set()
        self.rows: "list[dict]" = []
        self.stopped_at: "int | None" = None
        self.finding: "Finding | None" = None

    def allowed_any(self) -> "set[str]":
        return self.sent_ever | {CALIBRATION}

    def look(self, label: str, allowed_show: "set[str]", known: "Finding | None" = None,
             **options) -> Finding:
        """Inspects the folder and reports every problem as a FAIL of its own -
        ONCE. A state an earlier look already failed on, which no session since
        has put right, is the same finding and not a second one, so it is only
        said again; what is new is kept for the caller. A problem's identity
        carries its evidence (`Finding.problem`), so the same fault struck
        again by a later kill is new, and fails again.

        EVERY show.xml A LOOK SEES IS A SAVE THAT LANDED, and is kept: a folder
        the engine passes over for its mark must have been outgrown by one of
        them (`inspect`, the hidden offer). Only a session's save changes
        show.xml - a victim's, or the final one's - and the look after it sees
        its last."""
        finding = inspect(self.bundle, self.template, allowed_show, self.allowed_any(),
                          owner=self.owner, landed=self.landed, **options)

        if finding.show_marker is not None:
            self.landed.add(finding.show_marker)

        seen = set(known.keys) if known is not None else set()

        for key, problem in zip(finding.keys, finding.problems):
            if key in seen:
                print(f"  still {label}: {problem}")
            else:
                finding.new_problems.append(problem)
                finding.new_kinds.add(key[0])
                self.report.check(False, f"{label}: {problem}")

        for note in finding.notes:
            print(f"  note {label}: {note}")

        return finding

    # --- 0 ------------------------------------------------------------------
    def calibrate(self) -> bool:
        report = self.report
        print("\n--- calibration: the show as bytes, with one hole in it ---")

        server = start(report, "calibration", self.bundle, log=self.room / "calibration.wfglog",
                       locale=self.locale)

        if server is None:
            return False

        with dying(server):
            report.check(doc.reads(server, doc.RECOVERY, False),
                         "calibration: a fresh copy of the fixture has nothing to recover",
                         f"{doc.RECOVERY} reads {doc.value_of(server, doc.RECOVERY)!r}")
            report.check(doc.reads(server, doc.DIRTY, False), "calibration: and nothing unsaved",
                         f"{doc.DIRTY} reads {doc.value_of(server, doc.DIRTY)!r}")

            for cue in BALLAST:
                address = f"/godot/cue/{cue}/notes"
                text = ballast_for(cue)
                doc.write(server, address, text)

                report.check(doc.reads(server, address, text),
                             f"calibration: {cue} carries {len(text)} characters of notes",
                             f"its notes read {len(str(doc.value_of(server, address) or ''))} characters")

            doc.write(server, NAME, CALIBRATION)
            report.check(doc.reads(server, NAME, CALIBRATION),
                         f"calibration: the marker cue is named {CALIBRATION}",
                         f"the name reads {doc.value_of(server, NAME)!r}")

            landed, why = save_until_clean(server)
            report.check(landed, "calibration: the save lands, and the dot goes out", why)

            if not landed:
                return False

        calibrated = read_settled(self.bundle / SHOW) or b""
        count = calibrated.count(CALIBRATION.encode("ascii"))

        report.equal(count, 1, "calibration: show.xml names the marker exactly once, so the "
                               "template has one hole")
        report.check(b'goDebounce="0"' in calibrated and b'preWait="0.25"' in calibrated,
                     "calibration: goDebounce is 0 and the pre-wait is written 0.25, with a point "
                     "under every locale",
                     f"show.xml begins {calibrated[:300]!r}")

        leftovers = sorted(temps_of(self.bundle)) + (
            [RECOVERY_DIR + "/"] if (self.bundle / RECOVERY_DIR).exists() else [])
        report.check(not leftovers,
                     "calibration: a save that landed leaves no temp and no recovery/ behind it",
                     f"left: {leftovers}")

        print(f"  show.xml is {len(calibrated)} bytes")

        if count != 1:
            return False

        self.template = Template(calibrated)
        return True

    # --- 1 ------------------------------------------------------------------
    def controls(self) -> bytes:
        """The detectors, each shown to bite on a folder that is wrong on
        purpose - copies of the calibrated bundle, so the one the kills use is
        never touched - and the two temps planted for the first start."""
        report = self.report
        template = self.template
        print("\n--- the controls: every detector bites ---")

        planted = template.bytes_for(PLANTED)
        (self.bundle / f"{SHOW}.tmp-{PLANTED_PID}").write_bytes(planted)
        (self.bundle / RECOVERY_DIR).mkdir(exist_ok=True)
        (self.bundle / RECOVERY_DIR / f"{SHOW}.tmp-{PLANTED_PID}").write_bytes(planted)

        def copy(name: str) -> Path:
            return common.copy_bundle(self.bundle, self.room / name / BUNDLE)

        # A show.xml cut in half: flagged, and serve will not open it.
        torn = copy("torn")
        whole = (torn / SHOW).read_bytes()
        (torn / SHOW).write_bytes(whole[:len(whole) // 2])
        finding = inspect(torn, template, {CALIBRATION}, {CALIBRATION})

        report.check("torn-show" in finding.kinds and finding.fatal,
                     "control: inspect() flags a show.xml cut in half",
                     "; ".join(finding.problems) or "it found nothing wrong")

        try:
            server = Server(torn, locale=self.locale)
        except HarnessError as problem:
            report.check("could not be loaded" in str(problem),
                         "control: and serve will not open it: exit 2, 'could not be loaded'",
                         str(problem).strip())
        else:
            with dying(server):
                report.check(False, "control: and serve will not open it",
                             "serve started on a show.xml cut in half")

        # A recovery/show.xml cut in half: the offer is torn, and --recover
        # stops the start - which also proves recover=True reaches the binary.
        torn_offer = copy("torn-offer")
        (torn_offer / RECOVERY_DIR / SHOW).write_bytes(planted[:len(planted) // 2])
        finding = inspect(torn_offer, template, {CALIBRATION}, {CALIBRATION})

        report.check("torn-recovery" in finding.kinds and finding.fatal
                     and finding.offer == torn_offer / RECOVERY_DIR,
                     "control: inspect() flags an offered recovery/show.xml cut in half",
                     "; ".join(finding.problems) or "it found nothing wrong")

        try:
            server = Server(torn_offer, locale=self.locale, recover=True)
        except HarnessError as problem:
            report.check("could not be read" in str(problem),
                         "control: and serve --recover will not start on it: exit 2, "
                         "'could not be read' - the recover keyword reaches the binary",
                         str(problem).strip())
        else:
            with dying(server):
                report.check(False, "control: and serve --recover will not start on it",
                             f"serve started; it said {server.notices}")

        # The displaced show, as ReplaceFile's documented failure leaves it -
        # in a copy WITHOUT the planted temp, which is whole and would make the
        # diagnosis by itself, so that only the moved file can.
        displaced = copy("displaced")
        (displaced / f"{SHOW}.tmp-{PLANTED_PID}").unlink()
        (displaced / SHOW).rename(displaced / f"{SHOW}.tmp-{DISPLACED_PID}")
        finding = inspect(displaced, template, {CALIBRATION}, {CALIBRATION})

        report.check("displaced-show" in finding.kinds and finding.fatal
                     and any(f"{SHOW}.tmp-{DISPLACED_PID}" in text for text in finding.problems),
                     "control: inspect() names a displaced show: no show.xml, and its temp "
                     "whole beside it",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # And the same show caught between two renames of ReplaceFile's own: the
        # old bytes under its `~RF` name and nothing under the real one.
        swapped = copy("swapped")
        (swapped / f"{SHOW}.tmp-{PLANTED_PID}").unlink()
        (swapped / SHOW).rename(swapped / f"{SHOW}~RF1a2b3c4.TMP")
        finding = inspect(swapped, template, {CALIBRATION}, {CALIBRATION})

        report.check("displaced-show" in finding.kinds and finding.fatal
                     and any(f"{SHOW}~RF1a2b3c4.TMP" in text for text in finding.problems),
                     "control: and one caught inside ReplaceFile itself: no show.xml, its old "
                     "bytes whole under the ~RF name",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # And a recovery/show.xml caught there by a victim: the offer gone, its
        # old bytes under the `~RF` name and its new ones under the victim's temp.
        offer_gone = copy("offer-gone")
        kept = offer_gone / RECOVERY_DIR
        (kept / SHOW).write_bytes(template.bytes_for(CALIBRATION))
        (kept / SHOW).rename(kept / f"{SHOW}~RF1a2b3c4.TMP")
        (kept / f"{SHOW}.tmp-{DISPLACED_PID}").write_bytes(template.bytes_for(CALIBRATION))
        finding = inspect(offer_gone, template, {CALIBRATION}, {CALIBRATION},
                          victim_pid=DISPLACED_PID, before={})

        report.check("displaced-recovery-show" in finding.kinds and not finding.fatal,
                     "control: inspect() flags a recovery/show.xml under no name, its offer gone",
                     "; ".join(finding.problems) or "it found nothing wrong")
        report.check("stranded-rf" in finding.kinds,
                     "control: and fails the ~RF that kill stranded, which only ReplaceFile makes",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # A `~RF` an earlier kill stranded in recovery/, carried to
        # recovery.previous.1/ when a plain victim's first autosave moved the
        # folder aside: a new path, the same file, and not the next kill's.
        carried = copy("carried")
        (carried / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(CALIBRATION))
        (carried / RECOVERY_DIR / f"{SHOW}~RF1a2b3c4.TMP").write_bytes(b"")
        before = temps_of(carried)
        (carried / RECOVERY_DIR).rename(carried / "recovery.previous.1")
        finding = inspect(carried, template, {CALIBRATION}, {CALIBRATION},
                          victim_pid=DISPLACED_PID, before=before)

        report.check(bool(finding.stranded) and not finding.new_stranded,
                     "control: a ~RF carried into recovery.previous.N/ with its folder is not "
                     "counted as stranded by the kill after",
                     f"found {finding.stranded}; counted as this kill's: {finding.new_stranded}")

        # What a kill inside the deletion of a recovery.previous.N/, file by
        # file, would leave: the folder, with no show in it. Offered by its name
        # until H6b, it was the torn offer - `--recover` exited 2 on it. Now the
        # engine offers only a folder holding a show, and so does the mirror;
        # asked of the engine itself, since no look at the disk can tell.
        torn_previous = copy("torn-previous")
        folder = torn_previous / "recovery.previous.1"
        folder.mkdir()
        (folder / STATE).write_bytes((torn_previous / STATE).read_bytes())
        finding = inspect(torn_previous, template, {CALIBRATION}, {CALIBRATION})

        report.check(finding.offer is None and not finding.problems,
                     "control: inspect() offers no recovery.previous.N/ with no show.xml in it",
                     "; ".join(finding.problems) or f"it offers {finding.offer_name()}")
        self.nothing_to_recover(torn_previous, "a recovery.previous.N/ with no show.xml in it")

        # A show that went backwards: a whole state, but not one allowed.
        finding = inspect(self.bundle, template, {PLANTED}, {CALIBRATION})

        report.check("backwards" in finding.kinds and not finding.fatal,
                     "control: inspect() flags a whole show.xml holding a state nobody sent",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # A stale offer: one session sent `early` then `late`, its save of
        # `late` landed, and its autosave of `early` is still in recovery/.
        # In copies of their own, like every control, so nothing the loop does
        # can meet these markers.
        early, late, theirs = marker(99, 4), marker(99, 5), marker(98, 7)
        session = Session()
        session.states += [early, late]
        owner = {early: session, late: session}

        stale = copy("stale")
        (stale / SHOW).write_bytes(template.bytes_for(late))
        (stale / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(early))
        finding = inspect(stale, template, {late}, {early, late}, owner=owner)

        report.check("stale-offer" in finding.kinds and not finding.fatal and bool(finding.stale),
                     "control: inspect() flags an offer in recovery/ older than the show its own "
                     "session saved",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # And in the recovery.previous.N/ a session adopted, which its save
        # also retires: the adopted state comes before everything it was sent.
        adopter = Session(theirs, "recovery.previous.1")
        adopter.states.append(late)
        stale_adopted = copy("stale-adopted")
        (stale_adopted / SHOW).write_bytes(template.bytes_for(late))
        (stale_adopted / "recovery.previous.1").mkdir()
        (stale_adopted / "recovery.previous.1" / SHOW).write_bytes(template.bytes_for(theirs))
        finding = inspect(stale_adopted, template, {late}, {theirs, late}, owner={late: adopter})

        report.check("stale-offer" in finding.kinds
                     and finding.offer == stale_adopted / "recovery.previous.1",
                     "control: and in the recovery.previous.N/ that session adopted, which its "
                     "save retires too",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # But not an EARLIER session's unanswered offer, which a later save
        # leaves standing by design (§14.10): older, and not stale.
        (stale / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(theirs))
        finding = inspect(stale, template, {late}, {early, late, theirs}, owner=owner)

        report.check(not finding.problems and finding.offer_marker == theirs,
                     "control: but not an earlier session's unanswered offer beside a later save, "
                     "which the design keeps standing",
                     "; ".join(finding.problems) or f"the offer holds {finding.offer_marker}")

        # The stale offer as H6b leaves it: the same recovery/ from before the
        # save, now marked `superseded` with the show.xml that save wrote. No
        # offer, to the mirror or to the engine - which before H6b adopted it
        # and took the show back past the save.
        marked = copy("superseded")
        (marked / SHOW).write_bytes(template.bytes_for(late))
        (marked / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(early))
        (marked / RECOVERY_DIR / SUPERSEDED).write_bytes(fingerprint(template.bytes_for(late)))
        finding = inspect(marked, template, {late}, {early, late}, owner=owner)

        report.check(finding.offer is None and not finding.problems,
                     "control: inspect() offers no folder whose superseded mark names show.xml - "
                     "and passes it as rightly hidden, outgrown by a save of its own session",
                     "; ".join(finding.problems) or f"it offers {finding.offer_name()}")
        self.nothing_to_recover(marked, "a recovery/ a landed save superseded")

        # The same folder carried by a later save (fixer of H6b): a landed save
        # outgrew it and nothing could delete it, so the save after added its
        # own line. Hidden by whichever line names show.xml, here the second -
        # to the mirror, and to the engine.
        lines = copy("superseded-lines")
        (lines / SHOW).write_bytes(template.bytes_for(late))
        (lines / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(early))
        (lines / RECOVERY_DIR / SUPERSEDED).write_bytes(
            fingerprint(template.bytes_for(marker(99, 6))) + fingerprint(template.bytes_for(late)))
        finding = inspect(lines, template, {late}, {early, late}, owner=owner)

        report.check(finding.offer is None and not finding.problems,
                     "control: inspect() offers no folder whose mark names show.xml on its second "
                     "line",
                     "; ".join(finding.problems) or f"it offers {finding.offer_name()}")
        self.nothing_to_recover(lines, "a recovery/ whose mark names show.xml on its second line")

        # And the twin: a mark naming any other show - its save never landed -
        # hides nothing, so the mirror's comparison is not an "any mark" rule.
        # In a copy of its own, so that nothing the engine started on `marked`
        # did can matter, and asked of the engine too: `--recover` adopts it.
        twin = copy("superseded-twin")
        (twin / SHOW).write_bytes(template.bytes_for(late))
        (twin / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(early))
        (twin / RECOVERY_DIR / SUPERSEDED).write_bytes(fingerprint(template.bytes_for(early)))
        finding = inspect(twin, template, {late}, {early, late})

        report.check(finding.offer == twin / RECOVERY_DIR,
                     "control: but one whose mark names another show is offered",
                     f"it offers {finding.offer_name()}")
        self.adopted_at_start(twin, "a recovery/ whose mark names another show", early)

        # A HIDDEN OFFER (fixer of H6b): a mark no landed save could have left,
        # which the engine and the mirror both honour, so only this check sees
        # it. Over work NEWER than the show.xml the mark names - the afternoon
        # after the save, hidden as if the save had outgrown it...
        hidden = copy("hidden-later")
        (hidden / SHOW).write_bytes(template.bytes_for(early))
        (hidden / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(late))
        (hidden / RECOVERY_DIR / SUPERSEDED).write_bytes(fingerprint(template.bytes_for(early)))
        finding = inspect(hidden, template, {early}, {early, late}, owner=owner)

        report.check("hidden-offer" in finding.kinds and finding.offer is None
                     and not finding.fatal,
                     "control: inspect() flags a folder hidden by a mark over work newer than the "
                     "show.xml it names",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # ... and over an earlier session's unanswered offer, which no save of a
        # later session says anything about (§14.10).
        hidden = copy("hidden-theirs")
        (hidden / SHOW).write_bytes(template.bytes_for(late))
        (hidden / RECOVERY_DIR / SHOW).write_bytes(template.bytes_for(theirs))
        (hidden / RECOVERY_DIR / SUPERSEDED).write_bytes(fingerprint(template.bytes_for(late)))
        finding = inspect(hidden, template, {late}, {early, late, theirs}, owner=owner)

        report.check("hidden-offer" in finding.kinds and finding.offer is None,
                     "control: and one hidden over an earlier session's unanswered offer",
                     "; ".join(finding.problems) or "it found nothing wrong")

        # And the real folder, temps planted, has nothing wrong with it.
        finding = inspect(self.bundle, template, {CALIBRATION}, {CALIBRATION})

        report.check(not finding.problems and len(finding.other_temps) == 2,
                     "control: the real folder, with two whole temps planted as process 1, "
                     "inspects clean",
                     "; ".join(finding.problems) or f"temps: {finding.other_temps}")

        return planted

    def nothing_to_recover(self, bundle: Path, what: str) -> None:
        """A `--recover` start on a folder in which the engine must offer
        nothing: it starts rather than exiting 2, announces nothing, and says
        it has nothing to recover."""
        try:
            server = Server(bundle, locale=self.locale, recover=True)
        except HarnessError as problem:
            self.report.check(False, f"control: serve --recover starts on {what}, with nothing "
                                     f"to recover", str(problem).strip())
            return

        with dying(server):
            told = [line for line in server.notices if "recovery available" in line]

            self.report.check("wfg: nothing to recover" in server.notices and not told,
                              f"control: serve --recover starts on {what}, announces nothing and "
                              f"has nothing to recover",
                              " | ".join(server.notices))

    def adopted_at_start(self, bundle: Path, what: str, held: str) -> None:
        """A `--recover` start on a folder whose offer holds `held`: it starts
        rather than exiting 2, says it adopted the offer, and publishes it."""
        try:
            server = Server(bundle, locale=self.locale, recover=True)
        except HarnessError as problem:
            self.report.check(False, f"control: serve --recover starts on {what}, and adopts it",
                              str(problem).strip())
            return

        with dying(server):
            self.report.check("wfg: recovery adopted" in server.notices,
                              f"control: serve --recover adopts {what}",
                              " | ".join(server.notices))
            self.report.check(doc.reads(server, NAME, held),
                              f"control: and publishes {held}, the offer's",
                              f"the name reads {doc.value_of(server, NAME)!r}")

    # --- 2 ------------------------------------------------------------------
    def kill_loop(self) -> None:
        report = self.report
        rng = random.Random(self.seed)

        #  THE WHOLE PLAN IS DRAWN FIRST, so that kill seven has the same delay
        #  and the same kind of start on every run: drawn as the loop ran, the
        #  number of steps a stream took - which is the machine's - would shift
        #  every later draw. What a kill lands in is still the machine's, since
        #  the same delay finds the writer somewhere else each time; a red is
        #  known by the disk state the run prints, not by its kill number.
        plan = []

        for k in range(1, self.kills + 1):
            delay = rng.uniform(0.0, KILL_WINDOW)
            leave = rng.random() < LEAVE_OFFER
            plan.append((k, delay, leave and k != 1))

        print(f"\n--- the plan, from seed {self.seed} ---")

        for k, delay, leave in plan:
            print(f"  kill {k:02d}: {delay:.3f} s after arming; the victim starts "
                  + ("plain, and leaves any offer standing" if leave else "with --recover"))

        finding = inspect(self.bundle, self.template, {CALIBRATION}, self.allowed_any())

        for k, delay, leave in plan:
            label = f"kill {k:02d}"
            print(f"\n--- {label}: {delay:.3f} s after arming, "
                  f"{'plain' if leave else '--recover'}; on the disk: {describe(finding)} ---")

            # a. The --recover start this kill's victim will not make.
            if leave:
                server = start(report, f"{label} --recover", self.bundle,
                               log=self.room / f"recover-{k:02d}.wfglog",
                               locale=self.locale, recover=True)

                if server is None:
                    self.stopped_at = k
                    return

                with dying(server):
                    recovered_at_start(report, f"{label} --recover", server, finding)

                finding = self.look(f"{label} after --recover", {finding.show_marker}, known=finding)

                if not finding.new_problems:
                    report.check(True, f"{label}: the --recover session wrote nothing it should "
                                       f"not have")

                if finding.fatal:
                    self.stopped_at = k
                    return

            # b. The victim.
            before = temps_of(self.bundle)
            recover = not leave
            adopted = finding.offer_marker if recover and finding.offer is not None else None
            standing = finding.offer_marker if leave and finding.offer is not None else None
            session = Session(adopted, finding.offer.name if adopted else None)
            at_start = finding.show_marker
            victim_log = self.room / f"victim-{k:02d}.wfglog"

            victim = start(report, f"{label} victim", self.bundle, log=victim_log,
                           locale=self.locale, recover=recover)

            if victim is None:
                self.stopped_at = k
                return

            sent: "list[str]" = []

            with dying(victim):
                pid = victim.process.pid

                if recover:
                    recovered_at_start(report, f"{label} victim", victim, finding)
                else:
                    opened_plain(report, f"{label} victim", victim, finding)

                if k == 1:
                    report.check("wfg: nothing to recover" in victim.notices
                                 and doc.value_of(victim, NAME) == CALIBRATION,
                                 f"{label}: control: the temps planted as process 1, both whole "
                                 f"and holding {PLANTED}, are never read: nothing to recover, "
                                 f"and the show is {CALIBRATION}",
                                 f"notices {victim.notices}; the name reads "
                                 f"{doc.value_of(victim, NAME)!r}")

                stream = stream_until_kill(report, label, victim, victim_log, k, delay, sent)

            self.sent_ever.update(sent)
            session.states.extend(sent)
            self.owner.update((name, session) for name in sent)

            # d. The disk as the kill left it. An adopted offer that was STALE
            #    is not a state this show.xml may hold: it is older than the
            #    show.xml the victim started on, and saving it would make the
            #    loss the stale offer threatened a loss for good.
            allowed = {at_start, *sent} | ({adopted} if adopted and not finding.stale else set())
            after = self.look(label, allowed, known=finding, victim_pid=pid, before=before)

            if not after.problems:
                report.check(True, f"{label}: every file is one of the states written - "
                                   f"{describe(after)}")

            #    AND THE AFTERNOON A PLAIN VICTIM LEFT STANDING IS STILL ON OFFER
            #    (fixer of H6b). An earlier session's unanswered offer is left
            #    standing by a save on purpose (§14.10) and moved aside by the
            #    first autosave, never deleted and never marked. The hidden-offer
            #    check sees a mark on it only while the mark still matches: the
            #    next save that finds a folder superseded deletes it, so an
            #    engine that marked the offer loses the afternoon outright -
            #    which only asking for it by name can see. Seen so on 2026-09-30:
            #    a build whose save marked the offer passed every other check.
            if standing is not None:
                kept = [f"{folder.name}/" for folder in recovery_folders(self.bundle)
                        if holds_offer(self.bundle, folder)
                        and self.template.marker_of(read_settled(folder / SHOW)) == standing]

                report.check(bool(kept),
                             f"{label}: the afternoon it left standing, {standing}, is still on "
                             f"offer ({', '.join(kept) or 'nowhere'})",
                             f"no recovery folder offers {standing} any more: an earlier session's "
                             f"unanswered afternoon, gone or hidden by this victim - an engine "
                             f"finding, not a flake")

            self.rows.append({"k": k, "delay": delay, "mode": "plain" if leave else "recover",
                              "pid": pid, **stream, "temps": dict(after.victim_temps),
                              "stranded": dict(after.new_stranded),
                              "displaced": list(after.displaced),
                              "show": after.show_marker, "offer": after.offer_name(),
                              "offer_marker": after.offer_marker,
                              "stale": "stale-offer" in after.new_kinds,
                              "own_offer": after.offer is not None
                                           and after.offer.name == RECOVERY_DIR
                                           and after.offer_marker in sent})

            if after.fatal:
                #  THE RESTART THE PLAN ASKS FOR, made once more on a folder the
                #  inspection has already failed, so that the report says what
                #  an operator would meet - a show that will not open, and the
                #  engine's own sentence why - and not only what the disk
                #  holds. Then the loop stops: every later expectation would be
                #  built on a folder nobody can vouch for.
                probe = start(report, f"{label} check", self.bundle,
                              log=self.room / f"check-{k:02d}.wfglog", locale=self.locale)

                if probe is not None:
                    with dying(probe):
                        report.check(True, f"{label} check: serve still opens the folder the "
                                           f"kill left, whatever the inspection found")

                self.stopped_at = k
                return

            # e. A plain start on it, and document.recover.
            check_log = self.room / f"check-{k:02d}.wfglog"
            checker = start(report, f"{label} check", self.bundle, log=check_log,
                            locale=self.locale)

            if checker is None:
                self.stopped_at = k
                return

            with dying(checker):
                opened_plain(report, f"{label} check", checker, after)
                recover_or_refuse(report, f"{label} check", checker, check_log, after)

            # f. What the check left, which is what the next start will find.
            finding = self.look(f"{label} after the check", {after.show_marker}, known=after)

            if not finding.new_problems:
                report.check(True, f"{label}: the check session wrote nothing it should not have")

            if finding.fatal:
                self.stopped_at = k
                return

        self.finding = finding

    # --- 3 ------------------------------------------------------------------
    def final(self, planted: bytes) -> None:
        """A save after all the kills, and a reopen that reads it back - also the
        `--recover` claim for the last kill."""
        report = self.report
        finding = self.finding
        print(f"\n--- final: --recover, a save, and a reopen; on the disk: {describe(finding)} ---")

        server = start(report, "final", self.bundle, log=self.room / "final.wfglog",
                       locale=self.locale, recover=True)

        if server is None:
            return

        with dying(server):
            recovered_at_start(report, "final", server, finding)

            doc.write(server, NAME, FINAL)
            report.check(doc.reads(server, NAME, FINAL), f"final: the cue is renamed {FINAL}",
                         f"the name reads {doc.value_of(server, NAME)!r}")

            landed, why = save_until_clean(server)
            report.check(landed, "final: a save lands after every kill, and the dot goes out", why)

            #  A stat, which is safe with the engine alive: only a read would
            #  hold the file against the writer's own delete.
            gone = common.wait_until(lambda: not (self.bundle / RECOVERY_DIR / SHOW).exists())
            report.check(bool(gone), "final: and takes recovery/show.xml with it",
                         f"{RECOVERY_DIR}/{SHOW} is still there")

        #  The final session is a session like the victims, so an offer its save
        #  should have retired and did not is judged the same way.
        adopted = finding.offer_marker if finding.offer is not None else None
        session = Session(adopted, finding.offer.name if adopted else None)
        session.states.append(FINAL)
        self.owner[FINAL] = session
        self.sent_ever.add(FINAL)

        after = self.look("final", {FINAL}, known=finding)
        report.equal(after.show_marker, FINAL, "final: show.xml holds the save")

        try:
            reopened = Server(self.bundle, log=self.room / "reopen.wfglog", locale=self.locale)
        except HarnessError as problem:
            report.check(False, "reopen: serve starts on the saved folder", str(problem).strip())
            return

        #  THE ONE SESSION ENDED WITH `stop()`: nothing is looked at after it.
        with reopened:
            announced(report, "reopen", reopened, after)
            report.check(doc.reads(reopened, doc.RECOVERY, after.offer is not None),
                         f"reopen: /godot/document/recovery reads {after.offer is not None}, as the "
                         f"disk says", f"it reads {doc.value_of(reopened, doc.RECOVERY)!r}")
            report.check(doc.reads(reopened, NAME, FINAL), f"reopen: the show opens on the save, {FINAL}",
                         f"the name reads {doc.value_of(reopened, NAME)!r}")
            report.check(doc.reads(reopened, doc.DIRTY, False), "reopen: with the dot out",
                         f"{doc.DIRTY} reads {doc.value_of(reopened, doc.DIRTY)!r}")

        report.check(read_settled(self.bundle / f"{SHOW}.tmp-{PLANTED_PID}") == planted,
                     "control: the temp planted as process 1 is byte for byte as planted: "
                     "no engine ever wrote a temp that was not its own",
                     f"{SHOW}.tmp-{PLANTED_PID} changed")

    # --- 4 ------------------------------------------------------------------
    def summarise(self) -> None:
        report = self.report
        print("\n--- what each kill found ---")
        print("  kill  delay  mode     pid     steps  sets  saves  autos  show.xml        "
              "offer after the kill              temps the victim left")

        for row in self.rows:
            temps = ", ".join(f"{key} {state}" for key, state
                              in list(row["temps"].items()) + list(row["stranded"].items())) or "-"
            offer = row["offer"] + (f" {row['offer_marker']}" if row["offer_marker"] else "") \
                + (" STALE" if row["stale"] else "")
            print(f"  {row['k']:>4}  {row['delay']:.3f}  {row['mode']:<7}  {row['pid']:<6}  "
                  f"{row['steps']:>5}  {row['sets']:>4}  {row['saves']:>5}  {row['autosaves']:>5}  "
                  f"{row['show'] or '-':<14}  {offer:<32}  {temps}")

        left = [row for row in self.rows if row["temps"]]
        states = [state.split(" ")[0] for row in self.rows for state in row["temps"].values()]
        in_recovery = sum(1 for row in self.rows if row["offer"] == RECOVERY_DIR + "/")
        in_previous = sum(1 for row in self.rows if row["offer"].startswith("recovery.previous."))

        replaced = [row for row in self.rows if row["stranded"]]
        swaps = [state.split(" ")[0] for row in replaced for state in row["stranded"].values()]

        #  INSIDE A WRITE is either: a temp of the victim's own on the disk, or
        #  one of ReplaceFile's - which can be the only trace, when the kill came
        #  after the new file had taken its name and before the old was deleted.
        inside = [row for row in self.rows if row["temps"] or row["stranded"]]

        print(f"\n  {len(left)} of {len(self.rows)} kills left a temp of the victim's "
              f"({states.count('whole')} whole, {states.count('partial')} partial, "
              f"{states.count('empty')} empty); offers left in recovery/: {in_recovery}, "
              f"in recovery.previous.N/: {in_previous}, "
              f"none: {len(self.rows) - in_recovery - in_previous}")
        print(f"  {len(replaced)} of {len(self.rows)} kills landed inside ReplaceFile itself and "
              f"stranded its <file>~RF<hex>.TMP ({swaps.count('whole')} whole, "
              f"{swaps.count('partial')} partial, {swaps.count('empty')} empty)")

        displaced = [f"kill {row['k']:02d}: {', '.join(row['displaced'])}"
                     for row in self.rows if row["displaced"]]
        print(f"  {len(displaced)} of {len(self.rows)} kills left a file under no name, between "
              f"ReplaceFile's two moves" + (f" - {'; '.join(displaced)}" if displaced else ""))

        stale = [f"{row['k']:02d}" for row in self.rows if row["stale"]]
        print(f"  {len(stale)} of {len(self.rows)} kills left on offer an autosave older than the "
              f"show its own session had saved" + (f" - kills {', '.join(stale)}" if stale else ""))

        autosaves = sum(row["autosaves"] for row in self.rows)
        own = [row for row in self.rows if row["own_offer"]]
        print(f"  {autosaves} client autosaves applied in all; {len(own)} of {len(self.rows)} kills "
              f"left an offer in recovery/ that the victim itself had written")

        if self.stopped_at is not None:
            print(f"  the loop stopped at kill {self.stopped_at:02d}")

        if inside:
            report.check(True, f"{len(inside)} of {len(self.rows)} kills landed inside a write: a "
                               f"temp of the victim's own, or of its ReplaceFile, was on the disk "
                               f"when it died")
        else:
            report.void("a kill landed inside a write",
                        f"none of the {len(self.rows)} kills left a temp of the victim's, so every "
                        f"one landed between writes; the files held, but this run is no evidence "
                        f"about a write interrupted")

        #  THE RECOVERY HALF HAD SOMETHING TO TEST. Every check on `recovery/`
        #  passes on a folder nobody ever wrote: were client autosaves refused
        #  (an origin gate on the command nobody asks for), saves alone would
        #  still arm the stream, and the run would end green having killed no
        #  recovery write at all. So both halves of that are asked for, once
        #  the loop has run its course - a loop stopped early is red already.
        if self.stopped_at is None:
            report.check(autosaves > 0,
                         f"the victims applied {autosaves} client autosaves: recovery/ was written "
                         f"while the kills came",
                         "no applied document.autosave in any victim's log - were they refused? "
                         "Without them every recovery check in this run met an empty folder")
            report.check(bool(own),
                         f"{len(own)} of {len(self.rows)} kills left an offer in recovery/ that "
                         f"the victim itself had written, so the recovery checks met real offers",
                         "no kill left an offer of its own victim's in recovery/: every recovery "
                         "check passed on something the stream did not write")


def run(locale: "str | None", kills: int, seed: int) -> int:
    report = Report(f"H6: a save killed halfway through a write ({locale or 'C'})")

    if not FIXTURE.is_dir():
        raise HarnessError(f"no fixture bundle at {FIXTURE}")

    began = time.monotonic()

    #  ignore_cleanup_errors: a scanner still holding a fresh file when the
    #  folder is removed must not turn a finished report into a traceback.
    with tempfile.TemporaryDirectory(prefix="wfg-crash-write-", ignore_cleanup_errors=True) as scratch:
        story = Run(report, Path(scratch), locale, kills, seed)

        if story.calibrate():
            planted = story.controls()
            story.kill_loop()

            if story.stopped_at is None:
                story.final(planted)
            else:
                print(f"\n--- final: not attempted, the loop stopped at kill {story.stopped_at:02d} ---")

            story.summarise()

    print(f"\n  ran in {time.monotonic() - began:.1f} s")
    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"crash_during_write: {problem}", file=sys.stderr)
        return 2

    locale = None
    kills = KILLS
    seed = SEED

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
        elif argument.startswith("--kills="):
            kills = int(argument.split("=", 1)[1])
        elif argument.startswith("--seed="):
            seed = int(argument.split("=", 1)[1])

    try:
        return run(locale, kills, seed)
    except HarnessError as problem:
        print(f"crash_during_write: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
