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

"""Time groups and chase, driven from outside the process (namespace draft 60,
TG.11).

WHAT THIS FILE IS FOR. The unit suite proves each piece of incoming timecode
against the engine's own pieces: the decoder fed by Go.dot's generator, the MTC
reader fed by Go.dot's cursor, the watchers, the firing and the chase against a
fake player that records what it is asked. What none of it can prove is the
product: `wfg serve` hearing LTC on an input and MTC on a port, starting a time
group's members on the code's sample, steering its chasers - all of it heard in
a render. Here the code is written by a FOURTH implementation of the standards,
in Python, sharing nothing with the generator, the decoder, the MTC cursor or
the reader: an LTC writer (biphase mark, the sync word, BCD, the polarity bit,
each edge placed to its fraction of a sample) into the WAV `--input-wav` loops,
and an MTC writer (full frames and quarter frames) into the file `--midi-play-
loop` plays into the port's input. And every member's own sound is itself LTC
from 00:00:00:00, so the render, decoded by timecode.py's reader, says frame by
frame where in its file each member was on every sample - which, against the
code the driver wrote, is how far it was out of step.

THE SHOW (tests/fixtures/bundles/timegroup): a one-channel input "LTC in"
(`sync/ltcIn`), a port "MTC in" (`sync/mtcIn`), a one-channel output for each
member, `<Lists goDebounce="0">`. A time group on LTC holding, all at
01:00:02:00, a free runner, a chaser by jump, a chaser by varispeed and a chaser
by timestretch (whose sound is a 1 kHz tone in bursts, since a stretcher would
not leave LTC readable and a tone's pitch is what it keeps), and at 01:00:03:00
a timeline group chasing by jump, its two sounds 1.5 s apart; and a time group
on MTC holding a free runner and a chaser by jump at 01:00:02:00. The media are
written by the driver into its copy, never into the fixture.

THE STORY, the same for the LTC on the input and the MTC on the port, 75 s
looped: the code from 00:59:40:00 at +50 ppm (twenty seconds of lead-in, for the
GOs on a slow runner, then the members' times); at 01:00:12:00 a step to +0.1 %
for fifteen seconds, at which a chaser by jump at its own speed is ten
milliseconds out after some nine; a stop of two seconds; back 120 ms further on
than where it would have been, so the catch-up chasers have something to catch
up and the jump chaser something to jump; eighteen seconds on, a locate back
thirty seconds; eight seconds on, the loop point, itself a locate back. The GOs
land in the first pass's lead-in on a workstation; a runner that took longer is
read a pass on.

WHAT IT PROVES, from the render and the log: a member starts on the very sample
the engine's map gives its time, within a millisecond of the input frame its
time names (LTC) and within a few on MTC; the free runner plays at its own speed
and is never moved; each chaser follows the drift by its method - the jump
chaser's decoded position steps back into step, the varispeed one's speed (its
pitch) moves with the code and by 1 % while it catches up, the timestretch one
keeps its pitch throughout; the stop leaves every chaser playing; the return
and the locate re-seat the chasers and leave the free runner; the loop point
ends the chasers whose time is ahead again; `code.heard` is sparse - its turns
and a few re-anchors, not one a frame - from `ltc:<input>` and `mtc:<port>`;
and a second serve, held (`--hold`), fires the same: nothing a time group does
is a send, and sound is never held.

TIMING IS JUDGED LOOSELY ON CI ONLY WHERE THE CHECK IS THE RUNNER'S. LTC is in
the render's own samples: where it is heard, where a member starts and where a
chaser stands are the same on every machine - unless a launch came late, which
the engine owns up to (`run.late` in whole blocks, and its log's notes that the
tick thread ran tens of milliseconds behind, as a Debug engine loading sounds
does), and which voids that one start's checks and nothing else. A record is
applied on a tick, though, and a tick that ran late applied it later than the
tick's number says: where the driver reads when something happened from a
record's tick - the stop said, the map a chaser is held to - it allows the
lateness the engine's log notes there, never more. MTC is not in the render's
samples: it arrives on the steady clock and reaches them through the tick's clock
pairs, which a starved runner takes late - so on CI where an MTC member stands,
and how often the MTC watcher re-anchors, are printed and not judged, and off
CI a turn's placing is judged to MTC_PLACED_MS. A chaser's steering is decided
on the tick, so on CI its bounds widen. A member the engine owned up to
launching late is held to the chase law from where it began - no further from
its code than that, and at the code's speed except where it was past the law's
tolerance and catching up - and a run whose MTC arrived more than
MTC_LEAD_LIMIT ahead of the render, the rig's hosted pump fallen behind the wall
clock, says nothing of MTC: what it would say is the rig's (`mtc_lead`).

By hand, `--keep=<folder>` leaves both rooms there (`main`, `held`: the show,
the input, the MTC file, the render and the log) and `--analyse=<folder>`
judges rooms kept before without serving again. On a runner,
`WFG_KEEP_ROOMS=<folder>` keeps them in `<folder>/<locale>` when anything did not
hold, and takes them away when everything did: a runner's starvation cannot be
had again on demand, so the rooms of its red are the evidence, and CI uploads
them (.github/workflows/ci.yml).

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import array
import math
import os
import re
import statistics
import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import timecode as tc
from common import HarnessError, Report, Server
from first_sound import go, run_for_cue, value_of, wait_for, frames_on_disk, wait_for_render_tail

ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "timegroup"

SAMPLE_RATE = 48000
FPS = 25
FRAME = SAMPLE_RATE // FPS            # 1920 samples a frame at 25 and 48 kHz
BLOCK = 128
TICK = SAMPLE_RATE // 50
AMPLITUDE = 0.5                       # every file the driver writes, LTC and tone alike
MS = SAMPLE_RATE / 1000.0             # samples a millisecond

LTC_GROUP = "TG0G0001"
SCENE = "TG0G0002"
MTC_GROUP = "TG0G0003"
INPUT = "TG0N0001"
PORT = "TG0P0001"
LTC_ORIGIN = f"ltc:{INPUT}"
MTC_ORIGIN = f"mtc:{PORT}"


def frame_of(label: str) -> int:
    hours, minutes, seconds, frames = tc.parse_label(label)
    return ((hours * 60 + minutes) * 60 + seconds) * FPS + frames


def label_of(frame: float) -> str:
    whole = int(math.floor(frame)) % (24 * 3600 * FPS)
    return tc.label_text((whole // (3600 * FPS), whole // (60 * FPS) % 60, whole // FPS % 60, whole % FPS))


def digits(frame: int) -> "tuple[int, int, int, int]":
    return (frame // (3600 * FPS) % 24, frame // (60 * FPS) % 60, frame // FPS % 60, frame % FPS)


# =============================================================================
# LTC, written from SMPTE 12M's description
# =============================================================================

SYNC = (0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1)


def ltc_bits(frame: int) -> "list[int]":
    """The eighty bits of one frame of 25 fps LTC, bit 0 first: the units and
    tens of the frames, seconds, minutes and hours in BCD, the user bits and
    the flags nought, the sync word 0011 1111 1111 1101 at the end."""
    hours, minutes, seconds, frames = digits(frame)
    bits = [0] * 80

    def put(first: int, count: int, value: int) -> None:
        for k in range(count):
            bits[first + k] = (value >> k) & 1

    put(0, 4, frames % 10)
    put(8, 2, frames // 10)
    put(16, 4, seconds % 10)
    put(24, 3, seconds // 10)
    put(32, 4, minutes % 10)
    put(40, 3, minutes // 10)
    put(48, 4, hours % 10)
    put(56, 2, hours // 10)
    bits[64:80] = SYNC
    # The biphase mark polarity correction, at bit 59 in 25-frame code (bit 27
    # at 30): it makes the frame's ones even, so every frame begins on the
    # same edge and the code reads whichever way round the line is wired.
    bits[59] = sum(bits) % 2
    return bits


class Story:
    """A code's story in samples of the line: stretches of code, each from a
    whole frame for a whole number of frames at a speed (code seconds a
    second), and stretches of silence."""

    def __init__(self, pieces):
        self.pieces = []
        at = 0.0
        for piece in pieces:
            if piece[0] == "code":
                _, first, frames, speed = piece
                length = frames * FRAME / speed
                self.pieces.append(("code", at, at + length, first, frames, speed))
                at += length
            else:
                length = piece[1] * SAMPLE_RATE
                self.pieces.append(("quiet", at, at + length, None, 0, 0.0))
                at += length
        self.length = at

    def code_at(self, t: float):
        """The code's frame of the day, with its fraction, at sample `t` - or
        None where it is quiet or past the end."""
        for kind, start, end, first, _frames, speed in self.pieces:
            if start <= t < end:
                return None if kind == "quiet" else first + (t - start) * speed / FRAME
        return None

    def speed_at(self, t: float) -> float:
        for kind, start, end, _first, _frames, speed in self.pieces:
            if start <= t < end:
                return speed
        return 0.0

    def sample_of(self, frame: float, after: float = 0.0):
        """The first sample at or after `after` where the code reads `frame`,
        or None."""
        for kind, start, end, first, frames, speed in self.pieces:
            if kind != "code" or end <= after:
                continue
            if first <= frame < first + frames:
                t = start + (frame - first) * FRAME / speed
                if t >= after:
                    return t
        return None


def ltc_edges(story: Story):
    """(sample, level) for every edge the story's code makes: biphase mark,
    every bit cell beginning with a change of level and a one changing again
    half way; out of silence the first edge rises, into silence the line
    falls to nought on the frame boundary where the code stops."""
    edges = []
    level = 0
    for kind, start, _end, first, frames, speed in story.pieces:
        if kind == "quiet":
            if level != 0:
                edges.append((start, 0))
                level = 0
            continue
        bit = FRAME / speed / 80.0
        for k in range(frames):
            begin = start + k * FRAME / speed
            for i, value in enumerate(ltc_bits(first + k)):
                level = 1 if level == 0 else -level
                edges.append((begin + i * bit, level))
                if value:
                    level = -level
                    edges.append((begin + (i + 0.5) * bit, level))
    if level != 0:
        edges.append((story.length, 0))
    return edges


def write_levels(path: Path, edges, length: int, amplitude: float = AMPLITUDE) -> None:
    """A mono 16-bit WAV at -1, 0 and +1 times `amplitude`, changing at
    `edges`. A sample an edge falls inside holds each level for its share of
    the sample, so the edge is where it was asked to be to a fraction of a
    sample - which is how Go.dot's decoder, placing an edge by its area,
    finds it."""
    full = int(round(amplitude * 32767))
    out = array.array("h", bytes(2 * length))
    level = 0
    at = 0
    for when, new in edges:
        n = int(math.floor(when))
        if n >= length:
            break
        if n > at:
            out[at:n] = array.array("h", [level * full]) * (n - at)
        part = when - n
        out[n] = int(round((level * part + new * (1.0 - part)) * full))
        at = n + 1
        level = new
    if at < length:
        out[at:length] = array.array("h", [level * full]) * (length - at)
    write_mono(path, out)


def write_mono(path: Path, samples: "array.array") -> None:
    if sys.byteorder != "little":
        samples = array.array("h", samples)
        samples.byteswap()
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(samples.tobytes())


# =============================================================================
# MTC, written from the MIDI specification
# =============================================================================

MTC_RATE_CODE = 1                  # 25 fps, in bits 5 and 6 of the hours


def full_frame(frame: int) -> str:
    hours, minutes, seconds, frames = digits(frame)
    return f"F0 7F 7F 01 01 {(MTC_RATE_CODE << 5) | hours:02X} {minutes:02X} {seconds:02X} {frames:02X} F7"


def quarter_frames(frame: int) -> "list[str]":
    """The eight quarter frames of a cycle naming `frame`: pieces 0 to 7, the
    low and high nibbles of the frames, seconds, minutes and hours, the last
    carrying the rate's code above the hours' top bit."""
    hours, minutes, seconds, frames = digits(frame)
    nibbles = (frames & 0x0F, frames >> 4, seconds & 0x0F, seconds >> 4,
               minutes & 0x0F, minutes >> 4, hours & 0x0F, (hours >> 4) | (MTC_RATE_CODE << 1))
    return [f"F1 {(piece << 4) | nibble:02X}" for piece, nibble in enumerate(nibbles)]


def mtc_lines(story: Story) -> "list[str]":
    """The story as a `--midi-render` file, `sent_ns due_ns bytes` a line: a
    full frame wherever the code starts or locates, as a machine sends one on
    a locate, then cycles of quarter frames, each naming the frame its piece 0
    is sent on, a quarter of a frame apart at the code's speed. A stretch with
    an odd number of frames leaves its last frame without a cycle. Moments in
    nanoseconds from the first line, which `--midi-play` plays at once."""
    lines = []
    last_frame = None
    for kind, start, _end, first, frames, speed in story.pieces:
        if kind != "code":
            last_frame = None
            continue
        quarter = FRAME / speed / 4.0
        if first != last_frame:
            lines.append((start, full_frame(first)))
        for cycle in range(frames // 2):
            frame = first + 2 * cycle
            for piece, message in enumerate(quarter_frames(frame)):
                lines.append((start + (8 * cycle + piece) * quarter, message))
        last_frame = first + frames
    out = []
    for when, message in lines:
        ns = int(round(when * 1e9 / SAMPLE_RATE))
        out.append(f"{ns} {ns} {message}")
    return out


# =============================================================================
# The story, and the members' own media
# =============================================================================

STORY = Story([
    ("code", frame_of("00:59:40:00"), 800, 1.00005),   # lead-in, then the members' times, +50 ppm
    ("code", frame_of("01:00:12:00"), 375, 1.001),     # a tenth of a percent fast
    ("quiet", 2.0),                                     # a stop
    ("code", frame_of("01:00:29:03"), 450, 1.00005),   # back, 120 ms on from where it would be
    ("code", frame_of("01:00:17:03"), 200, 1.00005),   # a locate back 30 s
])

LOOP = int(math.ceil(STORY.length))         # the input file's length: --input-wav loops it
STEP_AT = STORY.pieces[1][1]
STOP_AT = STORY.pieces[2][1]
RETURN_AT = STORY.pieces[3][1]
LOCATE_AT = STORY.pieces[4][1]
RETURN_LEAP = 0.120                          # seconds the return lands beyond where the code would be
LEAD_IN = STORY.sample_of(frame_of("01:00:00:00"))

MEMBER_SECONDS = 50.0
BURST_PERIOD = SAMPLE_RATE // 2              # a burst every half second
# Five lengths in turn, 30 ms apart: which of them a burst is says which of
# five it is, so the burst a chaser plays is told for sure up to a second and a
# quarter either way of where the code says it should be.
BURST_LENGTHS = tuple(int(seconds * SAMPLE_RATE) for seconds in (0.20, 0.23, 0.26, 0.29, 0.32))
TONE_HZ = 1000.0

# How far from where the port's pass puts a moment Go.dot may place MTC, judged
# loosely: MTC's moments reach the samples through the tick's clock pairs, and a
# Debug engine playing eight voices holds its tick back by up to some fifty
# milliseconds, which moves them by several (M64). The sparse check below is
# the one that says so.
MTC_PLACED_MS = 20.0

# How far from its code a timestretch chaser may be heard (AJC): a stretcher's
# chunk of 256 samples on its clock, and up to three more in the stretcher
# itself (M14) - some 21 ms at 48 kHz.
STRETCHED_MS = 4 * 256 * 1000.0 / SAMPLE_RATE
# THE CHASE LAW'S BAND (timecode/Chase.h, AIS): a catch-up chaser is steered
# only once it is more than `sync/chaseTolerance` out of step, which the fixture
# leaves at its 10 ms - within it, it is in step and runs at the code's speed.
CHASE_TOLERANCE_MS = 10.0
# Where a stretched voice's catch-up ends: one of the stretcher's chunks (AJC).
STRETCH_SETTLE_MS = 256 * 1000.0 / SAMPLE_RATE
# A TICK THAT KEPT UP WITH ITS AUDIO: the engine notes its tick thread only once
# it is two ticks behind. Every healthy room kept notes it at most 60 ms behind
# after its first three seconds; every overloaded one from 100 ms to seconds
# (CI's red rooms of TG.12's CI run, part 3: 2.5 and 2.7 s).
TICK_BEHIND_LIMIT_MS = 100.0
TICK_WARM_UP = 150
# How far the unheld serve's tick fell behind, handed to the held one with the
# starts it is held to (no cue is named so).
BEHIND_KEY = "@behind"


def write_member(path: Path, length: float = MEMBER_SECONDS) -> None:
    """A member's own sound: LTC from 00:00:00:00 at speed one, every edge on
    a whole sample, so what the render plays says, frame by frame, where in its
    file the member is - frame k begins on the file's sample k * 1920."""
    edges = ltc_edges(Story([("code", 0, int(length * FPS), 1.0)]))
    write_levels(path, edges, int((length + 0.2) * SAMPLE_RATE))


def write_bursts(path: Path, length: float = MEMBER_SECONDS) -> None:
    """The timestretch chaser's sound: a 1 kHz tone in a burst every half
    second, the bursts 200, 230, 260, 290 and 320 ms long in turn, each with
    2 ms raised-cosine ends - where a burst is, and which of the five it is,
    say where in its file the member is, and the tone says its pitch."""
    full = AMPLITUDE * 32767
    ramp = int(0.002 * SAMPLE_RATE)
    periods = []
    for burst in BURST_LENGTHS:
        one = array.array("h", [0] * BURST_PERIOD)
        for n in range(burst):
            gain = 1.0
            if n < ramp:
                gain = 0.5 - 0.5 * math.cos(math.pi * n / ramp)
            elif n >= burst - ramp:
                gain = 0.5 - 0.5 * math.cos(math.pi * (burst - n) / ramp)
            one[n] = int(round(full * gain * math.sin(2.0 * math.pi * TONE_HZ * n / SAMPLE_RATE)))
        periods.append(one)
    out = array.array("h")
    for k in range(int(length * SAMPLE_RATE) // BURST_PERIOD):
        out.extend(periods[k % len(periods)])
    write_mono(path, out)


class Member:
    """One member: its cue, the output channel it is heard on, its words, how
    it chases, its group's source, its time, and the seconds after its time
    its sound starts (a scene's second sound)."""

    def __init__(self, cue, channel, words, chase, source, at, offset=0.0, sound="ltc"):
        self.cue = cue
        self.channel = channel
        self.words = words
        self.chase = chase
        self.source = source
        self.at = frame_of(at)
        self.offset = offset
        self.sound = sound


FREE = Member("TG0M0001", 0, "the free runner", "free", "ltc", "01:00:02:00")
JUMP = Member("TG0M0002", 1, "the jump chaser", "jump", "ltc", "01:00:02:00")
VARI = Member("TG0M0003", 2, "the varispeed chaser", "varispeed", "ltc", "01:00:02:00")
STRETCH = Member("TG0M0004", 3, "the timestretch chaser", "timestretch", "ltc", "01:00:02:00", sound="bursts")
SCENE_A = Member("TG0M0005", 4, "the scene's first sound", "jump", "ltc", "01:00:03:00")
SCENE_B = Member("TG0M0006", 5, "the scene's second sound", "jump", "ltc", "01:00:03:00", 1.5)
MTC_FREE = Member("TG0M0007", 6, "the MTC free runner", "free", "mtc", "01:00:02:00")
MTC_JUMP = Member("TG0M0008", 7, "the MTC jump chaser", "jump", "mtc", "01:00:02:00")
MEMBERS = (FREE, JUMP, VARI, STRETCH, SCENE_A, SCENE_B, MTC_FREE, MTC_JUMP)


def prepare(room: Path) -> Path:
    """The show copied into the room, its media written into the copy, and the
    input WAV and MTC file beside it."""
    bundle = common.copy_bundle(FIXTURE, room / "timegroup")
    write_member(bundle / "media" / "ltc.wav")
    write_bursts(bundle / "media" / "bursts.wav")
    write_levels(room / "ltc-in.wav", ltc_edges(STORY), LOOP)
    (room / "mtc-in.txt").write_text("\n".join(mtc_lines(STORY)) + "\n", encoding="ascii", newline="\n")
    return bundle


# =============================================================================
# The render, read fast
# =============================================================================

def _table(high: str, low: str, rest: str, first: int) -> bytes:
    table = bytearray(rest.encode("ascii") * 256)
    for byte in range(first, 0x80):
        table[byte] = ord(high)
        table[byte | 0x80] = ord(low)
    return bytes(table)


# A float's top byte is its sign and the top seven bits of its exponent: 0x3E
# and up is an eighth or more, 0xBE and up the same below nought. H, L or M.
CLASSES = _table("H", "L", "M", 0x3E)
SIGNS = _table("+", "-", "+", 0x00)


class Render:
    """A render, whole, as bytes, read a channel at a time through the top byte
    of each sample - which says its sign and whether it is an eighth or more,
    a slice and a translate away - and as floats only where an edge is.
    Pure Python reading every sample would take a minute a pass."""

    def __init__(self, path: Path, channels: "int | None" = None, raw: "bytes | None" = None):
        if raw is not None:
            self.raw, self.channels = raw, channels
        else:
            rate, self.channels, offset, frames = tc.wav_layout(path)
            if rate != SAMPLE_RATE:
                raise HarnessError(f"the render is at {rate} Hz, not {SAMPLE_RATE}")
            with path.open("rb") as file:
                file.seek(offset)
                self.raw = file.read(frames * self.channels * 4)
        self.frames = len(self.raw) // (4 * self.channels)

    @staticmethod
    def of_wav(path: Path) -> "Render":
        """A 16-bit mono file read as a one-channel render: the driver's own
        media, read back by the same reader as a check of both."""
        with wave.open(str(path)) as wav:
            pcm = array.array("h")
            pcm.frombytes(wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
        floats = array.array("f", (value / 32767.0 for value in pcm))
        if sys.byteorder != "little":
            floats.byteswap()
        return Render(path, 1, floats.tobytes())

    def sample(self, channel: int, n: int) -> float:
        return struct.unpack_from("<f", self.raw, (n * self.channels + channel) * 4)[0]

    def top(self, channel: int) -> bytes:
        return self.raw[channel * 4 + 3::4 * self.channels]

    def silent(self, channel: int, first: int, last: int) -> bool:
        """Nothing an eighth or more between two samples."""
        return self.top(channel)[max(0, first):max(0, last)].translate(CLASSES).strip(b"M") == b""


def edges_in(render: Render, channel: int, quiet: int = 9):
    """(sample, falls_quiet) for every edge of the LTC on a channel: where it
    crosses nought between a stretch above an eighth and one below minus an
    eighth, linear between the two samples either side; where it rises half
    way out of quiet; and where it falls quiet - inside the band for longer
    than `quiet` samples, which is the code ending. Placed half a sample on, so
    that an edge between two whole samples of a file whose levels change on a
    sample reads on that sample: the driver's own files read back exactly."""
    classes = render.top(channel).translate(CLASSES)
    edges = []
    previous = None
    half = 0.5 * AMPLITUDE

    for run in re.finditer(rb"H+|L+", classes):
        start = run.start()
        if previous is not None and start - previous.end() < quiet:
            if classes[start] != classes[previous.start()]:
                a = previous.end() - 1
                x0 = render.sample(channel, a)
                for k in range(a, start):
                    x1 = render.sample(channel, k + 1)
                    if (x0 < 0.0) != (x1 < 0.0):
                        edges.append((k + (0.0 - x0) / (x1 - x0) + 0.5, False))
                        break
                    x0 = x1
            previous = run
            continue
        if previous is not None:
            edges.append((previous.end() + 0.5, True))
        x0 = abs(render.sample(channel, start - 1)) if start > 0 else 0.0
        x1 = abs(render.sample(channel, start))
        edges.append((start - 1 + (half - x0) / (x1 - x0) + 0.5 if x1 > half > x0 else start + 0.5, False))
        previous = run

    if previous is not None:
        edges.append((previous.end() + 0.5, True))
    return edges


def ltc_frames(render: Render, channel: int) -> "list[tuple[float, int]]":
    """[(the sample its first bit began on, frame of the day)] of every frame
    that reads on a channel - sync word, even ones, decimal digits - through
    timecode.py's decoder, fed the edges found above."""
    decoder = tc.LtcDecoder(SAMPLE_RATE, tc.RATES["25"], AMPLITUDE)
    for at, falls_quiet in edges_in(render, channel):
        decoder._edge(at, falls_quiet=falls_quiet)
    out = []
    for frame in decoder.frames:
        if frame.parity_even and frame.bcd_ok:
            hours, minutes, seconds, frames = frame.label
            out.append((frame.start, ((hours * 60 + minutes) * 60 + seconds) * FPS + frames))
    return out


def bursts_in(render: Render, channel: int) -> "list[tuple[float, int, float]]":
    """[(centre sample, length, pitch or None)] of each burst of tone on a
    channel: a stretch with something an eighth or more in it, after at least
    10 ms of nothing, its centre half way between its first and last sample an
    eighth or more, its pitch from the rising zero crossings between 20 ms in
    and 20 ms before its end, the first and last placed to their fraction."""
    top = render.top(channel)
    classes = top.translate(CLASSES)
    signs = top.translate(SIGNS)
    gaps = [m.span() for m in re.finditer(rb"M{480,}", classes)]
    out = []
    for (_, begin), (end, _) in zip(gaps, gaps[1:]):
        if end - begin < 0.1 * SAMPLE_RATE:
            continue
        a, b = begin + int(0.02 * SAMPLE_RATE), end - int(0.02 * SAMPLE_RATE)
        first = signs.find(b"-+", a, b)
        last = signs.rfind(b"-+", a, b)
        count = signs.count(b"-+", a, b)
        pitch = None
        if first >= 0 and last > first and count > 2:
            def crossing(k: int) -> float:
                x0, x1 = render.sample(channel, k), render.sample(channel, k + 1)
                return k + (0.0 - x0) / (x1 - x0)
            pitch = (count - 1) * SAMPLE_RATE / (crossing(last) - crossing(first))
        out.append(((begin + end) / 2.0, end - begin, pitch))
    return out


def burst_flaws(render: Render, channel: int, centre: float, truth: "Truth") -> str:
    """What the cycles of a burst read off its pitch say, for the report - not
    judged: bursts_in's pitch is the rising zero crossings counted over the
    burst, so a hole in the sound a block long (cycles missing), a splice (a
    cycle long and the next short) and a pitch that really moved (every cycle
    longer or shorter) read alike there, and only the cycles tell them apart.
    Written after TG.12's CI run, where a macOS runner read one burst of 107 at
    989 Hz, a block's worth of cycles short, and nothing else said which."""
    burst = min(bursts_in(render, channel), key=lambda b: abs(b[0] - centre), default=None)
    if burst is None:
        return "no burst there"
    begin, end = int(burst[0] - burst[1] / 2.0), int(burst[0] + burst[1] / 2.0)
    crossings = []
    for k in range(begin, end):
        x0, x1 = render.sample(channel, k), render.sample(channel, k + 1)
        if x0 < 0.0 <= x1 and x1 > x0:
            crossings.append(k + (0.0 - x0) / (x1 - x0))
    periods = [(a, b - a) for a, b in zip(crossings, crossings[1:])]
    if not periods:
        return "no cycles"
    nominal = SAMPLE_RATE / TONE_HZ
    odd = [(a, p) for a, p in periods if abs(p - nominal) > 1.0]
    longest = max(periods, key=lambda x: x[1])
    shortest = min(periods, key=lambda x: x[1])
    median = statistics.median(p for _, p in periods)
    quiet = max((sum(1 for k in range(int(a), int(a + p)) if abs(render.sample(channel, k)) < 0.01 * AMPLITUDE)
                 for a, p in periods), default=0)
    return (f"{len(periods)} cycles, median {median:.3f} samples ({SAMPLE_RATE / median:.3f} Hz), "
            f"{len(odd)} more than a sample off a cycle's {nominal:g}: longest {longest[1]:.2f} at "
            f"{seconds(longest[0] - truth.origin)}, shortest {shortest[1]:.2f} at {seconds(shortest[0] - truth.origin)}"
            f", at most {quiet} samples near silence in one cycle")


# =============================================================================
# The engine's log
# =============================================================================

TOKEN = re.compile(r's:"((?:[^"\\]|\\.)*)"|([hidf]):(\S+)|(?<!\S)([TF])(?!\S)')


class Record:
    __slots__ = ("tick", "seq", "origin", "verb", "args")

    def __init__(self, tick, seq, origin, verb, args):
        self.tick, self.seq, self.origin, self.verb, self.args = tick, seq, origin, verb, args


def read_log(path: Path) -> "list[Record]":
    """Every applied record, its arguments as Python values."""
    records = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split(" ", 5)
        if len(parts) < 5 or parts[0] != "A":
            continue
        args = []
        for match in TOKEN.finditer(parts[5] if len(parts) > 5 else ""):
            text, kind, value, flag = match.groups()
            if text is not None:
                args.append(text)
            elif kind in ("h", "i"):
                args.append(int(value))
            elif kind in ("d", "f"):
                args.append(float(value))
            else:
                args.append(flag == "T")
        records.append(Record(int(parts[1]), int(parts[2]), parts[3], parts[4], args))
    return records


class Heard:
    """One `code.heard`: the source said it, the turn, the frame of the day on
    the session's sample, and the speed."""

    def __init__(self, record: Record):
        self.tick, self.seq, self.origin = record.tick, record.seq, record.origin
        self.source, self.what, self.rate = record.args[0], record.args[1], record.args[2]
        self.frame, self.sample, self.speed = record.args[3], record.args[4], record.args[5]
        self.runs = record.args[6:]

    def sample_of(self, frame: float) -> float:
        """Where this map puts a frame, in the session's samples."""
        speed = self.speed if self.speed > 0 else 1.0
        return self.sample + (frame - self.frame) * FRAME / speed


LATE_NOTE = re.compile(r"^# note tick thread up to ([0-9.]+) ms late, by tick (\d+)")


class Log:
    def __init__(self, path: Path):
        self.records = read_log(path)
        self.heard = [Heard(r) for r in self.records if r.verb == "code.heard" and len(r.args) >= 6]
        # The engine's own word that its tick thread fell behind its audio:
        # (the tick the note was made by, the worst since the note before).
        self.late_ticks = []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            found = LATE_NOTE.match(line)
            if found:
                self.late_ticks.append((int(found.group(2)), float(found.group(1))))

    def most_behind(self, after: int = 0) -> float:
        """The most the engine said its tick thread was behind its audio, in ms,
        in its notes from tick `after` on."""
        return max((ms for by, ms in self.late_ticks if by >= after), default=0.0)

    def tick_late_around(self, tick: int) -> float:
        """The most the engine said its tick thread was late by, in ms, in the
        stretch of ticks its notes covered around `tick` - nought when it said
        nothing there."""
        worst, before = 0.0, -1
        for by, ms in self.late_ticks:
            if before < tick + 3 and by >= tick - 3:
                worst = max(worst, ms)
            before = by
        return worst

    def of(self, verb: str) -> "list[Record]":
        return [r for r in self.records if r.verb == verb]

    def spawns_of(self, cue: str) -> "list[Record]":
        return [r for r in self.records if r.verb == "run.spawn" and len(r.args) >= 3 and r.args[1] == cue]

    def first(self, verb: str, run: str):
        return next((r for r in self.records if r.verb == verb and r.args and r.args[0] == run), None)

    def map_before(self, source: str, seq: int):
        """The last map of `source` applied before the record numbered `seq`:
        the one the Runner read the code by when it applied that record."""
        found = None
        for heard in self.heard:
            if heard.source == source and heard.seq < seq and heard.what != "stopped":
                found = heard
        return found


# =============================================================================
# Where each member stood, against where the code says it should
# =============================================================================

class Track:
    """Where a member was in its file, against the render's samples: points of
    (render sample, file second). From its LTC, one a frame; from its bursts,
    one a burst."""

    def __init__(self, points):
        self.points = sorted(points)

    def between(self, first: float, last: float):
        return [p for p in self.points if first <= p[0] < last]

    def rate(self, first: float, last: float, least: int = 10):
        """File seconds a render second, least squares over the points between
        two samples; None for fewer than `least`."""
        pts = self.between(first, last)
        if len(pts) < least:
            return None
        n = len(pts)
        mx = sum(t for t, _ in pts) / n
        my = sum(f for _, f in pts) / n
        sxx = sum((t - mx) ** 2 for t, _ in pts)
        sxy = sum((t - mx) * (f - my) for t, f in pts)
        return sxy / sxx * SAMPLE_RATE if sxx > 0 else None

    def onset(self, count: int = 25):
        """The render sample its file's second nought was played on, from a
        line through its first `count` points."""
        pts = self.points[:count]
        if len(pts) < 3:
            return None
        n = len(pts)
        mx = sum(f for _, f in pts) / n
        my = sum(t for t, _ in pts) / n
        sxx = sum((f - mx) ** 2 for f, _ in ((f, t) for t, f in pts))
        sxy = sum((f - mx) * (t - my) for t, f in pts)
        slope = sxy / sxx
        return my - slope * mx

    def moves(self, first: float, last: float, speed: float = 1.0, smallest_ms: float = 0.5):
        """[(render sample, ms)] where it moved in its file by more than
        `smallest_ms` beyond what playing at `speed` explains, between two
        points less than half a second apart: a jump of its own, whatever the
        code did."""
        out = []
        pts = self.between(first, last)
        for (t0, f0), (t1, f1) in zip(pts, pts[1:]):
            moved = (f1 - f0 - (t1 - t0) / SAMPLE_RATE * speed) * 1000.0
            if t1 - t0 < 0.5 * SAMPLE_RATE and abs(moved) > smallest_ms:
                out.append((t1, moved))
        return out

    def gaps(self, first: float, last: float, longest: float = 0.25 * SAMPLE_RATE):
        """Stretches between two samples longer than `longest` with no point:
        where it was not heard to play."""
        pts = [first] + [t for t, _ in self.between(first, last)] + [last]
        return [(a, b) for a, b in zip(pts, pts[1:]) if b - a > longest]


def ltc_track(render: Render, member: Member) -> Track:
    return Track((start, frame / FPS) for start, frame in ltc_frames(render, member.channel))


class Truth:
    """Where a code was, in render samples: the story from `origin`."""

    def __init__(self, origin: float, story: "Story | None" = None):
        self.origin = origin
        self.story = story if story is not None else STORY

    def code_at(self, t: float):
        return self.story.code_at(t - self.origin)

    def at(self, story_sample: float) -> float:
        return self.origin + story_sample

    def of(self, frame: float, after: float = 0.0) -> float:
        """Where the code first reads `frame` at or after the story's sample
        `after`, in render samples."""
        found = self.story.sample_of(frame, after)
        return None if found is None else self.origin + found

    def expected(self, member: Member, t: float):
        """The second of its file a member in step is at, at render sample `t`;
        None where the code is quiet."""
        code = self.code_at(t)
        return None if code is None else (code - member.at) / FPS - member.offset

    def errors(self, track: Track, member: Member, first: float, last: float):
        """[(render sample, ms ahead of the code)] between two samples."""
        out = []
        for t, file in track.between(first, last):
            want = self.expected(member, t)
            if want is not None:
                out.append((t, (file - want) * 1000.0))
        return out


def bursts_track(render: Render, member: Member, truth: Truth, shape) -> "tuple[Track, list]":
    """The timestretch chaser's bursts as points: each burst's file second is
    its number times half a second plus where its middle is in it. Which of
    the five lengths it is (`shape`, from burst_shape) gives its number to
    within five; of those, the one nearest where the code says the member is,
    or, where the code is quiet, the one after the last. And [(render sample,
    pitch)] for every burst."""
    period = BURST_PERIOD / SAMPLE_RATE
    points, pitches = [], []
    last = None
    for at, length, pitch in bursts_in(render, member.channel):
        kind = min(range(len(shape)), key=lambda k: abs(length - shape[k][0]))
        centre = shape[kind][1]
        want = truth.expected(member, at)
        if want is not None:
            near = (want - centre) / period
        elif last is not None:
            near = last + 1
        else:
            continue
        number = kind + len(shape) * round((near - kind) / len(shape))
        last = number
        points.append((at, number * period + centre))
        if pitch is not None:
            pitches.append((at, pitch))
    return Track(points), pitches


def steps_in(errors, first: float, last: float, smallest_ms: float):
    """[(render sample, ms)] where a member's error moved by more than
    `smallest_ms` between two points less than half a second apart: a jump."""
    out = []
    pts = [e for e in errors if first <= e[0] < last]
    for (t0, e0), (t1, e1) in zip(pts, pts[1:]):
        if t1 - t0 < 0.5 * SAMPLE_RATE and abs(e1 - e0) > smallest_ms:
            out.append((t1, e1 - e0))
    return out


def worst(errors, first: float, last: float):
    pts = [abs(e) for t, e in errors if first <= t < last]
    return max(pts) if pts else None


def seconds(sample: float) -> str:
    return f"{sample / SAMPLE_RATE:.3f} s"


# =============================================================================
# The scenario
# =============================================================================

LOCALE = next((arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith("--wfg-locale=")), None)


class Polled:
    """What the driver read while the story ran: the tick, the sync readouts,
    the chase readouts of two runs."""

    def __init__(self):
        self.rows = []

    def read(self, server: Server, runs: dict) -> None:
        try:
            tick = int(common.http_json(server.http_port, "/godot/engine/tick?VALUE")["VALUE"][0])
            sync = common.http_json(server.http_port, "/godot/sync")["CONTENTS"]
            row = {"tick": tick}
            for name in ("ltcInState", "ltcInSpeed", "ltcInCode", "mtcInState", "mtcInSpeed"):
                row[name] = sync.get(name, {}).get("VALUE", [None])[0]
            for key, run in runs.items():
                if run:
                    row[key] = value_of(server, f"/godot/run/{run}/chaseState")
            self.rows.append(row)
        except (HarnessError, OSError, ValueError, KeyError, IndexError):
            pass


def serve(report: Report, room: Path, held: bool, word: str, story_until: float):
    """One serve: the show copied, the code played in, both groups armed by GO,
    and the render kept until the story has reached `story_until` (samples of
    the pass analysed) - or past its loop point. (pass, log, render, polled),
    or None when the harness could not run it."""
    bundle = prepare(room)
    render = room / "out.wav"
    log = room / "session.wfglog"
    polled = Polled()
    patience = 120.0 if ON_CI else 30.0

    with Server(bundle, log=log, locale=LOCALE, sample_rate=SAMPLE_RATE, buffer_size=BLOCK, hosted=True,
                render=render, input_wav=room / "ltc-in.wav", hold=held,
                midi_play_loop={"MTC in": str(room / "mtc-in.txt")}) as server:
        report.equal(wait_for(server, "/godot/audio/status", "running", timeout=patience), "running",
                     f"{word}: the audio side comes up running")
        if held:
            report.equal(value_of(server, "/godot/pair/sending"), False,
                         f"{word}: /godot/pair/sending says this machine holds")

        armed = {}
        for group in (LTC_GROUP, MTC_GROUP):
            go(server)
            armed[group] = run_for_cue(server, group, timeout=patience)
            state = common.wait_until(lambda: value_of(server, f"/godot/run/{armed[group]}/state") == "playing",
                                      timeout=patience) if armed[group] else None
            report.check(bool(state), f"{word}: GO arms the time group {group} and it stays armed")

        # WHICH PASS OF THE STORY IS READ: the first whose lead-in the GOs came
        # in time for - two seconds before the members load, which is two
        # seconds before their time. On a workstation the GOs land in the
        # first, a few ticks in; a runner that took longer is read a pass on.
        tick = value_of(server, "/godot/engine/tick") or 0
        armed_at = tick * TICK
        story_pass = 0 if armed_at % LOOP < LEAD_IN - 2.5 * SAMPLE_RATE and armed_at < LOOP else armed_at // LOOP + 1
        target = story_pass * LOOP + story_until
        print(f"       armed by tick {tick} ({seconds(armed_at)}); reading pass {story_pass} of the story, "
              f"to {seconds(target)}")
        (room / "pass.txt").write_text(str(story_pass), encoding="ascii")

        runs = {}
        deadline = time.monotonic() + (target / SAMPLE_RATE) * (3.0 if ON_CI else 1.5) + patience
        while frames_on_disk(render) < target + SAMPLE_RATE and time.monotonic() < deadline:
            if not runs and frames_on_disk(render) > story_pass * LOOP + LEAD_IN + 2.5 * SAMPLE_RATE:
                runs = {"free": run_for_cue(server, FREE.cue, timeout=5.0),
                        "vari": run_for_cue(server, VARI.cue, timeout=5.0)}
            polled.read(server, runs)
            time.sleep(0.2)

        report.check(frames_on_disk(render) >= target, f"{word}: the render reaches {seconds(target)}",
                     f"it holds {seconds(frames_on_disk(render))}")
        report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                     f"{word}: nothing of Go.dot's allocated on the audio thread")
        wait_for_render_tail(render)

    if not render.is_file() or not log.is_file():
        report.check(False, f"{word}: the render and the log were written")
        return None
    return story_pass, Log(log), Render(render), polled


# HOW FAR AHEAD OF THE RENDER MTC MAY ARRIVE BEFORE ITS RUN IS NOT JUDGED AS
# MTC: see `mtc_lead`. Every healthy room kept reads at most 0.02 s; the CI
# rooms that went red, 7.8 and 10.3 s.
MTC_LEAD_LIMIT = 0.5


def mtc_lead(log: Log) -> float:
    """How far ahead of the engine's own clock the MTC arrived, at most, in
    seconds: each MTC record's anchor - the sample its frame was placed on -
    against the sample its tick was due at, less the lateness the log notes
    there. The serve renders on a hosted pump, its audio clock the pump's, while
    the MTC it plays to itself arrives on the steady clock; a pump that keeps up
    puts a record's anchor a few frames before its tick (-0.01 to -0.3 s in every
    healthy room kept). One a starved runner holds back falls behind the wall
    clock, and the MTC, placed on the samples through the tick's clock pairs,
    runs ahead of them by however far it fell - seconds, in TG.12's CI run,
    part 3: the locate's `jumped` said on a tick due 57.7 s into the render with
    its anchor at 67.0 s, and the MTC jump chaser re-seated nine seconds before
    the locate, against the port's own story as against the maps. A machine on
    its audio interface has no pump to fall behind, so what that run says of
    MTC is the rig's, not the engine's."""
    leads = [(h.sample - h.tick * TICK) / SAMPLE_RATE - log.tick_late_around(h.tick) / 1000.0
             for h in log.heard if h.source == MTC_ORIGIN]
    return max(leads, default=0.0)


def judge_turns(report: Report, word: str, log: Log, truth: Truth, r0: int, source: str, origin: str,
                sparse_judged: bool, ahead: float = 0.0, overloaded: str = "") -> None:
    """`code.heard` from outside: its origin the source's own, its turns where
    the story turns, and few of them. `ahead`, MTC's lead on the render
    (`mtc_lead`): past MTC_LEAD_LIMIT, where a turn was said is the rig's."""
    rig = (f"the MTC arrived up to {ahead:.2f} s ahead of the render: the rig's audio clock, a hosted pump, fell"
           " behind the wall clock MTC arrives on" if ahead > MTC_LEAD_LIMIT else "")
    first, last = truth.origin - 0.5 * SAMPLE_RATE, truth.origin + LOOP + 2.0 * SAMPLE_RATE
    mine = [h for h in log.heard if h.source == source and first <= h.sample + r0 < last]
    report.check(bool(mine) and all(h.origin == origin and h.source == origin for h in mine),
                 f"{word}: every code.heard of the {source[:3].upper()} comes from origin {origin}",
                 ", ".join(sorted({h.origin for h in mine})))

    def turn(what: str, frame: int, story_sample: "float | None"):
        """The record of a turn and how far from the story's moment: for a
        stop, how long after the line fell quiet it was said (the tick it was
        applied on); for a jump, how far its sample is from the frame's."""
        candidates = [h for h in mine if h.what == what and abs(h.frame - frame) <= 1]
        if what == "stopped":
            # SAID ON A TICK, WHICH IS DUE AT ITS SAMPLE BUT RUNS AS LATE AS THE
            # TICK THREAD DOES: the watcher decides a stop against the samples
            # counted when the tick runs, not those the tick was due at, so a
            # tick that ran late says it under a number whose sample can be
            # before the line fell quiet - by as much as the tick was behind,
            # which the engine notes in its log (TG.12's CI run, where a Windows
            # runner's ticks around the stop had the record due before the quiet
            # in both locales, for both codes). So a record whose tick was due
            # before the quiet is the stop's when the tick could have run after
            # it, by the lateness the log notes there; with no note, the tick
            # was under two ticks behind and the watcher's own five frames of
            # waiting put its due sample after the quiet anyway.
            quiet = truth.at(story_sample)
            hit = next((h for h in candidates
                        if quiet <= h.tick * TICK + r0 + log.tick_late_around(h.tick) * MS
                        and h.tick * TICK + r0 <= quiet + SAMPLE_RATE), None)
            return hit, (None if hit is None else hit.tick * TICK + r0 - quiet)
        want = truth.of(frame, story_sample)
        hit = min(candidates, key=lambda h: abs(h.sample + r0 - want), default=None)
        return hit, (None if hit is None else hit.sample + r0 - want)

    stopped_frame = STORY.pieces[1][3] + STORY.pieces[1][4] - 1
    returned = STORY.pieces[3][3] + 1
    located = STORY.pieces[4][3] + 1
    looped = STORY.pieces[0][3] + 1
    exact = 1.5 if source == LTC_ORIGIN else None
    turns = [("stopped", stopped_frame, STOP_AT, "the stop"),
             ("jumped", returned, RETURN_AT, "the return, 120 ms on"),
             ("jumped", located, LOCATE_AT, "the locate back 30 s")]
    for what, frame, story_sample, words in turns:
        hit, off = turn(what, frame, story_sample)
        if what == "stopped":
            fell = truth.at(story_sample)
            seen = "; ".join(f"{label_of(h.frame)} on tick {h.tick}, due {(h.tick * TICK + r0 - fell) / MS:+.1f} ms"
                             f" from the quiet, the tick noted up to {log.tick_late_around(h.tick):g} ms late"
                             for h in mine if h.what == "stopped")
            said = (f"none; stopped said: {seen or 'never'}" if hit is None
                    else f"said on a tick due {off / MS:+.1f} ms after the line fell quiet"
                         + (f", the tick noted up to {log.tick_late_around(hit.tick):g} ms late"
                            if log.tick_late_around(hit.tick) > 0.0 else ""))
            if rig and hit is None:
                report.void(f"{word}: `stopped` at {words}, on {label_of(frame)}", f"{said}; {rig}")
            else:
                report.check(hit is not None, f"{word}: `stopped` at {words}, on {label_of(frame)}", said)
        elif exact is not None:
            report.check(hit is not None and abs(off) <= exact,
                         f"{word}: `jumped` at {words}, {label_of(frame)} on the input frame's own sample",
                         "none" if hit is None else f"{off:+.2f} samples from it")
        else:
            # MTC arrives on the steady clock and is placed on the samples
            # through the tick's clock pairs, which a Debug engine playing eight
            # voices takes late (M64): the turn is judged to MTC_PLACED_MS, and
            # printed.
            print(f"       MTC `jumped` at {words}: "
                  + ("none" if hit is None else f"{off / MS:+.2f} ms from where the port's pass puts it"))
            placed = hit is not None and (ON_CI or abs(off) <= MTC_PLACED_MS * MS)
            jwords = f"{word}: `jumped` at {words}, {label_of(frame)} where the port played it"
            jdetail = "none" if hit is None else f"{off / MS:+.2f} ms from where the story's moment fell"
            if rig and not placed:
                report.void(jwords, f"{jdetail}; {rig}")
            else:
                report.check(placed, jwords, jdetail)
    # The loop point: the input begins again, the port's file again.
    loop_truth = Truth(truth.origin + LOOP)
    after_loop = [h for h in log.heard if h.source == source and h.what == "jumped" and abs(h.frame - looped) <= 1
                  and loop_truth.at(0) - 0.5 * SAMPLE_RATE <= h.sample + r0 <= loop_truth.at(0) + SAMPLE_RATE]
    lwords = f"{word}: and `jumped` at the loop point, back to {label_of(looped - 1)}"
    ldetail = f"{[(h.what, label_of(h.frame)) for h in mine[-3:]]}"
    if rig and not after_loop:
        # As the return and the locate above: a turn the rig's MTC brings is
        # said on the first tick that drains it, anchored where the code had
        # got to by then (99c90da's macOS run: 00:59:40:03, the return and the
        # locate 23 and 21 frames on, the MTC 53.7 s ahead of the render).
        report.void(lwords, f"{ldetail}; {rig}")
    else:
        report.check(bool(after_loop), lwords, ldetail)

    counted = [h for h in mine if truth.origin <= h.sample + r0 < truth.origin + LOOP]
    frames = sum(p[4] for p in STORY.pieces if p[0] == "code")
    words = f"{len(counted)} records in a pass of {frames} frames: " + ", ".join(
        f"{sum(1 for h in counted if h.what == w)} {w}" for w in ("jumped", "running", "stopped"))
    quiet = [h for h in mine if truth.origin + 2.0 * SAMPLE_RATE <= h.sample + r0 < truth.at(LEAD_IN) - 2.5 * SAMPLE_RATE]
    speeds = [h.speed for h in counted if h.what == "running"]
    print(f"       {source[:3].upper()}: {words}; {len(quiet)} in the lead-in after its first two seconds"
          + (f"; speeds written {min(speeds):.6f} to {max(speeds):.6f}" if speeds else ""))

    # A HANDFUL A PASS, for either code (AIO): its turns - here a jump at the
    # start, a stop, a return, a locate - and a re-anchor whenever the line
    # drifts half a millisecond (LTC) or one (MTC) from the line last written,
    # which at +50 ppm and +0.1 % is a few. MTC's moments are placed on Go.dot's
    # samples through the tick's clock pairs, so on CI, where the runner holds
    # the tick back, how often it re-anchors is the runner's.
    sparse = f"{word}: {source[:3].upper()}'s code.heard is sparse - its turns and a few re-anchors, not one a frame (AIO)"
    if sparse_judged and (rig or overloaded):
        # MTC's moments are placed on the samples through the tick's clock
        # pairs, so a tick held back re-anchors its watcher more often (above).
        report.void(sparse, f"{words}; {rig or overloaded}")
    elif sparse_judged:
        report.check(len(counted) <= 12, sparse, words)
    else:
        report.void(sparse, f"{words}; MTC is placed on the samples through the tick's clock pairs, which a starved"
                            " runner takes late, so how often its watcher re-anchors is the runner's on CI")


def asked(log: Log, member: Member, truth: Truth, r0: int):
    """How the engine asked for a member's first run of the pass: (the map of
    its code it read the time by, the samples that map put its time on, why a
    launch off that sample is the engine's to own up to - or "")."""
    origin = LTC_ORIGIN if member.source == "ltc" else MTC_ORIGIN
    spawns = [r for r in log.spawns_of(member.cue) if r.tick * TICK + r0 >= truth.origin]
    run = spawns[0].args[2] if spawns else None
    fired = log.first("run.fire", run) if run else None
    heard = log.map_before(origin, fired.seq) if fired else None
    late = log.first("run.late", run) if run else None
    behind = log.tick_late_around(fired.tick) if fired else 0.0
    # A LAUNCH IS NEVER PLACED IN THE PAST: a member asked for while the tick
    # thread runs behind its audio - which a Debug engine does by tens of
    # milliseconds while it loads sounds - starts as soon as it can. The engine
    # owns up to it as `run.late` in whole blocks, and to a tick two or more
    # behind in its log's notes; see `launched_late` for the rest.
    excuse = (f"the engine said it {late.args[1]} block(s) late (run.late)" if late
              else f"the engine's tick thread ran up to {behind:g} ms behind its audio around the ask (its log's"
                   " notes)" if behind >= 40.0 else "")
    on_map = None if heard is None else heard.sample_of(member.at) + r0
    return heard, on_map, excuse


def launched_late(onset: float, on_map: "float | None", excuse: str, r0: int) -> str:
    """Why a start after its map's sample is a late launch the engine places as
    soon as it can, rather than a launch on the wrong sample - or "" when it is
    not one. A launch that missed its sample lands on the first block it could
    still be placed on, so on a block boundary of the session; `run.late` counts
    whole blocks and says nothing of one late by less, and the engine notes a
    tick only once it is two behind - a tick one behind, which the ask's two
    ticks of margin can just miss, is said by nothing but where the sound began."""
    if on_map is None or onset - on_map <= 2.5:
        return ""
    if excuse:
        return excuse
    # On a block's first sample: exactly, or up to the sample or two a
    # resampler's interpolator sounds a voice launched at a speed not one.
    into = (onset - r0 + 0.25) % BLOCK
    if into <= 2.75:
        return (f"it began {onset - on_map:.0f} samples after its sample, on a block's first sample - launched as"
                " soon as it could be once asked for (the tick behind its audio); run.late counts whole blocks")
    return ""


def judge_start(report: Report, word: str, log: Log, member: Member, track: Track, truth: Truth,
                r0: int, mtc_exact: bool, onset_of=None) -> "float | None":
    """A member's first run of the pass: started on the very sample the map
    gives its time, and that sample where the code reaches it."""
    onset = onset_of if onset_of is not None else track.onset()
    if onset is None:
        report.check(False, f"{word}: {member.words} is heard", "no frame read on its channel")
        return None
    heard, on_map, excuse = asked(log, member, truth, r0)
    want_code = truth.of(member.at + member.offset * FPS)
    excuse = launched_late(onset, on_map, excuse, r0)
    late = bool(excuse)

    if heard is not None and not member.offset:
        # To the sample - give or take the sample or two a resampler's
        # interpolator delays a voice launched at a speed that is not one: a
        # varispeed chaser starts at the code's speed (TG.9).
        ok = abs(onset - on_map) <= 2.5
        words = f"{word}: {member.words} starts on the sample the map gave {label_of(member.at)} (launchNotBefore)"
        detail = f"onset {onset:.2f}, the map ({heard.what} {label_of(heard.frame)} on {heard.sample}) {on_map:.2f}"
        if late:
            report.void(words, f"{excuse}; {detail}")
        else:
            report.check(ok, words, detail)

    if want_code is None:
        return onset
    off = (onset - want_code) / MS
    if member.source == "ltc":
        words = f"{word}: {member.words} starts within a millisecond of the input frame {label_of(member.at)}" + (
            f" and {member.offset:g} s" if member.offset else "")
        if abs(off) > 1.0 and late:
            report.void(words, f"{excuse}; {off:+.3f} ms")
        else:
            report.check(abs(off) <= 1.0, words, f"{off:+.3f} ms")
    else:
        words = f"{word}: {member.words} starts within a few ms of where the port played {label_of(member.at)}"
        if mtc_exact and not late:
            report.check(abs(off) <= 3.0, words, f"{off:+.3f} ms, the port's moments placed as the engine heard"
                                                 " the pass's lead-in on average")
        else:
            report.void(words, f"{off:+.3f} ms; " + (excuse if late else
                                                     "MTC is placed through the tick's clock pairs, the runner's on CI"))
    print(f"       {member.words}: started {off:+.3f} ms from {label_of(member.at)}" + (f" ({excuse})" if late else ""))
    return onset


def judge_chasers(report: Report, word: str, render: Render, log: Log, truth: Truth, r0: int,
                  overloaded: str = "") -> None:
    """The LTC group's members, from their sound. `overloaded`, why the tick did
    not keep up with its audio, when it did not: a scene's placing to the sample
    rides on the tick that commits it, and is not judged against such a run when
    it misses."""
    s = SAMPLE_RATE
    at = truth.of(FREE.at)
    tracks = {m.cue: ltc_track(render, m) for m in (FREE, JUMP, VARI, SCENE_A, SCENE_B)}

    stretch, pitches = bursts_track(render, STRETCH, truth, burst_shape())
    tracks[STRETCH.cue] = stretch

    # LAUNCHED LATE, A CHASER IS HELD TO THE LAW FROM WHERE IT STARTED. A member
    # the engine owned up to launching late (`asked`, `launched_late`) began that
    # far behind its code, and the run's clock starts on the sample it really
    # began on, so the chaser measures it: within the law's tolerance it is in
    # step and only follows the code's speed, held where it began; past it, it
    # catches up and only ever comes closer (Chase.h: proportional, so it never
    # overshoots). Its distance from the code is therefore held to its late
    # start plus the bounds an on-time start is held to, and its speed to the
    # code's except where it was past the tolerance and so catching up. TG.12's
    # CI run, part 3: a Windows runner whose tick ran up to 117 ms behind
    # launched the free runner and both catch-up chasers three blocks late,
    # 9.1 ms, in step by the law and held to 1-3 ms by the bounds below - and
    # the jump and timestretch chasers with them.
    late: "dict[str, tuple[str, float]]" = {}
    onsets = {}
    for member in (FREE, JUMP, VARI):
        onsets[member.cue] = judge_start(report, word, log, member, tracks[member.cue], truth, r0, True)
        onset = onsets[member.cue]
        if onset is not None:
            _, on_map, excuse = asked(log, member, truth, r0)
            excuse = launched_late(onset, on_map, excuse, r0)
            if excuse:
                late[member.cue] = (excuse, (onset - at) / MS)
    first_burst = stretch.points[0][0] - stretch.points[0][1] * SAMPLE_RATE if stretch.points else None
    off = None if first_burst is None else (first_burst - at) / MS
    words = (f"{word}: {STRETCH.words} starts within 2 ms of {label_of(STRETCH.at)}, its first burst's middle"
             " where it should be")
    s_excuse = asked(log, STRETCH, truth, r0)[2]
    if off is not None and abs(off) > 2.0 and off > 0.0 and s_excuse:
        # A burst's middle is not a launch's sample, so `launched_late`'s block
        # rule cannot read it: the engine's own word that it launched late is
        # what voids it, as it voids its siblings' starts.
        report.void(words, f"{s_excuse}; {off:+.3f} ms")
        late[STRETCH.cue] = (s_excuse, off)
    else:
        report.check(off is not None and abs(off) <= 2.0, words, "no burst" if off is None else f"{off:+.3f} ms")

    def catching_up(member: Member, errs, first: float, last: float) -> bool:
        """Whether the law could have been catching a member up anywhere between
        two samples: its hysteresis (Chase.h) replayed over the errors heard
        from the member's start - on once it is past the tolerance, off only
        once it is back within where a catch-up ends - each with a margin for
        the hearing. A member launched past the tolerance is caught up from its
        launch on, and still is a second and more later, below the tolerance
        but not yet within the settle (TG.12's CI run, part 3: a timestretch
        chaser launched 16.9 ms late)."""
        settle = STRETCH_SETTLE_MS if member is STRETCH else 1.0
        on = False
        for t, e in errs:
            if t >= last:
                break
            if not on and abs(e) > CHASE_TOLERANCE_MS - 1.0:
                on = True
            elif on and abs(e) <= settle + 0.5:
                on = False
            if on and t >= first:
                return True
        return False

    def held_from(member: Member, errs, first: float, last: float, bound: float):
        """The bound a member's distance from its code is held to between two
        samples - an on-time start's, or that plus how far its late launch put
        it - whether it was past the law's tolerance there (so catching up, not
        held to the code's speed), and what to add to the check's detail."""
        excuse, began = late.get(member.cue, ("", 0.0))
        if not excuse:
            return bound, False, ""
        steered = catching_up(member, errs, first, last)
        return (bound + abs(began), steered,
                f"; launched late, {began:+.3f} ms from its code ({excuse}), so held to {bound:g} ms beyond that"
                + ("; past the law's tolerance here, so catching up rather than at the code's speed" if steered
                   else ""))
    a = judge_start(report, word, log, SCENE_A, tracks[SCENE_A.cue], truth, r0, True)
    b = tracks[SCENE_B.cue].onset()
    # A SCENE'S MEMBERS ARE PLACED ON THE CODE'S SAMPLES, NOT AFTER ONE ANOTHER
    # (TG.7): the placed scene's start is the sample its map gives the scene's
    # time, moved by each re-anchor until it is committed, and each member is
    # asked for on that start plus its offset in Go.dot's samples. The first
    # sound's offset is nought, so on time it began on the scene's start and the
    # second 1.5 s after it, to the sample, whatever map placed them. Launched
    # late - which the engine owns up to and which voids the first's own checks
    # above - it says nothing of the start, and the second is held instead to
    # the scene's start as one of the maps the engine held while the scene was
    # placed puts it (TG.12's CI run, where a Windows runner launched the first
    # five blocks late and the second on its own sample, 1.497589 s after the
    # first). The second launched late itself, said by the engine, voids this.
    _, a_on_map, a_excuse = asked(log, SCENE_A, truth, r0)
    a_late = "" if a is None else launched_late(a, a_on_map, a_excuse, r0)
    words = f"{word}: the scene's second sound starts 1.5 s after its first, to the sample"
    if a is None or b is None:
        report.check(False, words, f"first {a!r}, second {b!r}")
    elif not a_late:
        # The first on time, the second is held to it - unless the engine owned
        # up to launching the second late itself (TG.12's CI run, part 3: 118
        # samples late, its tick thread noted 343 ms behind its audio).
        _, _, b_excuse = asked(log, SCENE_B, truth, r0)
        b_late = launched_late(b, a + 1.5 * s, b_excuse, r0)
        if b_late and b - a - 1.5 * s > 1.0:
            report.void(words, f"{(b - a) / s:.6f} s; the second launched late ({b_late})")
        elif overloaded and abs(b - a - 1.5 * s) > 1.0:
            # TG.12's CI run, part 3: four to six samples short, only ever in a
            # run whose tick fell behind - not explained, so judged wherever the
            # tick kept up.
            report.void(words, f"{(b - a) / s:.6f} s; {overloaded}")
        else:
            report.check(abs(b - a - 1.5 * s) <= 1.0, words, f"{(b - a) / s:.6f} s")
    else:
        scene = [r for r in log.spawns_of(SCENE) if r.tick * TICK + r0 >= truth.origin]
        b_spawn = [r for r in log.spawns_of(SCENE_B.cue) if r.tick * TICK + r0 >= truth.origin]
        b_fired = log.first("run.fire", b_spawn[0].args[2]) if b_spawn else None
        held = []
        if scene and b_fired:
            before = log.map_before(LTC_ORIGIN, scene[0].seq)
            held = ([before] if before is not None else []) + [
                h for h in log.heard if h.source == LTC_ORIGIN and h.what != "stopped"
                and scene[0].seq < h.seq < b_fired.seq]
        # The sample a map gives a frame is the first at or after its moment
        # (CodeMap::sampleOf), so the scene's start is that moment rounded up.
        places = [math.ceil(h.sample_of(SCENE_A.at) + r0 - 1e-6) for h in held]
        start = min(places, key=lambda p: abs(b - 1.5 * s - p), default=None)
        detail = (f"{(b - a) / s:.6f} s after the first, which launched late ({a_late}); against the scene's start "
                  + ("on no map" if start is None else
                     f"as {len(places)} map(s) the engine held while it placed the scene put it, "
                     f"{(b - start) / s:.6f} s after it"))
        _, _, b_excuse = asked(log, SCENE_B, truth, r0)
        b_late = "" if start is None else launched_late(b, start + 1.5 * s, b_excuse, r0)
        if b_late and abs(b - start - 1.5 * s) > 1.0:
            report.void(words, f"the second launched late too ({b_late}); {detail}")
        elif overloaded and not (start is not None and abs(b - start - 1.5 * s) <= 1.0):
            report.void(words, f"{detail}; {overloaded}")
        else:
            report.check(start is not None and abs(b - start - 1.5 * s) <= 1.0, words, detail)

    # --- the free runner ----------------------------------------------------
    free = tracks[FREE.cue]
    rate = free.rate(at + s, truth.at(STOP_AT))
    report.check(rate is not None and abs(rate - 1.0) <= 2e-6,
                 f"{word}: the free runner plays at its own speed against +50 ppm and +0.1 % - it does not follow",
                 f"{rate!r}")
    errors = truth.errors(free, FREE, at, truth.at(LOOP))
    jumps = free.moves(at, truth.at(LOCATE_AT) + 4 * s)
    after_locate = [(t, e) for t, e in errors if t >= truth.at(LOCATE_AT) + 0.5 * s]
    report.check(not jumps and not free.gaps(at + 0.1 * s, truth.at(LOCATE_AT) + 4 * s),
                 f"{word}: and is never moved - not by the drift, the stop, the return or the locate back (AIH)",
                 f"steps {[(seconds(t - truth.origin), round(d, 2)) for t, d in jumps[:4]]}, "
                 f"gaps {[(seconds(x - truth.origin), seconds(y - truth.origin)) for x, y in free.gaps(at + 0.1 * s, truth.at(LOCATE_AT) + 4 * s)[:3]]}")
    if after_locate:
        report.check(after_locate[0][1] > 29000.0,
                     f"{word}: so after the locate back it is thirty seconds ahead of the code",
                     f"{after_locate[0][1]:+.1f} ms")

    # --- the varispeed chaser -------------------------------------------------
    vari = tracks[VARI.cue]
    errs = truth.errors(vari, VARI, at, truth.at(LOOP))
    loose = 3.0 if ON_CI else 1.0
    a0, a1 = at + 1.0 * s, truth.at(STEP_AT)
    rate = vari.rate(at + 1.5 * s, a1)
    bound, steered, why = held_from(VARI, errs, a0, a1, loose)
    report.check(rate is not None and (abs(rate - 1.00005) <= 5e-6 or steered) and (worst(errs, a0, a1) or 99) <= bound,
                 f"{word}: the varispeed chaser follows +50 ppm: at the code's speed and within {loose:g} ms",
                 f"speed {rate!r}, worst {worst(errs, a0, a1)!r} ms{why}")
    rate = vari.rate(truth.at(STEP_AT) + 1.5 * s, truth.at(STOP_AT))
    bound, steered, why = held_from(VARI, errs, a1, truth.at(STOP_AT), 3 * loose)
    report.check(rate is not None and (abs(rate - 1.001) <= 30e-6 or steered)
                 and (worst(errs, a1, truth.at(STOP_AT)) or 99) <= bound,
                 f"{word}: and the step to +0.1 %, within {3 * loose:g} ms",
                 f"speed {rate!r}, worst {worst(errs, a1, truth.at(STOP_AT))!r} ms{why}")
    judge_stop(report, word, VARI, vari, truth)
    r0_, r1_ = truth.at(RETURN_AT), truth.at(LOCATE_AT)
    early = [e for t, e in errs if r0_ <= t < r0_ + 0.3 * s]
    report.check(bool(early) and -150.0 <= early[0] <= -90.0,
                 f"{word}: back from the stop, the varispeed chaser is the 120 ms behind the return put it",
                 f"{early[:1]} ms")
    rate = vari.rate(r0_ + 1.0 * s, r0_ + 8.0 * s)
    report.check(rate is not None and abs(rate - 1.01 * 1.00005) <= 5e-4,
                 f"{word}: and catches up at 1 % over the code - its pitch 17 cents up (varispeed)",
                 f"speed {rate!r}")
    steps = steps_in(errs, r0_ + 0.2 * s, r1_, 3.0)
    settled = worst(errs, r1_ - 1.0 * s, r1_)
    report.check(not steps and settled is not None and settled <= (5.0 if ON_CI else 1.5),
                 f"{word}: without a jump, back in step before the locate (AIS)",
                 f"steps {[(seconds(t - truth.origin), round(d, 1)) for t, d in steps[:3]]}, "
                 f"{settled!r} ms out in the last second")
    judge_relocated(report, word, VARI, errs, truth)

    # --- the jump chaser ----------------------------------------------------------
    jump = tracks[JUMP.cue]
    errs = truth.errors(jump, JUMP, at, truth.at(LOOP))
    rate = jump.rate(at + 1.0 * s, a1)
    # A jump chaser is never steered, only jumped past the tolerance, so its
    # speed is its own however it began.
    bound, _, why = held_from(JUMP, errs, a0, a1, 2.0)
    report.check(rate is not None and abs(rate - 1.0) <= 2e-6 and (worst(errs, a0, a1) or 99) <= bound,
                 f"{word}: the jump chaser plays at its own speed between jumps, within 2 ms through +50 ppm",
                 f"speed {rate!r}, worst {worst(errs, a0, a1)!r} ms{why}")
    judge_jumping(report, word, JUMP, jump, errs, truth)
    judge_stop(report, word, JUMP, jump, truth)
    judge_returned(report, word, JUMP, errs, truth)
    judge_relocated(report, word, JUMP, errs, truth)

    # --- the timestretch chaser -------------------------------------------------
    off = [(t, p) for t, p in pitches if abs(p - TONE_HZ) > 0.5]
    report.check(len(pitches) > 60 and not off,
                 f"{word}: the timestretch chaser keeps its pitch, 1 kHz in every burst, catching up or not",
                 f"{len(pitches)} bursts; off: {[(seconds(t - truth.origin), round(p, 3)) for t, p in off[:4]]}"
                 + "".join(f"; at {seconds(t - truth.origin)}, {burst_flaws(render, STRETCH.channel, t, truth)}"
                           for t, _ in off[:2]))
    errs = truth.errors(stretch, STRETCH, at, truth.at(LOOP))
    held_to = STRETCHED_MS + (10.0 if ON_CI else 0.0)
    a_rate = stretch.rate(at + 1.5 * s, a1, 3)
    b_rate = stretch.rate(a1 + 1.5 * s, truth.at(STOP_AT), 3)
    _, a_steered, a_why = held_from(STRETCH, errs, at + 1.5 * s, a1, 0.0)
    _, b_steered, b_why = held_from(STRETCH, errs, a1 + 1.5 * s, truth.at(STOP_AT), 0.0)
    report.check(a_rate is not None and (abs(a_rate - 1.00005) <= 20e-6 or a_steered)
                 and b_rate is not None and (abs(b_rate - 1.001) <= 100e-6 or b_steered),
                 f"{word}: and runs at the code's speed - 1.00005, then 1.001 - as the varispeed one does (AIS)",
                 f"its bursts came at {a_rate!r}, then {b_rate!r}" + (b_why or a_why))
    bound, _, why = held_from(STRETCH, errs, a0, truth.at(STOP_AT), held_to)
    report.check((worst(errs, a0, truth.at(STOP_AT)) or 99) <= bound,
                 f"{word}: within {held_to:.1f} ms of the code throughout - a chunk on its clock and up to three in"
                 " the stretcher (AJC)",
                 f"worst {worst(errs, a0, truth.at(STOP_AT))!r} ms{why}; its bursts "
                 + ", ".join(f"{seconds(t - truth.origin)} {e:+.1f}" for t, e in errs[::12] if t < truth.at(STOP_AT)))
    judge_stop(report, word, STRETCH, stretch, truth)
    rate = stretch.rate(r0_ + 1.0 * s, r0_ + 8.0 * s, 3)
    report.check(rate is not None and abs(rate - 1.01 * 1.00005) <= 3e-3,
                 f"{word}: back from the stop it catches up at about 1 % - its bursts closer together, its pitch kept",
                 f"speed {rate!r}")
    settled = worst(errs, r1_ - 1.0 * s, r1_)
    report.check(settled is not None and settled <= held_to,
                 f"{word}: and is back within {held_to:.1f} ms before the locate",
                 f"{settled!r} ms")
    judge_relocated(report, word, STRETCH, errs, truth, STRETCHED_MS)

    # --- the scene --------------------------------------------------------------------
    for member in (SCENE_A, SCENE_B):
        track = tracks[member.cue]
        errs = truth.errors(track, member, truth.of(member.at), truth.at(LOOP))
        judge_jumping(report, word, member, track, errs, truth)
        judge_stop(report, word, member, track, truth)
        judge_returned(report, word, member, errs, truth)
        judge_relocated(report, word, member, errs, truth)

    # --- the loop point, a locate back before every member's time --------------------
    loop = truth.at(LOOP)
    still = [m.words for m in (JUMP, VARI, STRETCH, SCENE_A, SCENE_B)
             if not render.silent(m.channel, int(loop + 0.3 * s), int(loop + 2.0 * s))]
    report.check(not still, f"{word}: the loop point, a locate back before their time, ends every chaser (AIH)",
                 f"still sounding: {still}")


def burst_shape() -> "list[tuple[int, float]]":
    """For each of the five bursts, its length and where its middle is in its
    half second (seconds), as bursts_in reads them - from the driver's own
    file, so the reader and the writer agree. The file's first burst has no
    silence before it, so the five are read from the second on."""
    room = Path(tempfile.mkdtemp(prefix="wfg-bursts-"))
    try:
        write_bursts(room / "b.wav")
        found = bursts_in(Render.of_wav(room / "b.wav"), 0)
        shape = [(0, 0.0)] * len(BURST_LENGTHS)
        for number, (centre, length, _) in enumerate(found[:len(BURST_LENGTHS)], start=1):
            shape[number % len(BURST_LENGTHS)] = (length, (centre - number * BURST_PERIOD) / SAMPLE_RATE)
        return shape
    finally:
        for path in room.iterdir():
            path.unlink()
        room.rmdir()


def judge_stop(report: Report, word: str, member: Member, track: Track, truth: Truth) -> None:
    """Through the two seconds the code is gone, a chaser plays on at its own
    speed (AIG): heard throughout, a frame or a burst never missing."""
    first, last = truth.at(STOP_AT) + 0.1 * SAMPLE_RATE, truth.at(RETURN_AT) - 0.1 * SAMPLE_RATE
    gaps = track.gaps(first, last, 0.6 * SAMPLE_RATE if member.sound == "bursts" else 0.2 * SAMPLE_RATE)
    rate = track.rate(first, last, 3 if member.sound == "bursts" else 10)
    report.check(not gaps and rate is not None and abs(rate - 1.0) <= (2e-3 if member.sound == "bursts" else 1e-4),
                 f"{word}: the stop leaves {member.words} playing, at its own speed (AIG)",
                 f"gaps {[(seconds(a - truth.origin), seconds(b - truth.origin)) for a, b in gaps[:2]]}, "
                 f"speed {rate!r}")


def judge_jumping(report: Report, word: str, member: Member, track: Track, errs, truth: Truth) -> None:
    """Through the step to +0.1 %, a jump chaser at its own speed falls a
    millisecond a second behind, and jumps back each time it is ten out."""
    first, last = truth.at(STEP_AT), truth.at(STOP_AT)
    steps = steps_in(errs, first, last, 3.0)
    toward = [(t, d) for t, d in steps if 5.0 <= d <= 15.0]
    print(f"       {member.words}: {len(steps)} jump(s) through the step - "
          + ", ".join(f"{d:+.2f} ms at {seconds(t - truth.origin)}" for t, d in steps[:6]))
    bound = 30.0 if ON_CI else 12.0
    report.check(bool(toward) and len(toward) == len(steps) and (worst(errs, first, last) or 99) <= bound
                 and not track.gaps(first, last),
                 f"{word}: {member.words} steps back into step each time it is ten milliseconds out, never more "
                 f"than {bound:g} out (jump)",
                 f"steps {[(seconds(t - truth.origin), round(d, 2)) for t, d in steps[:4]]}, worst "
                 f"{worst(errs, first, last)!r} ms, gaps {[(seconds(a - truth.origin), seconds(b - truth.origin)) for a, b in track.gaps(first, last)[:2]]}")


def judge_returned(report: Report, word: str, member: Member, errs, truth: Truth) -> None:
    first, last = truth.at(RETURN_AT) + 0.5 * SAMPLE_RATE, truth.at(LOCATE_AT)
    out = worst(errs, first, last)
    bound = 10.0 if ON_CI else 2.0
    report.check(out is not None and out <= bound,
                 f"{word}: back from the stop, {member.words} is in step within half a second, and stays (AIG)",
                 f"{out!r} ms at worst from {seconds(first - truth.origin)}, against {bound:g}")


def judge_relocated(report: Report, word: str, member: Member, errs, truth: Truth, bound: float = 2.0) -> None:
    # To half a second before the loop point, as the MTC chaser's is judged: the
    # loop point's stop cuts the last burst short, and a burst cut short reads
    # as one of the shorter five - half a second out (TG.11's faults, part 1,
    # where the timestretch chaser was first heard re-seated at all).
    first, last = truth.at(LOCATE_AT) + 0.5 * SAMPLE_RATE, truth.at(LOOP) - 0.5 * SAMPLE_RATE
    out = worst(errs, first, last)
    bound = max(bound, 10.0) if ON_CI else bound
    report.check(out is not None and out <= bound,
                 f"{word}: the locate back 30 s re-seats {member.words}, in step within half a second (AIH)",
                 f"{out!r} ms at worst from {seconds(first - truth.origin)}, against {bound:g}")


def judge_mtc(report: Report, word: str, render: Render, log: Log, truth: Truth, r0: int,
              ahead: float = 0.0) -> None:
    """The MTC group's members, against where the engine heard the port's
    pass begin. `ahead`, MTC's lead on the render (`mtc_lead`): past
    MTC_LEAD_LIMIT the chaser followed the rig's MTC, not the port's, and its
    step with the code is not judged - the free runner, which follows nothing,
    still is, up to a second firing the rig's MTC made (below)."""
    for member in (MTC_FREE, MTC_JUMP):
        track = ltc_track(render, member)
        judge_start(report, word, log, member, track, truth, r0, not ON_CI)
        if member is MTC_FREE:
            # FIRED AGAIN IS NOT MOVED. A code that runs into a member's time
            # again after a jump back starts it again, the run still playing
            # stopped as the new one starts (AIR, RunnerTime.cpp). The port's
            # code never does so in the span judged here; an MTC ahead of the
            # render can: its locate back 30 s, anchored seconds ahead of the
            # tick that applied it, puts the code before the member's time at
            # that tick (99c90da's macOS run: 16.8 s ahead, fired again at
            # 51.86 s, read as a step of -29.86 s). Past MTC_LEAD_LIMIT it is
            # judged up to its group's next spawn of it, which comes before the
            # launch, and what follows is the rig's.
            last = truth.at(LOCATE_AT) + 4 * SAMPLE_RATE
            again = [r.tick * TICK + r0 for r in log.spawns_of(member.cue)
                     if truth.of(member.at) < r.tick * TICK + r0 < last]
            cut = min(again) if again and ahead > MTC_LEAD_LIMIT else None
            rate = track.rate(truth.of(member.at) + SAMPLE_RATE, min(truth.at(STOP_AT), cut or last))
            jumps = track.moves(truth.of(member.at), cut or last)
            report.check(rate is not None and abs(rate - 1.0) <= 2e-6 and not jumps,
                         f"{word}: the MTC free runner plays at its own speed and is never moved",
                         f"speed {rate!r}, steps {[(seconds(t - truth.origin), round(d, 2)) for t, d in jumps[:3]]}"
                         + ("" if cut is None else f"; judged to {seconds(cut - truth.origin)}, where its group"
                            f" spawned it again on an MTC {ahead:.2f} s ahead of the render"))
        else:
            # IN STEP WITH THE CODE AS GO.DOT HEARS IT: against the map the
            # engine's own `code.heard` drew at each moment, so what is judged
            # is the chase and not where the hosted pump let MTC land (above).
            errs = map_errors(track, member, log, r0)
            bound = 30.0 if ON_CI else 12.0
            for first, last, what in ((RETURN_AT, LOCATE_AT, "back from the stop"),
                                      (LOCATE_AT, LOOP - 0.5 * SAMPLE_RATE, "after the locate back 30 s")):
                out = worst(errs, truth.at(first) + 0.5 * SAMPLE_RATE, truth.at(last))
                cwords = (f"{word}: {what}, the MTC jump chaser is in step within half a second with the code as"
                          f" Go.dot hears it, never more than {bound:g} ms out (jump)")
                cdetail = f"{out!r} ms at worst, against {bound:g}"
                if ahead > MTC_LEAD_LIMIT and not (out is not None and out <= bound):
                    report.void(cwords, f"{cdetail}; the MTC arrived up to {ahead:.2f} s ahead of the render: the"
                                        " rig's audio clock, a hosted pump, fell behind the wall clock MTC arrives on")
                else:
                    report.check(out is not None and out <= bound, cwords, cdetail)


def map_errors(track: Track, member: Member, log: Log, r0: int):
    """[(render sample, ms ahead)] of a member against the map of its code
    the engine had applied at each point - where Go.dot heard the code.

    A map is applied on the tick its record names, which is due at that tick's
    sample but runs as late as the tick thread does, and a chaser it re-seats
    moves a launch horizon and a crossfade after that. So from the tick's due
    sample - never before the code's own turn the map describes, its anchor -
    until the tick could have run, by the lateness the engine's log notes there
    (two ticks when it notes none), and the chaser moved, a point is held to
    whichever of the maps it straddles it is nearer, and only after that to the
    new one alone. Judged from the due sample alone, a tick that ran a hundred
    milliseconds late at the locate back 30 s held the chaser, still in step
    with the code before the locate, to the map after it - 30 s out - and one
    late at the loop point 45 s (TG.12's CI run, on a Windows runner, while the
    LTC chasers judged against the story itself were in step throughout)."""
    source = LTC_ORIGIN if member.source == "ltc" else MTC_ORIGIN
    maps = [h for h in log.heard if h.source == source]
    reseat = 0.1 * SAMPLE_RATE

    def applied(h: Heard) -> "tuple[float, float]":
        due = h.tick * TICK + r0
        first = due if h.what == "running" else max(due, h.sample + r0)
        return first, due + max(log.tick_late_around(h.tick), 2 * 1000.0 * TICK / SAMPLE_RATE) * MS + reseat

    spans = [applied(h) for h in maps]

    def ahead(heard: Heard, t: float, file: float) -> float:
        code = heard.frame + (t - r0 - heard.sample) * heard.speed / FRAME
        return (file - ((code - member.at) / FPS - member.offset)) * 1000.0

    out = []
    k = -1
    for t, file in track.points:
        while k + 1 < len(maps) and spans[k + 1][0] <= t:
            k += 1
        if k < 0:
            continue
        # The newest map applied by now, and those before it that may still have
        # been the one in force - each newer one not yet certainly applied.
        candidates = [maps[k]]
        j = k
        while j > 0 and spans[j][1] > t:
            j -= 1
            candidates.append(maps[j])
        errors = [ahead(h, t, file) for h in candidates if h.what != "stopped"]
        if errors:
            out.append((t, min(errors, key=abs)))
    return out


def judge_polled(report: Report, word: str, polled: Polled, truth: Truth, r0: int, why: str = "") -> None:
    """The readouts the window shows, read while the story ran: polled over
    HTTP on the driver's clock against the story on the render's, so a run whose
    rig fell behind the wall clock, or whose tick fell behind its audio, read
    them at moments the story had not reached - `why` says which."""
    def rows(first: float, last: float):
        return [row for row in polled.rows if truth.at(first) <= row["tick"] * TICK + r0 < truth.at(last)]

    def judged(ok: bool, words: str, detail: str) -> None:
        if not polled.rows:
            report.void(words, "nothing was read while the story ran (a room judged again)")
        elif not ok and (ON_CI or why):
            report.void(words, f"{detail}; " + (why or "read over HTTP while the story ran, which a starved runner"
                                                        " reads late"))
        else:
            report.check(ok, words, detail)

    seen = [row.get("ltcInState") for row in rows(STOP_AT + 0.3 * SAMPLE_RATE, RETURN_AT - 0.1 * SAMPLE_RATE)]
    judged("stopped" in seen, f"{word}: sync/ltcInState reads stopped through the stop", f"{seen[:6]}")
    speeds = [row.get("ltcInSpeed") for row in rows(LEAD_IN + 4 * SAMPLE_RATE, STEP_AT)]
    judged(bool(speeds) and all(isinstance(v, float) and abs(v - 1.00005) <= 5e-6 for v in speeds[-5:]),
           f"{word}: sync/ltcInSpeed reads 1.00005, fifty parts per million fast", f"{speeds[-3:]}")
    speeds = [row.get("ltcInSpeed") for row in rows(STEP_AT + 3 * SAMPLE_RATE, STOP_AT)]
    judged(bool(speeds) and all(isinstance(v, float) and abs(v - 1.001) <= 5e-5 for v in speeds[-5:]),
           f"{word}: and 1.001 after the step", f"{speeds[-3:]}")
    states = [row.get("vari") for row in rows(RETURN_AT + 0.5 * SAMPLE_RATE, RETURN_AT + 6 * SAMPLE_RATE)]
    judged("catching" in states, f"{word}: the varispeed chaser's run/chaseState reads catching while it catches up",
           f"{sorted(set(map(str, states)))}")
    states = [row.get("vari") for row in rows(LOCATE_AT - 1.5 * SAMPLE_RATE, LOCATE_AT - 0.2 * SAMPLE_RATE)]
    judged(bool(states) and states[-1] == "inStep", f"{word}: and inStep once it has", f"{states[-3:]}")
    states = {row.get("free") for row in rows(LEAD_IN + 3 * SAMPLE_RATE, LOOP)} - {None}
    judged(states == {"free"}, f"{word}: and the free runner's reads free", f"{sorted(states)}")


def mtc_truth(log: Log, ltc_truth: Truth, r0: int) -> "Truth | None":
    """Where the port's pass of the story began, in render samples: the
    median, over the MTC records of the pass's first stretch, of where each
    put its frame against where the story's moments put it. The port's pass
    starts when serve starts its player, beside the audio, so it is the LTC's
    pass give or take a few milliseconds; how Go.dot places a moment of the
    steady clock on its samples - a block or so early, through the tick's
    clock pairs - is in it."""
    first, last = ltc_truth.origin - 0.5 * SAMPLE_RATE, ltc_truth.at(STEP_AT)
    offsets = []
    for heard in log.heard:
        if heard.source == MTC_ORIGIN and heard.what != "stopped" and first <= heard.sample + r0 < last:
            moment = STORY.sample_of(heard.frame)
            if moment is not None:
                offsets.append(heard.sample + r0 - moment)
    return Truth(statistics.median(offsets)) if offsets else None


def scenario(report: Report, room: Path, held: bool, word: str, unheld: dict) -> dict:
    until = LOOP + 2.5 * SAMPLE_RATE if not held else LEAD_IN + 14 * SAMPLE_RATE
    served = serve(report, room, held, word, until)
    return {} if served is None else analyse(report, word, held, *served, unheld)


def starts_of(render: Render, log: Log, truth: Truth, mtc: Truth, r0: int) -> dict:
    """Where each member's sound began against its code's frame, in samples,
    whether it was launched late for a reason the engine owned up to, where it
    began against the sample the engine's map gave its time, and that map:
    {cue: (samples, late, samples from the map's, (frame, sample, speed))}."""
    out = {}
    for member in MEMBERS:
        if member.sound != "ltc":
            continue
        code = truth if member.source == "ltc" else mtc
        onset = ltc_track(render, member).onset()
        want = code.of(member.at + member.offset * FPS)
        if onset is not None and want is not None:
            heard, on_map, excuse = asked(log, member, code, r0)
            # A scene's member is asked for on the scene's start plus its offset
            # in Go.dot's samples (TG.7), so that is its own place on the map;
            # and the sample a map gives a frame is the first at or after its
            # moment (CodeMap::sampleOf), so a start on its map's sample is
            # that moment rounded up, which two maps round differently.
            place = None if on_map is None else on_map + member.offset * SAMPLE_RATE
            out[member.cue] = (onset - want, bool(launched_late(onset, place, excuse, r0)),
                               None if place is None else onset - math.ceil(place - 1e-6),
                               None if heard is None else (heard.frame, heard.sample, heard.speed))
    return out


def analyse(report: Report, word: str, held: bool, story_pass: int, log: Log, render: Render,
            polled: Polled, unheld: dict) -> dict:
    """Everything judged of one serve; where each member started, for the
    held serve to be held to."""
    truth = Truth(story_pass * LOOP)

    # --- the input, as the engine heard it ---------------------------------------
    first = next((h for h in log.heard if h.source == LTC_ORIGIN and h.what == "jumped"
                  and truth.origin - SAMPLE_RATE <= h.sample < truth.origin + LEAD_IN), None)
    if not report.check(first is not None, f"{word}: the engine hears the LTC on {INPUT} (code.heard jumped)"):
        return {}
    exact = truth.of(first.frame)
    r0 = BLOCK * int(round((exact - first.sample) / BLOCK))
    report.check(abs(exact - first.sample - r0) <= 1.5 and abs(r0) <= 4 * BLOCK,
                 f"{word}: on its own sample - {label_of(first.frame)} heard where the input's frame begins",
                 f"the input's frame at {exact:.3f}, heard on {first.sample}, the session {r0} samples into the render")
    print(f"       the session's sample nought is the render's {r0}")

    mtc = mtc_truth(log, truth, r0)
    if not report.check(mtc is not None, f"{word}: the engine hears the MTC on {PORT} (code.heard)"):
        return {}
    print(f"       the port's pass began {seconds(mtc.origin - truth.origin)} after the input's")

    if held:
        for member in (FREE, JUMP, VARI, SCENE_A):
            judge_start(report, word, log, member, ltc_track(render, member), truth, r0, True)
        for member in (MTC_FREE, MTC_JUMP):
            judge_start(report, word, log, member, ltc_track(render, member), mtc, r0, not ON_CI)
        report.check(any(h.source == MTC_ORIGIN and h.origin == MTC_ORIGIN for h in log.heard)
                     and any(h.source == LTC_ORIGIN and h.origin == LTC_ORIGIN for h in log.heard),
                     f"{word}: held, both codes are still heard - only sends are held, and these are not sends")
        starts = starts_of(render, log, truth, mtc, r0)
        if unheld:
            # FIRING THE SAME is starting each member where the map of its code
            # gives its time, as unheld does. Both serves hear the same input,
            # but each draws its map on its own ticks: a jump said on a tick
            # that ran late has more frames in its fit, so it can already know
            # the code's +50 ppm, and then needs no re-anchor before the members'
            # time - TG.12's CI run, where the held serve's map (the first jump,
            # at 1.00005) and the unheld one's (a re-anchor ten seconds on) put
            # 01:00:02:00 two samples apart, each within a millisecond of the
            # input's frame as judge_start holds them. So where both serves drew
            # the same map, the two starts are held to the same sample of the
            # code; where they drew two, each start is held to its own map's
            # sample, the same distance from it as unheld - the hold's question,
            # and not the watcher's.
            def same_start(member: Member) -> bool:
                mine, theirs = starts.get(member.cue), unheld.get(member.cue)
                if mine is None or theirs is None:
                    return False
                if mine[3] is not None and mine[3] == theirs[3]:
                    return abs(mine[0] - theirs[0]) <= 1.0
                return mine[2] is not None and theirs[2] is not None and abs(mine[2] - theirs[2]) <= 1.0

            def said(member: Member, start) -> str:
                if start is None:
                    return "none"
                place = "no map" if start[2] is None else f"{start[2]:+.2f} from the map's sample"
                drawn = "" if start[3] is None else f", the map {label_of(start[3][0])} on {start[3][1]} at {start[3][2]:.6f}"
                return f"{start[0]:+.2f} samples from the input's frame, {place}{drawn}"

            ltc_members = [m for m in MEMBERS if m.source == "ltc" and m.sound == "ltc"
                           and not starts.get(m.cue, (0, False))[1] and not unheld.get(m.cue, (0, False))[1]]
            same = [m for m in ltc_members if same_start(m)]
            fwords = (f"{word}: every LTC member starts where its map of the code puts its time, as unheld does -"
                      " it fires the same")
            fdetail = "; ".join(f"{m.words} held {said(m, starts.get(m.cue))}, unheld {said(m, unheld.get(m.cue))}"
                                for m in ltc_members)
            # Each serve draws its maps and commits its scene on its own ticks,
            # so where either serve's tick fell behind its audio the two may
            # differ by what the late ticks changed - the scene's second sound
            # four to six samples, in TG.12's CI run, part 3 - not by what the
            # hold does.
            behind = max(log.most_behind(TICK_WARM_UP), unheld.get(BEHIND_KEY, 0.0))
            fired = bool(ltc_members) and len(same) == len(ltc_members)
            if not fired and behind >= TICK_BEHIND_LIMIT_MS:
                report.void(fwords, f"{fdetail}; a serve's tick noted up to {behind:g} ms behind its audio,"
                                    " an overloaded run")
            else:
                report.check(fired, fwords, fdetail)
        return starts

    judge_turns(report, word, log, truth, r0, LTC_ORIGIN, LTC_ORIGIN, True)
    # THE TICK KEPT UP WITH ITS AUDIO, judged once and on its own: everything
    # below that a tick held back changes - MTC placed through its clock pairs,
    # the readouts it publishes - is not judged against a run where it did not,
    # so an overloaded machine says so in one check rather than in a scatter of
    # others. Not voided where it is judged: an engine grown slow is what this
    # driver caught in TG.11's faults, part 3 (the tick 15.8 s behind after
    # thirty passes). On CI a runner's starvation is not the engine's, and a
    # show machine is not overloaded (the author, 2026-10-14).
    behind = log.most_behind(TICK_WARM_UP)
    kept_up = (f"{word}: the engine's tick kept up with its audio - never noted {TICK_BEHIND_LIMIT_MS:g} ms behind or"
               " more after its first three seconds")
    if behind >= TICK_BEHIND_LIMIT_MS and ON_CI:
        report.void(kept_up, f"noted up to {behind:g} ms behind; a CI runner's, not a show machine's")
    else:
        report.check(behind < TICK_BEHIND_LIMIT_MS, kept_up,
                     f"noted up to {behind:g} ms behind" + ("" if behind < TICK_BEHIND_LIMIT_MS else
                     "; the machine overloaded, or the engine grown slow - what a tick held back changes is not"
                     " judged as the engine's below"))
    overloaded = (f"the engine's tick noted up to {behind:g} ms behind its audio, an overloaded run"
                  if behind >= TICK_BEHIND_LIMIT_MS else "")
    ahead = mtc_lead(log)
    print(f"       MTC arrived at most {ahead:+.3f} s ahead of the render (its records' anchors against their ticks)")
    judge_turns(report, word, log, mtc, r0, MTC_ORIGIN, MTC_ORIGIN, not ON_CI, ahead, overloaded)
    judge_chasers(report, word, render, log, truth, r0, overloaded)
    judge_mtc(report, word, render, log, mtc, r0, ahead)
    judge_polled(report, word, polled, truth, r0,
                 f"the MTC arrived up to {ahead:.2f} s ahead of the render: the rig's audio clock, a hosted pump, fell"
                 " behind the wall clock the driver polls on" if ahead > MTC_LEAD_LIMIT else overloaded)
    starts = starts_of(render, log, truth, mtc, r0)
    starts[BEHIND_KEY] = behind
    return starts


# =============================================================================

def self_check(report: Report) -> None:
    """The driver's writers read back by its readers before anything is asked
    of the engine: a member's file frame by frame on its own samples, the input
    every frame within a tenth of a sample of the story, the bursts at 1 kHz,
    the MTC file through timecode.py's MTC reader."""
    room = Path(tempfile.mkdtemp(prefix="wfg-timegroup-self-"))
    try:
        write_member(room / "m.wav")
        frames = ltc_frames(Render.of_wav(room / "m.wav"), 0)
        report.check(len(frames) == MEMBER_SECONDS * FPS and all(abs(s - k * FRAME) <= 1e-3 for s, k in frames),
                     "the driver's member file reads back frame by frame, frame k on its sample k * 1920",
                     f"{len(frames)} frames, the worst {max((abs(s - k * FRAME) for s, k in frames), default=None)!r}")
        write_levels(room / "in.wav", ltc_edges(STORY), LOOP)
        frames = ltc_frames(Render.of_wav(room / "in.wav"), 0)
        expected = sum(p[4] for p in STORY.pieces if p[0] == "code")
        out = []
        for start, frame in frames:
            nearest = min((p[1] + (frame - p[3]) * FRAME / p[5] for p in STORY.pieces
                           if p[0] == "code" and p[3] <= frame < p[3] + p[4]), key=lambda t: abs(t - start))
            out.append(abs(start - nearest))
        report.check(len(frames) == expected and max(out) <= 0.1,
                     "the driver's input reads back as the story, every frame within a tenth of a sample",
                     f"{len(frames)} of {expected} frames, the worst {max(out, default=None)!r}")
        write_bursts(room / "b.wav")
        bursts = bursts_in(Render.of_wav(room / "b.wav"), 0)
        shape = burst_shape()
        lengths = sorted(length for length, _ in shape)
        report.check(len(bursts) == int(MEMBER_SECONDS * 2) - 1
                     and all(p is not None and abs(p - TONE_HZ) <= 0.01 for _, _, p in bursts)
                     and all(b - a > 25 * MS for a, b in zip(lengths, lengths[1:])),
                     "the driver's bursts read back at 1 kHz, each of the five lengths told from the others",
                     f"{len(bursts)} bursts, lengths {[round(x / MS, 2) for x in lengths]} ms")
        lines = [(int(a), int(b), tuple(int(x, 16) for x in rest.split()))
                 for a, b, rest in (line.split(" ", 2) for line in mtc_lines(STORY))]
        cycles, broken = tc.mtc_cycles([line for line in lines if tc.is_quarter_frame(line[2])])
        wrong = []
        for due, label, code in cycles:
            read = STORY.code_at(due * SAMPLE_RATE / 1e9 + 0.5)
            if code != MTC_RATE_CODE or read is None or abs(read - frame_of(tc.label_text(label))) > 0.01:
                wrong.append((due, tc.label_text(label)))
        locates = [tc.label_text(tc.full_frame_of(line[2])[0]) for line in lines if tc.is_full_frame(line[2])]
        report.check(not wrong and not broken and len(cycles) == sum(p[4] // 2 for p in STORY.pieces)
                     and locates == ["00:59:40:00", "01:00:29:03", "01:00:17:03"],
                     "the driver's MTC file reads back, through timecode.py's reader, as the story: cycles naming"
                     " the frame each began on, a full frame at the start, the return and the locate",
                     f"{len(cycles)} cycles, {broken} broken, wrong {wrong[:3]}, full frames {locates}")
    finally:
        for path in room.iterdir():
            path.unlink()
        room.rmdir()


def option(name: str) -> "str | None":
    return next((arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith(f"--{name}=")), None)


def run() -> int:
    """Both serves in rooms of their own - or, by hand, `--keep=<folder>` to
    leave each room there (`<folder>/main`, `<folder>/held`) and
    `--analyse=<folder>` to judge rooms kept before without serving again."""
    report = Report(f"time groups and chase: LTC and MTC in ({LOCALE or 'C'})")
    self_check(report)
    kept, again = option("keep"), option("analyse")
    if_red = None if kept or again else os.environ.get("WFG_KEEP_ROOMS")
    if if_red:
        kept = str(Path(if_red) / (LOCALE or "C"))
    unheld: dict = {}
    for held, word, name in ((False, "LTC and MTC", "main"), (True, "held", "held")):
        if again:
            room = Path(again) / name
            story_pass = int((room / "pass.txt").read_text(encoding="ascii"))
            starts = analyse(report, word, held, story_pass, Log(room / "session.wfglog"),
                             Render(room / "out.wav"), Polled(), unheld)
        elif kept:
            room = Path(kept) / name
            if room.exists():
                common.shutil.rmtree(room)
            room.mkdir(parents=True)
            starts = scenario(report, room, held, word, unheld)
        else:
            with tempfile.TemporaryDirectory(prefix="wfg-timegroup-") as scratch:
                starts = scenario(report, Path(scratch), held, word, unheld)
        if not held:
            unheld = starts
    code = report.finish()
    if if_red and code == 0:
        common.shutil.rmtree(kept, ignore_errors=True)
    return code


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"timegroup: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"timegroup: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    try:
        return run()
    except HarnessError as problem:
        print(f"timegroup: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
