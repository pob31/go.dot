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
"""
Go.dot's own dynamics stage in a SERVED show (namespace draft §141, SC.7).

The `dyn` fixture, both cues a 1 kHz sine at a half:

    - cue 1, on the main pair, its stage a gate keyed by the named input
      "Talkback" (range -40 dB), which is silent until KEY_FROM and then
      speaks: shut to a hundredth, then open;
    - cue 2, on a pair of its own, its stage a compressor hearing itself, four
      to one over minus twenty: its half-scale peaks come out near 0.15;
    - and the session replays.

Written from the rows alone (dynOn, dynKey...): what the document holds is
what the voices do. Through nothing but `wfg serve`, OSC and the render.
"""

import argparse
import math
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import first_sound  # noqa: E402
from common import Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "dyn"

RATE = 48000
BLOCK = 64
SOURCE = 0.5
KEY_FROM = 8.0
SHUT = SOURCE * 0.01            # the gate's range, -40 dB
COMPRESSED = SOURCE * 0.30      # -10.5 dB off a -6 dBFS peak, four to one over -20


def write_sine(path: Path, amplitude: float, seconds: float, silent_until: float = 0.0) -> None:
    frames = int(RATE * seconds)
    quiet = int(RATE * silent_until)
    samples = [0 if n < quiet else int(amplitude * 32767 * math.sin(2.0 * math.pi * 1000.0 * n / RATE))
               for n in range(frames)]
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * frames, *samples))


def peak(samples: "list[float]", start: int, seconds: float = 0.25) -> float:
    part = samples[start:start + int(RATE * seconds)]
    return max((abs(s) for s in part), default=-1.0)


def run(locale: "str | None") -> int:
    report = Report(f"Go.dot's own dynamics in a served show ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-dyn-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "dyn")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        talkback = room / "talkback.wav"
        (bundle / "media").mkdir(exist_ok=True)
        write_sine(bundle / "media" / "sine.wav", SOURCE, 30.0)
        write_sine(talkback, SOURCE, 40.0, silent_until=KEY_FROM)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, input_wav=talkback, engine_folder=room / "engine") as server:
            went_at = first_sound.frames_on_disk(render)
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= went_at + RATE // 2, timeout=30.0),
                "the render runs half a second past the first GO")
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= int(RATE * (KEY_FROM + 3.0)), timeout=60.0),
                "the render runs three seconds past the talkback's first word")
            first_sound.wait_for_render_tail(render)

        channels, data = first_sound.read_render(render)
        report.check(channels >= 4, "the render has the main pair and the monitor's", f"{channels} channels")

        if channels >= 4:
            key_at = int(RATE * KEY_FROM)
            bed = first_sound.first_above(data[0], 0.001)
            voice = first_sound.first_above(data[2], 0.01)
            report.check(0 <= bed < key_at - 2 * RATE, "the gated cue is under way before the talkback speaks",
                         f"first sound at {bed / RATE:.2f} s")
            report.check(0 <= voice < key_at, "and the compressed one", f"first sound at {voice / RATE:.2f} s")

            if 0 <= bed < key_at - 2 * RATE:
                shut = peak(data[0], key_at - RATE)
                opened = peak(data[0], key_at + RATE)
                report.check(abs(shut - SHUT) <= SHUT * 0.15,
                             "with the talkback silent, the gate shut to its range", f"{shut:.4f}")
                report.check(abs(opened - SOURCE) <= 0.01,
                             "once it speaks, the gate open", f"{opened:.4f}")

            if voice >= 0:
                squeezed = peak(data[2], voice + RATE)
                report.check(abs(squeezed - COMPRESSED) <= COMPRESSED * 0.1,
                             "the other cue compressed by itself, four to one over minus twenty", f"{squeezed:.4f}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session", (out + err).strip()[-400:])

    return report.finish()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wfg-locale", default=None)
    args = parser.parse_args()
    return run(args.wfg_locale)


if __name__ == "__main__":
    sys.exit(main())
