#!/usr/bin/env python3
"""M33 - a parameter write to its sound: how long after node.set the render moves.

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

A MEASUREMENT, NOT A GATE (namespace draft §17.9): M26's idiom on the FX door.
The test-gain child on a voice, a constant source rendered hosted; the
frames-on-disk counter is read the moment `node.set p0` is sent, and the render
is searched for the frame where the level steps - the difference, in
milliseconds, is what a rotary feels: the tick (up to 20 ms), the push, and the
child's next block. Ten writes, alternating between a half and a whole, the
percentiles printed. Run by hand on a quiet machine:

    WFG_BINARY=<wfg> python tests/blackbox/m33_param_latency.py
"""

import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import first_sound  # noqa: E402
from common import Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "fx"
RATE = 48000
BLOCK = 64
MEDIA = "FX000002"
PLUGIN = "FX000006"
SOURCE = 0.25


def write_constant(path, seconds=30.0):
    frames = int(RATE * seconds)
    sample = int(SOURCE * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * (frames * 2), *([sample] * (frames * 2))))


def value_of(server, address):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def step_after(samples, start, target, window=RATE * 2):
    """The first frame at or after `start` where a 64-sample block's mean level is within 5% of target."""
    end = min(len(samples), start + window)
    for frame in range(start, end - BLOCK, BLOCK):
        part = samples[frame:frame + BLOCK]
        mean = sum(abs(s) for s in part) / BLOCK
        if abs(mean - target) <= target * 0.05:
            return frame
    return -1


def main(argv):
    import socket
    with tempfile.TemporaryDirectory(prefix="wfg-m33-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "fx")
        (bundle / "media").mkdir(exist_ok=True)
        write_constant(bundle / "media" / "tone.wav")
        render = room / "out.wav"
        sent = []
        with Server(bundle, hosted=True, sample_rate=RATE, buffer_size=BLOCK, render=render,
                    proxy_deadline_us=20000) as server:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

            def send(address, args=None):
                sock.sendto(common.osc_encode(address, args), (common.HOST, server.osc_port))

            assert common.wait_until(lambda: value_of(server, f"/godot/plugin/{PLUGIN}/state") == "loaded", timeout=20.0)
            send("/godot/cmd/fx/create", [MEDIA, PLUGIN])
            fx_id = None

            def made():
                nonlocal fx_id
                listed = value_of(server, f"/godot/cue/{MEDIA}/fx")
                if listed:
                    fx_id = str(listed).split()[0]
                return bool(fx_id)

            assert common.wait_until(made, timeout=10.0)
            send("/godot/cmd/go")
            assert common.wait_until(lambda: first_sound.frames_on_disk(render) >= RATE * 2, timeout=30.0)

            # THE ENGINE'S OWN CLOCK, not the file's size: the render on disk
            # lags the audio by up to the writer's flush interval, and a first
            # draft of this probe read that cadence as a half-second latency on
            # every other write. The tick is published once per tick, so the
            # reference is good to twenty milliseconds, plus one HTTP round trip.
            per_tick = int(value_of(server, "/godot/engine/samplesPerTick") or RATE // 50)
            level = 0.5
            for n in range(10):
                level = 1.0 if level == 0.5 else 0.5
                at = int(value_of(server, "/godot/engine/tick") or 0) * per_tick
                send("/godot/cmd/node/set", [f"/godot/fx/{fx_id}/p0", level])
                sent.append((at, level))
                time.sleep(1.5)

            first_sound.wait_for_render_tail(render)
            sock.close()

        channels, data = first_sound.read_render(render)
        left = data[0]
        latencies = []
        print("M33 - a parameter write to its sound (test gain, 48 kHz, 64-sample blocks, tick 50 Hz)")
        print("write | sent at frame | step at frame | latency ms")
        for at, level in sent:
            step = step_after(left, at, SOURCE * level)
            if step < 0:
                print(f"  ? | {at} | not found")
                continue
            ms = (step - at) * 1000.0 / RATE
            latencies.append(ms)
            print(f"  {level:.1f} | {at} | {step} | {ms:.1f}")
        if latencies:
            latencies.sort()
            print(f"latency ms: min {latencies[0]:.1f}, median {latencies[len(latencies) // 2]:.1f}, max {latencies[-1]:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
