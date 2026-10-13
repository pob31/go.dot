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

"""Timecode generated, driven from outside the process (namespace draft 59,
TC.9).

WHAT THIS FILE IS FOR. The unit suite proves each piece of outgoing timecode
against the C++ readers of the tests' own: the bits, the generator, the MTC lane
on a hand clock, the Runner's placing. What none of it can prove is the
product: `wfg serve` with a show, fired over OSC, writing its sound to a WAV
(`--hosted --render`) and its MIDI to a file (`--midi-render=TC=<file>`), and
what comes out read back by a THIRD implementation of the standard - the LTC
decoder and the MTC reader below, written in Python from SMPTE 12M's and the
MIDI specification's own description, sharing nothing with the encoder, the
generator or `tests/LtcReader.h`. A mistake the engine and its own test reader
share is invisible to the unit suite; it is not invisible here.

THE SHOW (tests/fixtures/bundles/timecode): a stereo output "Tone" and a
one-channel output "LTC", a MIDI port "TC", `<Sync frameRate="25">` naming both.
A timeline group holding a steady tone and an LTC cue from 01:00:00:00; an LTC
cue from 02:00:00:00, which takes the line over; and a timeline group holding an
MTC cue from 10:00:00:00 for two seconds, whose footer holds a memo cue.

THE SCENARIO, by GO as an operator runs it: GO, the song and its code; GO, the
second code takes over; Esc; GO, the MTC. Three passes, each on a copy of the
fixture in a temporary room (the autosave writes recovery/ beside the show it
serves, and the media are written here, never into tests/fixtures):

1. 25 fps at 48 kHz. The tone's first sample and the code's first sample are
   the same sample (AHN); the code decodes as consecutive frames from
   01:00:00:00, each at its exact place, sync word and parity right; the second
   cue's code begins at its launch with no quiet between them (AHC, AHL) and the
   first run reads done; Esc ends the code on a frame boundary within a frame of
   the press, the line silent after the edge filter's tail (AHK); the MTC file
   opens with a full frame, then quarter frames a quarter of a frame apart in
   their due times, cycles naming labels two frames apart, nothing due after the
   length, each sent near its moment (AHO, AHP); the run done, the footer run.
2. The same with `--hold` (section 58, AGG, AGH): no MTC leaves - the file holds
   nothing - and its run still ends done, unwarned; the LTC is still in the WAV,
   since sound is never held.
3. 29.97 drop-frame at 44.1 kHz, the song's code starting at 01:00:59:00 so the
   minute turns a second in: every frame carries the drop flag, and 01:00:59;29
   is followed by 01:01:00;02.

TIMING IS JUDGED LOOSELY ON CI. Where a message leaves against its moment
measures the runner as much as Go.dot; off CI the bounds are this machine's,
on CI only a clock that has stopped trips them. Samples are never loose: where
the code begins, where each frame begins and where it stops are the render's
own arithmetic, the same on every machine. What a machine too busy to keep
time makes the engine do, and the engine owns up to, is allowed anywhere and
nothing else is: a quarter frame dropped late, counted in `sync/mtcLate`, the
code picking up at a cycle's start; a start that reached the audio side or the
lane after its sample, counted in `sync/ltcLate` or `sync/mtcLate`, beginning
some frames into its count.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import array
import os
import re
import statistics
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
from common import HarnessError, Report, Server
from first_sound import go, run_for_cue, value_of, wait_for_run_state, wait_for_render_tail

ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "timecode"

TONE = "TC0M0001"          # the steady tone, in the song's timeline group
SONG_CODE = "TC0T0001"     # LTC from 01:00:00:00, beside the tone
TAKEOVER = "TC0T0002"      # LTC from 02:00:00:00, which takes the line over
MTC_CUE = "TC0T0003"       # MTC from 10:00:00:00 for two seconds
FOOTER_MEMO = "TC0F0002"   # the memo cue in the MTC group's footer

TONE_CHANNEL = 0           # the output "Tone", channels 0 and 1
LTC_CHANNEL = 2            # the output "LTC", one channel
BLOCK = 128                # common.Server's buffer
LTC_LEVEL_DB = -18.0       # sync/ltcLevel's default (AHH)
TAIL = 64                  # samples the edge filter takes to reach nought (AHJ)
MTC_LENGTH = 2.0           # the MTC cue's length, seconds
SPREAD = 0.75              # how far apart, in samples, two frames' edge delays may read


# =============================================================================
# Rates and labels, from the standards
# =============================================================================

class Rate:
    """A frame rate as the show names it: the exact frames a second
    (`num / den`), the labels' count a second, the drop flag, and MIDI Time
    Code's rate code."""

    def __init__(self, word: str, num: int, den: int, nominal: int, drop: bool, mtc_code: int):
        self.word = word
        self.num = num
        self.den = den
        self.nominal = nominal
        self.drop = drop
        self.mtc_code = mtc_code

    def frame_samples(self, sample_rate: int) -> float:
        return sample_rate * self.den / self.num

    def frames_in(self, seconds: float) -> int:
        """Whole frames a length takes, rounded up (AHF): the generator ends a
        length on the frame boundary at or after it."""
        exact = seconds * self.num / self.den
        whole = int(exact)
        return whole if exact - whole < 1e-9 else whole + 1


RATES = {
    "23.976": Rate("23.976", 24000, 1001, 24, False, 0),
    "24": Rate("24", 24, 1, 24, False, 0),
    "25": Rate("25", 25, 1, 25, False, 1),
    "29.97df": Rate("29.97df", 30000, 1001, 30, True, 2),
    "29.97": Rate("29.97", 30000, 1001, 30, False, 3),
    "30": Rate("30", 30, 1, 30, False, 3),
}


def parse_label(text: str):
    """(hours, minutes, seconds, frames) from `HH:MM:SS:FF` - a semicolon or a
    full stop read too - or None."""
    parts = re.split(r"[:;.]", (text or "").strip())
    if len(parts) != 4 or not all(part.isdigit() for part in parts):
        return None
    return tuple(int(part) for part in parts)


def label_text(label, drop: bool = False) -> str:
    hours, minutes, seconds, frames = label
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}{';' if drop else ':'}{frames:02d}"


def step_label(label, rate: Rate, count: int = 1):
    """The label `count` frames on, counted the way a reader counts them: the
    frames wrap at the rate's count, the seconds at sixty, the minutes at sixty,
    the hours at twenty-four, and at drop-frame the labels 00 and 01 of every
    minute that is not a tenth one do not exist. Stepped one frame at a time on
    purpose - the engine computes labels from a frame count, and this walks
    them, so the two share no arithmetic."""
    hours, minutes, seconds, frames = label

    for _ in range(count):
        frames += 1
        if frames >= rate.nominal:
            frames = 0
            seconds += 1
        if seconds >= 60:
            seconds = 0
            minutes += 1
        if minutes >= 60:
            minutes = 0
            hours = (hours + 1) % 24
        if rate.drop and seconds == 0 and frames < 2 and minutes % 10 != 0:
            frames = 2

    return (hours, minutes, seconds, frames)


def steps_between(start, target, rate: Rate, step: int, limit: int):
    """How many steps of `step` frames lead from one label to another, up to
    `limit` steps - or None when none does."""
    label = start

    for count in range(limit + 1):
        if label == target:
            return count
        label = step_label(label, rate, step)

    return None


# =============================================================================
# The render, read as floats
# =============================================================================

def wav_layout(path: Path) -> "tuple[int, int, int, int]":
    """(sample rate, channels, offset of the samples, frames the header covers)
    of a 32-bit float WAV, the chunks walked by hand - Python's `wave` reads
    integer PCM only, and the hosted render is float end to end. Only what the
    header covers is counted: the writer rewrites it once a second, and the
    bytes past the last rewrite are bytes no reader looks at."""
    with path.open("rb") as file:
        head = file.read(12)
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise HarnessError(f"{path} is not a RIFF/WAVE file")

        fmt_tag = bits = channels = rate = 0
        at = 12

        while True:
            file.seek(at)
            chunk = file.read(8)
            if len(chunk) < 8:
                break
            name, size = chunk[:4], struct.unpack("<I", chunk[4:])[0]

            if name == b"fmt ":
                body = file.read(size)
                fmt_tag, channels, rate = struct.unpack_from("<HHI", body, 0)
                bits = struct.unpack_from("<H", body, 14)[0]
                if fmt_tag == 0xFFFE and len(body) >= 26:
                    fmt_tag = struct.unpack_from("<H", body, 24)[0]
            elif name == b"data":
                if fmt_tag != 3 or bits != 32 or channels == 0:
                    raise HarnessError(f"{path} is format {fmt_tag} at {bits} bits, not 32-bit float")
                return rate, channels, at + 8, size // (4 * channels)

            at += 8 + size + (size & 1)

    raise HarnessError(f"{path} has no samples")


def channel_chunks(path: Path, channel: int, chunk_frames: int = 1 << 16):
    """One channel of the render, a piece at a time, as `array('f')`: a ten
    minute render read whole would be a third of a gigabyte of floats."""
    _rate, channels, offset, frames = wav_layout(path)
    swap = sys.byteorder != "little"

    with path.open("rb") as file:
        file.seek(offset)
        left = frames

        while left > 0:
            take = min(chunk_frames, left)
            raw = file.read(take * channels * 4)
            raw = raw[:len(raw) - len(raw) % (4 * channels)]
            if not raw:
                return
            data = array.array("f")
            data.frombytes(raw)
            if swap:
                data.byteswap()
            yield data[channel::channels]
            left -= take


def channel_of(path: Path, channel: int) -> "array.array":
    whole = array.array("f")
    for piece in channel_chunks(path, channel):
        whole.extend(piece)
    return whole


def first_nonzero(samples, start: int = 0) -> int:
    for n in range(start, len(samples)):
        if samples[n] != 0.0:
            return n
    return -1


# =============================================================================
# LTC, decoded from SMPTE 12M's description
# =============================================================================

SYNC_WORD = (0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1)


class LtcFrame:
    """One frame read off the line: where its first bit began and its last
    ended (fractional samples), its label, its flags and whether it reads."""

    __slots__ = ("start", "end", "label", "drop", "colour", "parity_even", "bcd_ok")

    def __init__(self, start, end, label, drop, colour, parity_even, bcd_ok):
        self.start = start
        self.end = end
        self.label = label
        self.drop = drop
        self.colour = colour
        self.parity_even = parity_even
        self.bcd_ok = bcd_ok


class LtcDecoder:
    """BIPHASE MARK, READ AS A RECEIVER READS IT, from a stream of samples.

    EDGES are zero crossings with hysteresis: the line is high above half the
    code's level and low below minus half, and a change between the two is an
    edge, placed at the fraction of a sample where it crossed. A line that stays
    inside the band for longer than a transition takes is QUIET: the code has
    ended, and where it fell into the band is its last edge - which is how the
    last bit of a stopped code is completed, every bit needing the edge that
    starts the next. From quiet, the first excursion is the first edge.

    BITS: every cell begins with an edge, and a one has a second in its middle.
    So an interval shorter than three quarters of a bit is half a bit, two of
    which make a one, and a longer one is a zero. An interval far from either
    length, a half-bit with no partner, or a stretch with no edge at all is a
    bit error, kept with where it happened.

    FRAMES: eighty bits, sent bit 0 first, ending in the sync word 0011 1111
    1111 1101. When the last sixteen bits read it, the eighty before are a
    frame: BCD units and tens of the frames, seconds, minutes and hours, the
    drop-frame and colour flags at bits 10 and 11, and the polarity bit making
    the ones even - which a frame that reads must show."""

    def __init__(self, sample_rate: int, rate: Rate, level: float):
        self.bit = sample_rate * rate.den / (rate.num * 80.0)
        self.threshold = 0.5 * level
        self.quiet_limit = max(4, int(0.4 * self.bit))
        self.frames: "list[LtcFrame]" = []
        self.errors: "list[tuple[float, str]]" = []
        self.starts: "list[float]" = []     # where a code rose out of quiet
        self.quiets: "list[float]" = []     # where a code fell quiet
        self._state = 0
        self._prev = 0.0
        self._n = 0
        self._quiet = 0
        self._entered = 0.0
        self._last_edge = None
        self._bit_start = 0.0
        self._pending = False
        self._bits: "list[tuple[int, float]]" = []

    def feed(self, samples) -> None:
        h = self.threshold
        prev = self._prev
        n = self._n
        state = self._state
        quiet = self._quiet
        limit = self.quiet_limit

        for x in samples:
            if x > h:
                if state != 1:
                    self._edge(n - 1 + (h - prev) / (x - prev))
                    state = 1
                quiet = 0
            elif x < -h:
                if state != -1:
                    self._edge(n - 1 + (-h - prev) / (x - prev))
                    state = -1
                quiet = 0
            else:
                if quiet == 0:
                    edge_level = h if prev > h else (-h if prev < -h else None)
                    self._entered = (n - 1 + (edge_level - prev) / (x - prev)) if edge_level is not None else n
                quiet += 1
                if quiet == limit and state != 0:
                    self._edge(self._entered, falls_quiet=True)
                    state = 0
            prev = x
            n += 1

        self._prev = prev
        self._n = n
        self._state = state
        self._quiet = quiet

    def _edge(self, at: float, falls_quiet: bool = False) -> None:
        if self._last_edge is None:
            self.starts.append(at)
            self._last_edge = at
            self._bit_start = at
            self._pending = False
            self._bits = []
            return

        interval = at - self._last_edge
        bit = self.bit

        if interval < 0.75 * bit:
            if not 0.3 * bit <= interval <= 0.7 * bit:
                self.errors.append((at, f"a half-bit {interval:.2f} samples long, against {bit / 2:.2f}"))
            if self._pending:
                self._pending = False
                self._emit(1, self._bit_start, at)
                self._bit_start = at
            else:
                self._pending = True
        elif interval <= 1.25 * bit:
            if not 0.8 * bit <= interval <= 1.2 * bit:
                self.errors.append((at, f"a bit {interval:.2f} samples long, against {bit:.2f}"))
            if self._pending:
                self.errors.append((at, "half a one with no other half"))
                self._pending = False
                self._emit(0, self._last_edge, at)
            else:
                self._emit(0, self._bit_start, at)
            self._bit_start = at
        else:
            self.errors.append((at, f"no edge for {interval / bit:.2f} bits"))
            self._pending = False
            self._bits = []
            self._bit_start = at

        self._last_edge = at

        if falls_quiet:
            self.quiets.append(at)
            self._last_edge = None
            self._pending = False
            self._bits = []

    def _emit(self, value: int, start: float, end: float) -> None:
        bits = self._bits
        bits.append((value, start))

        if len(bits) >= 80 and tuple(b for b, _ in bits[-16:]) == SYNC_WORD:
            word = [b for b, _ in bits[-80:]]
            self.frames.append(self._frame(word, bits[-80][1], end))
            self._bits = []
        elif len(bits) > 400:
            del bits[:-80]

    @staticmethod
    def _frame(word, start: float, end: float) -> LtcFrame:
        def field(first: int, count: int) -> int:
            return sum(word[first + k] << k for k in range(count))

        units = (field(0, 4), field(16, 4), field(32, 4), field(48, 4))
        tens = (field(8, 2), field(24, 3), field(40, 3), field(56, 2))
        bcd_ok = all(unit <= 9 for unit in units)
        label = (units[3] + 10 * tens[3], units[2] + 10 * tens[2],
                 units[1] + 10 * tens[1], units[0] + 10 * tens[0])
        return LtcFrame(start, end, label, word[10] == 1, word[11] == 1, sum(word) % 2 == 0, bcd_ok)


def decode_ltc(samples, sample_rate: int, rate: Rate, level: float) -> LtcDecoder:
    decoder = LtcDecoder(sample_rate, rate, level)
    decoder.feed(samples)
    decoder.feed([0.0] * (decoder.quiet_limit + 1))    # a line cut by the file's end still ends
    return decoder


def check_run_of_frames(frames: "list[LtcFrame]", first_label, rate: Rate, frame_len: float,
                        zero: float, low: float, high: float) -> "tuple[bool, str]":
    """Whether frames are consecutive from a label, each a frame long, and each
    after the first beginning where its count of frames from `zero` puts it -
    `zero + k * frame_len`, never accumulated - give or take what the edge
    filter and the threshold add to every edge alike: `low` to `high` samples,
    and the same for all of them to within SPREAD. A code that drifted, slipped
    a sample, or lost or doubled a frame falls outside at once. (ok, what was
    heard when not)."""
    expected = first_label

    for k, frame in enumerate(frames):
        if frame.label != expected:
            return False, (f"frame {k} read {label_text(frame.label, rate.drop)} where "
                           f"{label_text(expected, rate.drop)} was due; heard "
                           + ", ".join(label_text(f.label, rate.drop) for f in frames[max(0, k - 3):k + 3]))

        off = frame.start - (zero + k * frame_len)
        if k > 0 and not low <= off <= high:
            return False, (f"frame {k} ({label_text(frame.label, rate.drop)}) began at {frame.start:.2f}, "
                           f"{off:+.2f} samples from {zero + k * frame_len:.2f}")
        if abs(frame.end - frame.start - frame_len) > 2.0:
            return False, (f"frame {k} ({label_text(frame.label, rate.drop)}) lasted "
                           f"{frame.end - frame.start:.2f} samples, against {frame_len:.2f}")

        expected = step_label(expected, rate)

    offsets = [frame.start - (zero + k * frame_len) for k, frame in enumerate(frames)][1:]

    if offsets and max(offsets) - min(offsets) > SPREAD:
        worst = max(range(len(offsets)), key=lambda k: abs(offsets[k] - offsets[0]))
        return False, (f"the frames' starts wander by {max(offsets) - min(offsets):.2f} samples; frame "
                       f"{worst + 1} is {offsets[worst] - offsets[0]:+.2f} from where frame 1's puts it")

    return True, ""


# =============================================================================
# MTC, read from the MIDI specification
# =============================================================================

def read_midi_render(path: Path) -> "list[tuple[int, int, tuple]]":
    """(sent_ns, due_ns, bytes) per line of a `--midi-render` file; a queue's
    message, sent as soon as it could be, has a due of nought."""
    lines = []
    try:
        text = path.read_text(encoding="ascii", errors="replace")
    except OSError:
        return lines

    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            lines.append((int(parts[0]), int(parts[1]), tuple(int(byte, 16) for byte in parts[2:])))
        except ValueError:
            continue

    return lines


def is_full_frame(message: tuple) -> bool:
    return (len(message) == 10 and message[:5] == (0xF0, 0x7F, 0x7F, 0x01, 0x01)
            and message[9] == 0xF7)


def is_quarter_frame(message: tuple) -> bool:
    return len(message) == 2 and message[0] == 0xF1


def full_frame_of(message: tuple) -> "tuple[tuple, int]":
    """(label, rate code) of `F0 7F 7F 01 01 hh mm ss ff F7`: the rate's code
    in bits 5-6 of the hours byte."""
    return (message[5] & 0x1F, message[6], message[7], message[8]), (message[5] >> 5) & 0x03


def mtc_cycles(quarters: "list[tuple[int, int, tuple]]") -> "tuple[list, int]":
    """Cycles of eight quarter frames put together as a receiver does: pieces
    0 to 7 in order, nibbles low then high of the frames, seconds, minutes and
    hours, the last piece carrying the rate's code above the hours' top bit.
    ([(due of piece 0, label, rate code)], how many cycles broke off)."""
    cycles = []
    broken = 0
    nibbles: "list[int]" = []
    due0 = 0

    for _sent, due, message in quarters:
        piece, nibble = message[1] >> 4, message[1] & 0x0F

        if piece == 0:
            if nibbles:
                broken += 1
            nibbles = [nibble]
            due0 = due
            continue

        if piece != len(nibbles):
            if nibbles:
                broken += 1
            nibbles = []
            continue

        nibbles.append(nibble)

        if len(nibbles) == 8:
            label = (nibbles[6] | ((nibbles[7] & 0x01) << 4), nibbles[4] | ((nibbles[5] & 0x03) << 4),
                     nibbles[2] | ((nibbles[3] & 0x03) << 4), nibbles[0] | ((nibbles[1] & 0x01) << 4))
            cycles.append((due0, label, (nibbles[7] >> 1) & 0x03))
            nibbles = []

    if nibbles:
        broken += 1

    return cycles, broken


def lateness_ms(lines: "list[tuple[int, int, tuple]]") -> "list[float]":
    return [(sent - due) / 1e6 for sent, due, _ in lines if due != 0]


def percentile(values: "list[float]", fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


# =============================================================================
# The show, copied and driven
# =============================================================================

def write_tone(path: Path, sample_rate: int, seconds: float) -> None:
    """first_sound's steady tone at the session's rate: every sample the same
    constant, so the first that is not nought is the moment the cue started and
    the first that drops is the moment Esc began its fade."""
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = int(first_sound.AMPLITUDE * 32767)
    frames = int(sample_rate * seconds)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(sample_rate)
        out.writeframes(struct.pack("<hh", sample, sample) * frames)


def readout(server: Server, run: str):
    return parse_label(value_of(server, f"/godot/run/{run}/timecode") or "")


def wait_for_readout(server: Server, run: str, at_least, timeout: float):
    """Waits until a timecode run's readout reaches a label - asked of the run,
    not of a stopwatch, so a slow runner waits longer rather than fails."""
    reached = common.wait_until(lambda: (readout(server, run) or (-1,)) >= at_least, timeout=timeout)
    return bool(reached), readout(server, run)


def text_of(server: Server, address: str) -> str:
    """A node's value, or "" for one that is not published - a run's warning
    row is there only when it says something."""
    value = value_of(server, address)
    return "" if value is None else str(value)


def scenario(report: Report, room: Path, rate: Rate, sample_rate: int, held: bool,
             song_from: str, word: str) -> None:
    bundle = common.copy_bundle(FIXTURE, room / "show")
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('frameRate="25"', f'frameRate="{rate.word}"')
    text = text.replace('from="01:00:00:00"', f'from="{song_from}"')
    show.write_text(text, encoding="utf-8", newline="\n")

    write_tone(bundle / "media" / "tone.wav", sample_rate, 30.0)
    render = room / "out.wav"
    mtc_file = room / "mtc.txt"

    first_label = parse_label(song_from)
    second_label = (2, 0, 0, 0)
    mtc_label = (10, 0, 0, 0)
    mtc_frames = rate.frames_in(MTC_LENGTH)
    patience = 120.0 if ON_CI else 30.0

    runs = {}

    with Server(bundle, locale=LOCALE, sample_rate=sample_rate, buffer_size=BLOCK, hosted=True,
                render=render, hold=held, midi_render={"TC": str(mtc_file)}) as server:

        report.equal(first_sound.wait_for(server, "/godot/audio/status", "running", timeout=patience),
                     "running", f"{word}: the audio side comes up running")
        if held:
            report.equal(value_of(server, "/godot/pair/sending"), False,
                         f"{word}: /godot/pair/sending says this machine holds")

        # --- GO: the song and its code -------------------------------------
        go(server)
        runs["code"] = run_for_cue(server, SONG_CODE, timeout=patience)
        runs["tone"] = run_for_cue(server, TONE, timeout=patience)

        if not report.check(bool(runs["code"]) and bool(runs["tone"]),
                            f"{word}: GO makes runs of the song and its code", str(runs)):
            return

        report.equal(wait_for_run_state(server, runs["code"], "playing", patience), "playing",
                     f"{word}: the song's code plays")
        report.equal(wait_for_run_state(server, runs["tone"], "playing", patience), "playing",
                     f"{word}: and the song")

        two_seconds = step_label(first_label, rate, rate.frames_in(2.0))
        reached, at = wait_for_readout(server, runs["code"], two_seconds, patience)
        report.check(reached, f"{word}: the code's readout counts on from {label_text(first_label, rate.drop)}",
                     f"it read {at} waiting for {label_text(two_seconds, rate.drop)}")

        # --- GO: the code taken over ----------------------------------------
        go(server)
        runs["second"] = run_for_cue(server, TAKEOVER, timeout=patience)

        if report.check(bool(runs["second"]), f"{word}: GO makes a run of the second LTC cue"):
            report.equal(wait_for_run_state(server, runs["second"], "playing", patience), "playing",
                         f"{word}: the second code plays")
            report.equal(wait_for_run_state(server, runs["code"], "done", patience), "done",
                         f"{word}: and the song's code, taken over, reads done")
            report.equal(text_of(server, f"/godot/run/{runs['code']}/error"), "",
                         f"{word}: unfailed")
            one_second = step_label(second_label, rate, rate.frames_in(1.0))
            reached, at = wait_for_readout(server, runs["second"], one_second, patience)
            report.check(reached, f"{word}: the second code's readout counts on from 02:00:00:00",
                         f"it read {at}")

        # --- Esc ----------------------------------------------------------------
        common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/run/stopAll"))

        if runs.get("second"):
            report.equal(wait_for_run_state(server, runs["second"], "done", patience), "done",
                         f"{word}: Esc ends the second code's run")
        report.equal(wait_for_run_state(server, runs["tone"], "done", patience), "done",
                     f"{word}: and the song's, once its fade is over")

        # --- GO: two seconds of MTC ----------------------------------------------
        go(server)
        runs["mtc"] = run_for_cue(server, MTC_CUE, timeout=patience)

        if report.check(bool(runs["mtc"]), f"{word}: GO makes a run of the MTC cue"):
            report.equal(wait_for_run_state(server, runs["mtc"], "done", patience), "done",
                         f"{word}: the MTC run ends done, by its length")
            report.equal(text_of(server, f"/godot/run/{runs['mtc']}/error"), "",
                         f"{word}: unfailed")
            report.equal(text_of(server, f"/godot/run/{runs['mtc']}/warning"), "",
                         f"{word}: and unwarned" + (", held and so counted as sent (AGG)" if held else ""))
            last = step_label(mtc_label, rate, mtc_frames - 1)
            report.equal(readout(server, runs["mtc"]), last,
                         f"{word}: its readout stops on its last frame, {label_text(last, rate.drop)}")

            memo = run_for_cue(server, FOOTER_MEMO, timeout=10.0)
            report.check(bool(memo), f"{word}: its group's footer runs its memo cue")
            if memo:
                report.equal(wait_for_run_state(server, memo, "done", patience), "done",
                             f"{word}: which is done")

        report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                     f"{word}: nothing of Go.dot's allocated on the audio thread")

        counted_late = value_of(server, "/godot/sync/mtcLate")
        ltc_late = value_of(server, "/godot/sync/ltcLate")
        print(f"       sync/ltcLate {ltc_late}, "
              f"sync/mtcLate {counted_late}, "
              f"sync/mtcWorst {value_of(server, '/godot/sync/mtcWorst')} ms, "
              f"engine/latenessMax {value_of(server, '/godot/engine/latenessMax')} samples")

        wait_for_render_tail(render)

    # --- the line, decoded --------------------------------------------------------
    if not report.check(render.is_file(), f"{word}: the render was written"):
        return

    got_rate, channels, _offset, _frames = wav_layout(render)
    report.equal((got_rate, channels), (sample_rate, 3), f"{word}: the render is the show's three channels at its rate")
    if channels <= LTC_CHANNEL:
        return

    judge_ltc(report, word, rate, sample_rate, channel_of(render, TONE_CHANNEL),
              channel_of(render, LTC_CHANNEL), first_label, int(ltc_late or 0))
    judge_mtc(report, word, rate, read_midi_render(mtc_file), held, int(counted_late or 0))


def judge_ltc(report: Report, word: str, rate: Rate, sample_rate: int, tone, line, first_label,
              counted_late: int = 0) -> None:
    """WHAT THE LINE SAYS, read off the render: the song's code from its
    first sample, the second code taking it over, and Esc. `tone` and `line`
    are the render's tone and LTC channels, whole.

    A START THAT REACHED THE AUDIO SIDE LATE - a tick that fell behind its
    audio by more than the launch lead, which a loaded runner's can - begins
    at its own clock's next frame boundary, and the engine owns up to it in
    `sync/ltcLate` (`counted_late`). Then, and only then, the code may begin
    some frames into its count, and the song's sample and the code's part by
    up to a frame: the one check that cannot be made is voided, saying why,
    and every other is made from the frame the code began on."""
    second_label = (2, 0, 0, 0)
    level = 10.0 ** (LTC_LEVEL_DB / 20.0)
    frame_len = rate.frame_samples(sample_rate)
    lead = rate.frames_in(2.0)

    tone_at = first_nonzero(tone)
    zero = first_nonzero(line)
    same = f"{word}: the tone's first sample and the LTC's first sample are the same sample (AHN)"
    if zero > 0 and tone_at != zero and counted_late > 0:
        report.void(same, f"the engine counted {counted_late} LTC command(s) late (sync/ltcLate), and a late"
                          f" start begins at its clock's next frame boundary; the tone began at {tone_at},"
                          f" the code at {zero}")
    else:
        report.check(zero > 0 and tone_at == zero, same, f"the tone began at {tone_at}, the code at {zero}")
    if zero <= 0:
        return

    decoder = decode_ltc(line, sample_rate, rate, level)
    frames = decoder.frames
    second_at = next((k for k, f in enumerate(frames) if f.label[0] == second_label[0]), -1)
    skipped = steps_between(first_label, frames[0].label, rate, 1, lead if counted_late else 0) if frames else None
    # THE TAKEOVER'S FIRST FRAME MAY BE THE ONE A READER RELOCKS ON. The new
    # code's first edge lands wherever the old code's bit happens to be, so the
    # interval across the splice is a fraction of a bit, and a reader pairing
    # half-bits can lose its place until the next frame - as a desk does at any
    # locate. Seen on CI (run 38299523050, Windows and macOS alike, 29.97
    # drop-frame at 44.1 kHz): the code jumped with the line never quiet, and
    # the frames read from 02:00:00;01. One frame is let go here; the unit
    # tests hold the generator's own takeover frame by frame.
    skipped_second = (steps_between(second_label, frames[second_at].label, rate, 1,
                                    max(1, lead if counted_late else 0))
                      if second_at >= 0 else None)
    if skipped or skipped_second:
        print(f"       LTC: a start the engine counted late (sync/ltcLate {counted_late}) began {skipped or 0}"
              f" and {skipped_second or 0} frame(s) into its count")
    first_run = frames[:second_at] if second_at >= 0 else frames
    second_run = frames[second_at:] if second_at >= 0 else []
    takeover = frames[second_at].start if second_at >= 0 else float("inf")

    plateau = [abs(line[n]) for n in range(zero, zero + int(0.5 * sample_rate), 7)
               if abs(line[n]) > 0.9 * level]
    middle = statistics.median(plateau) if plateau else 0.0
    report.check(abs(middle - level) <= 0.01 * level, f"{word}: the code plays at {LTC_LEVEL_DB:g} dBFS (AHH)",
                 f"its level reads {middle:.5f} against {level:.5f}")

    #  The first frame's edge rises out of silence and crosses the threshold
    #  within a sample of the line's first sample; every later edge swings
    #  across from the other level and crosses a little over a sample after
    #  its exact moment, the filter's own delay - the same for each.
    ok, why = check_run_of_frames(first_run, step_label(first_label, rate, skipped or 0), rate, frame_len,
                                  zero, -0.5, 2.5)
    report.check(skipped is not None and bool(first_run) and abs(first_run[0].start - zero) <= 1.0 and ok,
                 f"{word}: the song's code decodes as consecutive frames from {label_text(first_label, rate.drop)},"
                 f" each where its count from the first sample puts it",
                 why or (f"first frame at {first_run[0].start:.2f}, the line rose at {zero}" if first_run
                         else "no frame read"))

    report.check(len(first_run) >= lead - (skipped or 0),
                 f"{word}: as many as the two seconds it played before the takeover",
                 f"{len(first_run)} frames read")

    report.check(bool(frames) and all(f.parity_even and f.bcd_ok for f in frames),
                 f"{word}: every frame ends in the sync word, its ones even and its digits decimal",
                 "; ".join(f"{label_text(f.label)} parity {'even' if f.parity_even else 'odd'}"
                           for f in frames if not (f.parity_even and f.bcd_ok))[:400])

    report.check(bool(frames) and all(f.drop == rate.drop for f in frames),
                 f"{word}: the drop-frame flag {'set' if rate.drop else 'clear'} in every frame",
                 f"{sum(1 for f in frames if f.drop)} of {len(frames)} frames carry it")

    if rate.drop:
        labels = [f.label for f in frames]
        turned = any(a[1:] == (0, 59, rate.nominal - 1) and b[1:] == (1, 0, 2)
                     for a, b in zip(labels, labels[1:]))
        missing = [label_text(l, True) for l in labels if l[2] == 0 and l[3] < 2 and l[1] % 10 != 0]
        report.check(turned and not missing,
                     f"{word}: the dropped labels are skipped - 01:00:59;29 is followed by 01:01:00;02",
                     f"turned {turned}, labels that should not exist: {missing}; heard "
                     + ", ".join(label_text(l, True) for l in labels[25:35]))

    # --- the takeover -----------------------------------------------------------
    if not report.check(second_at >= 0 and skipped_second is not None,
                        f"{word}: the code jumps to 02:00:00:00 at the second cue's launch",
                        "heard " + ", ".join(label_text(f.label, rate.drop)
                                             for f in frames[max(0, second_at - 3):second_at + 3])):
        return

    quiet_at = decoder.quiets[-1] if decoder.quiets else float("inf")
    ok, why = check_run_of_frames(second_run, step_label(second_label, rate, skipped_second), rate, frame_len,
                                  takeover, -0.75, 0.75)
    report.check(ok and len(second_run) >= rate.frames_in(1.0),
                 f"{word}: and runs on from there frame by frame, each where its count puts it",
                 why or f"{len(second_run)} frames read")

    # WHERE THE SPLICE WAS: the first frame read of the second code is
    # `skipped_second` frames into its count when a reader relocked on the
    # takeover's first frame (above), so the splice is that many frames
    # before it - the checks of the cut and of the bit errors are about the
    # splice, not about the first frame a reader happened to read.
    splice = takeover - (skipped_second or 0) * frame_len

    cut = first_run[-1].end if first_run else zero
    report.check(cut <= splice + 2.0 and splice - cut < frame_len,
                 f"{word}: the song's code drawn up to the second's first sample, a frame cut short at most (AHL)",
                 f"the last whole frame ended at {cut:.2f}, the second code began at {splice:.2f}")

    quiet_before = [q for q in decoder.quiets if q < quiet_at]
    rose = [s for s in decoder.starts if s > zero + 2]
    report.check(not quiet_before and not rose,
                 f"{word}: the line never quiet between the two codes (AHC)",
                 f"quiet at {quiet_before}, rising again at {rose}")

    bit = decoder.bit
    errors = [e for e in decoder.errors if not (splice - 1.5 * bit <= e[0] <= takeover + 1.5 * bit)]
    report.check(not errors, f"{word}: not one bit error in either code, the takeover's own edge aside",
                 "; ".join(f"at {at:.1f}: {what}" for at, what in errors[:6]))

    # --- Esc --------------------------------------------------------------------
    whole = round((quiet_at - takeover) / frame_len)
    boundary = takeover + whole * frame_len
    report.check(abs(quiet_at - boundary) <= 2.0 and abs(second_run[-1].end - quiet_at) <= 2.0,
                 f"{word}: Esc ends the code on a frame boundary, the last frame drawn whole (AHK)",
                 f"the line fell quiet at {quiet_at:.2f}, the boundary is {boundary:.2f}, "
                 f"the last frame ended at {second_run[-1].end:.2f}")

    #  WHERE ESC LANDED, read off the song: the tick that applied it began the
    #  song's panic fade and posted the code's stop, both taken by the next
    #  block, so the code falls quiet at the first frame boundary after the
    #  fade began - give or take a block or two of the audio side's own.
    steady = tone[zero + sample_rate // 10]
    faded = next((n for n in range(int(takeover), len(tone)) if abs(tone[n]) < abs(steady) * 0.999), -1)
    slack = 2 * BLOCK
    print(f"       Esc: the code fell quiet {quiet_at - faded:.1f} samples "
          f"({(quiet_at - faded) / frame_len:+.3f} frames) after the song began its fade")
    report.check(faded > 0 and faded - slack <= quiet_at <= faded + frame_len + slack,
                 f"{word}: within one frame of Esc, read where the song began its fade",
                 f"the fade began at {faded}, the code fell quiet at {quiet_at:.2f}, "
                 f"{(quiet_at - faded) / frame_len:+.3f} frames after it")

    after = int(quiet_at) + TAIL
    noise = next((n for n in range(after, len(line)) if line[n] != 0.0), -1)
    report.check(after < len(line) and noise < 0,
                 f"{word}: and the line is digital silence once the edge filter's tail has gone",
                 f"a sample of {line[noise]:.3g} at {noise}, {noise - quiet_at:.0f} after the code fell quiet"
                 if noise >= 0 else f"the render ends at {len(line)}")


def judge_mtc(report: Report, word: str, rate: Rate, lines, held: bool, counted_late: int) -> None:
    """WHAT THE PORT SAID, read off the `--midi-render` file: a full frame,
    then cycles of quarter frames, each line with when it left and when it
    was due - or, held, nothing at all.

    EACH QUARTER FRAME IS PLACED BY ITS DUE TIME, counted in quarters from the
    full frame's, and its piece must be that count modulo eight: the pieces in
    their order, and every cycle beginning on an even frame from the start.

    ONE MAY BE MISSING, AND ONLY SO: a quarter frame the lane finds more than a
    quarter late is dropped, and the code picks up at the next cycle that begins
    after then (AHQ) - a loaded machine's doing, which the engine owns up to in
    `sync/mtcLate` (`counted_late`). So a gap must land on a cycle's first
    piece, there may be no more gaps than late messages the engine counted, and
    every cycle no gap broke is whole and names its own label."""
    mtc_label = (10, 0, 0, 0)
    mtc_frames = rate.frames_in(MTC_LENGTH)

    if held:
        report.check(not lines, f"{word}: the MTC file holds no timecode - held, nothing left (AGG)",
                     f"{len(lines)} lines, the first {lines[:2]}")
        return

    if not report.check(bool(lines) and is_full_frame(lines[0][2]),
                        f"{word}: MTC opens with a full frame (AHO)",
                        f"{len(lines)} lines, the first {lines[:2]}"):
        return

    #  A START THE LANE FOUND LATE - its tick behind its audio - is dropped
    #  and opens at a later cycle with a full frame, counted in sync/mtcLate:
    #  then the full frame may name that cycle's label, two frames on a cycle.
    label, code = full_frame_of(lines[0][2])
    opened = steps_between(mtc_label, label, rate, 2, mtc_frames // 2 if counted_late else 0)
    report.check(opened is not None and code == rate.mtc_code,
                 f"{word}: naming 10:00:00:00 at the rate's code",
                 f"it named {label_text(label, rate.drop)} at code {code}, against {rate.mtc_code}"
                 + (f"; {counted_late} counted late" if counted_late else ""))
    opened = opened or 0
    if opened:
        print(f"       MTC: a start the lane found late opened {opened} cycle(s) on (sync/mtcLate {counted_late})")

    quarters = lines[1:]
    report.check(all(is_quarter_frame(message) for _, _, message in quarters),
                 f"{word}: then quarter frames only",
                 str([message for _, _, message in quarters if not is_quarter_frame(message)][:4]))
    quarters = [entry for entry in quarters if is_quarter_frame(entry[2])]

    if not report.check(bool(quarters), f"{word}: and there are quarter frames"):
        return

    #  Off CI each is due a quarter after the last, give or take what the
    #  lane's line between samples and moments moves by as each tick corrects
    #  it (a fifth of a millisecond here); on CI a loaded runner moves it more.
    quarter_ms = 1000.0 / (4 * rate.num / rate.den)
    leeway = 0.35 * quarter_ms if ON_CI else 0.5
    full_due = lines[0][1]

    def piece(entry) -> int:
        return entry[2][1] >> 4

    first = (quarters[0][1] - full_due) / 1e6 / quarter_ms
    index = 8 * opened + round(first)
    indices = [index]
    on_time = abs(first - round(first)) * quarter_ms <= leeway and round(first) >= 0
    off = [] if on_time else [round(first * quarter_ms, 3)]
    drops = 1 if round(first) > 0 else 0
    landed_mid_cycle = [] if index % 8 == 0 else [index]

    for before, after in zip(quarters, quarters[1:]):
        gap = (after[1] - before[1]) / 1e6
        whole = round(gap / quarter_ms)

        if whole < 1 or abs(gap - whole * quarter_ms) > leeway:
            off.append(round(gap, 3))
            whole = max(1, whole)

        index += whole
        indices.append(index)

        if whole > 1:
            drops += 1
            if index % 8 != 0:
                landed_mid_cycle.append(index)

    report.check(not off,
                 f"{word}: due {quarter_ms:.3f} ms apart, the first with the full frame - the frame's own sample",
                 f"{len(off)} of {len(quarters)} off by more than {leeway:.2f} ms: {off[:8]}")

    wrong_piece = [(i, piece(entry)) for i, entry in zip(indices, quarters) if piece(entry) != i % 8]
    report.check(not wrong_piece, f"{word}: each quarter frame the piece its moment makes it, cycles on even frames",
                 f"(quarter, piece): {wrong_piece[:8]}")

    if drops:
        print(f"       MTC: {drops} gap(s) where quarter frames were dropped late and the code picked up"
              f" at a later cycle; sync/mtcLate {counted_late}")
    report.check(drops <= counted_late and not landed_mid_cycle,
                 f"{word}: none missing but those the engine dropped late, picked up at a cycle's start (AHQ)",
                 f"{drops} gap(s), {counted_late} counted late; picked up mid-cycle at {landed_mid_cycle}")

    cycles, broken = mtc_cycles(quarters)
    cycle_ms = 8 * quarter_ms
    wrong = []
    for due0, label, code in cycles:
        cycle = opened + round((due0 - full_due) / 1e6 / cycle_ms)
        wanted = step_label(mtc_label, rate, 2 * cycle)
        if label != wanted or code != rate.mtc_code:
            wrong.append(f"cycle {cycle} read {label_text(label, rate.drop)} code {code}, "
                         f"wanted {label_text(wanted, rate.drop)} code {rate.mtc_code}")
    whole_cycles = mtc_frames // 2
    report.check(not wrong and broken <= drops and (drops > 0 or len(cycles) == whole_cycles - opened),
                 f"{word}: cycles of eight read labels two frames apart from 10:00:00:00",
                 f"{len(cycles)} whole cycles of {whole_cycles}, {broken} broken by {drops} gap(s); "
                 + "; ".join(wrong[:4]))

    length = mtc_frames * rate.den / rate.num
    last = indices[-1]
    span = (quarters[-1][1] - full_due) / 1e9
    report.check(last < 4 * mtc_frames and span < length
                 and (drops > 0 or len(quarters) == 4 * mtc_frames - 8 * opened),
                 f"{word}: {4 * mtc_frames} quarter frames, nothing due after the {length:.3f} s length",
                 f"{len(quarters)} quarter frames, the last quarter {last}, due {span:.4f} s after the full frame")

    late = lateness_ms(lines)
    worst = max(late)
    middle = statistics.median(late)
    print(f"       MTC left after its moment: median {middle:.3f} ms, p99 {percentile(late, 0.99):.3f} ms, "
          f"worst {worst:.3f} ms, earliest {min(late):.3f} ms, over {len(late)} messages")
    bound = 250.0 if ON_CI else 20.0
    report.check(min(late) >= -0.5 and worst <= bound and (ON_CI or middle <= 2.0),
                 f"{word}: each message left near its moment, never before it (AHP)",
                 f"median {middle:.3f} ms, worst {worst:.3f} ms, earliest {min(late):.3f} ms; "
                 f"bound {bound:g} ms{' (CI)' if ON_CI else ', median 2 ms'}")


# =============================================================================

LOCALE = next((arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith("--wfg-locale=")), None)


def run() -> int:
    report = Report(f"timecode generated: LTC and MTC ({LOCALE or 'C'})")

    passes = (
        (RATES["25"], 48000, False, "01:00:00:00", "25 fps at 48 kHz"),
        (RATES["25"], 48000, True, "01:00:00:00", "held"),
        (RATES["29.97df"], 44100, False, "01:00:59:00", "29.97 drop-frame at 44.1 kHz"),
    )

    for rate, sample_rate, held, song_from, word in passes:
        with tempfile.TemporaryDirectory(prefix="wfg-timecode-") as scratch:
            scenario(report, Path(scratch), rate, sample_rate, held, song_from, word)

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"timecode: {problem}", file=sys.stderr)
        return 2

    if not FIXTURE.is_dir():
        print(f"timecode: no fixture at {FIXTURE}", file=sys.stderr)
        return 2

    try:
        return run()
    except HarnessError as problem:
        print(f"timecode: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
