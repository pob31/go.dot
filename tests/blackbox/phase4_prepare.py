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
"""Phase 4's done-when, as a program: rehearsal-grade behaviour, headless.

WHAT THE PHASE PROMISED, in the devplan's own words: *load-to-time into the
middle of a scene lands the right cues at the right offsets with the right
slots claimed; reordering cues produces the right overlap warnings; a claim on
a busy slot waits and says so; all headless.* Everything below is one of those
sentences, asked of the shipped binary over UDP and HTTP by a program that
shares no code with it.

THE SHOW. One scene group holding a bed that feeds a processor input and takes
an insert on a rack channel; a preset osc cue prepared by the scene's header; a
nested timeline group of a RAMP and a constant beside it; a footer that releases
the input. Then a cue after the scene that feeds the SAME processor input, so a
claim has somebody to wait for, and a fade. A persistent section holding a desk
value. A second list with a trigger.

TWO MEDIA FILES, and each is a different kind of evidence. `segments.wav` is
three constants, so the render says WHICH cue is sounding. `ramp.wav` is a ramp
whose sample value is its own position, so the render says WHERE in the file a
cue started - which is the only honest way to check a jump landed at an offset.

WHAT IS DELIBERATELY NOT ASSERTED HERE: wall-clock timing of anything. Audio
time is not wall time on a loaded runner (`phase3_groups.py` says why at
length), so what is timed is timed against the log's own ticks or the render's
own samples, and everything else waits for a state rather than for a duration.
"""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "phase4"

RATE = 48000
BLOCK = 64

#  The same three constants phase 3 uses, and for the same reason: no two of
#  them and no sum of two of them is equal to any other, so a channel carrying
#  0.25 is the bed and one carrying 0.3125 is the bed plus the -12 dB cue.
SEGMENTS = (0.25, 0.5, 0.75)
TOLERANCE = 0.01

#  The ramp's own arithmetic. Eight seconds rising to 0.9, so a sample IS a
#  position: value / SLOPE is the second it was taken at, and a jump that
#  landed at four seconds is a first sample of about 0.45.
RAMP_SECONDS = 8.0
RAMP_PEAK = 0.9
SLOPE = RAMP_PEAK / RAMP_SECONDS

LIST = "P4ACT001"
FOYER = "P4FYR001"
SCENE = "P4GRP001"
BED = "P4MED001"
PRESET = "P4MSG001"
INNER = "P4GRP002"
RAMP = "P4RMP001"
BESIDE = "P4MED002"
FOOTER_CUE = "P4MSG002"
AFTER = "P4MED003"
FADE = "P4FAD001"
HELD = "P4MSG003"
DOORS = "P4MEM001"
SLOT = "P4SXA001"
CHANNEL = "P4CHN001"

DESK_FADER = "/desk/fader"
DESK_SCENE = "/desk/scene"


# =============================================================================
# The media
# =============================================================================

def write_segments(path: Path) -> None:
    """Six seconds: three two-second constants, so the render says WHICH."""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = int(RATE * 2)
    body = b""

    for level in SEGMENTS:
        sample = int(level * 32767)
        body += struct.pack("<hh", sample, sample) * frames

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(body)


def write_ramp(path: Path) -> None:
    """Eight seconds rising from nought, so the render says WHERE.

    A jump into the middle of a file is the one thing a constant cannot
    measure: every sample of a constant is the same sample. Here the value at
    the first frame after the launch IS the offset the cue started at, to
    whatever the sample rate resolves - which is how "landed at four seconds"
    stops being a claim about a clock and becomes a reading off the file.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = int(RATE * RAMP_SECONDS)
    body = bytearray()

    for n in range(frames):
        sample = int((n / frames) * RAMP_PEAK * 32767)
        body += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(body))


# =============================================================================
# Reading the engine
# =============================================================================

def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return json.loads(body)["VALUE"][0]
    except (KeyError, IndexError, ValueError):
        return None


def wait_for(server: Server, address: str, wanted, seconds: float = 25.0):
    """Polls until a node reads `wanted`; answers with what it last read."""
    deadline = time.monotonic() + seconds
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == wanted:
            return actual

        time.sleep(0.02)

    return actual


def spelled_position(text: str) -> "tuple[str, float]":
    """`"<cue> <offset>"` back into its two halves, or ("", -1).

    READ RATHER THAN COMPARED AS TEXT, because the offset is a number and this
    suite runs under `fr_FR`: `spellAim` writes it through the project's own
    formatter, and a driver that compared the string would be asserting which
    decimal separator the engine chose rather than where the finger is.
    """
    cue, _, offset = str(text or "").partition(" ")

    try:
        return cue, float(offset.replace(",", "."))
    except ValueError:
        return cue, -1.0


def aim_of(server: Server) -> "tuple[str, float]":
    return spelled_position(value_of(server, f"/godot/list/{LIST}/aim"))


def position_of(server: Server) -> "tuple[str, float]":
    return spelled_position(value_of(server, f"/godot/list/{LIST}/statePosition"))


def runs_in(server: Server) -> "list[str]":
    return (value_of(server, "/godot/run/order") or "").split()


def fresh_run_for_cue(server: Server, cue: str, known: "set[str]",
                      seconds: float = 25.0) -> str:
    """A run of this cue that did not exist a moment ago.

    A finished run is published for a few seconds after it ends, so a cue that
    has already sounded once has a run to find - and asking "the run of the
    ramp" just after a jump can answer with the one the scene made. What a jump
    is asked about is the run the JUMP made, so the ones that were already there
    are named and excluded.
    """
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        for run in runs_in(server):
            if run not in known and value_of(server, f"/godot/run/{run}/cue") == cue:
                return run

        time.sleep(0.02)

    return ""


def run_for_cue(server: Server, cue: str, seconds: float = 25.0) -> str:
    """The LATEST run of a cue, or "". A cue fired twice has two."""
    deadline = time.monotonic() + seconds
    found = ""

    while time.monotonic() < deadline:
        for run in runs_in(server):
            if value_of(server, f"/godot/run/{run}/cue") == cue:
                found = run

        if found:
            return found

        time.sleep(0.02)

    return found


def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def go(server: Server) -> None:
    command(server, "go")


def park(server: Server, cue: str) -> None:
    command(server, "standby.set", [cue])


def wait_ticks(server: Server, count: int, seconds: float = 120.0) -> bool:
    """Waits `count` engine ticks. Audio time, not wall time."""
    start = value_of(server, "/godot/engine/tick")

    if not isinstance(start, int):
        return False

    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        now = value_of(server, "/godot/engine/tick")

        if isinstance(now, int) and now - start >= count:
            return True

        time.sleep(0.02)

    return False


# =============================================================================
# Reading the desk
# =============================================================================

def desk_value(target, address: str):
    """What the mock says it holds, or None for "nothing to say"."""
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{target.query_port}{address}?VALUE", timeout=4) as answer:
            if answer.status != 200:
                return None

            return json.loads(answer.read().decode("utf-8"))["VALUE"][0]
    except (urllib.error.URLError, OSError, ValueError, KeyError, IndexError):
        return None


def desk_received(target) -> int:
    """How many datagrams the mock has taken, ever."""
    try:
        with urllib.request.urlopen(
                f"http://127.0.0.1:{target.query_port}/_mock/received", timeout=4) as answer:
            return int(json.loads(answer.read().decode("utf-8"))["VALUE"][0])
    except (urllib.error.URLError, OSError, ValueError, KeyError, IndexError):
        return -1


def tweak_desk(target, address: str, value: float) -> bool:
    """A hand on the desk: a datagram to the device itself, not to Go.dot.

    Nothing new is needed for this - a person at a lighting desk moves a fader
    and the desk knows; here a driver sends the device an OSC message and the
    device knows. What matters is that GO.DOT DOES NOT, until it asks.

    WAITED FOR AT THE DESK, because a datagram is not an arrival: the device
    reads its socket on a thread of its own, and a driver that moved a fader and
    asked in the same breath is asking whether the operating system was quick,
    not whether the desk took it. The hand is down when the desk says so.
    """
    common.send_udp(target.osc_port, common.osc_encode(address, [float(value)]))

    return bool(common.wait_until(
        lambda: abs((desk_value(target, address) or -1.0e9) - value) < 0.001, timeout=10.0))


# =============================================================================
# Reading the render
# =============================================================================

def segment_of(value: float) -> int:
    for index, level in enumerate(SEGMENTS):
        if abs(abs(value) - level) < TOLERANCE:
            return index

    return -1


def first_above(samples: "list[float]", floor: float) -> int:
    for n, value in enumerate(samples):
        if abs(value) > floor:
            return n

    return -1


def loud_starts(samples: "list[float]", floor: float = 0.3) -> "list[int]":
    """Every frame where silence is followed at once by a loud sample.

    Nothing that started at the top of this ramp can do that, because the top of
    it is nought - so each of these is a cue that began PARTWAY IN, which is
    what a jump to an offset sounds like.

    ALL OF THEM AND NOT THE LAST, because the render's writer drops a block
    rather than blocking when the disk is busy (`HostedAudioDriver`'s FIFO says
    so in as many words), and a dropped block is a hole: silence, then the ramp
    again at whatever it had reached. On a loaded runner that hole looks exactly
    like a launch and lands after the real one. So the reading below asks
    whether the jump's offset is among these rather than whether it is the last,
    which is the honest question - a dropped block is the recorder's accident
    and says nothing about where the cue started.
    """
    out = []

    for n in range(1, len(samples) - 1):
        if abs(samples[n - 1]) < 0.005 and abs(samples[n]) > floor:
            out.append(n)

    return out


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 4: prepare, claims and the jump ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase4-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "phase4")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_segments(bundle / "media" / "segments.wav")
        write_ramp(bundle / "media" / "ramp.wav")

        with first_sound.MockTarget("agree") as target:
            first_sound.point_mount_at(bundle, target)

            #  THE DESK ALREADY HOLDS SOMETHING, which is what a desk in a room
            #  does and what anticipation needs: a value can only be pre-sent
            #  if there is something to put back. A processor with nothing to
            #  say cannot be prepared, and §13.6 says so - which is a different
            #  test from this one.
            tweak_desk(target, DESK_FADER, 0.20)

            #  AND IT ALREADY HOLDS WHAT THE PERSISTENT SECTION SAYS IT SHOULD.
            #  The section is checked after every applied trigger, so a desk
            #  that disagreed with it would be corrected on the first GO - which
            #  is right, and would make "the GO sent nothing" a check about the
            #  section rather than about anticipation. Set here, the assertion
            #  finds it agreeing and leaves it alone, which is the other half of
            #  §3.29 and is asserted further down by moving it and watching it
            #  come back.
            tweak_desk(target, DESK_SCENE, 7)

            with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                        buffer_size=BLOCK, hosted=True, render=render) as server:

                report.equal(wait_for(server, "/godot/audio/status", "running"),
                             "running", "the audio side comes up running")

                # --- what the show declares --------------------------------
                report.equal(value_of(server, f"/godot/slot/{SLOT}/kind"), "processorInput",
                             "the mount's slot is published as a processor input")
                report.equal(value_of(server, f"/godot/slot/{CHANNEL}/kind"), "rackChannel",
                             "and the rack's channel as a rack channel")
                report.equal(value_of(server, f"/godot/list/{LIST}/persistentOrder"), HELD,
                             "the persistent section is published, and holds one cue")
                report.equal(value_of(server, f"/godot/cue/{HELD}/role"), "persistent",
                             "whose role says where it sits")
                report.equal(value_of(server, f"/godot/cue/{SCENE}/headerDerived"), PRESET,
                             "the preset member is derived into the scene's header",
                             "nothing is written into the Header element")

                # --- the horizon -------------------------------------------
                #  PARKED, AND THEN THE DESK IS READ. Everything about the
                #  prepare happens before any GO: that is the whole of §3.12.
                park(server, SCENE)

                report.equal(wait_for(server, f"/godot/cue/{SCENE}/prepare", "verified", 30.0),
                             "verified",
                             "the pointer landing on the scene prepares it, to verified")

                report.check(abs((desk_value(target, DESK_FADER) or 0.0) - 0.75) < 0.001,
                             "and the desk has the header's value BEFORE any GO",
                             f"the desk holds {desk_value(target, DESK_FADER)!r}")

                bed = run_for_cue(server, BED)
                report.check(bool(bed), "the bed is armed under the prepared scene")

                if bed:
                    report.equal(value_of(server, f"/godot/run/{bed}/state"), "armed",
                                 "armed rather than playing: nothing sounds before GO")
                    report.equal(value_of(server, f"/godot/run/{bed}/claims"),
                                 f"{SLOT} {CHANNEL}",
                                 "holding the processor input and the rack channel")
                    report.equal(value_of(server, f"/godot/slot/{SLOT}/holder"), bed,
                                 "and the slot says who holds it")

                # --- and its rollback --------------------------------------
                #  The pointer moves to a cue outside the scene. Everything the
                #  horizon was holding comes back: the desk's own value, the
                #  voice, and the slot - and the restore is a `node.set` like
                #  any other write, so it takes a tick and is waited for rather
                #  than read on the way past.
                park(server, AFTER)

                restored = common.wait_until(
                    lambda: abs((desk_value(target, DESK_FADER) or 0.0) - 0.20) < 0.001,
                    timeout=25.0)

                report.check(bool(restored),
                             "moving the pointer away puts the desk back where it was",
                             f"the desk holds {desk_value(target, DESK_FADER)!r}")

                if bed:
                    report.equal(wait_for(server, f"/godot/run/{bed}/warning", "revoked"),
                                 "revoked", "and the prepared run says it was revoked")
                    report.equal(value_of(server, f"/godot/run/{bed}/state"), "done",
                                 "the run is over, which is the whole of freeing its voice")

                    #  AND ITS SLOT IS SOMEBODY ELSE'S NOW, which is a stronger
                    #  reading than "released": parking on the cue after the
                    #  scene armed THAT cue, whose Feed wants the same slot, and
                    #  a released slot goes to the head of the queue in the same
                    #  breath. An empty holder would mean nobody wanted it.
                    waiting_now = run_for_cue(server, AFTER)

                    report.equal(wait_for(server, f"/godot/slot/{SLOT}/holder", waiting_now),
                                 waiting_now,
                                 "and its slot passes to the cue that was waiting for it")

                # --- GO: the scene runs ------------------------------------
                #  Back to the scene. The cue that was armed at the pointer lets
                #  go of the slot as the pointer leaves it, so the scene's own
                #  bed can claim it again - which is the same revocation read
                #  from the other side.
                park(server, SCENE)
                report.equal(wait_for(server, f"/godot/cue/{SCENE}/prepare", "verified", 30.0),
                             "verified", "coming back prepares it again")

                sent_before_go = desk_received(target)
                go(server)

                bed = run_for_cue(server, BED)
                report.equal(wait_for(server, f"/godot/run/{bed}/state", "playing"),
                             "playing", "GO starts the prepared bed")

                #  AND THE GO DOES NOT RUN THE PRESET CUE AGAIN. The horizon
                #  read the desk, wrote the value and watched the desk agree;
                #  the cue's own row has nothing left to do, and a second run of
                #  it at entry would be the anticipation undone. Asked of the
                #  log rather than of a node, because the run it would make
                #  might come and go between two polls: nothing the GO wrote
                #  down may name that cue at all.
                after_go = len(log.read_text(encoding="utf-8").splitlines())
                wait_ticks(server, 25)

                since = log.read_text(encoding="utf-8").splitlines()[after_go:]
                named = [line for line in since if PRESET in line]

                report.check(not named,
                             "the GO did not run the prepared cue again at its own row",
                             "\n".join(named))

                report.check(desk_received(target) == sent_before_go,
                             "and GO sent the desk nothing it had already been sent",
                             f"{desk_received(target) - sent_before_go} datagrams on the GO")

                # --- a claim on a busy slot --------------------------------
                #  Fired by name while the scene runs: its Feed wants the slot
                #  the bed is holding, so it waits - in words, with no sound.
                command(server, "cue.fire", [AFTER])
                waiting = run_for_cue(server, AFTER)

                if report.check(bool(waiting), "the cue after the scene has a run of its own"):
                    report.equal(wait_for(server, f"/godot/run/{waiting}/pending", SLOT),
                                 SLOT, "which is PENDING on the slot the bed holds")
                    report.check(value_of(server, f"/godot/run/{waiting}/state") != "playing",
                                 "and is not playing while it waits",
                                 f"state is {value_of(server, f'/godot/run/{waiting}/state')!r}")
                    report.equal(value_of(server, f"/godot/slot/{SLOT}/pending"), waiting,
                                 "the slot says who is waiting for it")

                # --- the overlap, at edit time ------------------------------
                #  THE ORDER OF THE ROWS IS THE ANSWER, and this is the whole
                #  of §3.9c in one gesture. Written as it is, the two cues that
                #  feed the same processor input cannot overlap: the scene
                #  releases the input when it ends and the other cue is after
                #  it. Move that cue ABOVE the scene and they can - it is a
                #  manual row that may still be sounding when the scene starts -
                #  so the pair appears. Move it back and it goes.
                report.check(not value_of(server, f"/godot/slot/{SLOT}/overlaps"),
                             "as the show is written, nothing overlaps on the shared slot",
                             f"overlaps: {value_of(server, f'/godot/slot/{SLOT}/overlaps')!r}")

                command(server, "object.move", [AFTER, LIST, 0])

                appeared = common.wait_until(
                    lambda: value_of(server, f"/godot/slot/{SLOT}/overlaps") or None,
                    timeout=20.0)

                report.check(bool(appeared),
                             "moving it above the scene makes the pair appear",
                             f"overlaps: {appeared!r}")

                if appeared:
                    report.check(BED in str(appeared) and AFTER in str(appeared),
                                 "and the pair names both cues", str(appeared))

                warnings = value_of(server, "/godot/document/warnings") or ""
                report.check(SLOT in warnings,
                             "the document's warnings name the slot too",
                             warnings)

                command(server, "object.move", [AFTER, LIST, 1])

                gone = common.wait_until(
                    lambda: not value_of(server, f"/godot/slot/{SLOT}/overlaps"),
                    timeout=20.0)

                report.check(bool(gone),
                             "and moving it back takes the warning away",
                             f"overlaps: {value_of(server, f'/godot/slot/{SLOT}/overlaps')!r}")

                # --- the claim lands when its HOLDER ends -------------------
                #  Not when the SCENE ends: what the claim waits for is the bed,
                #  and the bed is over long before the scene it is the first
                #  member of. Asked of the thing it is actually about.
                if waiting:
                    report.equal(wait_for(server, f"/godot/run/{waiting}/state", "playing", 60.0),
                                 "playing", "the waiting cue lands once the bed lets the slot go")

                    late = value_of(server, f"/godot/run/{waiting}/late")
                    report.check(isinstance(late, int) and late > 0,
                                 "saying how late the claim made it, in ticks",
                                 f"late by {late!r}")

                    report.equal(value_of(server, f"/godot/slot/{SLOT}/holder"), waiting,
                                 "and the slot has changed hands")

                # --- and the scene reaches its own end ----------------------
                scene_run = run_for_cue(server, SCENE)

                report.equal(wait_for(server, f"/godot/run/{scene_run}/state", "done", 90.0),
                             "done", "the scene reaches its end on its own")

                # --- load to time -------------------------------------------
                #  Into the nested timeline group, four seconds into the ramp.
                #  The render is what says whether it landed: the ramp's value
                #  IS its position.
                command(server, "list.aim", [LIST, RAMP, 4.0])

                aimed = common.wait_until(
                    lambda: aim_of(server) if aim_of(server)[0] == RAMP else None, timeout=20.0)

                report.check(bool(aimed) and abs(aimed[1] - 4.0) < 0.001,
                             "the aim is published as a cue and an offset",
                             f"aim reads {value_of(server, f'/godot/list/{LIST}/aim')!r}")

                before_jump = desk_received(target)
                known_runs = set(runs_in(server))

                command(server, "list.loadToTime", [LIST])

                jumped = fresh_run_for_cue(server, RAMP, known_runs)

                if report.check(bool(jumped), "the jump created a run for the ramp"):
                    report.equal(wait_for(server, f"/godot/run/{jumped}/state", "playing", 30.0),
                                 "playing", "which is sounding")
                    landed_at = position_of(server)

                    report.check(landed_at[0] == RAMP and abs(landed_at[1] - 4.0) < 0.001,
                                 "and the state position says where the show was put",
                                 f"statePosition reads "
                                 f"{value_of(server, f'/godot/list/{LIST}/statePosition')!r}")

                report.check(desk_received(target) - before_jump <= 1,
                             "the jump sent the desk only what differed",
                             f"{desk_received(target) - before_jump} datagrams")

                #  Everything the scene had running is over: a jump does not
                #  leave two shows going.
                #  Asked as "is anything of the old scene still going", because a
                #  finished run stops being published once its retention is up
                #  and the question is about the SHOW rather than about one
                #  record: a node that has gone is a run that is over.
                left = value_of(server, f"/godot/run/{bed}/state")

                report.check(left is None or left == "done",
                             "and what the jump abandoned is ended",
                             f"the bed's run reads {left!r}")

                wait_ticks(server, 50)

                # --- the persistent section ---------------------------------
                #  A hand on the desk between two presses, and the section puts
                #  it back at the next one - and not before.
                report.check(tweak_desk(target, DESK_SCENE, 99.0),
                             "a value moved by hand on the desk is moved",
                             f"the desk holds {desk_value(target, DESK_SCENE)!r}")

                park(server, AFTER)
                go(server)

                report.check(common.wait_until(
                                 lambda: desk_value(target, DESK_SCENE) == 7, timeout=30.0)
                             is not None,
                             "and the persistent cue puts it back at the next trigger",
                             f"the desk holds {desk_value(target, DESK_SCENE)!r}")

                # --- the steps ----------------------------------------------
                history = (value_of(server, f"/godot/list/{LIST}/history") or "").split()

                report.check(len(history) >= 3,
                             "the list remembers the steps it took",
                             f"history: {history}")

                if history:
                    newest = history[0].split(":")
                    report.equal(len(newest), 3,
                                 "each spelled tick:cue:origin", history[0])
                    report.check(newest[2] in ("g", "f", "t"),
                                 "with a letter saying how it was asked for", history[0])

                # --- a trigger on the other list ----------------------------
                foyer_before = value_of(server, f"/godot/list/{FOYER}/standby")
                common.send_udp(server.osc_port, common.osc_encode("/foyer/doors"))

                report.check(bool(run_for_cue(server, DOORS, 10.0)),
                             "a datagram from the foyer fires its memo")
                report.equal(value_of(server, f"/godot/list/{FOYER}/standby"), foyer_before,
                             "and moves nobody's pointer")

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "nothing of Go.dot's allocated on the audio thread")

                wait_ticks(server, 25)
                first_sound.wait_for_render_tail(render)

        # --- what came out ---------------------------------------------------
        if report.check(render.is_file(), "the render was written"):
            channels, samples = first_sound.read_render(render)

            report.check(channels >= 6, "the render carries the rig's channels",
                         f"{channels}")

            #  THE JUMP, READ OFF THE FILE. The ramp is on the foldback bus,
            #  channel 2, and the value where it starts is the offset it
            #  started at. Four seconds into an eight-second ramp rising to
            #  0.9 is 0.45, and nothing else in this show is near it.
            if channels >= 3:
                ramp = samples[2]

                #  THE JUMP'S OWN LAUNCH, AND NOT THE SCENE'S. The ramp sounds
                #  twice in this session: once when the scene reaches it, from
                #  the top, and once when the jump puts the show four seconds
                #  in. The first rises out of silence gradually - its own first
                #  sample IS nought - and the second arrives already loud,
                #  which is exactly what "started partway in" sounds like. So
                #  the edge to look for is silence followed in one frame by a
                #  value no beginning could have.
                starts = loud_starts(ramp)

                if report.check(bool(starts),
                                "the ramp is heard starting partway into itself",
                                "no frame in the render goes from silence straight to "
                                "the middle of the file"):
                    #  Read a hundredth of a second in, so the measurement is
                    #  clear of the click suppressor's ramp rather than on it.
                    at = [abs(ramp[n + RATE // 100]) / SLOPE
                          for n in starts if n + RATE // 100 < len(ramp)]

                    report.check(any(abs(seconds - 4.0) < 0.2 for seconds in at),
                                 "and that is four seconds into its own file",
                                 "the render starts the ramp at "
                                 + ", ".join(f"{seconds:.2f} s" for seconds in at))

        # --- and it reproduces ------------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))

        report.equal(code, 0, "and `wfg replay` reproduces the session record for record",
                     (out + err).strip()[:2000])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"phase4: {problem}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"phase4: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
