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
An output keys a plugin, in a SERVED show (namespace draft §141, BIB, DO.7).

The `key-output` fixture: cue 1, a mono bed on the main pair through the test
ducker, its key picked as the output "Monitor". Cue 2, a mono voice played on
that output. GO plays the bed; a second GO plays the voice. Heard in the
render:

    - the bed at the ducker's gain alone while Monitor is silent;
    - once the voice plays, the bed ducked by what Monitor sends - one block
      after the voice's first sample, since outputs are summed after the whole
      graph and an output is a key one block late, always (BIB);
    - the voice on Monitor at its own level;
    - and the session replays.

Through nothing but the product's own doors: `wfg serve`, OSC, the render.
"""

import argparse
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "key-output"

RATE = 48000
BLOCK = 64
SOURCE = 0.25          # the bed
GAIN = 0.5             # the test ducker's gain at rest
KEY = 0.5              # the voice, on Monitor
PLAIN = SOURCE * GAIN
DUCKED = SOURCE * GAIN * (1.0 - KEY)
TOLERANCE = 0.01

PLUGIN = "KQ000008"


def write_mono_constant(path: Path, value: float, seconds: float = 30.0) -> None:
    frames = int(RATE * seconds)
    sample = int(value * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * frames, *([sample] * frames)))


def level(samples: "list[float]", start_frame: int, seconds: float = 0.5) -> float:
    """The median block's mean size: steady against a dropped block."""
    part = samples[start_frame:start_frame + int(RATE * seconds)]
    if len(part) < BLOCK:
        return -1.0
    blocks = sorted(sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK))
    return blocks[len(blocks) // 2]


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def run(locale: "str | None") -> int:
    report = Report(f"an output keys a plugin in a served show ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-key-output-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "key-output")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_mono_constant(bundle / "media" / "tone.wav", SOURCE)
        write_mono_constant(bundle / "media" / "voice.wav", KEY)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, proxy_deadline_us=20000, engine_folder=room / "engine") as server:
            report.check(common.wait_until(
                lambda: value_of(server, f"/godot/plugin/{PLUGIN}/state") == "loaded", timeout=30.0),
                "the test ducker's child comes up", str(value_of(server, f"/godot/plugin/{PLUGIN}/state")))

            went_at = first_sound.frames_on_disk(render)
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= went_at + int(RATE * 3.0), timeout=30.0),
                "the render runs three seconds past the bed's GO")

            voice_went_at = first_sound.frames_on_disk(render)
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= voice_went_at + int(RATE * 3.0), timeout=30.0),
                "and three seconds past the voice's")
            first_sound.wait_for_render_tail(render)

        channels, data = first_sound.read_render(render)
        report.check(channels >= 4, "the render has the main pair and the voice's", f"{channels} channels")

        if channels >= 4:
            bed = first_sound.first_above(data[0], 0.01)
            voice = first_sound.first_above(data[2], 0.01)
            report.check(bed >= 0, "the bed is heard on the main pair")
            report.check(voice > bed + RATE, "the voice a second or more after it, on its own pair",
                         f"bed at {bed / RATE:.2f} s, voice at {voice / RATE:.2f} s")

            if bed >= 0 and voice > bed + RATE:
                before = level(data[0], bed + RATE // 4)
                after = level(data[0], voice + RATE // 2)
                report.check(abs(before - PLAIN) <= TOLERANCE,
                             "with Monitor silent, the bed at the ducker's gain alone", f"{before:.4f}")
                report.check(abs(after - DUCKED) <= TOLERANCE,
                             "once the voice plays on Monitor, ducked by that output", f"{after:.4f}")

                #  The step against the voice's first sample: one block later (BIB).
                step = next((n for n in range(voice - 4 * BLOCK, voice + 6 * BLOCK)
                             if abs(data[0][n]) < (PLAIN + DUCKED) / 2), -1)
                report.check(step >= 0 and abs(step - (voice + BLOCK)) <= 2,
                             "one block after the voice, as an output always is",
                             f"step at {step - voice:+d} samples from the voice")

            #  The key is heard, never added: Monitor holds the voice and nothing more.
            if voice >= 0:
                heard = level(data[2], voice + RATE // 2)
                report.check(abs(heard - KEY) <= TOLERANCE,
                             "the voice on Monitor at its own level", f"{heard:.4f}")

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
