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

"""A media cue's speed, heard (namespace draft §22), and M14.

WHAT THIS IS FOR. The unit suite proves the speed's arithmetic, the Runner's
clock against a fake player, and a real Tracktion graph driven by hand. What
none of them says is that the SHIPPED BINARY, serving a bundle whose media cues
carry a speed, plays them at it - in either mode, from the document - and
that a speed edited while a cue sounds and a fade cue that takes it to nought
are heard as they should be.

THE SOUND IS ONE TONE, a 750 Hz sine at a quarter of full scale: two seconds
of it for the four cues M14 measures, eight for the tape. Its period, 64
samples at 48 kHz, divides the block, so a block the hosted render drops on a
loaded machine (§17.12) costs a reading nothing but its own length.

THE BUNDLE IS tests/fixtures/bundles/rate:

  1  "Twice, varispeed"  - tone.wav at 2: a second long, at 1500 Hz.
  2  "Half, varispeed"   - at 0.5: four seconds, at 375 Hz.
  3  "Twice, stretched"  - at 2 in timestretch: a second, at 750 Hz still.
  4  "Half, stretched"   - at 0.5 in timestretch: four seconds, at 750 Hz.
  5  "Tape"              - tape.wav at 1, its speed edited to 1.5 while it
                           sounds, then:
  6  "Tape stop"         - a fade with only its speed switch on, taking the
                           tape to nought over a second: a tape stop. The cue
                           is still playing afterwards - stopped in time, not
                           ended - and exactly silent.

M14 (namespace draft §22.8) is what this prints for the first four: how long
each sounded and at what pitch - moved with the speed in varispeed, held in
timestretch.

ON A SHARED CI RUNNER THE TIMING IS NOT JUDGED (the author, 2026-09-28). A
cue's own length and pitch are the audio thread's and are judged everywhere;
WHEN an edit or a fade is heard follows the tick, so what is read around one
waits a margin past it, and nothing here times a tick.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import math
import os
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "rate"

RATE = 48000
BLOCK = 64
SAMPLES_PER_TICK = RATE // 50

TONE_HZ = 750.0
AMPLITUDE = 0.25
TONE_SECONDS = 2.0
TAPE_SECONDS = 8.0

# The four M14 measures: the cue, what it is, its speed, and whether it stretches.
M14 = [
    ("RT000002", "twice, varispeed", 2.0, False),
    ("RT000005", "half, varispeed", 0.5, False),
    ("RT000007", "twice, stretched", 2.0, True),
    ("RT000009", "half, stretched", 0.5, True),
]

TAPE = "RT00000B"
TAPE_EDITED = 1.5

# What a cue has to sound for: its file's length over its speed, to within a
# block or so in varispeed; a stretched cue also sounds the stretcher's own tail,
# a few milliseconds, until the clip is stopped. Before patch 0002 learned to
# prime a stretcher for its speed and after its file was read, the half-speed
# stretched cue was 52 ms short (2026-09-29). M14 prints all four.
LENGTH_TOLERANCE_S = {False: 0.010, True: 0.020}
PITCH_TOLERANCE = 0.02

FLOOR = 0.005                    # what counts as sounding
GAP_S = 0.3                      # the silence between two cues

# ON A SHARED CI RUNNER THE TIMING IS NOT JUDGED (lane_level's rule, the author's).
ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"
CI_REASON = ("timing is judged off CI: a shared runner holds the tick back for tens of "
             "milliseconds at a time (namespace draft §20.8)")


# =============================================================================
# The media, and reading it back out of the render
# =============================================================================

def write_tone(path: Path, seconds: float) -> None:
    """A stereo 750 Hz sine at a quarter of full scale."""
    path.parent.mkdir(parents=True, exist_ok=True)

    out = bytearray()

    for n in range(int(RATE * seconds)):
        sample = int(round(AMPLITUDE * math.sin(2.0 * math.pi * TONE_HZ * n / RATE) * 32767.0))
        out += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(bytes(out))


def bursts(samples: "list[float]", start: int = 0) -> "list[tuple[int, int]]":
    """The stretches of sound in `samples` from `start`, first and last frame
    of each, separated by at least GAP_S of silence."""
    found = []
    first = last = None
    gap = int(GAP_S * RATE)

    for n in range(start, len(samples)):
        if abs(samples[n]) <= FLOOR:
            continue

        if first is None:
            first = n
        elif n - last > gap:
            found.append((first, last))
            first = n

        last = n

    if first is not None:
        found.append((first, last))

    return found


def pitch(samples: "list[float]", first: int, last: int) -> float:
    """Hertz, from rising zero crossings between two frames."""
    count = 0

    for n in range(max(first, 1), last):
        if samples[n - 1] < 0.0 <= samples[n]:
            count += 1

    return count * RATE / max(1, last - first)


def silent(samples: "list[float]", first: int, last: int) -> bool:
    """Exactly: a stopped tape holds no value (decision DR)."""
    return all(samples[n] == 0.0 for n in range(first, min(last, len(samples))))


def timed(report: Report, ok: bool, words: str, detail: str) -> None:
    """A check whose answer depends on the tick keeping time: judged off CI only."""
    if ON_CI:
        report.void(words, f"{detail}; {CI_REASON}")
        return

    report.check(ok, words, detail)


def wait_for_frames(render: Path, frames: int, timeout: float = 60.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


def clock_frame(server: Server) -> int:
    """Where the audio had got to when the engine last ticked (lane_level's mark)."""
    try:
        _, answer = common.http_get(server.http_port, "/godot/engine")
        contents = common.json.loads(answer)["CONTENTS"]
        tick = int(contents["tick"]["VALUE"][0])
        lateness = int(contents["lateness"]["VALUE"][0])
        return tick * SAMPLES_PER_TICK + max(0, lateness)
    except Exception:
        return 0


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"a media cue's speed, heard ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-rate-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "rate")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_tone(bundle / "media" / "tone.wav", TONE_SECONDS)
        write_tone(bundle / "media" / "tape.wav", TAPE_SECONDS)

        edit_at = stop_at = 0

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")

            report.equal(first_sound.value_of(server, f"/godot/cue/{M14[0][0]}/rate"), 2.0,
                         "a cue's speed is published as the document says it")
            report.equal(first_sound.value_of(server, f"/godot/cue/{M14[2][0]}/rateMode"), "timestretch",
                         "and its mode")

            # --- M14: four cues, one after the other, each left to finish ------
            for cue, words, speed, stretched in M14:
                gone = first_sound.frames_on_disk(render)
                first_sound.go(server)

                run_id = first_sound.run_for_cue(server, cue)
                report.check(run_id != "", f"GO starts the {words} cue")

                report.check(wait_for_frames(render, gone + int(RATE * (TONE_SECONDS / speed + 1.5))),
                             f"the render runs past the {words} cue's end")

                if run_id:
                    report.equal(first_sound.wait_for_run_state(server, run_id, "done"), "done",
                                 f"and the {words} cue ends on its own, where its file does")

            # --- the tape: an edit while it sounds, then a fade to nought -------
            gone = first_sound.frames_on_disk(render)
            first_sound.go(server)

            tape_run = first_sound.run_for_cue(server, TAPE)
            report.check(tape_run != "", "GO starts the tape")
            report.check(wait_for_frames(render, gone + int(RATE * 1.5)), "and it plays a second and more")

            common.send_udp(server.osc_port,
                            common.osc_encode("/godot/cmd/node/set", [f"/godot/cue/{TAPE}/rate", TAPE_EDITED]))
            edit_at = clock_frame(server)

            report.equal(first_sound.wait_for(server, f"/godot/run/{tape_run}/rate", TAPE_EDITED), TAPE_EDITED,
                         "an edit of its speed reaches the sounding run")
            report.check(wait_for_frames(render, edit_at + int(RATE * 1.2)),
                         "and the render runs past it")

            first_sound.go(server)
            stop_at = clock_frame(server)

            report.equal(first_sound.wait_for(server, f"/godot/run/{tape_run}/rate", 0.0), 0.0,
                         "the fade takes the tape's speed to nought")
            report.check(wait_for_frames(render, stop_at + int(RATE * 2.5)),
                         "and the render runs on past it")

            report.equal(first_sound.value_of(server, f"/godot/run/{tape_run}/state"), "playing",
                         "a stopped tape is still a loaded one: the cue plays on, stopped in time")
            report.equal(first_sound.value_of(server, f"/godot/cue/{TAPE}/rate"), TAPE_EDITED,
                         "and the document still says the speed somebody decided, not the fade's")

            report.equal(first_sound.value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/run/stopAll"))

            first_sound.wait_for_render_tail(render)

        # --- what came out ---------------------------------------------------
        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render is stereo", f"{channels} channels")

        left = data[0] if data else []
        heard = bursts(left)

        if not report.check(len(heard) >= 5, "five sounds were heard: four cues and the tape",
                            f"{len(heard)} found"):
            return report.finish()

        measured = []

        for (cue, words, speed, stretched), (first, last) in zip(M14, heard):
            length = (last - first + 1) / RATE
            expected = TONE_SECONDS / speed
            middle = (first + int(0.2 * (last - first)), last - int(0.2 * (last - first)))
            hertz = pitch(left, *middle)
            wanted = TONE_HZ if stretched else TONE_HZ * speed
            measured.append(f"{words} {length:.3f} s of {expected:.3f}, {hertz:.1f} Hz of {wanted:.0f}")

            length_words = f"M14: the {words} cue lasts its file's length over its speed"
            length_detail = f"{length:.3f} s, {expected:.3f} expected"
            pitch_words = (f"M14: and sounds at {wanted:.0f} Hz - "
                           + ("its pitch held" if stretched else "its pitch moved with the speed"))

            if stretched:
                # A STRETCHED VOICE ON A SHARED RUNNER reads a few percent off (CI,
                # 2026-09-29: up to 1.030 s and 736 Hz on Windows, where a resampled
                # one on the same runner is exact) - a stretcher costs three to ten
                # times a resampler (M46), and a starved hosted render shows it
                # first. So it is judged to the letter where the machine keeps
                # time, and everywhere to what no fault could pass: the pitch held,
                # not moved, and the length its file's over its speed within a tenth.
                timed(report, abs(length - expected) <= LENGTH_TOLERANCE_S[True], length_words, length_detail)
                timed(report, abs(hertz - wanted) <= PITCH_TOLERANCE * wanted, pitch_words, f"{hertz:.1f} Hz")
                report.check(abs(length - expected) <= 0.1 and abs(hertz - wanted) <= 0.05 * wanted,
                             f"M14: and on any machine the {words} cue holds its pitch and lasts about its "
                             f"file's length over its speed", f"{length:.3f} s, {hertz:.1f} Hz")
            else:
                report.check(abs(length - expected) <= LENGTH_TOLERANCE_S[False], length_words, length_detail)
                report.check(abs(hertz - wanted) <= PITCH_TOLERANCE * wanted, pitch_words, f"{hertz:.1f} Hz")

        print("M14: " + "; ".join(measured))

        # The tape: 750 Hz before the edit, 1125 after, then silence - exactly.
        tape_first, tape_last = heard[4]

        before = pitch(left, tape_first + int(0.2 * RATE), tape_first + int(0.8 * RATE))
        report.check(abs(before - TONE_HZ) <= PITCH_TOLERANCE * TONE_HZ,
                     "the tape plays at its own pitch at one", f"{before:.1f} Hz")

        # Read from half a second past the edit - a tick and its horizon, and
        # room for a runner that held the tick back - to just before the fade.
        after = pitch(left, edit_at + int(0.5 * RATE), stop_at - int(0.05 * RATE))
        report.check(abs(after - TONE_HZ * TAPE_EDITED) <= PITCH_TOLERANCE * TONE_HZ * TAPE_EDITED,
                     "and at half as high again once its speed is edited to 1.5", f"{after:.1f} Hz")

        # WHEN it falls silent follows the tick: the fade's second, its horizon
        # and the last ramp - judged where the tick keeps time.
        timed(report, tape_last < stop_at + int(RATE * 1.3),
              "the tape stop falls silent within the fade, its horizon and a margin",
              f"last sound {(tape_last - stop_at) / RATE:.3f} s after the fade's GO")

        # And silent is EXACT from a tenth past the last sound - the gate's own
        # slope to its floor - until the cue was stopped.
        report.check(tape_last + int(0.1 * RATE) < stop_at + int(RATE * 2.4)
                     and silent(left, tape_last + int(0.1 * RATE), stop_at + int(RATE * 2.4)),
                     "and it is silent - exactly, no value held - while the cue still plays")

        # --- and the session replays -------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session", (out + err).strip()[-400:])
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
