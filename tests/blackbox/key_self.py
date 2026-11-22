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
A cue keys its own effect after its level, in a SERVED show (namespace draft
§141, SC.4), at the author's example: "the cue's sound after DCA on itself to
compress when loud and ease off the compression when the DCA brings down the
level".

The `key-self` fixture: one mono cue on the DCA "Band", through the test
ducker keyed by "this cue, after its level". The ducker turns the cue down by
its key: out = in x gain x (1 - |key|). With the key the cue times its level,
bringing the DCA down six decibels eases the ducking, so the cue comes down by
LESS than six decibels - the sum of the DCA and the eased ducker:

    - at the DCA's rest, the cue ducked by itself at full level;
    - six decibels down on the DCA, ducked by a half of itself: louder than
      the same cue would be with a key that ignored the DCA;
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "key-self"

RATE = 48000
BLOCK = 64
SOURCE = 0.5           # the cue
GAIN = 0.5             # the test ducker's gain at rest
DOWN = 0.5             # the DCA six decibels down
AT_REST = SOURCE * GAIN * (1.0 - SOURCE)                     # 0.125
EASED = SOURCE * GAIN * (1.0 - SOURCE * DOWN) * DOWN         # 0.09375
UNEASED = AT_REST * DOWN                                     # 0.0625: a key deaf to the DCA
TOLERANCE = 0.005

PLUGIN = "KS000006"
DCA = "KS000010"


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
    report = Report(f"a cue keys its own effect after its level ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-key-self-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "key-self")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_mono_constant(bundle / "media" / "tone.wav", SOURCE)

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, proxy_deadline_us=20000, engine_folder=room / "engine") as server:
            report.check(common.wait_until(
                lambda: value_of(server, f"/godot/plugin/{PLUGIN}/state") == "loaded", timeout=30.0),
                "the test ducker's child comes up", str(value_of(server, f"/godot/plugin/{PLUGIN}/state")))

            went_at = first_sound.frames_on_disk(render)
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go", None))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= went_at + int(RATE * 3.0), timeout=30.0),
                "the render runs three seconds past GO")

            trimmed_at = first_sound.frames_on_disk(render)
            common.send_udp(server.osc_port, common.osc_encode(
                "/godot/cmd/node/set", [f"/godot/dca/{DCA}/trim", "-6.0206"]))
            report.check(common.wait_until(
                lambda: isinstance(value_of(server, f"/godot/dca/{DCA}/trim"), (int, float))
                and abs(value_of(server, f"/godot/dca/{DCA}/trim") + 6.0206) < 0.01, timeout=10.0),
                "the DCA comes down six decibels", str(value_of(server, f"/godot/dca/{DCA}/trim")))
            report.check(common.wait_until(
                lambda: first_sound.frames_on_disk(render) >= trimmed_at + int(RATE * 3.0), timeout=30.0),
                "and the render runs three seconds past it")
            first_sound.wait_for_render_tail(render)

        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render has the main pair", f"{channels} channels")

        if channels >= 2:
            start = first_sound.first_above(data[0], 0.01)
            report.check(start >= 0, "the cue is heard")

            if start >= 0:
                before = level(data[0], start + RATE // 2)
                after = level(data[0], len(data[0]) - int(RATE * 1.5))
                report.check(abs(before - AT_REST) <= TOLERANCE,
                             "at the DCA's rest, ducked by itself at full level", f"{before:.4f}")
                report.check(abs(after - EASED) <= TOLERANCE,
                             "six decibels down, the ducking eased by the DCA", f"{after:.4f}")
                report.check(after > UNEASED + 2 * TOLERANCE,
                             "louder than a key deaf to the DCA would leave it", f"{after:.4f} > {UNEASED:.4f}")

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
