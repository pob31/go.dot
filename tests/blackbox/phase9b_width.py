#!/usr/bin/env python3
"""Finishing plugin hosting, stage 8 - a plugin makes a mono cue stereo, heard.

    This file is part of Go.dot - https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later

WHAT THIS PROVES, end to end and through nothing but the product's own doors
(the author's decision of 2026-09-26: a plugin can make a mono cue stereo): a
mono cue on a stereo direct out, through the widening test child (one in, two
out: left the input times the gain, right that at a half), is heard as two
different sides - left at a half, right at a quarter of a constant source;
switched out mid-cue, both sides play the mono source at full, exactly as a
mono cue always has; sent to a mono bus with the insert back in, the two sides
are heard summed at a half each; and the session replays with no child at all.

A CONSTANT SOURCE, read as the median of block means (phase9a_fx.py says why).
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "width"

RATE = 48000
BLOCK = 64
MEDIA = "WD000002"
PLUGIN = "WD000006"
FX = "WD000007"
CENTRE = "WD000008"
SOURCE = 0.25
GAIN = 0.5
TOLERANCE = 0.03


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


class Hand:
    def __init__(self, server: Server):
        import socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.port = server.osc_port

    def send(self, address: str, args: "list | None" = None) -> None:
        self.sock.sendto(common.osc_encode(address, args), (common.HOST, self.port))

    def close(self) -> None:
        self.sock.close()


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def wait_for(server: Server, address: str, expected, timeout: float = 20.0):
    seen = None

    def ready():
        nonlocal seen
        seen = value_of(server, address)
        return seen == expected

    common.wait_until(ready, timeout=timeout)
    return seen


def wait_for_frames(render: Path, frames: int, timeout: float = 30.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


def run(locale: "str | None") -> int:
    report = Report(f"stage 8: a plugin makes a mono cue stereo ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-phase9b-width-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "width")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_mono_constant(bundle / "media" / "tone.wav")

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, proxy_deadline_us=20000, engine_folder=room / "engine") as server:
            hand = Hand(server)
            try:
                report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                             "the widening test child comes up")
                report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/layout"), "mono in, stereo out",
                             "and says what it takes, in words")
                report.equal(wait_for(server, f"/godot/cue/{MEDIA}/chainChannels", 2), 2,
                             "the mono cue reads two channels wide through it")

                hand.send("/godot/cmd/go")
                report.check(wait_for_frames(render, int(RATE * 3.0)), "the render runs three seconds past GO")

                switched_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/node/set", [f"/godot/fx/{FX}/enabled", False])
                report.equal(wait_for(server, f"/godot/cue/{MEDIA}/chainChannels", 1), 1,
                             "switched out, the cue reads mono again")
                report.check(wait_for_frames(render, switched_at + int(RATE * 3.0)),
                             "and the render runs three seconds past the switch")

                centred_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/node/set", [f"/godot/fx/{FX}/enabled", True])
                hand.send("/godot/cmd/node/set", [f"/godot/cue/{MEDIA}/directOut", CENTRE])
                report.equal(wait_for(server, f"/godot/cue/{MEDIA}/directOut", CENTRE), CENTRE,
                             "switched back in and sent to the mono bus")
                report.check(wait_for_frames(render, centred_at + int(RATE * 3.0)),
                             "and the render runs three seconds past that")
                first_sound.wait_for_render_tail(render)
            finally:
                hand.close()

        channels, data = first_sound.read_render(render)
        report.check(channels >= 3, "the render has the stereo bus and the mono one", f"{channels} channels")

        if channels >= 3:
            left, right, centre = data[0], data[1], data[2]
            start = first_sound.first_above(left, 0.01)
            report.check(start >= 0, "the cue was heard at all")

            if start >= 0 and len(left) > centred_at + int(RATE * 2.0):
                wide_l, wide_r = level(left, start + int(RATE * 1.0)), level(right, start + int(RATE * 1.0))
                off_l, off_r = level(left, switched_at + int(RATE * 1.5)), level(right, switched_at + int(RATE * 1.5))
                mid = level(centre, centred_at + int(RATE * 1.5))
                mid_l = level(left, centred_at + int(RATE * 1.5))

                report.check(abs(wide_l - SOURCE * GAIN) <= TOLERANCE and abs(wide_r - SOURCE * GAIN / 2) <= TOLERANCE,
                             "through the widener the two sides differ: left at a half, right at a quarter",
                             f"left {wide_l:.4f}, right {wide_r:.4f}")
                report.check(abs(off_l - SOURCE) <= TOLERANCE and abs(off_r - SOURCE) <= TOLERANCE,
                             "switched out, the mono cue plays on both sides at full, as it always has",
                             f"left {off_l:.4f}, right {off_r:.4f}")
                report.check(abs(mid - SOURCE * GAIN * 0.75) <= TOLERANCE and mid_l <= 0.01,
                             "on the mono bus the two sides are summed at a half each, and the stereo bus is quiet",
                             f"centre {mid:.4f} against {SOURCE * GAIN * 0.75:.4f}, left {mid_l:.4f}")
            else:
                report.check(False, "the render is long enough to read every window", f"{len(left)} frames")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session with no child at all", (out + err).strip()[-400:])

    return report.finish()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wfg-locale", default=None)
    args = parser.parse_args()
    return run(args.wfg_locale)


if __name__ == "__main__":
    sys.exit(main())
