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

"""Phase 2's done-when clause, driven from outside the process.

The devplan's criterion, word for word: *`Rien à faire` → load show → GO → sound
→ fade → GO → next cue, driven entirely over OSC, and the replay log reproduces
it.* This is that sentence as a program.

WHAT IS REAL HERE. The shipped binary, a generated Tracktion Edit, a real
playback graph, Go.dot's own output plugin and routing matrix, a UDP socket, an
OSCQuery server, and a WAV written from what actually came out. On the other end
of the network cue, a device written in another language from the specification
(`mock_target.py`), which agrees or disagrees on demand. Nothing in this file
links the library, includes a header, or knows how any of it works.

THE SOUND IS ASSERTED AS ARITHMETIC RATHER THAN AS A TOLERANCE, and that is what
the generated tone buys. Every sample of the media file is the same known
constant, so a routing coefficient of 0.5 means the right output carries exactly
half of it, a fade to −20 dB means exactly a tenth, and silence means zeros and
not "small". A test that asserted "roughly quiet" would pass against a bug that
made every cue 3 dB down.

WHY THE MEDIA FILE IS WRITTEN HERE. It is not in the repository: a committed WAV
is a blob nobody can review and no diff can explain, and — the real reason — a
file somebody replaced once would silently become the thing the assertions
measure. See `tests/fixtures/bundles/phase2/media/README.md`.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
The third is separate on purpose — "Go.dot is broken" and "this machine has no
build" want different people to look.
"""

from __future__ import annotations

import math
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "phase2"
MOCK = Path(__file__).resolve().parent / "mock_target.py"

RATE = 48000
BLOCK = 64

# The one number every sound assertion is derived from. 0.5 rather than 1.0 so
# that a bug which doubled a level shows up as a value out of range rather than
# as a clip nobody notices.
AMPLITUDE = 0.5


# =============================================================================
# The media file, and reading back what came out
# =============================================================================

def write_tone(path: Path, seconds: float = 6.0) -> None:
    """A file whose every sample is AMPLITUDE, in both channels.

    CONSTANT, NOT A SINE, and it is the whole reason the assertions below can be
    exact. With a steady value the output sample IS the gain: the first sample
    that is not zero is the moment the cue started, and the value at any instant
    is the level at that instant. A sine would need a window, a peak detector and
    a tolerance, and each of those is somewhere for a real error to hide.
    """
    path.parent.mkdir(parents=True, exist_ok=True)

    frames = int(RATE * seconds)
    sample = int(AMPLITUDE * 32767)
    block = struct.pack("<hh", sample, sample) * frames

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(block)


def read_render(path: Path) -> "tuple[int, list[list[float]]]":
    """(channel count, per-channel samples as floats in -1..1).

    RIFF PARSED BY HAND, because Go.dot renders 32-bit FLOAT and Python's `wave`
    module refuses format tag 3 outright - it reads integer PCM and nothing
    else. Which is the right format for Go.dot to write: the graph is float end
    to end, and quantising a render to 16 bits before anybody can look at it
    would put a rounding error between the engine and the assertion that is
    supposed to be measuring it.

    Twenty lines, no dependency, and it handles both tags so a change of render
    format is a different number here rather than a crash.
    """
    raw = path.read_bytes()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise HarnessError(f"{path} is not a RIFF/WAVE file")

    fmt_tag = bits = channels = 0
    data = b""
    at = 12

    while at + 8 <= len(raw):
        name = raw[at:at + 4]
        size = struct.unpack_from("<I", raw, at + 4)[0]
        body = raw[at + 8:at + 8 + size]

        if name == b"fmt ":
            fmt_tag, channels = struct.unpack_from("<HH", body, 0)
            bits = struct.unpack_from("<H", body, 14)[0]

            # WAVE_FORMAT_EXTENSIBLE keeps the real tag in its GUID's first two
            # bytes. Without this a perfectly ordinary float render reads as an
            # unknown format.
            if fmt_tag == 0xFFFE and len(body) >= 26:
                fmt_tag = struct.unpack_from("<H", body, 24)[0]
        elif name == b"data":
            data = body

        at += 8 + size + (size & 1)          # chunks are word aligned

    if channels == 0 or not data:
        raise HarnessError(f"{path} has no format or no samples")

    if fmt_tag == 3 and bits == 32:
        fmt, scale = "<f", 1.0
    elif fmt_tag == 1 and bits == 16:
        fmt, scale = "<h", 32768.0
    elif fmt_tag == 1 and bits == 32:
        fmt, scale = "<i", 2147483648.0
    else:
        raise HarnessError(f"{path} is format {fmt_tag} at {bits} bits, "
                           "which this cannot read")

    step = struct.calcsize(fmt)
    out = [[] for _ in range(channels)]

    for n in range(len(data) // step):
        value = struct.unpack_from(fmt, data, n * step)[0] / scale
        out[n % channels].append(value)

    return channels, out


def first_above(samples: "list[float]", floor: float) -> int:
    for n, value in enumerate(samples):
        if abs(value) > floor:
            return n

    return -1


def db(value: float, reference: float = AMPLITUDE) -> float:
    ratio = abs(value) / reference
    return 20.0 * math.log10(ratio) if ratio > 0.0 else -400.0


# =============================================================================
# The device on the other end of the network cue
# =============================================================================

class MockTarget:
    """`mock_target.py`, on ports it chose, in a process of its own."""

    def __init__(self, behaviour: str = "agree"):
        self.process = subprocess.Popen(
            [sys.executable, str(MOCK), f"--behaviour={behaviour}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        line = self.process.stdout.readline().split()

        if len(line) != 4 or line[0] != "osc" or line[2] != "query":
            self.stop()
            raise HarnessError(f"the mock target did not report its ports: {line!r}")

        self.osc_port = int(line[1])
        self.query_port = int(line[3])

    def __enter__(self) -> "MockTarget":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()


def point_mount_at(bundle: Path, target: MockTarget) -> None:
    """Rewrites the show's mount to name the ports the mock actually got.

    The bundle declares placeholders. Every port in this suite is chosen by the
    operating system — a fixed number makes a test that cannot run twice at once
    — so the document has to be told after the fact, which a copy of a bundle is
    exactly the right thing to do to.
    """
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")

    text = text.replace('port="9000"', f'port="{target.osc_port}"')
    text = text.replace('queryPort="5005"', f'queryPort="{target.query_port}"')

    show.write_text(text, encoding="utf-8", newline="\n")


# =============================================================================
# Driving
# =============================================================================

def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")

    if status != 200:
        return None

    try:
        return common.json.loads(body)["VALUE"][0]
    except (ValueError, KeyError, IndexError):
        return None


def wait_for(server: Server, address: str, expected, timeout: float = 20.0):
    """Waits for a node to reach a value. Returns what it ended up at."""
    deadline = time.monotonic() + timeout
    actual = None

    while time.monotonic() < deadline:
        actual = value_of(server, address)

        if actual == expected:
            return actual

        time.sleep(0.02)

    return actual


def runs_in(server: Server) -> "list[str]":
    """Every run the engine currently has, by id."""
    status, body = common.http_get(server.http_port, "/godot/run")

    if status != 200:
        return []

    try:
        return sorted(common.json.loads(body).get("CONTENTS", {}).keys())
    except ValueError:
        return []


def go(server: Server) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go"))


def wait_for_run_state(server: Server, run: str, expected: str,
                       timeout: float = 20.0) -> str:
    deadline = time.monotonic() + timeout
    actual = ""

    while time.monotonic() < deadline:
        actual = value_of(server, f"/godot/run/{run}/state") or ""

        if actual == expected:
            return actual

        time.sleep(0.02)

    return actual


def new_run(server: Server, before: "list[str]", timeout: float = 20.0) -> str:
    """The run that appeared since `before`, or "" if none did."""
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        fresh = [r for r in runs_in(server) if r not in before]

        if fresh:
            return fresh[0]

        time.sleep(0.02)

    return ""


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None") -> int:
    report = Report(f"phase 2: first sound ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-first-sound-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "phase2")
        render = room / "out.wav"
        log = room / "session.wfglog"
        saved = room / "saved"

        write_tone(bundle / "media" / "tone.wav")

        with MockTarget("agree") as target:
            point_mount_at(bundle, target)

            with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                        buffer_size=BLOCK, hosted=True, render=render) as server:

                # --- Rien à faire ------------------------------------------
                # The engine is up, the graph is built, and nothing is playing.
                # The phrase is the devplan's; the check is that a show which
                # has not been told to do anything does nothing at all.
                # WAITED FOR RATHER THAN ASSERTED, and the reason is a rule
                # rather than caution. The graph coming into being is an EVENT
                # (§3.15) - `audio.editBuilt`, submitted before serve prints its
                # ports and applied on the tick after it - so a client that read
                # this node the instant it could connect would be reading before
                # the engine had been asked. Everything in this file that looks
                # at state waits for it for the same reason.
                report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                             "the audio side comes up running, without being asked")
                report.equal(runs_in(server), [],
                             "nothing is running before the first GO")

                before = runs_in(server)

                # --- GO: sound ----------------------------------------------
                go(server)
                media = new_run(server, before)

                if not report.check(bool(media), "GO created a run"):
                    return report.finish()

                report.equal(wait_for_run_state(server, media, "playing"), "playing",
                             "the first cue is playing")
                report.equal(value_of(server, f"/godot/run/{media}/kind"), "media",
                             "and it is the media cue's run")

                # Long enough to be unmistakably sounding before the fade.
                time.sleep(0.6)

                # --- GO: fade -----------------------------------------------
                # Standby moved to the fade cue when the first GO fired, so the
                # second GO is the fade. Nobody addressed it by name: this is a
                # list being run, which is the thing being tested.
                go(server)

                level = wait_for(server, f"/godot/run/{media}/level", -20.0, timeout=8.0)
                report.check(level is not None and abs(float(level) + 20.0) < 0.001,
                             "the fade took the cue to -20 dB",
                             f"level ended at {level!r}")

                # --- GO: the network cue, verified --------------------------
                # The mock agrees, so this run should finish. It is the whole of
                # PRD 3.11 end to end: a message out over UDP, a value read back
                # over HTTP from another program, and a cue that reports done
                # only because the device confirmed it.
                before = runs_in(server)
                go(server)
                network = new_run(server, before)

                if report.check(bool(network), "GO created a run for the network cue"):
                    report.equal(wait_for_run_state(server, network, "done", 15.0), "done",
                                 "the network cue was verified by the device")
                    report.equal(value_of(server, f"/godot/run/{network}/error"), "",
                                 "and it did not fail")

                report.equal(value_of(server, "/godot/mount/K3PV7WRB/sent"), 1,
                             "one message left for the desk")

                # --- GO: the second media cue -------------------------------
                before = runs_in(server)
                go(server)
                second = new_run(server, before)

                if report.check(bool(second), "GO created a run for the second cue"):
                    report.equal(wait_for_run_state(server, second, "playing"), "playing",
                                 "the second cue is playing while the first still is")
                    report.check(value_of(server, f"/godot/run/{media}/state") == "playing",
                                 "and the first was not disturbed by it")

                time.sleep(0.4)

                # --- GO: the stop -------------------------------------------
                go(server)
                report.equal(wait_for_run_state(server, media, "done", 15.0), "done",
                             "the fade-and-stop ended the first cue")

                # AND THE SECOND ONE, by name, so the render ends with nothing
                # playing rather than with something the session forgot. This is
                # `run.kill` - the primitive Phase 10's three levels of stop will
                # be built on - reached over UDP like every other command.
                if second:
                    common.send_udp(server.osc_port,
                                    common.osc_encode("/godot/cmd/run/kill", [second]))
                    report.equal(wait_for_run_state(server, second, "done", 15.0), "done",
                                 "run.kill ended the second cue")

                # A SECOND, AND IT IS THE WRITER'S RATHER THAN THE ENGINE'S.
                # The render goes to disk through a background thread with a
                # two-second buffer, so the end of the FILE lags the end of the
                # SESSION - and `serve` is stopped with TerminateProcess on
                # Windows, which runs no handler and flushes nothing. Without
                # this wait the last thing in the WAV is whatever the writer had
                # got round to, which reads as a cue that never stopped.
                time.sleep(1.0)

                # --- what the machine says about itself ---------------------
                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "nothing of Go.dot's allocated on the audio thread")

                # REPORTED ALWAYS, ASSERTED LOOSELY, and the split is the
                # honest one. Lateness on a shared CI runner measures the
                # RUNNER: a macOS job sharing a box with three others fell a
                # tick and a half behind and failed this, which said nothing
                # about Go.dot. The number itself is what somebody wants, so it
                # goes in the output on every run whether it passes or not, and
                # the bound is generous enough that only a clock which has
                # actually stopped trips it.
                #
                # The tight version of this check lives where it can mean
                # something: the hardware checklist, on a machine that is not
                # being shared.
                lateness = value_of(server, "/godot/engine/latenessMax")
                worst = float(lateness) if lateness is not None else 1e9
                print(f"       latenessMax {worst:.0f} samples "
                      f"({worst / RATE * 1000:.1f} ms, "
                      f"{worst / (RATE / 50):.2f} ticks)")
                report.check(worst < RATE,
                             "the tick clock did not stop",
                             f"latenessMax was {lateness!r} samples - a whole second")

        # --- the sound itself ---------------------------------------------
        if not report.check(render.is_file(), "the render was written"):
            return report.finish()

        channels, samples = read_render(render)
        report.check(channels >= 2, "the render has the rig's channels",
                     f"got {channels}")

        left, right = samples[0], samples[1]

        began = first_above(left, 0.01)
        report.check(began >= 0, "there is sound in it at all")

        if began >= 0:
            # THE ROUTING, AS ARITHMETIC. The first cue's gains are `1 0.5`,
            # so with a constant source the left output carries the whole of it
            # and the right exactly half. A coefficient applied to the wrong
            # channel, or the row-major order reversed, is a different number
            # here rather than a quieter one.
            at = began + RATE // 20            # 20 ms in, clear of the start
            report.check(abs(abs(left[at]) - AMPLITUDE) < 0.01,
                         "the left output carries the cue at its coefficient",
                         f"expected {AMPLITUDE}, got {left[at]:.4f}")
            report.check(abs(abs(right[at]) - AMPLITUDE / 2) < 0.01,
                         "and the right carries exactly half of it",
                         f"expected {AMPLITUDE / 2}, got {right[at]:.4f}")

            # THE FADE'S DESTINATION, IN THE AUDIO, counted rather than
            # sampled at a moment.
            #
            # The first version of this looked at a fixed window a quarter of a
            # second in, and failed on a loaded runner where every tick had
            # slipped - which measured the runner's scheduler, not the fade.
            # What the fade actually guarantees is that the output SPENDS TIME
            # at its destination: -20 dB is a tenth of AMPLITUDE, and it sits
            # there for the whole gap between the fade cue and the stop cue.
            #
            # So: count the frames within 20% of a tenth, and require enough of
            # them to be a plateau rather than a slew passing through. A cue
            # that jumped instead of fading would cross this level for a
            # handful of samples; a cue that never faded would not reach it.
            target = AMPLITUDE / 10.0
            held = sum(1 for v in left[began:]
                       if target * 0.8 <= abs(v) <= target * 1.2)

            report.check(held >= RATE // 10,
                         "the fade is audible in the render, and it stays there",
                         f"only {held} frames ({held / RATE * 1000:.0f} ms) sat at "
                         f"-20 dB; wanted at least 100 ms")

        # DIGITAL SILENCE AT THE END, and exactly that. A cue faded to silence
        # and then stopped leaves zeros; anything else is a level nobody asked
        # for, running for the rest of the show, costing what a loud one costs.
        # DIGITAL SILENCE AT THE END, and exactly that. A cue faded to silence
        # and then stopped leaves zeros; anything else is a level nobody asked
        # for, running for the rest of the show, costing what a loud one costs
        # and denormalising while it does.
        #
        # Measured as "how long has it been silent for" rather than "is the last
        # 100 ms silent", because the second phrasing cannot tell a show that
        # ended properly from a file that was cut off mid-note.
        span = RATE // 10
        quiet = 0

        for n in range(len(samples[0]) - 1, -1, -1):
            if any(channel[n] != 0.0 for channel in samples):
                break
            quiet += 1

        report.check(quiet >= span,
                     "the render ends in digital silence, not something small",
                     f"only {quiet} of the last frames were zero "
                     f"({quiet / RATE * 1000:.0f} ms; wanted at least 100)")

        # --- and the log reproduces it -------------------------------------
        # NOT "did it crash". Replay re-executes every record against a fresh
        # engine and requires the same log out, which is the property the whole
        # phase was shaped around - and it does it with no audio engine, no
        # socket and no device. A verified cue replays because the read-back is
        # a record; a fade replays because its fifty values a second are not.
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={saved}",
                                        *( [f"--wfg-locale={locale}"] if locale else [] ))

        report.equal(code, 0, "the session's own log replays record for record",
                     (out + err).strip())

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"first_sound: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"first_sound: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    if not MOCK.is_file():
        print(f"first_sound: no mock target at {MOCK}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"first_sound: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
