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

"""A sampler bank armed by GO and played by hand, heard in the render.

WHAT THIS IS FOR. The unit suite drives the Runner with a fake player and
proves every decision a sampler group makes; `sampler.wfglog` proves those
decisions are records that replay. What neither can say is that a pad pressed
over the network MAKES A SOUND, at the level the press asked for, and that
letting go of a hold clip makes it stop - which is the whole point of a pad.
This drives the shipped binary with a real Tracktion graph under it, the way a
virtual surface or a tablet would, and reads what came out.

THE SOUND IS ARITHMETIC, as in first_sound.py: every sample of every member's
file is the same constant, so the output sample IS the gain. Rain has velocity
on and is pressed at 64, which the member's floor of -40 dB maps to exactly
-20 dB - a tenth of the constant. Thunder is a hold clip pressed with no
velocity, so it plays at unity - the constant itself - until the hand lets go.
Between them Rain's fader is pulled to the bottom, which on a play-out clip is
a mute and not a stop. So the render must read: silence, a tenth, silence,
the constant, silence - and after the bank is disarmed, digital silence.

AND A TOUCH STARTS A CLIP (author, 2026-09-23): a sample's fader waits at its
initial level, and a hand landing on it is the start. Door's fader is pulled to
the bottom first by a write with no hand on it - a level set in advance, which
starts nothing - so the touch starts it silent and the render keeps its
arithmetic.

THE FIXTURE IS tests/fixtures/bundles/sampler, which has no audio routing of
its own (its log is recorded with no device). This copies it and gives it a
bus and a route per member - a copy of a bundle is exactly the thing to change,
and the committed fixture stays the one `sampler.wfglog` was recorded from.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import re
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "sampler"

RATE = 48000
BLOCK = 64

BANK_A = "SMP00002"
DISARM_A = "SMP0000A"
BANK_B = "SMP00007"
SURFACE = "SVRF0001"

THUNDER_STRIP = "STRP0001"      # Thunder: release = hold
RAIN_STRIP = "STRP0002"         # Rain: velocity on, floor -40 dB
DOOR_STRIP = "STRP0004"         # Door: play-out, touched to start
BANK_A_STRIPS = ["STRP0001", "STRP0002", "STRP0003", "STRP0004"]

# The constant every file holds, and the two levels the render must show.
UNITY = first_sound.AMPLITUDE
TENTH = first_sound.AMPLITUDE / 10.0

# A plateau is read within this. The render is float and the tone is 16-bit,
# so a reading is exact to a few parts in a hundred thousand; this is far below
# the gap between a tenth and the constant and far above that error.
TOLERANCE = 0.002

# Quieter than this is silence while a muted clip still runs: -120 dB of the
# constant is five parts in ten million, and a trim at the bottom reads as zero
# gain anyway. After the disarm the render is held to digital silence instead.
QUIET = 1.0e-5


# =============================================================================
# The bundle
# =============================================================================

def give_it_a_bus(bundle: Path) -> None:
    """One stereo bus, and every member routed to it at unity.

    Identifiers in the fixture's own family, drawn so as not to collide with
    anything it declares: SMPB for the bus, SMPR plus the member's tail for
    each route.
    """
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")

    text = text.replace(
        '<Audio tracks="4"/>',
        '<Audio tracks="4">\n    <Bus id="SMPB0001" name="Main L/R" width="2"/>\n  </Audio>')

    def routed(match: "re.Match[str]") -> str:
        member, rest = match.group(1), match.group(2)
        route = "SMPR" + member[-4:]
        return (f'<Media id="{member}"{rest}>\n'
                f'          <Route id="{route}" bus="SMPB0001" gains="1 1"/>\n'
                f'        </Media>')

    text, count = re.subn(r'<Media id="(SMP[0-9A-Z]{5})"([^>]*?)/>', routed, text)

    if count != 6 or 'bus="SMPB0001"' not in text:
        raise HarnessError(f"the sampler fixture is not the shape this expects ({count} media)")

    show.write_text(text, encoding="utf-8", newline="\n")

    for name in ("thunder", "rain", "bell", "door", "wind", "owl"):
        first_sound.write_tone(bundle / "media" / f"{name}.wav")


# =============================================================================
# Driving
# =============================================================================

class Hand:
    """One UDP socket for every message, so a held pad is pressed and let go
    by the same origin - which is what a hold clip's ownership is keyed on."""

    def __init__(self, server: Server):
        self.port = server.osc_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, address: str, args: "list | None" = None) -> None:
        self.sock.sendto(common.osc_encode(address, args), ("127.0.0.1", self.port))

    def close(self) -> None:
        self.sock.close()


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return json.loads(body)["VALUE"][0]
    except (KeyError, IndexError, ValueError):
        return None


def wait_for(server: Server, address: str, wanted, seconds: float = 20.0):
    """Polls until a node reads `wanted`, and answers with what it last read."""
    deadline = time.monotonic() + seconds
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == wanted:
            return actual

        time.sleep(0.02)

    return actual


def wait_until(server: Server, predicate, seconds: float = 20.0) -> bool:
    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        if predicate():
            return True

        time.sleep(0.02)

    return predicate()


def wait_ticks(server: Server, count: int, seconds: float = 120.0) -> bool:
    """The engine's own clock, and not the wall's: phase3_groups.py says why."""
    start = value_of(server, "/godot/engine/tick")

    if not isinstance(start, int):
        return False

    return wait_until(server,
                      lambda: isinstance(now := value_of(server, "/godot/engine/tick"), int)
                      and now - start >= count,
                      seconds)


def run_for_cue(server: Server, cue: str, seconds: float = 20.0) -> str:
    found = ""

    def look() -> bool:
        nonlocal found

        for run in (value_of(server, "/godot/run/order") or "").split():
            if value_of(server, f"/godot/run/{run}/cue") == cue:
                found = run
                return True

        return False

    wait_until(server, look, seconds)
    return found


# =============================================================================
# Reading the render
# =============================================================================

def settles_into(samples: "list[float]", level: float, start: int = 0,
                 stable: int = RATE // 10) -> int:
    """The first frame at or after `start` where the render sits at `level` for
    a tenth of a second. A plateau, not a sample: a fade or an edge passes
    through every level on its way somewhere else."""
    run = 0

    for n in range(max(0, start), len(samples)):
        if abs(abs(samples[n]) - level) < TOLERANCE:
            run += 1

            if run >= stable:
                return n - run + 1
        else:
            run = 0

    return -1


def settles_quiet(samples: "list[float]", start: int = 0, stable: int = RATE // 10) -> int:
    run = 0

    for n in range(max(0, start), len(samples)):
        if abs(samples[n]) < QUIET:
            run += 1

            if run >= stable:
                return n - run + 1
        else:
            run = 0

    return -1


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 6: a sampler bank played by hand ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase6-sampler-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "sampler")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        give_it_a_bus(bundle)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            hand = Hand(server)

            try:
                report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                             "the audio side comes up running")

                # --- the panel the show declares ----------------------------
                # A virtual surface is the client's own and needs no port, so
                # it is connected the moment the show is open (§16.2).
                report.equal(value_of(server, f"/godot/surface/{SURFACE}/connected"), True,
                             "the virtual surface is connected with no MIDI in the room")
                report.equal(value_of(server, f"/godot/slot/{RAIN_STRIP}/kind"), "strip",
                             "and its strips are slots of the fourth kind")

                # --- GO arms the bank ---------------------------------------
                # Every member onto a strip, in order, each its own run and its
                # own voice, none of them sounding. The standby moves PAST the
                # bank: a sampler group is a place to stand, never to descend.
                hand.send("/godot/cmd/go")

                armed = wait_until(server, lambda: all(
                    value_of(server, f"/godot/slot/{strip}/word") == "armed"
                    for strip in BANK_A_STRIPS), seconds=30.0)

                report.check(armed, "GO armed all four members onto the panel's strips",
                             str([value_of(server, f"/godot/slot/{s}/word") for s in BANK_A_STRIPS]))

                bank = run_for_cue(server, BANK_A)
                report.check(bool(bank), "and the bank is a run of its own")

                if bank:
                    report.equal(value_of(server, f"/godot/run/{bank}/state"), "playing",
                                 "which is playing, having launched nothing")

                report.equal(value_of(server, "/godot/list/SMP00001/standby"), BANK_B,
                             "and the standby moved past the bank, not into it")

                if not armed:
                    return report.finish()

                # Silence for a while, so the render opens with it.
                wait_ticks(server, 15)

                # --- Rain, pressed at 64 ------------------------------------
                # Velocity on and a floor of -40 dB: 64 is halfway up the
                # scale in dB, so the clip starts at -20 dB, a tenth.
                rain = value_of(server, f"/godot/slot/{RAIN_STRIP}/holder")
                hand.send("/godot/cmd/strip/press", [RAIN_STRIP, 64])

                report.equal(wait_for(server, f"/godot/run/{rain}/state", "playing"), "playing",
                             "a pad press over the network plays the clip")
                trim = value_of(server, f"/godot/run/{rain}/trim")
                report.check(isinstance(trim, (int, float)) and abs(float(trim) + 20.0) < 1.0e-3,
                             "at the level its velocity asked for", f"trim {trim!r}")
                report.equal(value_of(server, f"/godot/slot/{RAIN_STRIP}/word"), "playing",
                             "and the strip says so in a word")

                wait_ticks(server, 50)

                # --- Rain's fader to the bottom -----------------------------
                # Play-out: silence on the fader is a mute, and the clip runs
                # on under it.
                hand.send(f"/godot/run/{rain}/trim", [-120.0])

                report.equal(wait_for(server, f"/godot/run/{rain}/trim", -120.0), -120.0,
                             "a hand on the fader rides the run's trim")
                wait_ticks(server, 25)
                report.equal(value_of(server, f"/godot/run/{rain}/state"), "playing",
                             "and a play-out clip at the bottom is muted, not stopped")

                # --- Thunder, held ------------------------------------------
                thunder = value_of(server, f"/godot/slot/{THUNDER_STRIP}/holder")
                hand.send("/godot/cmd/strip/press", [THUNDER_STRIP])

                report.equal(wait_for(server, f"/godot/run/{thunder}/state", "playing"), "playing",
                             "a hold clip plays while the pad is down")
                report.equal(value_of(server, f"/godot/slot/{THUNDER_STRIP}/word"), "held",
                             "and its strip says held")
                report.equal(value_of(server, f"/godot/run/{thunder}/held"), True,
                             "and the run knows which hand owns it")

                wait_ticks(server, 50)

                # --- let go -------------------------------------------------
                # A short fade and a stop, and the member arms again on the
                # same strip at once: any number of times.
                hand.send("/godot/cmd/strip/release", [THUNDER_STRIP])

                report.equal(wait_for(server, f"/godot/run/{thunder}/state", "done"), "done",
                             "letting go of a hold clip stops it")

                again = wait_until(server, lambda: (
                    value_of(server, f"/godot/slot/{THUNDER_STRIP}/holder") not in (None, "", thunder)
                    and value_of(server, f"/godot/slot/{THUNDER_STRIP}/word") == "armed"))

                report.check(again, "and the member is armed again on its strip, as a new run",
                             str(value_of(server, f"/godot/slot/{THUNDER_STRIP}/holder")))

                rearmed = value_of(server, f"/godot/slot/{THUNDER_STRIP}/holder")

                if again and rearmed:
                    report.equal(value_of(server, f"/godot/run/{rearmed}/trim"), 0.0,
                                 "back at its initial level, where its fader waits for a touch")

                wait_ticks(server, 25)

                # --- Door, touched ------------------------------------------
                door = value_of(server, f"/godot/slot/{DOOR_STRIP}/holder") or ""
                door_trim = f"/godot/run/{door}/trim"

                report.equal(value_of(server, door_trim), 0.0,
                             "Door's fader waits at its initial level")

                hand.send(door_trim, [-120.0])
                report.equal(wait_for(server, door_trim, -120.0), -120.0,
                             "a write with no hand on it moves the level")
                wait_ticks(server, 5)
                report.equal(value_of(server, f"/godot/run/{door}/state"), "armed",
                             "and starts nothing")

                hand.send("/godot/cmd/node/touch", [door_trim])
                report.equal(wait_for(server, f"/godot/run/{door}/state", "playing"), "playing",
                             "a hand landing on the fader starts the clip")
                report.equal(value_of(server, door_trim), -120.0,
                             "at the level the fader is at, silent here")

                hand.send("/godot/cmd/node/release", [door_trim])
                wait_ticks(server, 10)

                # --- the bank disarmed --------------------------------------
                # The transport cue aimed at the bank: every member ends - the
                # muted Rain with them - the footer runs, the strips are free.
                hand.send("/godot/cmd/cue/fire", [DISARM_A])

                if bank:
                    report.equal(wait_for(server, f"/godot/run/{bank}/state", "done"), "done",
                                 "a transport cue aimed at the bank disarms it")

                freed = wait_until(server, lambda: all(
                    value_of(server, f"/godot/slot/{strip}/word") == "free"
                    for strip in BANK_A_STRIPS))

                report.check(freed, "and every strip it held is free",
                             str([value_of(server, f"/godot/slot/{s}/word") for s in BANK_A_STRIPS]))

                wait_ticks(server, 25)
                first_sound.wait_for_render_tail(render)

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "nothing of Go.dot's allocated on the audio thread")
            finally:
                hand.close()

        # --- what came out ----------------------------------------------------
        channels, samples = first_sound.read_render(render)

        if report.check(channels >= 2, "the render has the bus's channels", str(channels)):
            main = samples[0]

            tenth = settles_into(main, TENTH)
            muted = settles_quiet(main, max(0, tenth))
            held = settles_into(main, UNITY, max(0, muted))
            released = settles_quiet(main, max(0, held))

            report.check(tenth >= 0, "Rain is heard at a tenth: velocity 64 is -20 dB",
                         f"frame {tenth}")
            report.check(muted > tenth >= 0, "then silence under the fader at the bottom",
                         f"frame {muted}")
            report.check(held > muted > 0, "then Thunder at the constant: a press with no "
                         "velocity plays at unity", f"frame {held}")
            report.check(released > held > 0, "then silence when the hand lets go",
                         f"frame {released}")

            if released > 0:
                tail = main[released:]
                loudest = max((abs(v) for v in tail), default=0.0)

                # Rain was still running, muted, until the disarm; a trim at
                # the bottom is zero gain, so nothing may be heard after the
                # release at all.
                report.check(loudest < QUIET, "and nothing is heard after it",
                             f"loudest after the release: {loudest}")

            final = main[-RATE // 4:]
            report.check(max((abs(v) for v in final), default=0.0) < 1.0e-6,
                         "and the render ends in digital silence")

        # --- the records ------------------------------------------------------
        applied = [line.split() for line in log.read_text(encoding="utf-8").splitlines()
                   if line.startswith("A ")]

        presses = [parts for parts in applied if len(parts) > 4 and parts[4] == "strip.press"]
        releases = [parts for parts in applied if len(parts) > 4 and parts[4] == "strip.release"]
        by_hand = [parts for parts in presses if parts[3].startswith("udp:")]
        by_touch = [parts for parts in presses if parts[3] == "engine"]

        report.equal(len(by_hand), 2, "two presses in the log, with the origin of the hand")
        report.check(any(parts[-1] == "i:64" for parts in by_hand),
                     "and the velocity travels with the press", str(by_hand))
        report.equal(len(by_touch), 1,
                     "and one the engine made of a touch, a record a replay can be handed",
                     str(presses))
        report.equal(len(releases), 1, "one release")

        # --- and it reproduces ------------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}")

        report.equal(code, 0, "and `wfg replay` reproduces the session",
                     (out + err).strip()[:400])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"phase6_sampler: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"phase6_sampler: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"phase6_sampler: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
