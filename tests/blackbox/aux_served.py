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
An aux in a SERVED show (namespace draft §93, and the fault found 2026-11-18).

The host's own tests build an aux by filling the graph's spec by hand. What
turns a show into that spec is `wfg serve`, and until 2026-11-18 it never said
which rack channels are auxes: a shared channel was built as a live-input
channel, and a send into it went nowhere. This serves a show with one mono cue
on the main pair and a send into a stereo aux whose own out is the pair after,
and listens there.

    - the cue is heard on the main pair;
    - and on the aux's pair, through the aux;
    - and the session replays.
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "aux-send"

RATE = 48000
BLOCK = 64
SOURCE = 0.25


def write_mono_constant(path: Path, seconds: float = 20.0) -> None:
    frames = int(RATE * seconds)
    sample = int(SOURCE * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * frames, *([sample] * frames)))


def level(samples: "list[float]", start_frame: int, seconds: float = 1.0) -> float:
    part = samples[start_frame:start_frame + int(RATE * seconds)]
    if len(part) < BLOCK:
        return -1.0
    blocks = sorted(sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK))
    return blocks[len(blocks) // 2]


def run(locale: "str | None") -> int:
    report = Report(f"an aux in a served show ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-aux-served-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "aux-send")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_mono_constant(bundle / "media" / "tone.wav")

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, engine_folder=room / "engine") as server:
            import socket
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                went_at = first_sound.frames_on_disk(render)
                sock.sendto(common.osc_encode("/godot/cmd/go", None), (common.HOST, server.osc_port))
                report.check(common.wait_until(
                    lambda: first_sound.frames_on_disk(render) >= went_at + int(RATE * 3.0), timeout=30.0),
                    "the render runs three seconds past GO")
                first_sound.wait_for_render_tail(render)
            finally:
                sock.close()

        channels, data = first_sound.read_render(render)
        report.check(channels >= 4, "the render has the main pair and the aux's", f"{channels} channels")

        if channels >= 4:
            start = first_sound.first_above(data[0], 0.01)
            report.check(start >= 0, "the cue is heard on the main pair")

            if start >= 0:
                at = start + int(RATE * 1.0)
                main = level(data[0], at)
                aux_left, aux_right = level(data[2], at), level(data[3], at)
                report.check(abs(main - SOURCE) <= 0.03, "at its level", f"{main:.4f}")
                report.check(aux_left >= 0.1, "and on the aux's left, through the aux", f"{aux_left:.4f}")
                report.check(aux_right >= 0.1, "and on its right", f"{aux_right:.4f}")

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
