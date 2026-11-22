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
An input keys a plugin, in a SERVED show (namespace draft §141, SC.1).

The `key-input` fixture: one mono sound cue through the test ducker, its key
picked as the named input "Talkback". The hosted interface is fed from a WAV
whose first input is silent for the first seconds and then steady, as a
console's talkback that someone keys. Heard on the main pair:

    - with the talkback silent, the cue at the ducker's gain alone;
    - once it speaks, ducked by it - in the same block it arrives, since an
      input reaches the graph before the graph runs (BHP);
    - the plugin says it offers a sidechain, two channels wide;
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "key-input"

RATE = 48000
BLOCK = 64
SOURCE = 0.25          # the cue's tone
GAIN = 0.5             # the test ducker's gain at rest
KEY = 0.5              # the talkback once it speaks
KEY_FROM = 8.0         # seconds into the interface's input
PLAIN = SOURCE * GAIN
DUCKED = SOURCE * GAIN * (1.0 - KEY)
TOLERANCE = 0.01

PLUGIN = "KY000006"


def write_mono_constant(path: Path, value: float, seconds: float = 30.0) -> None:
    frames = int(RATE * seconds)
    sample = int(value * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * frames, *([sample] * frames)))


def write_talkback(path: Path, seconds: float = 40.0) -> None:
    """Silence on the first input until KEY_FROM, then a steady level."""
    frames = int(RATE * seconds)
    quiet = int(RATE * KEY_FROM)
    sample = int(KEY * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * frames, *([0] * quiet + [sample] * (frames - quiet))))


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
    report = Report(f"an input keys a plugin in a served show ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-key-input-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "key-input")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        inputs = room / "talkback.wav"
        (bundle / "media").mkdir(exist_ok=True)
        write_mono_constant(bundle / "media" / "tone.wav", SOURCE)
        write_talkback(inputs)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, input_wav=inputs, proxy_deadline_us=20000,
                    engine_folder=room / "engine") as server:
            report.check(common.wait_until(
                lambda: value_of(server, f"/godot/plugin/{PLUGIN}/state") == "loaded", timeout=30.0),
                "the test ducker's child comes up", str(value_of(server, f"/godot/plugin/{PLUGIN}/state")))
            report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/keyInputs"), 2,
                         "it offers a sidechain two channels wide")

            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= int(RATE * (KEY_FROM + 4.0)), timeout=60.0),
                "the render runs four seconds past the talkback's first word")
            first_sound.wait_for_render_tail(render)

        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render has the main pair", f"{channels} channels")

        if channels >= 2:
            start = first_sound.first_above(data[0], 0.01)
            key_at = int(RATE * KEY_FROM)
            report.check(0 <= start < key_at - RATE, "the cue is heard before the talkback speaks",
                         f"first sound at {start / RATE:.2f} s")

            if 0 <= start < key_at - RATE:
                before = level(data[0], max(start + RATE // 4, key_at - RATE))
                after = level(data[0], key_at + RATE)
                report.check(abs(before - PLAIN) <= TOLERANCE,
                             "with the talkback silent, the cue at the ducker's gain alone", f"{before:.4f}")
                report.check(abs(after - DUCKED) <= TOLERANCE,
                             "once it speaks, ducked by it", f"{after:.4f}")

                #  The step, on the left side: where it falls against where the input's does.
                step = next((n for n in range(key_at - 4 * BLOCK, key_at + 4 * BLOCK)
                             if abs(data[0][n]) < (PLAIN + DUCKED) / 2), -1)
                report.check(step >= 0 and abs(step - key_at) <= BLOCK,
                             "in the block the talkback arrives in", f"step at {step - key_at:+d} samples")

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
