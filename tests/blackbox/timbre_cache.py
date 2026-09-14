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
hue climbs. Plan decision 8's stops run purple (280 degrees) to deep blue (240)
and then the other way round the wheel through red, orange, yellow and green,
so from 40 Hz to 150 Hz the hue turns back. What is monotonic is the lightness,
by construction. So the sweep is asserted twice: its lightness never falls, and
every frame's hue is the ramp's hue AT THE SWEEP'S FREQUENCY THEN - which is a
stronger check than monotonic would have been, and one a machine can make.

AND THE CACHE BEHAVES LIKE ONE: the same bytes under two names are analysed
once, a second run does no work, `--force` writes the same bytes again, a file
the show names and the bundle lacks fails the run, and a `.timbre` that cannot
be a folder costs the cache and not the colours.

AND THE THREAD A SESSION RUNS IT ON: last, one `wfg serve` of a fresh copy,
which must colour every file the show names without being asked, and colour a
file a `node.set` introduces mid-session without being reopened - the serve
wiring, which no unit case reaches.

No audio device anywhere: `wfg analyse` is a verb and runs where `wfg validate`
runs, and the server is the dummy clock's. Exit codes: 0 everything held, 1
something did not, 2 the harness could not run.
"""

from __future__ import annotations

import hashlib
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
FORMAT_VERSION = 1

# (hertz, hue in degrees UNWRAPPED): 360 is red and 480 green, so deep blue to
# red goes through magenta and not back through cyan.
RAMP = [(40.0, 280.0), (150.0, 240.0), (500.0, 360.0),
        (1500.0, 390.0), (4000.0, 420.0), (12000.0, 480.0)]

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


def show_xml(files: "list[str]") -> str:
    cues = "".join(
        f'      <Media id="{IDS[n]}" file="{name}" name="Take {n + 1}" number="{n + 1}"/>\n'
        for n, name in enumerate(files))
    return ("<Show>\n  <Lists>\n    <List id=\"7K2QM9X4\" name=\"Timbre\">\n"
            + cues + "    </List>\n  </Lists>\n</Show>\n")


def make_bundle(folder: Path, files: "list[str]") -> Path:
    folder.mkdir(parents=True)
    (folder / f"{folder.name}.wfg").write_text(MANIFEST, newline="\n")
    (folder / "show.xml").write_text(show_xml(files), newline="\n")

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
