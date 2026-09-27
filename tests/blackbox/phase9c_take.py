#!/usr/bin/env python3
"""Live sampling, stage 9c.3 - a take recorded, looped, layered and held.

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
(namespace draft 19.3 and 19.6): the `take` fixture served on the hosted
interface with a steady level at its first input. GO fires the mic cue "Loop
voice" on the sampling channel Looper, and its take waits (decision CF). The
transport cue "Rec" starts the take and "Loop it" closes it: the channel is
silent while it records - `through` is off, so the voice is not doubled - and
then plays the take at a half, the test gain before the recorder printed into
it; the engine logs the length the take closed at. The loop points are ridden
from the tree's door and read back. A layer laid by `take.overdub` adds the
input again on every pass round the loop, as a looper's overdub does, and
`take.undo` takes the whole layer off again. `take.keep` writes the take into
the show's media as a float WAV named after its channel (stage 9c.6), and Keep
as cue adds a media cue after the mic cue that plays it. Esc lets go of the
channel and the take is held, silent and kept; the mic cue "Scene 5 loop", whose GO loops the
take it finds, plays it again with its input heard through as well. Clear
empties the channel, and a double Esc ends it. And `wfg replay` reproduces the
session with no audio at all.
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
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "take"

RATE = 48000
BLOCK = 128
INPUT = 0.25                    # the level at the first input
GAIN = 0.5                      # the test gain's default, before the recorder
LOOPED = INPUT * GAIN           # what the take holds
TOLERANCE = 0.02

MIC = "TK000002"
SCENE_5 = "TK000008"
CHANNEL = "TK000011"
PLUGIN = "TK000012"


def write_inputs(path: Path, seconds: float = 40.0) -> None:
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


def take_word(server: Server) -> str:
    return value_of(server, f"/godot/slot/{CHANNEL}/take") or ""


def number(server: Server, row: str) -> float:
    value = value_of(server, f"/godot/slot/{CHANNEL}/{row}")
    try:
        return float(value)
    except (TypeError, ValueError):
        return -1.0


def read_float_wav(path: Path) -> "tuple[int, int, int, int, list[float]]":
    """(format, channels, rate, bits, the first channel's samples) of a WAV -
    the standard library's `wave` reads integer samples only, and a kept take
    is 32-bit float, plain or in the extensible header."""
    data = path.read_bytes()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        return 0, 0, 0, 0, []
    pos, fmt, channels, rate, bits, samples = 12, 0, 0, 0, 0, []
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt, channels, rate = struct.unpack("<HHI", body[0:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if fmt == 0xFFFE and len(body) >= 26:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif tag == b"data" and channels and bits == 32:
            count = size // 4
            values = struct.unpack("<%df" % count, body[:count * 4])
            samples = list(values[0::channels])
        pos += 8 + size + (size & 1)
    return fmt, channels, rate, bits, samples


def loudest_block(samples: "list[float]", start_frame: int, end_frame: int) -> float:
    """The loudest block's mean level between two frames."""
    part = samples[max(0, start_frame):max(0, end_frame)]
    blocks = [sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK)]
    return max(blocks) if blocks else 0.0


def logged_closes(log: Path) -> "list[str]":
    """The `take.closed` records the session's log holds, as written."""
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    return [line for line in lines if " take.closed " in line]


def run(locale: "str | None", keep_log: "str | None" = None) -> int:
    report = Report(f"stages 9c.3 and 9c.6: a take recorded, looped, layered, kept and held ({locale or 'C'})")
    with tempfile.TemporaryDirectory(prefix="wfg-phase9c-take-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "take")
        inputs = room / "inputs.wav"
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        write_inputs(inputs)

        marks: "dict[str, int]" = {}

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK, hosted=True,
                    render=render, input_wav=inputs, proxy_deadline_us=20000,
                    engine_folder=room / "engine") as server:
            report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                         "the Looper's own child comes up, and its plugin reads loaded")
            report.equal(take_word(server), "empty", "the channel's take starts empty")

            # GO: the mic cue claims the channel, and its take waits (decision CF).
            send(server, "/godot/cmd/go")
            holder = common.wait_until(lambda: value_of(server, f"/godot/slot/{CHANNEL}/holder") or None,
                                       timeout=10.0)
            report.check(bool(holder), "GO claims the channel: its holder is the mic cue's run", str(holder))
            report.equal(wait_for(server, f"/godot/run/{holder}/state", "playing") if holder else None,
                         "playing", "and the run sounds")
            report.equal(take_word(server), "empty", "GO leaves the take waiting: onGo is wait")
            report.check(wait_for_frames(render, first_sound.frames_on_disk(render) + int(RATE * 0.5)),
                         "the render runs half a second past GO")

            # REC, from a transport cue aimed at the mic cue - which plays on.
            send(server, "/godot/cmd/go")
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "recording"), "recording",
                         "the transport cue Rec starts the take")
            report.equal(value_of(server, f"/godot/slot/{CHANNEL}/holder"), holder,
                         "and the mic cue plays on: a press is not a stop")
            marks["recording"] = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, marks["recording"] + int(RATE * 1.2)),
                         "the render runs on while it records")

            # LOOP IT: the take closed, and the length it closed at in the log.
            send(server, "/godot/cmd/go")
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "looping"), "looping",
                         "the transport cue Loop it closes the take, which loops")
            marks["looping"] = first_sound.frames_on_disk(render)
            # The account loops at the press; the length is the recorder's, logged
            # as take.closed once the close has landed at the sample it was placed at.
            common.wait_until(lambda: number(server, "takeLength") > 0.0, timeout=5.0)
            length = number(server, "takeLength")
            report.check(0.5 <= length <= 5.0, "the take is as long as it was recorded for",
                         f"{length:.3f} s")
            closes = logged_closes(log)
            report.check(len(closes) == 1 and f'"{CHANNEL}"' in closes[0] and '"pressed"' in closes[0],
                         "the engine logs the length it closed at, as take.closed ... pressed",
                         "; ".join(closes) or "none logged")
            report.check(wait_for_frames(render, marks["looping"] + int(RATE * 1.5)),
                         "the render runs on while it loops")

            # THE POINTS, ridden from the tree's door and read back.
            send(server, "/godot/cmd/node/set", [f"/godot/slot/{CHANNEL}/loopIn", 0.1])
            send(server, "/godot/cmd/node/set", [f"/godot/slot/{CHANNEL}/loopOut", 0.4])
            report.check(common.wait_until(lambda: abs(number(server, "loopOut") - 0.4) < 1e-6, timeout=5.0)
                         and abs(number(server, "loopIn") - 0.1) < 1e-6,
                         "the loop points move from the tree's door, and read back",
                         f"in {number(server, 'loopIn')}, out {number(server, 'loopOut')}")
            # The points reach the recorder at the sample they were placed at, and a
            # playhead past the new out point jumps to the in point.
            inside = common.wait_until(lambda: 0.1 - 0.02 <= number(server, "playhead") <= 0.4 + 0.02,
                                       timeout=5.0)
            report.check(inside, "and the playhead goes round inside them", f"{number(server, 'playhead'):.3f} s")

            # A LAYER, laid by command and taken off again.
            send(server, "/godot/cmd/take/overdub", [CHANNEL])
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "overdubbing"), "overdubbing",
                         "take.overdub lays a layer")
            report.check(wait_for_frames(render, first_sound.frames_on_disk(render) + int(RATE * 0.8)),
                         "the render runs on while it lays")
            send(server, "/godot/cmd/take/loop", [CHANNEL])
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/takeLayers", 1), 1,
                         "take.loop closes the layer: one on the take")
            marks["layered"] = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, marks["layered"] + int(RATE * 1.2)),
                         "the render runs on with the layer")

            send(server, "/godot/cmd/take/undo", [CHANNEL])
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/takeLayers", 0), 0,
                         "take.undo takes it off again")
            marks["undone"] = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, marks["undone"] + int(RATE * 1.2)),
                         "the render runs on without it")

            # KEEP: the take made a file in the show's media, under a name the writer found free.
            length = number(server, "takeLength")
            send(server, "/godot/cmd/take/keep", [CHANNEL])
            kept = common.wait_until(lambda: value_of(server, f"/godot/slot/{CHANNEL}/kept") or None, timeout=10.0)
            report.equal(kept, "takes/Looper take 1.wav",
                         "take.keep writes the take into media/takes, named after its channel")
            written = bundle / "media" / "takes" / "Looper take 1.wav"
            report.check(written.exists(), "and the file is there, whole", str(written))
            if written.exists():
                fmt, wav_channels, rate, bits, samples = read_float_wav(written)
                report.check(fmt == 3 and wav_channels == 2 and rate == RATE and bits == 32,
                             "a stereo float WAV at the session's rate",
                             f"format {fmt}, {wav_channels} channels, {rate} Hz, {bits} bits")
                report.check(abs(len(samples) - round(length * RATE)) <= 1,
                             "as long as the take", f"{len(samples)} frames against {length:.4f} s")
                heard = sorted(abs(s) for s in samples if abs(s) > 0.002)
                middle = heard[len(heard) // 2] if heard else 0.0
                report.check(abs(middle - LOOPED) <= TOLERANCE,
                             "and it is the take as it loops: at a half, the layer undone",
                             f"{middle:.4f} against {LOOPED:.4f}")

            # KEEP AS CUE: a media cue after the one sounding there, looping the take.
            before = (value_of(server, "/godot/list/TK000001/order") or "").split()
            send(server, "/godot/cmd/take/keep", [CHANNEL, True])
            second = common.wait_until(
                lambda: (value_of(server, f"/godot/slot/{CHANNEL}/kept") or "").endswith("take 2.wav") or None,
                timeout=10.0)
            report.check(bool(second), "Keep as cue writes the next file", str(second))
            after = (value_of(server, "/godot/list/TK000001/order") or "").split()
            made = [cue for cue in after if cue not in before]
            report.check(len(made) == 1 and MIC in after and after.index(made[0]) == after.index(MIC) + 1,
                         "and a cue after the mic cue", f"{before} then {after}")
            if len(made) == 1:
                report.equal(value_of(server, f"/godot/cue/{made[0]}/kind"), "media", "a media cue")
                report.equal(value_of(server, f"/godot/cue/{made[0]}/file"), "takes/Looper take 2.wav",
                             "playing the file it wrote")

            # ESC: the channel let go of, and the take held - silent, and kept.
            send(server, "/godot/cmd/run/stopAll")
            freed = common.wait_until(lambda: not value_of(server, f"/godot/slot/{CHANNEL}/holder"), timeout=5.0)
            report.check(freed, "Esc frees the channel once the tail has rung out")
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "held"), "held",
                         "and the take is held, not lost")
            marks["held"] = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, marks["held"] + int(RATE * 1.0)), "the render runs on, held")

            # SCENE 5: a later mic cue loops the take it finds, its input heard through as well.
            send(server, "/godot/cmd/go")
            again = common.wait_until(lambda: value_of(server, f"/godot/slot/{CHANNEL}/holder") or None,
                                      timeout=10.0)
            report.check(bool(again) and again != holder, "GO on Scene 5 loop takes the channel", str(again))
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "looping"), "looping",
                         "and loops the take it found: onGo is loop")
            marks["scene5"] = first_sound.frames_on_disk(render)
            report.check(wait_for_frames(render, marks["scene5"] + int(RATE * 1.2)),
                         "the render runs on with the scene's loop")

            # CLEAR, then a double Esc: over at once.
            send(server, "/godot/cmd/take/clear", [CHANNEL])
            report.equal(wait_for(server, f"/godot/slot/{CHANNEL}/take", "empty"), "empty",
                         "take.clear empties the channel")
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

        if len(left) > marks.get("scene5", 0) + int(RATE * 1.2) > 0:
            quiet = loudest_block(left, marks["recording"] + int(RATE * 0.1), marks["looping"] - int(RATE * 0.2))
            report.check(quiet <= 0.001, "silent while it records: through is off, and nothing loops yet",
                         f"loudest block {quiet:.4f}")

            # WHAT THE LOOP PLAYS: the take, printed through the test gain before the
            # recorder. A block the child answered late is silent in the take, so the
            # level is read off the blocks it answered (namespace draft 18.13).
            starved = common.logged_before(log, "plugin.failed", containing="stopped answering")
            windows = (("looping", "the loop plays the take at a half: the plugin before it printed in"),
                       ("undone", "and after Undo it is the take alone again"))
            for mark, words in windows:
                start, end = marks[mark] + int(RATE * 0.4), marks[mark] + int(RATE * 1.1)
                for side, where in ((left, "left"), (right, "right")):
                    common.check_level(report, common.answered_level(side, start, end, BLOCK), LOOPED,
                                       TOLERANCE, starved, f"{words} ({where})")

            # THE LAYER: the input laid again on every pass while it was held - the
            # loop is 0.3 s and the overdub most of a second - so at least one pass
            # of it on the take, and every level a whole number of passes.
            layered = common.answered_level(left, marks["layered"] + int(RATE * 0.4),
                                            marks["layered"] + int(RATE * 1.1), BLOCK)
            passes = (layered[0] or 0.0) / LOOPED
            report.check(layered[0] is not None and passes >= 2.0 - 0.15 and abs(passes - round(passes)) <= 0.15,
                         "a layer on it adds the input again, a whole pass at a time",
                         f"{layered[0]} is {passes:.2f} times the take; {layered[1]} of {layered[2]} blocks flat")

            held = loudest_block(left, marks["held"] + int(RATE * 0.3), marks["held"] + int(RATE * 1.0))
            report.check(held <= 0.001, "silent while it is held", f"loudest block {held:.4f}")

            scene = common.answered_level(left, marks["scene5"] + int(RATE * 0.4), marks["scene5"] + int(RATE * 1.1),
                                          BLOCK)
            report.check(scene[0] is not None and scene[0] > LOOPED + TOLERANCE,
                         "Scene 5 is louder than the loop alone: its input is heard through as well",
                         f"{scene[0]} against the loop's {LOOPED:.4f}; {scene[1]} of {scene[2]} blocks flat")
        else:
            report.check(False, "the render is long enough to read every window",
                         f"{len(left)} frames; marks {marks}")

        # Keep as cue edits the show, which autosaves: the replay is given somewhere to save to.
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session with no audio at all",
                     (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")

        # The session's log, handed out for a fixture: tests/fixtures/logs/take.wfglog
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
