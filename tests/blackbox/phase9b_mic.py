#!/usr/bin/env python3
"""The live rack, stage 9b.5 - a mic cue, heard through its rack channel.

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
(namespace draft 18.5 and 18.10): the `mic` fixture served on the hosted
interface with a steady level at its first input. GO fires the mic cue "Voix
solo": it claims its channel, Vox 1, whose own child - the test gain at a half
- comes up loaded; the input is heard through it at a half on both sides, the
plugin having made the mono voice stereo, after rising over the cue's
half-second fade-in rather than stepping. Esc shuts the input, the tail rings
out, the run ends and the channel is free - and silent. Fired again, and the
child killed from its own parameter, the plugin reads failed in words naming the
channel that plays without it, and the voice goes on dry. A double Esc ends it
at once. And `wfg replay` reproduces the session with no child at all.
"""

import argparse
import shutil
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "mic"

RATE = 48000
BLOCK = 128
INPUT = 0.25                    # the level at the first input
GAIN = 0.5                      # the test gain's default
STEADY = INPUT * GAIN
TOLERANCE = 0.02

MIC = "MC000002"
CHANNEL = "MC000011"
PLUGIN = "MC000012"
FX = "MC000005"


def write_inputs(path: Path, seconds: float = 30.0) -> None:
    """A steady level on the first input and silence on the second."""
    frames = int(RATE * seconds)
    level = int(INPUT * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * (frames * 2), *([level, 0] * frames)))


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def send(server: Server, address: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args or []))


def wait_for(server: Server, address: str, wanted, timeout: float = 10.0):
    common.wait_until(lambda: value_of(server, address) == wanted, timeout=timeout)
    return value_of(server, address)


def wait_for_frames(render: Path, frames: int, timeout: float = 30.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


def level(samples: "list[float]", start_frame: int, seconds: float = 0.5) -> float:
    """The median of the window's blocks' mean levels, as the plugin driver
    reads one: what the processed blocks are at, a late one or two apart."""
    part = samples[max(0, start_frame):max(0, start_frame) + int(RATE * seconds)]
    if len(part) < BLOCK:
        return 0.0
    blocks = sorted(sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK
                    for i in range(0, len(part) - BLOCK + 1, BLOCK))
    return blocks[len(blocks) // 2]


def shares(samples: "list[float]", start_frame: int, seconds: float = 0.5) -> str:
    """WHAT THE WINDOW'S BLOCKS WERE AT, each put with the level it is nearest -
    for a failure's detail line only, as the plugin driver's is."""
    part = samples[max(0, start_frame):max(0, start_frame) + int(RATE * seconds)]
    if len(part) < BLOCK:
        return "no blocks in the window"
    levels = {"dry": INPUT, "processed": STEADY, "silent": 0.0}
    blocks = [sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK)]
    named = [min(levels, key=lambda word: abs(levels[word] - b)) for b in blocks]
    return ", ".join(f"{named.count(word) / len(named):.0%} {word}" for word in levels if named.count(word))


def run(locale: "str | None", keep_log: "str | None" = None) -> int:
    report = Report(f"stage 9b.5: a mic cue, heard ({locale or 'C'})")
    with tempfile.TemporaryDirectory(prefix="wfg-phase9b-mic-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "mic")
        inputs = room / "inputs.wav"
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        write_inputs(inputs)

        stopped_at = fired_at = killed_at = 0

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, input_wav=inputs, proxy_deadline_us=20000,
                    engine_folder=room / "engine") as server:
            report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                         "Vox 1's own child comes up, and its plugin reads loaded")

            # GO: the standby is the mic cue.
            send(server, "/godot/cmd/go")
            holder = common.wait_until(lambda: value_of(server, f"/godot/slot/{CHANNEL}/holder") or None,
                                       timeout=10.0)
            report.check(bool(holder), "GO claims the channel: its holder is the mic cue's run", str(holder))
            report.equal(wait_for(server, f"/godot/run/{holder}/state", "playing") if holder else None,
                         "playing", "and the run sounds")

            went_at = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, went_at + int(RATE * 2.0)), "the render runs two seconds past GO")

            # Esc: the input shut, the tail rung out, the run over and the channel free.
            stopped_at = first_sound.frames_on_disk(render)
            send(server, "/godot/cmd/run/stopAll")
            freed = common.wait_until(lambda: not value_of(server, f"/godot/slot/{CHANNEL}/holder"), timeout=5.0)
            report.check(freed, "Esc frees the channel once the tail has rung out")
            report.equal(value_of(server, f"/godot/run/{holder}/state") if holder else None, "done",
                         "and the run is done")
            report.check(wait_for_frames(render, stopped_at + int(RATE * 1.5)), "the render runs on past the stop")

            # Fired again, then its plugin's child killed from its own parameter.
            send(server, "/godot/cmd/cue/fire", [MIC])
            again = common.wait_until(lambda: value_of(server, f"/godot/slot/{CHANNEL}/holder") or None,
                                      timeout=10.0)
            report.check(bool(again), "fired again, it takes the channel again", str(again))
            fired_at = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, fired_at + int(RATE * 1.5)), "the render runs past the second GO")

            killed_at = first_sound.frames_on_disk(render)
            send(server, "/godot/cmd/node/set", [f"/godot/fx/{FX}/p1", 1.0])
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "failed", timeout=5.0), "failed",
                         "p1 at one kills the child, and the plugin reads failed")
            problem = value_of(server, f"/godot/plugin/{PLUGIN}/problem") or ""
            report.check("Vox 1" in problem, "in words naming the channel that plays without it", problem)

            # DOWN FOR GOOD before the dry voice is measured. The host relaunches a
            # failed child once, two seconds on, and the lane still asks it to die,
            # so it fails again and stays down - which is where "dry" is a promise
            # and not a moment between two children (a restart on a slow CI runner
            # landed inside the window, 2026-09-26).
            down = common.wait_until(lambda: "stays down" in (value_of(server, f"/godot/plugin/{PLUGIN}/problem") or ""),
                                     timeout=10.0)
            report.check(down, "relaunched once, it fails again and stays down, saying so",
                         value_of(server, f"/godot/plugin/{PLUGIN}/problem") or "")
            killed_at = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, killed_at + int(RATE * 2.0)), "the render runs past the kill")

            # A double Esc: over at once.
            send(server, "/godot/cmd/run/killAll")
            report.check(common.wait_until(lambda: not value_of(server, f"/godot/slot/{CHANNEL}/holder"),
                                           timeout=2.0),
                         "a double Esc frees the channel at once")

            report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")
            first_sound.wait_for_render_tail(render)

        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render is stereo", f"{channels} channels")
        left = data[0] if data else []
        right = data[1] if len(data) > 1 else []
        start = first_sound.first_above(left, 0.001)
        report.check(start >= 0, "the mic cue was heard at all")

        if start >= 0 and len(left) > killed_at + int(RATE * 1.5):
            rising = level(left, start + int(RATE * 0.01), 0.03)
            steady = level(left, start + int(RATE * 1.0))
            beside = level(right, start + int(RATE * 1.0))
            after = level(left, stopped_at + int(RATE * 0.8))
            dry = level(left, killed_at + int(RATE * 1.0))

            report.check(rising < STEADY * 0.5,
                         "it rises over its half-second fade-in rather than stepping",
                         f"{rising:.4f} in its first forty milliseconds against {STEADY:.4f}")
            report.check(abs(steady - STEADY) <= TOLERANCE,
                         "through Vox 1's plugin at a half", f"{steady:.4f} against {STEADY:.4f}")
            report.check(abs(beside - STEADY) <= TOLERANCE,
                         "on both sides: the plugin made the mono voice stereo", f"{beside:.4f} against {STEADY:.4f}")
            report.check(after <= 0.001, "silent after Esc", f"{after:.4f}")
            report.check(abs(dry - INPUT) <= TOLERANCE,
                         "with its plugin's child dead, the voice goes on dry",
                         f"{dry:.4f} against {INPUT:.4f}; {shares(left, killed_at + int(RATE * 1.0))}")
        else:
            report.check(False, "the render is long enough to read every window",
                         f"{len(left)} frames, killed at {killed_at}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session with no child at all",
                     (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")

        # The session's log, handed out for a fixture: tests/fixtures/logs/mic.wfglog
        # was recorded this way.
        if keep_log:
            shutil.copyfile(log, keep_log)

    return report.finish()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wfg-locale", default=None)
    parser.add_argument("--keep-log", default=None, help="copy the session's log here")
    args = parser.parse_args()
    return run(args.wfg_locale, args.keep_log)


if __name__ == "__main__":
    sys.exit(main())
