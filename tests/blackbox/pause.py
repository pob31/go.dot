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

"""Pause / Play end to end (PRD §3.34): the show held and let go, asked of the
SHIPPED BINARY over OSC, and read back from what was HEARD (the render), what
the engine SAYS (its readouts) and what it WROTE (its log). Then the session
replays.

THE SHOW is tests/fixtures/bundles/pause: a ramp, a tone that waits three
seconds before it sounds, and a last tone.

THE RAMP IS THE CLOCK, as in go_doh.py: `ramp.wav` rises from 0.1 to 0.9 over
eight seconds, so a sample of it is a position in the file. A pause that held
the file where it was reads, after Play, exactly where a show that never
paused would read - against the render's own frames, which hold show time only:
no block is run, and so none is written, while the show is held.

THE SESSION:

 1. GO: the ramp. GO: the tone, waiting its three seconds.
 2. A second in, Pause: the sound ramps down over a tenth of a second, the tick
    stands, `/godot/engine/paused` reads true, and a GO is refused `paused` -
    the pointer does not move.
 3. Longer than the wait had left, Play: the wait goes on from where it stood,
    so the tone sounds three seconds of SHOW time after its GO, and the ramp
    carries on from the same sample.
 4. Pause again, then Esc: the show stops and the pause is over, and not a
    sample of what was paused is heard again.
 5. The session's log replays record for record.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import array
import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "pause"

RATE = 48000
BLOCK = 128
TICK = RATE // 50
RAMP_TICKS = 5                    # the de-click: a tenth of a second

LIST = "PSSHW001"
RAMP = "PSRMP001"
LATER = "PSWT0001"
NEXT = "PSNXT001"

RAMP_CHANNEL = 0
LATER_CHANNEL = 2

RAMP_FROM = 0.1
RAMP_SLOPE = 0.1                  # per second of the file
RAMP_SECONDS = 8.0
TONE_LEVEL = 0.25
PRE_WAIT = 3.0

#  How far a launch may land after its tick: the launch latency, a few ticks.
LAUNCH_SLACK = 0.2


def write_constant(path: Path, level: float, seconds: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = int(level * 32767)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<hh", sample, sample) * int(RATE * seconds))


def write_ramp(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = int(RATE * RAMP_SECONDS)
    body = bytearray()

    for n in range(frames):
        sample = int((RAMP_FROM + RAMP_SLOPE * n / RATE) * 32767)
        body += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(body))


# =============================================================================
# The engine
# =============================================================================

def value_of(server: Server, address: str):
    return first_sound.value_of(server, address)


def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def tick_of(server: Server) -> int:
    now = value_of(server, "/godot/engine/tick")
    return now if isinstance(now, int) else -1


def standby_of(server: Server) -> str:
    return value_of(server, f"/godot/list/{LIST}/standby") or ""


def records(log: Path) -> "list[list[str]]":
    """Every record of the session's log, split on spaces: kind, tick, sequence,
    origin, [the reason, for a refusal,] the command and its arguments."""
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []

    return [line.split(" ") for line in lines if line[:2] in ("A ", "R ")]


def ticks_of(log: Path, command_name: str, kind: str = "A") -> "list[int]":
    """The ticks a command was applied at, or refused at with kind R."""
    at = 4 if kind == "A" else 5
    return [int(record[1]) for record in records(log)
            if record[0] == kind and len(record) > at and record[at] == command_name]


# =============================================================================
# The render
# =============================================================================

def read_channels(path: Path, wanted: "list[int]") -> "dict[int, array.array]":
    raw = path.read_bytes()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise HarnessError(f"{path} is not a RIFF/WAVE file")

    channels = bits = tag = 0
    data = memoryview(b"")
    at = 12

    while at + 8 <= len(raw):
        name = raw[at:at + 4]
        size = struct.unpack_from("<I", raw, at + 4)[0]

        if name == b"fmt ":
            tag, channels = struct.unpack_from("<HH", raw, at + 8)
            bits = struct.unpack_from("<H", raw, at + 22)[0]

            if tag == 0xFFFE and size >= 26:
                tag = struct.unpack_from("<H", raw, at + 32)[0]
        elif name == b"data":
            data = memoryview(raw)[at + 8:at + 8 + size]

        at += 8 + size + (size & 1)

    if tag != 3 or bits != 32 or channels == 0:
        raise HarnessError(f"{path} is format {tag} at {bits} bits; this reads 32-bit float")

    frames = len(data) // (4 * channels)
    floats = data[:frames * channels * 4].cast("f")
    return {c: array.array("f", floats[c::channels]) for c in wanted if c < channels}


def first_above(samples: "array.array", floor: float, start: int = 0) -> int:
    for n in range(max(0, start), len(samples)):
        if abs(samples[n]) > floor:
            return n

    return -1


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"Pause / Play end to end ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-pause-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "pause")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_ramp(bundle / "media" / "ramp.wav")
        write_constant(bundle / "media" / "tone.wav", TONE_LEVEL, 20.0)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK,
                    hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            report.equal(value_of(server, "/godot/engine/paused"), False, "the show is not paused at the start")

            session(report, server)

            report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            first_sound.wait_for_render_tail(render)

        heard(report, render, log)

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))
        report.equal(code, 0, "`wfg replay` reproduces the session record for record",
                     (out + err).strip()[-2000:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def session(report: Report, server: Server) -> None:
    # --- 1. the ramp, and a tone waiting ------------------------------------
    command(server, "go")
    report.check(bool(common.wait_until(lambda: standby_of(server) == LATER, timeout=20)),
                 "GO fires the ramp and the pointer moves on")
    first_sound.run_for_cue(server, RAMP)
    command(server, "go")
    report.check(bool(common.wait_until(lambda: standby_of(server) == NEXT, timeout=20)),
                 "GO starts the tone's wait")

    time.sleep(1.0)

    # --- 2. Pause -----------------------------------------------------------
    command(server, "run.pauseAll")
    report.equal(first_sound.wait_for(server, "/godot/engine/paused", True), True,
                 "Pause: /godot/engine/paused reads true")

    time.sleep(0.5)                       # past the de-click
    stood = tick_of(server)
    time.sleep(3.5)                       # longer than the wait had left
    report.equal(tick_of(server), stood, "the tick stands while the show is held")

    command(server, "go")
    refused = bool(common.wait_until(lambda: "paused" in (value_of(server, "/godot/engine/lastError") or ""),
                                     timeout=5))
    report.check(refused, "a GO while paused is refused paused",
                 str(value_of(server, "/godot/engine/lastError")))
    report.equal(standby_of(server), NEXT, "and the pointer does not move")

    # --- 3. Play ------------------------------------------------------------
    command(server, "run.playAll")
    report.equal(first_sound.wait_for(server, "/godot/engine/paused", False), False,
                 "Play: /godot/engine/paused reads false")
    report.check(bool(common.wait_until(lambda: tick_of(server) > stood + 10, timeout=10)),
                 "the tick moves on from where it stood")

    time.sleep(3.0)                       # the rest of the wait, and some sound

    # --- 4. Pause, then Esc -------------------------------------------------
    command(server, "run.pauseAll")
    first_sound.wait_for(server, "/godot/engine/paused", True)
    time.sleep(0.5)
    command(server, "run.stopAll")
    report.equal(first_sound.wait_for(server, "/godot/engine/paused", False), False,
                 "Esc while paused ends the pause")
    going = ("waiting", "playing", "stopping", "postWait")
    report.check(bool(common.wait_until(lambda: not [r for r in first_sound.runs_in(server)
                                                       if value_of(server, f"/godot/run/{r}/state") in going],
                                        timeout=10)),
                 "and stops everything - the standby's preparation left ready, as Esc leaves it")
    time.sleep(1.0)


def heard(report: Report, render: Path, log: Path) -> None:
    channels = read_channels(render, [RAMP_CHANNEL, LATER_CHANNEL])
    ramp = channels.get(RAMP_CHANNEL)
    later = channels.get(LATER_CHANNEL)

    if ramp is None or later is None:
        report.check(False, "the render has the ramp's and the tone's channels")
        return

    gos = ticks_of(log, "go")
    pauses = ticks_of(log, "run.pauseAll")
    plays = ticks_of(log, "run.playAll")
    escs = ticks_of(log, "run.stopAll")

    if len(gos) < 2 or len(pauses) < 2 or not plays or not escs:
        report.check(False, "the log holds both GOs, both pauses, the play and the Esc",
                     f"go {gos}, pause {pauses}, play {plays}, stopAll {escs}")
        return

    report.equal(len(ticks_of(log, "go", "R")), 1, "the log holds one GO refused")

    # --- the ramp's launch, and where it reads after Play ---------------------
    launched = first_above(ramp, 0.05)
    report.check(launched >= 0, "the ramp is heard")

    play_frame = plays[0] * TICK
    probe = play_frame + RATE                # a second after Play, well past the de-click
    expected = RAMP_FROM + RAMP_SLOPE * (probe - launched) / RATE

    if 0 <= probe < len(ramp):
        report.check(abs(ramp[probe] - expected) < 0.01,
                     "after Play the ramp reads where a show that never paused would: the file held its place",
                     f"read {ramp[probe]:.4f}, expected {expected:.4f}")

    # --- the de-click at the pause --------------------------------------------
    pause_frame = pauses[0] * TICK
    window = ramp[pause_frame:pause_frame + (RAMP_TICKS + 4) * TICK]
    report.check(len(window) > 0 and min(abs(v) for v in window) < 0.005,
                 "the ramp goes quiet within a tenth of a second of Pause")
    before = ramp[pause_frame - TICK] if pause_frame >= TICK else 0.0
    report.check(abs(before) > 0.1, "and was sounding just before it", f"{before:.4f}")

    # --- the wait counted show time, not the pause ----------------------------
    sounded = first_above(later, 0.05)
    waited = (sounded - gos[1] * TICK) / RATE if sounded >= 0 else -1.0
    report.check(PRE_WAIT <= waited <= PRE_WAIT + LAUNCH_SLACK,
                 "the tone sounds three seconds of show time after its GO - the pause not counted",
                 f"{waited:.3f} s")

    # --- Esc while paused: nothing of it heard again --------------------------
    second_pause = pauses[1] * TICK + RAMP_TICKS * TICK + BLOCK
    tail_ramp = max((abs(v) for v in ramp[second_pause:]), default=0.0)
    tail_later = max((abs(v) for v in later[second_pause:]), default=0.0)
    report.check(tail_ramp < 0.001 and tail_later < 0.001,
                 "after Esc while paused, not a sample of what was paused is heard again",
                 f"ramp {tail_ramp:.5f}, tone {tail_later:.5f}")


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        common.find_binary()
        return run(locale)
    except HarnessError as error:
        print(f"pause: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
