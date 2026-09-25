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

"""Spectral colour, checked on the cache alone, before anything is drawn.

WHAT THIS IS FOR. PRD §3.30 asks for it by name and namespace draft §14.12 will
not let the phase forget it: a 1 kHz sine must come out saturated at 1 kHz's
hue, white noise grey, and a sweep must walk the ramp. A colour ramp is exactly
the kind of thing that looks right and is wrong - a log axis computed from bin
INDEX instead of bin frequency, a flatness on power where it should be on
magnitude, a window that leaks and drags every centroid up - and each of those
shifts the picture rather than breaking it. Over unfamiliar material a shifted
ramp and a correct one look the same, because nobody knows what colour that
clip should be. Three signals whose answers are known before the code runs are
the only honest check, and this driver generates them itself.

THE SECOND READER IS THE POINT. The unit suite asserts the same three facts
through the engine's own decoder, which proves the writer agrees with itself
and nothing else. This one decodes the `.tpy` with its own `struct` reader,
written from the layout in `Timbre.h`, and restates the ramp, the lightness and
the rule for a coarser level from the specification - common.py's standing
rule, a separate codec so a driver cannot share a mistake with the code under
test. If the author moves a stop, this file moves with it, on purpose.

WHAT THE RAMP DOES NOT PROMISE, and so what this does not assert: that a sweep's
hue climbs. The stops (the author's since 2026-09-25, from plan decision 8's)
run purple (280 degrees) to deep blue (240) and then the other way round the
wheel through violet, red, orange, yellow and green, so from 40 Hz to 250 Hz
the hue turns back. What is monotonic is the lightness,
by construction. So the sweep is asserted twice: its lightness never falls, and
every frame's hue is the ramp's hue AT THE SWEEP'S FREQUENCY THEN - which is a
stronger check than monotonic would have been, and one a machine can make.

AND THE CACHE BEHAVES LIKE ONE: the same bytes under two names are analysed
once, a second run does no work, `--force` writes the same bytes again, a file
the show names and the bundle lacks fails the run, and a `.timbre` that cannot
be a folder costs the cache and not the colours.

AND THE THREAD A SESSION RUNS IT ON: one `wfg serve` of a fresh copy, which
must colour every file the show names without being asked, and colour a file a
`node.set` introduces mid-session without being reopened - the serve wiring,
which no unit case reaches.

AND WHAT A SESSION SAYS WITH IT (PR 5.8, namespace draft §14.5): last, a second
`wfg serve`, hosted, of a copy routed to a bus and parked on the sweep. The tree
carries what is true now and a route what is true always, and both are read
from outside. The sweep's cue and the sine's must each name its file's SHA-256 -
as hashlib computes it, not as the engine reports it - once the analyser has
reached it, and the sine's copy under another name the same one. GO must play
the sweep, and at every playhead this driver catches, its run must publish the
very frame this driver's reader finds in the cache under that playhead - and
the ramp's colour at the sweep's frequency then.

THE SWEEP AND NOT THE SINE, which is what this check was first written with and
could not fail. Every steady frame of a 1 kHz sine is the same four bytes, so a
playhead rounded where §14.5 floors it, placed one frame late, or read off the
level above would each have published a colour the check took for the right
one. A sweep's neighbouring frames differ, and the driver goes on reading until
it holds, for each of those three, a reading of a frame unlike both its
neighbours placed where that mistake would have picked another: a reading that
could only have come from under its own playhead.

Then the route beside the tree must hand back the cache's own bytes, level by
level, and its header as JSON with the format version the `.tpy` carries, every
answer marked `no-cache` - a browser may keep a copy, and must ask again before
it reuses one. NOT IMMUTABLE, though the URL is a content hash, because the
content it names is the AUDIO and not the analysis: a moved ramp stop bumps the
format version and rebuilds the pyramid under the same name (§14.12), and the
author will move stops while looking at the bar. A year of `immutable` would
have gone on showing the old colours, as though the move had done nothing.
And it must refuse what it does not hold with `no-store`, since a hash nothing
has analysed yet is a 200 on the day somebody imports it.

No audio device anywhere: `wfg analyse` is a verb and runs where `wfg validate`
runs, the first server is the dummy clock's, and the second is `--hosted` - a
real playback graph with no interface under it. It has to be: a launch is only
ever placed for a media run holding a track, so on the dummy clock a run's
playhead never moves and there is no frame under it to read. Exit codes: 0
everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import hashlib
import json
import math
import random
import shutil
import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report


# =============================================================================
# The specification, restated - Timbre.h, plan decision 8, §14.12
# =============================================================================

RATE = 48000
WINDOW = 2048
HOP = 1024
COARSEST = 64
FORMAT_VERSION = 3

# (hertz, hue in degrees UNWRAPPED): 360 is red and 480 green, so deep blue to
# red goes through magenta and not back through cyan.
RAMP = [(40.0, 280.0), (250.0, 240.0), (800.0, 360.0),
        (2500.0, 390.0), (6000.0, 420.0), (12000.0, 480.0)]

LOWEST, HIGHEST = 40.0, 16000.0
DARKEST, BRIGHTEST = 0.15, 0.85


def ramp_hue(hertz: float) -> float:
    if hertz <= RAMP[0][0]:
        return RAMP[0][1] % 360.0
    for (low_hz, low_hue), (high_hz, high_hue) in zip(RAMP, RAMP[1:]):
        if hertz < high_hz:
            along = math.log(hertz / low_hz) / math.log(high_hz / low_hz)
            return (low_hue + along * (high_hue - low_hue)) % 360.0
    return RAMP[-1][1] % 360.0


def ramp_lightness(hertz: float) -> float:
    along = math.log(hertz / LOWEST) / math.log(HIGHEST / LOWEST)
    return DARKEST + (BRIGHTEST - DARKEST) * min(max(along, 0.0), 1.0)


def hue_apart(one: float, other: float) -> float:
    """Degrees between two hues, the short way round."""
    apart = (one - other) % 360.0
    return min(apart, 360.0 - apart)


def paired(first: tuple, second: tuple) -> tuple:
    """One frame of a coarser level from two of the finer, as Timbre.h words
    it: silence lends nothing; saturation and lightness are the mean, a half
    rounded up; the hue moves from the first towards the second the shorter way
    round by the second's share of the saturations; peak is the larger."""
    h1, s1, l1, p1 = first
    h2, s2, l2, p2 = second
    peak = max(p1, p2)

    if l1 == 0 and l2 == 0:
        return (0, 0, 0, peak)
    if l1 == 0:
        return (h2, s2, l2, peak)
    if l2 == 0:
        return (h1, s1, l1, peak)

    arc = (h2 - h1) % 256
    if arc >= 128:
        arc -= 256

    w1, w2 = (s1, s2) if s1 + s2 > 0 else (1, 1)
    step = (2 * arc * w2 + (w1 + w2)) // (2 * (w1 + w2))

    return ((h1 + step) % 256, (s1 + s2 + 1) // 2, (l1 + l2 + 1) // 2, peak)


def fnv1a(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


class Pyramid:
    """A `.tpy`, read from the layout in Timbre.h and nothing else."""

    def __init__(self, data: bytes):
        if len(data) < 32:
            raise ValueError(f"{len(data)} bytes is shorter than the header")
        if data[:4] != b"WFGT":
            raise ValueError(f"magic {data[:4]!r}")

        (version, levels, rate, window, hop,
         checksum, samples) = struct.unpack_from("<HHIIIIQ", data, 4)

        if version != FORMAT_VERSION:
            raise ValueError(f"version {version}")
        if (window, hop) != (WINDOW, HOP):
            raise ValueError(f"window {window}, hop {hop}")
        if fnv1a(data[32:]) != checksum:
            raise ValueError("checksum does not match the bytes")

        # Kept, though the check above has just made it FORMAT_VERSION: the
        # route's ?INFO is compared with what the .tpy's own header says, and
        # not with what this file expects it to say.
        self.version = version
        self.rate = rate
        self.samples = samples
        self.levels: "list[list[tuple]]" = []

        expected = (samples + HOP - 1) // HOP
        offset = 32 + 8 * levels

        for level in range(levels):
            frames, at = struct.unpack_from("<II", data, 32 + 8 * level)
            if frames != expected:
                raise ValueError(f"level {level} has {frames} frames, expected {expected}")
            if at != offset:
                raise ValueError(f"level {level} starts at {at}, expected {offset}")
            self.levels.append([tuple(data[at + 4 * i: at + 4 * i + 4])
                                for i in range(frames)])
            offset += 4 * frames
            expected = (expected + 1) // 2

        if offset != len(data):
            raise ValueError(f"{len(data) - offset} bytes past the last level")

        # The level count is the one the halving stops at, and no other.
        if len(self.levels[-1]) > COARSEST:
            raise ValueError("the coarsest level is wider than a Gogo bar needs")
        if len(self.levels) > 1 and len(self.levels[-2]) <= COARSEST:
            raise ValueError("a level beyond the one the halving stops at")

    @property
    def finest(self) -> "list[tuple]":
        return self.levels[0]


def hue_of(frame: tuple) -> float:
    return frame[0] * 360.0 / 256.0


def unit(byte: int) -> float:
    return byte / 255.0


# =============================================================================
# Three signals whose answers are known in advance
# =============================================================================

def write_wav(path: Path, samples: "list[float]") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for sample in samples:
        frames += struct.pack("<h", int(round(max(-1.0, min(1.0, sample)) * 32767)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(frames))


SINE_HZ, SINE_SECONDS = 1000.0, 3.0
NOISE_SECONDS = 3.0
SWEEP_FROM, SWEEP_TO, SWEEP_SECONDS = 100.0, 8000.0, 8.0


def sine() -> "list[float]":
    return [0.5 * math.sin(2 * math.pi * SINE_HZ * n / RATE)
            for n in range(int(SINE_SECONDS * RATE))]


def noise() -> "list[float]":
    draw = random.Random(20260914)
    return [draw.uniform(-0.5, 0.5) for _ in range(int(NOISE_SECONDS * RATE))]


def sweep_hertz(seconds: float) -> float:
    return SWEEP_FROM * (SWEEP_TO / SWEEP_FROM) ** (seconds / SWEEP_SECONDS)


def sweep() -> "list[float]":
    """Exponential, so equal times climb equal fractions of an octave - the
    axis the centroid is taken on."""
    growth = math.log(SWEEP_TO / SWEEP_FROM)
    scale = 2 * math.pi * SWEEP_FROM * SWEEP_SECONDS / growth
    return [0.5 * math.sin(scale * (math.exp(growth * (n / RATE) / SWEEP_SECONDS) - 1.0))
            for n in range(int(SWEEP_SECONDS * RATE))]


def frame_seconds(index: int) -> float:
    """The centre of the stretch a finest-level frame describes."""
    return (index * HOP + HOP / 2) / RATE


# =============================================================================
# The bundle
# =============================================================================

MANIFEST = '<Bundle formatVersion="1"/>\n'

FILES = ["sine.wav", "noise.wav", "sweep.wav", "renamed/sine.wav"]
IDS = ["B3N8R5TW", "E4GP6QSC", "F7HR8TVD", "D9FH2JKA", "G1JS4VWE"]
LIST = "7K2QM9X4"

# The cues that play `sine.wav`, its copy under another name, and the sweep.
SINE_CUE = IDS[FILES.index("sine.wav")]
RENAMED_CUE = IDS[FILES.index("renamed/sine.wav")]
SWEEP_CUE = IDS[FILES.index("sweep.wav")]

# What a routed copy adds: one bus two wide, and one route per media cue onto
# it. Every file is mono, so a route is one row of two coefficients.
BUS = "J3MT5XYA"
ROUTES = ["R2TB7KXM", "R4VC8NYP", "R6WD9PZQ", "R8XE2QAS", "R9YF3RBT"]


def show_xml(files: "list[str]", routed: bool = False) -> str:
    """The show. `routed` gives it what a GO needs before a media cue can
    sound - an Audio section with tracks and a bus, and a route from every cue
    to that bus - which neither the verb nor the dummy clock ever asked for."""
    cues = ""

    for n, name in enumerate(files):
        opening = f'      <Media id="{IDS[n]}" file="{name}" name="Take {n + 1}" number="{n + 1}"'
        if routed:
            cues += (opening + ">\n"
                     + f'        <Route id="{ROUTES[n]}" bus="{BUS}" gains="1 1"/>\n'
                     + "      </Media>\n")
        else:
            cues += opening + "/>\n"

    audio = (f'  <Audio tracks="4">\n    <Bus id="{BUS}" name="Main L/R" width="2"/>\n'
             "  </Audio>\n") if routed else ""

    return (f'<Show>\n  <Lists>\n    <List id="{LIST}" name="Timbre">\n'
            + cues + "    </List>\n  </Lists>\n" + audio + "</Show>\n")


def make_bundle(folder: Path, files: "list[str]", routed: bool = False,
                standby: "str | None" = None) -> Path:
    folder.mkdir(parents=True)
    (folder / f"{folder.name}.wfg").write_text(MANIFEST, newline="\n")
    (folder / "show.xml").write_text(show_xml(files, routed), newline="\n")

    # The operator's pointer, which is state and not show: parked on a media
    # cue, it arms that cue before any GO.
    if standby is not None:
        (folder / "state.xml").write_text(
            f'<State formatVersion="1">\n  <List id="{LIST}" standby="{standby}"/>\n</State>\n',
            newline="\n")

    media = folder / "media"
    write_wav(media / "sine.wav", sine())
    write_wav(media / "noise.wav", noise())
    write_wav(media / "sweep.wav", sweep())
    (media / "renamed").mkdir()
    shutil.copyfile(media / "sine.wav", media / "renamed" / "sine.wav")
    return folder


# =============================================================================
# The verb
# =============================================================================

def analyse(bundle: Path, locale: "str | None", *extra: str) -> "tuple[int, dict, str, str]":
    """(exit code, {path: fields}, stdout, stderr). Each line is
    `status hash seconds frames levels hash-ms analysis-ms bytes path`, the
    path last because it is the one field that may hold a space."""
    argv = ["analyse", str(bundle), *extra]
    if locale is not None:
        argv.append(f"--wfg-locale={locale}")

    code, out, err = common.run_wfg(*argv)
    lines = {}

    for line in out.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split(" ", 8)
        if len(parts) != 9:
            raise HarnessError(f"wfg analyse printed a line with {len(parts)} fields: {line!r}")
        status, digest, seconds, frames, levels, hash_ms, work_ms, size, path = parts
        lines[path] = {"status": status, "hash": digest, "seconds": seconds,
                       "frames": frames, "levels": levels, "hash_ms": hash_ms,
                       "work_ms": work_ms, "bytes": size}

    return code, lines, out, err


def cache_file(bundle: Path, digest: str) -> Path:
    return bundle / "media" / ".timbre" / f"{digest}.tpy"


def steady(frames: "list[tuple]") -> "list[tuple[int, tuple]]":
    """The frames whose windows lie wholly inside the signal, with their
    indices: the first two and the last two reach past an edge."""
    return list(enumerate(frames))[2:-2]


# =============================================================================
# The checks
# =============================================================================

def check_facts(report: Report, bundle: Path, lines: dict) -> None:
    pyramids = {}

    for name in FILES:
        entry = lines.get(name)
        if entry is None:
            report.check(False, f"{name} has a line of its own")
            continue

        on_disk = hashlib.sha256((bundle / "media" / name).read_bytes()).hexdigest()
        report.equal(entry["hash"], on_disk,
                     f"{name}: the key is the SHA-256 of the file's bytes, as hashlib reads them")

        try:
            pyramid = Pyramid(cache_file(bundle, entry["hash"]).read_bytes())
        except (OSError, ValueError, struct.error) as problem:
            report.check(False, f"{name}: the cache decodes with this driver's own reader",
                         str(problem))
            continue

        report.check(True, f"{name}: the cache decodes with this driver's own reader")
        report.equal(pyramid.rate, RATE, f"{name}: the pyramid records the file's rate")
        pyramids[name] = pyramid

        for level in range(1, len(pyramid.levels)):
            finer = pyramid.levels[level - 1]
            expected = [paired(finer[i], finer[min(i + 1, len(finer) - 1)])
                        for i in range(0, len(finer), 2)]
            if pyramid.levels[level] != expected:
                wrong = next(i for i, (got, want) in
                             enumerate(zip(pyramid.levels[level], expected)) if got != want)
                report.check(False, f"{name}: level {level} is level {level - 1}, paired",
                             f"frame {wrong}: {pyramid.levels[level][wrong]} "
                             f"where the rule gives {expected[wrong]}")
                break
        else:
            report.check(True, f"{name}: every level is the one below it, paired "
                               f"({len(pyramid.levels)} levels, "
                               f"{len(pyramid.finest)} frames at the finest)")

    # --- the sine ----------------------------------------------------------
    if "sine.wav" in pyramids:
        frames = pyramids["sine.wav"].finest
        report.equal(len(frames), math.ceil(SINE_SECONDS * RATE / HOP),
                     "the sine has one frame per hop")

        want_hue = ramp_hue(SINE_HZ)
        want_light = ramp_lightness(SINE_HZ)
        grey = [i for i, f in steady(frames) if unit(f[1]) <= 0.8]
        off_hue = [i for i, f in steady(frames) if hue_apart(hue_of(f), want_hue) > 3.0]
        off_light = [i for i, f in steady(frames) if abs(unit(f[2]) - want_light) > 0.01]

        report.check(not grey, "a 1 kHz sine is saturated above 0.8 in every frame",
                     f"frames {grey[:5]} are not - the first reads {frames[grey[0]] if grey else ''}")
        report.check(not off_hue,
                     f"at the ramp's 1 kHz hue, {want_hue:.1f} degrees, within 3",
                     f"frames {off_hue[:5]}; the first reads "
                     f"{hue_of(frames[off_hue[0]]) if off_hue else 0:.1f}")
        report.check(not off_light,
                     f"and at the ramp's 1 kHz lightness, {want_light:.3f}, within 0.01",
                     f"frames {off_light[:5]}")
        report.check(all(abs(unit(f[3]) - 0.5) < 0.01 for _, f in steady(frames)),
                     "its peak is the half of full scale it was written at")

    # --- the noise ---------------------------------------------------------
    if "noise.wav" in pyramids:
        frames = [f for _, f in steady(pyramids["noise.wav"].finest)]
        saturation = sum(unit(f[1]) for f in frames) / len(frames)
        lightness = sum(unit(f[2]) for f in frames) / len(frames)

        report.check(saturation < 0.2, "white noise is grey: saturation under 0.2",
                     f"the mean is {saturation:.3f}")
        report.check(all(f[2] > 0 for f in frames),
                     "and still a reading, not the silence a missing cache would be: "
                     "every frame has a lightness")
        report.check(lightness > 0.6,
                     "and a bright one: white noise has its power where the bins are, at the top",
                     f"the mean lightness is {lightness:.3f}")

    # --- the sweep ---------------------------------------------------------
    if "sweep.wav" in pyramids:
        frames = pyramids["sweep.wav"].finest
        off_hue = []
        off_light = []

        for index, frame in steady(frames):
            hertz = sweep_hertz(frame_seconds(index))
            if hue_apart(hue_of(frame), ramp_hue(hertz)) > 6.0:
                off_hue.append((index, round(hertz), round(hue_of(frame), 1),
                                round(ramp_hue(hertz), 1)))
            if abs(unit(frame[2]) - ramp_lightness(hertz)) > 0.02:
                off_light.append((index, round(hertz), unit(frame[2])))

        report.check(not off_hue,
                     "a 100 Hz to 8 kHz sweep walks the ramp: every frame's hue is the "
                     "ramp's at the sweep's frequency then, within 6 degrees",
                     f"{len(off_hue)} frames are not; (frame, Hz, read, ramp): {off_hue[:4]}")
        report.check(not off_light,
                     "and its lightness is the ramp's at that frequency, within 0.02",
                     f"{len(off_light)} frames are not: {off_light[:4]}")

        lights = [f[2] for _, f in steady(frames)]
        falls = [i for i in range(1, len(lights)) if lights[i] < lights[i - 1] - 1]
        report.check(not falls,
                     "and the lightness never falls - the one thing on the ramp that is "
                     "monotonic by construction",
                     f"it falls at steady frames {falls[:5]}")
        report.check(unit(lights[-1]) - unit(lights[0]) > 0.4,
                     "from dark to bright across the sweep",
                     f"{unit(lights[0]):.3f} to {unit(lights[-1]):.3f}")


def serves(report: Report, room: Path, locale: "str | None") -> None:
    """THE SAME FUNCTION ON `wfg serve`'S THREAD, which no unit case reaches:
    the analyser started after the first publish and handed every file the
    show names, and the tick thread handing it a file a show edit introduces.

    Waited on by the cache FILE, which is the one thing both checks read and
    which appears whole or not at all - it is written to a temp and moved into
    place, so a file that exists is a file that decodes (trap 3)."""
    bundle = make_bundle(room / "served", FILES)
    digests = {hashlib.sha256((bundle / "media" / name).read_bytes()).hexdigest()
               for name in FILES}

    late = bundle / "media" / "late.wav"
    write_wav(late, [0.5 * math.sin(2 * math.pi * 500.0 * n / RATE) for n in range(RATE)])
    late_digest = hashlib.sha256(late.read_bytes()).hexdigest()

    with common.Server(bundle, locale=locale) as server:
        caches = [cache_file(bundle, digest) for digest in sorted(digests)]

        report.check(common.wait_until(lambda: all(c.is_file() for c in caches),
                                       timeout=60.0) is not None,
                     "wfg serve colours every file the show names on its own, after it opens",
                     f"present: {[c.name for c in caches if c.is_file()]}")
        report.check(not cache_file(bundle, late_digest).exists(),
                     "and nothing the show does not name")

        # A plain datagram to a node is `node.set`: the first cue now plays a
        # file that was not in the show when it opened.
        common.send_udp(server.osc_port,
                        common.osc_encode(f"/godot/cue/{IDS[0]}/file", ["late.wav"]))

        landed = common.wait_until(lambda: cache_file(bundle, late_digest).is_file(),
                                   timeout=60.0) is not None
        report.check(landed, "a file an edit introduces mid-session is coloured too, "
                             "without the show being reopened")

        if landed:
            try:
                pyramid = Pyramid(cache_file(bundle, late_digest).read_bytes())
                report.equal(len(pyramid.finest), math.ceil(RATE / HOP),
                             "and its cache is whole: one second, one frame per hop")
            except (OSError, ValueError, struct.error) as problem:
                report.check(False, "and its cache is whole", str(problem))


# =============================================================================
# What a session says with it - PR 5.8, namespace draft §14.5
# =============================================================================

# How far a published number may sit from its byte: the tree rounds a hue to a
# tenth of a degree and the other two to a thousandth, so half of that and no
# more. A byte is 1.4 degrees of hue and 0.004 of the unit range, so this tells
# one byte from the next - and a hue scaled by 255 where the wheel has 256
# steps, nearly a degree out at the sweep's blues, from the right one.
HUE_ROUNDING = 0.05
UNIT_ROUNDING = 0.0005
SLACK = 1e-9

# The stretch of the sweep a playing run is read over, in seconds into the
# file: clear of the edge frames at both ends and of the nought a launch
# placed a few ticks ahead is clamped to, and a second short of the end, so
# that every reading is of a clip still playing and none is of one held at its
# last frame.
READ_FROM, READ_TO = 0.8, 7.0

# How long a playing run is read for at the most, in real time from the GO.
# The sweep is eight seconds and the reading stops as soon as it has what it
# needs, so this is only ever reached by a clock that does not move.
LISTENING = 20.0


def value_at(port: int, address: str):
    """The value at one address, or None when there is none to read."""
    status, body = common.http_get(port, address + "?VALUE")

    if status != 200:
        return None

    return json.loads(body)["VALUE"][0]


def runs_of(port: int, cue: str) -> "list[str]":
    """Every published run of one cue, found through `/godot/run/order` - the
    node whose job is to list the runs - rather than the container's keys,
    which hold that roster node too."""
    order = value_at(port, "/godot/run/order") or ""
    return [each for each in order.split() if value_at(port, f"/godot/run/{each}/cue") == cue]


def reading_of(port: int, run_id: str) -> "dict | None":
    """A run's state, playhead and timbre, from ONE request. A `GET` of the
    run's own container is answered whole out of one snapshot, so the playhead
    and the reading taken at it come from the same tick. Three `?VALUE`s are
    three snapshots, and a check comparing them would compare two moments."""
    status, body = common.http_get(port, f"/godot/run/{run_id}")

    if status != 200:
        return None

    contents = json.loads(body).get("CONTENTS") or {}

    def value(name: str):
        values = (contents.get(name) or {}).get("VALUE") or [None]
        return values[0]

    return {"state": value("state"), "position": value("position"),
            "timbre": value("timbre")}


def first_reading(read, accept, timeout: float):
    """(what `read` returned when `accept` first held of it, or None; and the
    last thing it returned that was there at all). `common.wait_until`
    underneath, so the wait is on the thing and bounded in real time; the
    second half is so that a check which fails can say what it saw rather than
    only that it waited - and it is the last thing THERE because a run is
    published for five seconds after it ends and then answers nothing, which
    would otherwise be the whole of what a failed wait on it reports."""
    seen = [None]

    def attempt():
        value = read()
        if value is not None:
            seen[0] = value
        return value if accept(value) else None

    return common.wait_until(attempt, timeout=timeout), seen[0]


def three_numbers(text: str) -> "tuple | None":
    """`"<hue> <saturation> <lightness>"`, one space apart, as three floats -
    or None. A comma where a point belongs is a ValueError here, which is what
    the fr_FR run is for."""
    parts = text.split(" ")

    if len(parts) != 3:
        return None

    try:
        numbers = tuple(float(part) for part in parts)
    except ValueError:
        return None

    return numbers if all(math.isfinite(number) for number in numbers) else None


def media_type(headers: dict) -> str:
    return headers.get("content-type", "").split(";")[0].strip()


def directives(headers: dict) -> "list[str]":
    """A `Cache-Control` header's directives, each on its own and lower-cased,
    so a check asks for a directive rather than for a spelling of the line."""
    return [part.strip().lower()
            for part in headers.get("cache-control", "").split(",") if part.strip()]


def asked_again(headers: dict) -> bool:
    """`no-cache`, and nothing that would let a browser skip the asking: no
    `immutable`, and no `max-age` of any length. A browser may keep what it
    fetched; it may not reuse it without checking that the answer has not
    moved - which it will, under the same URL, the day a ramp stop does."""
    said = directives(headers)
    return ("no-cache" in said and "immutable" not in said
            and not any(each.startswith("max-age") for each in said))


def placed(position: float, pyramid: Pyramid) -> "tuple[int, float]":
    """(the finest frame under a playhead, and how far into that frame it
    sits, from nought to one), placed as §14.5 and `frameAt` place it: the
    seconds times the rate, over the hop, ROUNDED DOWN, and held at the first
    frame before the file and at the last one after it. Multiplied and then
    divided, in the engine's order, so that the two floors agree to the last
    bit of a position the tree printed round-trip."""
    along = position * pyramid.rate / HOP
    index = min(max(math.floor(along), 0), len(pyramid.finest) - 1)
    return index, along - index


def discriminating(frames: "list[tuple]", index: int) -> bool:
    """Whether a reading of this frame could only be this frame. Its colour -
    hue, saturation and lightness, the three the tree publishes, and never the
    peak, which it does not - differs from both neighbours', and a byte apart
    is more than twice the rounding either way, so a playhead placed a frame
    early or a frame late would have published a colour that fails the match.
    A frame with no neighbour on one side is never counted: half a proof."""
    if index <= 0 or index >= len(frames) - 1:
        return False

    colour = frames[index][:3]
    return colour != frames[index - 1][:3] and colour != frames[index + 1][:3]


def matches(numbers: tuple, frame: tuple) -> bool:
    """Whether three published numbers are this frame, within the rounding the
    tree publishes with and not a hair more."""
    hue, saturation, lightness = numbers
    return (hue_apart(hue, hue_of(frame)) <= HUE_ROUNDING + SLACK
            and abs(saturation - unit(frame[1])) <= UNIT_ROUNDING + SLACK
            and abs(lightness - unit(frame[2])) <= UNIT_ROUNDING + SLACK)


def listen(port: int, run_id: str, pyramid: Pyramid) -> "tuple[list[dict], dict | None]":
    """(every reading of the playing sweep caught between READ_FROM and
    READ_TO, each placed on its frame; and the last thing the run's container
    said at all, for a check that fails to show).

    ONE GET PER READING, of the run's own container, so the playhead a reading
    is placed by and the colour it is checked for come out of one snapshot -
    see `reading_of`.

    READ UNTIL EVERY WRONG WAY TO PLACE A PLAYHEAD HAS BEEN GIVEN ITS CHANCE TO
    SHOW, over at least three frames. Each is refuted by a reading
    DISCRIMINATING enough that only its own frame could have produced it, and
    each needs a different one. A frame early or a frame late: any of them.
    Rounding where §14.5 floors: one in the later half of its frame, where the
    two pick different frames. Reading the level above: one at an EVEN frame,
    because a frame of that level is an even frame and the odd one after it,
    paired, and - the sweep climbing in hue and lightness through the whole
    stretch, and a pair rounding its half up - it never comes out as the even
    one. Or until the stretch is behind the playhead, or the run has ended, or
    LISTENING has passed in real time: `common.wait_until` underneath, so the
    wait is on the thing and bounded.

    The later half and the even frame are waited FOR and never asserted. A
    clock whose every playhead fell on a frame's edge would make rounding and
    flooring the same function, and one whose block was two frames long could
    land on odd frames alone; a run read to the end of the stretch without
    them has shown everything that clock can show."""
    heard: "list[dict]" = []
    last: "list[dict | None]" = [None]

    def enough() -> bool:
        telling = [each for each in heard if discriminating(pyramid.finest, each["index"])]
        return (len({each["index"] for each in heard}) >= 3
                and any(each["index"] % 2 == 0 for each in telling)
                and any(each["into"] >= 0.5 for each in telling))

    def attempt() -> bool:
        now = reading_of(port, run_id)

        # Published no longer: the run ended and its five seconds ran out.
        if now is None:
            return last[0] is not None

        last[0] = now
        position = now["position"]

        if not isinstance(position, (int, float)):
            return False

        if READ_FROM <= position <= READ_TO:
            index, into = placed(float(position), pyramid)
            heard.append({"position": float(position), "index": index, "into": into,
                          "timbre": now["timbre"]})

        return enough() or position > READ_TO or now["state"] in ("done", "failed")

    common.wait_until(attempt, timeout=LISTENING)
    return heard, last[0]


def check_readings(report: Report, heard: "list[dict]", last: "dict | None",
                   pyramid: Pyramid) -> None:
    """The sweep's colour at every playhead caught: the cache's own frame under
    that playhead, to the byte - and a claim that could fail, because at least
    one of those frames is unlike both its neighbours - and the ramp's colour
    at the sweep's frequency then, as the verb's checks assert it of the bytes."""
    if not report.check(bool(heard),
                        f"GO plays the sweep, and between {READ_FROM} s and {READ_TO} s into "
                        "the file its run publishes where it is and what it sounds like there",
                        f"the last read was {last}"):
        return

    frames = pyramid.finest
    unread = []
    misplaced = []
    telling = []
    grey = []
    off_hue = []
    off_light = []

    for each in heard:
        position, index, text = each["position"], each["index"], each["timbre"]
        numbers = three_numbers(text) if isinstance(text, str) else None

        if numbers is None:
            unread.append((round(position, 4), text))
            continue

        frame = frames[index]
        hue, saturation, lightness = numbers
        hertz = sweep_hertz(frame_seconds(index))

        if not matches(numbers, frame):
            misplaced.append((round(position, 4), index, text, frame))

        if discriminating(frames, index):
            telling.append(each)

        if saturation <= 0.8:
            grey.append((round(position, 4), saturation))

        # The ramp's tolerances, and the tree's rounding on top: the verb's
        # check holds the byte within 6 degrees and 0.02, and a number printed
        # from that byte may sit half a rounding further off.
        if hue_apart(hue, ramp_hue(hertz)) > 6.0 + HUE_ROUNDING + SLACK:
            off_hue.append((round(position, 4), round(hertz), hue, round(ramp_hue(hertz), 1)))

        if abs(lightness - ramp_lightness(hertz)) > 0.02 + UNIT_ROUNDING + SLACK:
            off_light.append((round(position, 4), round(hertz), lightness,
                              round(ramp_lightness(hertz), 3)))

    # Each playhead once, with the frame it is placed on: what every failure
    # below prints, so it says where it looked and not only that it looked.
    where = list(dict.fromkeys((round(each["position"], 4), each["index"]) for each in heard))
    shown = f"(seconds, frame): {where[:12]}" + (f" and {len(where) - 12} more"
                                                   if len(where) > 12 else "")
    spanned = sorted({index for _, index in where})
    even = sum(1 for each in telling if each["index"] % 2 == 0)
    later = sum(1 for each in telling if each["into"] >= 0.5)
    proofs = [(round(each["position"], 4), each["index"], round(each["into"], 3))
              for each in telling[:6]]

    report.check(not unread,
                 "every reading is three numbers one space apart - hue, saturation, "
                 "lightness - each written with a point under every locale, and none is "
                 "the empty of a file not analysed yet after its hash was on the tree",
                 f"{len(unread)} are not; (seconds, reading): {unread[:4]}")
    report.check(not misplaced,
                 f"every reading is the cache's own frame under its playhead, to the byte - "
                 f"floor(seconds times rate over hop) - over {len(heard)} readings, frames "
                 f"{spanned[0]} to {spanned[-1]}",
                 f"{len(misplaced)} are not; (seconds, frame, tree, bytes): {misplaced[:4]}; "
                 + shown)
    report.check(len(spanned) >= 3 and bool(telling),
                 f"and that was a claim that could fail: {len(spanned)} frames, and "
                 f"{len(telling)} readings of a frame unlike both its neighbours - {even} at "
                 f"an even frame, which the level above never reads the same, and {later} in "
                 "the later half of one, where rounding and flooring disagree",
                 f"discriminating (seconds, frame, how far into it): {proofs}; " + shown)
    report.check(not grey,
                 "the sweep's run reads saturated at every playhead, above 0.8",
                 f"{len(grey)} do not; (seconds, saturation): {grey[:4]}")
    report.check(not off_hue,
                 "at the ramp's hue for the sweep's frequency then, within 6 degrees",
                 f"{len(off_hue)} are not; (seconds, Hz, read, ramp): {off_hue[:4]}")
    report.check(not off_light,
                 "and at the ramp's lightness for it, within 0.02",
                 f"{len(off_light)} are not; (seconds, Hz, read, ramp): {off_light[:4]}")


def check_route(report: Report, port: int, digest: str, pyramid: "Pyramid | None") -> None:
    """`GET /media/<hash>/timbre`, on the tree's port and never inside the
    tree: kilobytes a file, unchanged for as long as the file and the analysis
    are, and with no value at a moment - so not a node, and not in the poll
    every client makes ten times a second. Every body is checked against this
    driver's own decoding of the `.tpy`, never against another answer from the
    engine.

    Asked of the SWEEP, the pyramid already decoded for the run, and the harder
    of the two to match by accident: its frames change along its whole length,
    where a steady sine's are one frame repeated, so a body a frame out of step
    differs from the cache almost everywhere rather than only at its ends."""
    route = f"/media/{digest}/timbre"

    status, headers, body = common.http_get_bytes(port, route + "?INFO")
    report.check(status == 200 and media_type(headers) == "application/json"
                 and asked_again(headers),
                 "?INFO answers 200 in JSON, marked no-cache, with no max-age and never "
                 "immutable: the URL is named by the audio and not by the analysis, and a "
                 "moved ramp stop rebuilds the pyramid under the same name",
                 f"{status}, {media_type(headers)!r}, Cache-Control "
                 f"{headers.get('cache-control', '')!r}: {body[:160]!r}")

    try:
        info = json.loads(body.decode("utf-8"))
    except ValueError:
        info = None

    # Without a decoded cache there is nothing of the driver's own to compare
    # a body with, and that has already been reported as the failure it is.
    if pyramid is not None:
        text = body.decode("utf-8", "replace")

        if not isinstance(info, dict):
            report.check(False, "its body is a JSON object", text[:160])
        else:
            report.check(list(info) == ["sha256", "formatVersion", "seconds", "sampleRate",
                                        "window", "hop", "levels"]
                         and " " not in text,
                         "its keys are §14.5's, in that order with formatVersion second, and "
                         "no space anywhere", text)

            # THE ONE NUMBER THAT SAYS WHICH ANALYSIS THE BYTES ARE, and the
            # reason a client cannot cache by the hash alone. Compared with the
            # version this driver's reader decoded from the .tpy's own header,
            # never with a constant: the route must say what the file says.
            version = info.get("formatVersion")
            report.check(type(version) is int and version == pyramid.version,
                         f"and its formatVersion is the .tpy's own, {pyramid.version}, as this "
                         "driver's reader decodes it from the header",
                         f"?INFO says {version!r}: {text}")

            seconds = info.get("seconds")
            report.check(info.get("sha256") == digest
                         and isinstance(seconds, (int, float))
                         and abs(seconds - pyramid.samples / pyramid.rate) < 1e-9
                         and (info.get("sampleRate"), info.get("window"), info.get("hop"))
                         == (pyramid.rate, WINDOW, HOP),
                         "and its header is the cache's: the hash it was asked by, the "
                         "file's seconds and rate, window 2048, hop 1024", text)

            report.equal(info.get("levels"),
                         [{"frames": len(level), "bytes": 4 * len(level)}
                          for level in pyramid.levels],
                         "and its levels are the ones this driver's reader finds in the "
                         ".tpy, frames and bytes alike")

        status, headers, body = common.http_get_bytes(port, route + "?level=0")
        finest = bytes(byte for frame in pyramid.finest for byte in frame)

        report.check(status == 200 and media_type(headers) == "application/octet-stream"
                     and asked_again(headers),
                     "?level=0 answers 200 in raw bytes, application/octet-stream, and is "
                     "marked no-cache too",
                     f"{status}, {media_type(headers)!r}, Cache-Control "
                     f"{headers.get('cache-control', '')!r}")
        report.check(body == finest,
                     "and its body is the finest level byte for byte as the .tpy holds it: "
                     "hue, saturation, lightness and peak, four a frame",
                     f"{len(body)} bytes against {len(finest)}")

        beyond = len(pyramid.levels)
        status, headers, body = common.http_get_bytes(port, f"{route}?level={beyond}")
        report.check(status == 404 and "no-store" in directives(headers),
                     f"?level={beyond}, one past the coarsest, is 404, marked no-store: a "
                     "level this pyramid does not have",
                     f"{status}, Cache-Control {headers.get('cache-control', '')!r}: "
                     f"{body[:160]!r}")

    # The refusals, which need no cache to check.
    status, headers, body = common.http_get_bytes(port, f"/media/{'0' * 64}/timbre?level=0")
    said = directives(headers)
    report.check(status == 404 and "no-store" in said and "immutable" not in said,
                 "a hash nothing has analysed is 404, marked no-store and never immutable: "
                 "the same URL is a 200 on the day that file is imported",
                 f"{status}, Cache-Control {headers.get('cache-control', '')!r}: "
                 f"{body[:160]!r}")

    status, headers, body = common.http_get_bytes(port, "/media/NOTHEX/timbre?level=0")
    report.check(status == 400 and "no-store" in directives(headers),
                 "a hash that is not sixty-four hex characters is a bad request, marked "
                 "no-store and refused before anything is looked up - no request text ever "
                 "becomes a path",
                 f"{status}, Cache-Control {headers.get('cache-control', '')!r}: "
                 f"{body[:160]!r}")


def plays(report: Report, room: Path, locale: "str | None") -> None:
    """WHAT A SESSION SAYS WITH THE CACHE, read from outside: the hash on the
    cue, the colour on the run, and the pyramid on a route that is not an
    address.

    HOSTED, because a playhead needs a track; parked on the sweep, so the one
    GO plays it and nothing has to be addressed by name. THE SWEEP, because
    its neighbouring frames differ, and that is what lets a reading be checked
    against the frame under ITS playhead rather than against a frame like it -
    see `listen`. Every wait is on the node its check then reads, bounded in
    real time - the hash on the hash, the colour on the run's own container -
    and the GO waits for the sweep's hash, because the analyser publishes a
    record only with its pyramid: a hash a client can read is a pyramid the
    run's colour is read from and the route holds, and a GO before it would
    spend the stretch being read on the empty of a file not analysed yet."""
    bundle = make_bundle(room / "played", FILES, routed=True, standby=SWEEP_CUE)
    locale_argument = [f"--wfg-locale={locale}"] if locale is not None else []

    code, out, err = common.run_wfg("validate", str(bundle), *locale_argument)

    if not report.check(code == 0 and "is valid" in out,
                        "the routed copy is a show wfg validate accepts: tracks, a bus two "
                        "wide, a route on every media cue, the standby on the sweep",
                        (out + err).strip()):
        return

    sweep_digest = hashlib.sha256((bundle / "media" / "sweep.wav").read_bytes()).hexdigest()
    sine_digest = hashlib.sha256((bundle / "media" / "sine.wav").read_bytes()).hexdigest()
    cache = cache_file(bundle, sweep_digest)

    with common.Server(bundle, locale=locale, sample_rate=RATE, hosted=True) as server:
        port = server.http_port

        report.check(common.wait_until(
                         lambda: value_at(port, "/godot/audio/status") == "running",
                         timeout=20.0) is not None,
                     "under --hosted the audio side comes up running, unasked")

        # --- the hash, on the cue ------------------------------------------
        published, seen = first_reading(
            lambda: value_at(port, f"/godot/cue/{SWEEP_CUE}/hash"),
            lambda value: value == sweep_digest, timeout=60.0)
        report.check(published is not None,
                     "the sweep's cue publishes its file's SHA-256, as hashlib computes it, "
                     "once the analyser has reached it",
                     f"it still read {seen!r} after 60 s; hashlib says {sweep_digest}")

        # The cache that hash names, decoded by this driver's own reader: what
        # the run's colour and the route's bytes are both checked against.
        # Waited on as a file, which appears whole or not at all (trap 3).
        pyramid = None
        problem = "no cache file appeared within 60 s"

        if common.wait_until(cache.is_file, timeout=60.0):
            try:
                pyramid = Pyramid(cache.read_bytes())
            except (OSError, ValueError, struct.error) as refused:
                problem = str(refused)

        report.check(pyramid is not None,
                     "and the cache it names decodes with this driver's own reader", problem)

        # --- the colour, on the run ----------------------------------------
        # ASKED FOR BY CUE, AND BEFORE THE GO: the standby arms its cue ahead,
        # so the run a GO launches exists before the GO does, and "the run that
        # appeared" would find nothing. The session is fresh, so no earlier run
        # of this cue is still published to answer in its place (trap 5).
        def standby_run():
            found = runs_of(port, SWEEP_CUE)
            return found[0] if len(found) == 1 else None

        sweep_run = common.wait_until(standby_run, timeout=20.0)
        armed = report.check(bool(sweep_run),
                             "the standby armed the sweep ahead: its run is there before any GO")

        # Without a decoded cache there is nothing to place a reading on, and
        # that has already been reported as the failure it is.
        if armed and pyramid is not None:
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/go"))

            heard, last = listen(port, sweep_run, pyramid)
            check_readings(report, heard, last, pyramid)

        # --- the pyramid, on a route that is not an address ----------------
        check_route(report, port, sweep_digest, pyramid)

        # --- the sine's hash, and the same bytes under another name ---------
        # About the hash and not the reading: both cues must name the one
        # SHA-256 hashlib finds, whichever the analyser happened to reach first.
        published, seen = first_reading(
            lambda: value_at(port, f"/godot/cue/{SINE_CUE}/hash"),
            lambda value: value == sine_digest, timeout=60.0)
        report.check(published is not None,
                     "the sine's cue publishes its file's SHA-256 too, as hashlib computes it",
                     f"it still read {seen!r} after 60 s; hashlib says {sine_digest}")

        renamed, seen = first_reading(
            lambda: value_at(port, f"/godot/cue/{RENAMED_CUE}/hash"),
            lambda value: value == sine_digest, timeout=60.0)
        report.check(renamed is not None,
                     "the copy under another name publishes the same hash: the key is the "
                     "bytes and not the path, so both cues are drawn from one pyramid",
                     f"it still read {seen!r} after 60 s")


def run(locale: "str | None") -> int:
    report = Report(f"timbre: the cache, before anything is drawn ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-timbre-") as scratch:
        room = Path(scratch)
        bundle = make_bundle(room / "timbre", FILES)

        # --- the first run builds, once per distinct file ----------------------
        code, lines, out, err = analyse(bundle, locale)
        report.equal(code, 0, "wfg analyse over a bundle whose every file is audio exits 0",
                     err.strip())
        report.equal(sorted(lines), sorted(FILES), "one line per file the show names",
                     out.strip())

        if code != 0 or sorted(lines) != sorted(FILES):
            return report.finish()

        report.equal([lines[n]["status"] for n in FILES[:3]], ["built"] * 3,
                     "the three signals are analysed and cached")
        report.equal(lines["renamed/sine.wav"]["hash"], lines["sine.wav"]["hash"],
                     "the same bytes under another name have the same key")
        report.equal(lines["renamed/sine.wav"]["status"], "cached",
                     "and are analysed once: the second name finds the first's cache")
        numbers = [value for entry in lines.values()
                   for key, value in entry.items() if key not in ("status", "hash")]
        report.check(lines["sine.wav"]["seconds"] == "3.000"
                     and not any("," in value for value in numbers),
                     "numbers print with a point under every locale",
                     f"the sine's seconds read {lines['sine.wav']['seconds']!r}")

        check_facts(report, bundle, lines)

        caches = {name: cache_file(bundle, lines[name]["hash"]).read_bytes()
                  for name in FILES}

        # --- the second run does no work ---------------------------------------
        code, again, _, err = analyse(bundle, locale)
        report.equal(code, 0, "a second run exits 0", err.strip())
        report.equal(sorted({entry["status"] for entry in again.values()}), ["cached"],
                     "a second run reads every file from the cache and analyses nothing")
        report.check(all(cache_file(bundle, lines[n]["hash"]).read_bytes() == caches[n]
                         for n in FILES),
                     "and leaves every cache file as it found it")

        # --- --force writes the same bytes -------------------------------------
        code, forced, _, err = analyse(bundle, locale, "--force")
        report.equal(code, 0, "--force exits 0", err.strip())
        report.equal(sorted({entry["status"] for entry in forced.values()}), ["built"],
                     "--force analyses every file again, the second name included")
        report.check(all(cache_file(bundle, lines[n]["hash"]).read_bytes() == caches[n]
                         for n in FILES),
                     "and writes the same bytes: the analysis is a function of the audio")

        # --- a torn cache is a cache miss --------------------------------------
        torn = cache_file(bundle, lines["noise.wav"]["hash"])
        torn.write_bytes(caches["noise.wav"][:-3])
        code, mended, _, _ = analyse(bundle, locale)
        report.equal(mended["noise.wav"]["status"], "built",
                     "a cache file cut short is refused and built again, not drawn")
        report.check(torn.read_bytes() == caches["noise.wav"],
                     "and the file is whole again afterwards")

        # --- a file the show names and the bundle lacks ------------------------
        gone = make_bundle(room / "gone", FILES + ["missing.wav"])
        code, lines, _, _ = analyse(gone, locale)
        report.equal(code, 1, "a file the show names and the bundle lacks fails the run")
        report.equal((lines.get("missing.wav") or {}).get("status"), "missing",
                     "and is named on its own line")
        report.equal((lines.get("sine.wav") or {}).get("status"), "built",
                     "while every other file is still analysed")

        # --- a .timbre that cannot be a folder ---------------------------------
        blocked = make_bundle(room / "blocked", FILES)
        (blocked / "media" / ".timbre").write_text("in the way\n")
        code, lines, _, _ = analyse(blocked, locale)
        report.equal(code, 1, "a cache that cannot be written fails the verb, which was asked for one")
        report.equal(sorted({entry["status"] for entry in lines.values()}), ["memory"],
                     "but every file is still analysed - in memory, as a session would keep it")
        report.check((blocked / "media" / ".timbre").is_file(),
                     "and what stood in the way is left standing")

        # --- nothing to analyse with -------------------------------------------
        code, _, _, _ = analyse(room / "nowhere", locale)
        report.equal(code, 2, "a folder that is not there is a harness failure, exit 2")

        # --- and the thread a session runs it on -------------------------------
        serves(report, room, locale)

        # --- and what a session says with it -----------------------------------
        plays(report, room, locale)

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"timbre_cache: {problem}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"timbre_cache: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
