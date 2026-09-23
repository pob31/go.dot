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

"""A media cue's EQ, heard: one tone cut by twenty decibels, the other left.

WHAT THIS IS FOR. The unit suite proves the filters against their own maths
and the Runner's push against a fake player; AudioTests proves a peak on the
voice through the whole chain in one process. What none of them says is that
the SHIPPED BINARY, serving a bundle whose cue carries the nineteen rows, cuts
what the rows say through a real Tracktion graph, and that a node.set to the
cue over the network while it sounds is heard - which is the rotary's path,
minus the rotary.

THE SOUND IS TWO TONES, not the steady constant the other drivers play,
because a constant passes a peak at three kilohertz exactly untouched. The
file is a 750 Hz sine and a 3 kHz sine, a quarter each. The cue's EQ is a
-20 dB peak at 3 kHz with a width of four, whose skirt at 750 Hz is under a
fifth of a decibel. So a Goertzel filter at each tone, over a second of the
render while the cue plays shaped, must read the 3 kHz tone twenty decibels
below what it reads after the band is written back to nought, and the 750 Hz
tone the same in both.

WHY 750 AND 3000, AND NOT 100. Their periods - sixty-four and sixteen samples
at 48 kHz - divide the block. The hosted render on a loaded Debug build DROPS
WHOLE BLOCKS now and then (found here, 2026-09-23: a window of the render that
happened to hold no drop read a 100 Hz tone exactly, and its neighbours read
it four decibels low with leakage into every other bin), and a tone whose
period divides the block keeps its phase across a dropped block, so the
reading survives what the constant-tone drivers never see. That the render
can drop a block is a finding about `--render`, recorded in the namespace
draft; it is not this driver's to fix, and this driver does not hide it: the
frame count it waits for is what it reads.

THE FIXTURE IS tests/fixtures/bundles/eq, which ships no audio (no fixture
does). This copies it and writes the file into the copy.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import math
import socket
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "eq"

RATE = 48000
BLOCK = 64

MEDIA = "EQ000002"
LOW_HZ = 750.0
HIGH_HZ = 3000.0
AMPLITUDE = 0.25

# What the cue's rows say, and what the render must therefore read.
CUT_DB = -20.0
CUT_TOLERANCE_DB = 1.0
UNTOUCHED_TOLERANCE_DB = 0.5


# =============================================================================
# The media file
# =============================================================================

def write_two_tones(path: Path, seconds: float = 12.0) -> None:
    """A stereo file: a 750 Hz sine and a 3 kHz sine summed, a quarter each."""
    path.parent.mkdir(parents=True, exist_ok=True)

    frames = int(RATE * seconds)
    out = bytearray()
    scale = 32767.0

    for n in range(frames):
        t = n / RATE
        value = AMPLITUDE * math.sin(2.0 * math.pi * LOW_HZ * t) \
              + AMPLITUDE * math.sin(2.0 * math.pi * HIGH_HZ * t)
        sample = int(round(value * scale))
        out += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(bytes(out))


# =============================================================================
# Reading a tone out of the render
# =============================================================================

def goertzel_db(samples: "list[float]", frequency: float) -> float:
    """The power of one frequency in a window, in dB re a full-scale sine.

    Goertzel rather than an FFT because the question is two numbers, the
    standard library has no FFT, and a window of an integer number of cycles
    makes the reading exact to the sine's own arithmetic.
    """
    count = len(samples)

    if count == 0:
        return -400.0

    k = round(frequency * count / RATE)
    omega = 2.0 * math.pi * k / count
    coefficient = 2.0 * math.cos(omega)
    s_prev = s_prev2 = 0.0

    for x in samples:
        s = x + coefficient * s_prev - s_prev2
        s_prev2 = s_prev
        s_prev = s

    power = s_prev2 * s_prev2 + s_prev * s_prev - coefficient * s_prev * s_prev2
    amplitude = 2.0 * math.sqrt(max(power, 0.0)) / count

    return 20.0 * math.log10(amplitude) if amplitude > 0.0 else -400.0


def window(samples: "list[float]", start_frame: int, seconds: float = 1.0) -> "list[float]":
    return samples[start_frame:start_frame + int(RATE * seconds)]


# =============================================================================
# Driving
# =============================================================================

class Hand:
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


def wait_for(server: Server, address: str, expected, timeout: float = 20.0):
    deadline = time.monotonic() + timeout
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == expected:
            return actual

        time.sleep(0.05)

    return actual


def wait_for_frames(render: Path, frames: int, timeout: float = 30.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 9a: a media cue's EQ, heard ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase9a-eq-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "eq")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_two_tones(bundle / "media" / "tones.wav")

        edit_at = 0

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            hand = Hand(server)

            try:
                report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                             "the audio side comes up running")

                # The rows the fixture carries, published as the cue's own.
                report.equal(value_of(server, f"/godot/cue/{MEDIA}/eqB3Gain"), CUT_DB,
                             "the cue's EQ band reads what the show says")
                report.equal(value_of(server, f"/godot/cue/{MEDIA}/eqB1Gain"), 0.0,
                             "and a band nobody touched reads its default, flat")
                report.equal(value_of(server, f"/godot/cue/{MEDIA}/eqOn"), True,
                             "and the EQ is on")

                # --- GO, and three seconds of it shaped ------------------------
                hand.send("/godot/cmd/go")

                report.check(wait_for_frames(render, int(RATE * 3.5)),
                             "the render runs three and a half seconds past GO")

                # --- the band written back to nought while it sounds -----------
                # The rotary's path, minus the rotary: node.set on the cue's own
                # row, over the network, coalesced into the show's history.
                edit_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/node/set", [f"/godot/cue/{MEDIA}/eqB3Gain", 0.0])

                report.equal(wait_for(server, f"/godot/cue/{MEDIA}/eqB3Gain", 0.0), 0.0,
                             "the write lands on the cue")

                report.check(wait_for_frames(render, edit_at + int(RATE * 3.5)),
                             "and the render runs three and a half seconds past it")

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "Go.dot's own code allocated nothing on the audio thread")

                first_sound.wait_for_render_tail(render)
            finally:
                hand.close()

        # --- what came out ---------------------------------------------------
        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render is stereo", f"{channels} channels")

        left = data[0] if data else []
        start = first_sound.first_above(left, 0.01)
        report.check(start >= 0, "the cue was heard at all")

        if start >= 0 and len(left) > edit_at + int(RATE * 3.0):
            # A second of it shaped, once the cue and the filter have settled;
            # a second of it flat, once the write has landed and settled.
            shaped = window(left, start + int(RATE * 1.0))
            flat = window(left, edit_at + int(RATE * 1.5))

            high_shaped = goertzel_db(shaped, HIGH_HZ)
            high_flat = goertzel_db(flat, HIGH_HZ)
            low_shaped = goertzel_db(shaped, LOW_HZ)
            low_flat = goertzel_db(flat, LOW_HZ)

            cut = high_shaped - high_flat
            report.check(abs(cut - CUT_DB) <= CUT_TOLERANCE_DB,
                         "the 3 kHz tone is twenty decibels down while the band is in",
                         f"shaped {high_shaped:.2f} dB, flat {high_flat:.2f} dB, cut {cut:.2f} dB")

            drift = low_shaped - low_flat
            report.check(abs(drift) <= UNTOUCHED_TOLERANCE_DB,
                         "and the 750 Hz tone is untouched either way",
                         f"shaped {low_shaped:.2f} dB, flat {low_flat:.2f} dB")

            report.check(abs(high_flat - low_flat) <= UNTOUCHED_TOLERANCE_DB,
                         "and flat, the two tones read the same",
                         f"3 kHz {high_flat:.2f} dB, 750 Hz {low_flat:.2f} dB")
        else:
            report.check(False, "the render is long enough to read both windows",
                         f"{len(left)} frames, edit at {edit_at}")

        # --- and the session replays -------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session",
                     (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
