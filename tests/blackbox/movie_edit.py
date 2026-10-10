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
"""A movie edited inside Go.dot: its sections, its sound cut in step, the dissolve, the pair frozen
(namespace draft section 55.5).

The author, 2026-10-10: "Can we get to work on the video in a similar way? HAP conversion prior to
editing is okay."

Driven through the shipped serve, hosted on the dummy device, with nothing but named commands over
OSC and the tree:

 1. A three-second HAP movie of three shots - red, green, blue, ten frames each - with a ramp locked
    to it as its sound, the sound's lane dipped over its third second. The movie split at one and two
    seconds and its shots put in the order blue, green, red: the sound's sections are the movie's,
    copied, and its dip is on the first half second.
 2. A dissolve of four frames at the join into the green shot. Both renders say `done`; the movie's
    under media/.edits is thirty frames on the file's grid, every frame outside the dissolve the
    source's own bytes in the new order, the four inside it something else.
 3. `media.freeze` on the movie: the pair is frozen in one record - "three (edit).mov" and
    "ramp (edit).wav" beside their sources - and a GO plays both.
 4. `media.unfreeze` on the movie: both are on their files again, the renders found rather than made.
 5. The session's log holds every record and replays record for record.
"""
import struct
import sys
import tempfile
from pathlib import Path

import common
import first_sound
import media_edit
from common import Server

RATE = first_sound.RATE
BLOCK = first_sound.BLOCK
LIST = "7K2QM9X4"
MOVIE = "MV000001"
SOUND = "MV000002"
CANVAS = "CV000001"
FPS = 10
WIDTH, HEIGHT = 16, 8
COLOURS = (0xFF0000, 0x00FF00, 0x0000FF)   # a second each

SHOW = ('<Show><Lists goDebounce="0"><List id="7K2QM9X4" name="Edit">'
        f'<Video id="{MOVIE}" canvas="{CANVAS}" source="movie" file="three.mov" name="Three" number="1"/>'
        f'<Media id="{SOUND}" file="ramp.wav" lockedTo="{MOVIE}" name="Three (sound)" number="2" channels="1">'
        '<Route id="MV000003" bus="J3MT5XYA" gains="1 1"/></Media></List></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" width="2"/></Audio>'
        f'<Canvases><Canvas id="{CANVAS}" name="Stage"/></Canvases></Show>')


# =============================================================================
# A HAP movie of the driver's own - the tests' HapMovieWriter, in Python
# =============================================================================

def be32(value: int) -> bytes:
    return struct.pack(">I", value)


def be16(value: int) -> bytes:
    return struct.pack(">H", value)


def box(kind: bytes, body: bytes) -> bytes:
    return be32(len(body) + 8) + kind + body


def rgb565(rgb: int) -> int:
    r, g, b = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def solid_dxt1(width: int, height: int, rgb: int) -> bytes:
    """A DXT1 texture of one colour, every block both ends that colour."""
    colour = rgb565(rgb)
    block = struct.pack("<HH", colour, colour) + b"\0" * 4
    return block * ((width // 4) * (height // 4))


def hap_section(kind: int, body: bytes) -> bytes:
    n = len(body)
    return bytes([n & 255, (n >> 8) & 255, (n >> 16) & 255, kind]) + body


def hap_movie(width: int, height: int, frames: "list[bytes]", fps: int, codec: bytes = b"Hap1") -> bytes:
    """A QuickTime movie of HAP frames: ftyp, mdat, then moov with one video track, every frame in one chunk."""
    ftyp = box(b"ftyp", b"qt  " + b"\0" * 4 + b"qt  ")
    mdat = box(b"mdat", b"".join(frames))
    first = len(ftyp) + 8
    count = len(frames)

    mdhd = b"\0" * 4 + be32(0) + be32(0) + be32(fps) + be32(count) + b"\0" * 4
    hdlr = b"\0" * 8 + b"vide" + b"\0" * 12
    entry = b"\0" * 6 + be16(1) + b"\0" * 16 + be16(width) + be16(height) + b"\0" * 50
    stsd = b"\0" * 4 + be32(1) + box(codec, entry)
    stts = b"\0" * 4 + be32(1) + be32(count) + be32(1)
    stsc = b"\0" * 4 + be32(1) + be32(1) + be32(count) + be32(1)
    stsz = b"\0" * 4 + be32(0) + be32(count) + b"".join(be32(len(frame)) for frame in frames)
    stco = b"\0" * 4 + be32(1) + be32(first)

    stbl = box(b"stbl", box(b"stsd", stsd) + box(b"stts", stts) + box(b"stsc", stsc) + box(b"stsz", stsz) + box(b"stco", stco))
    mdia = box(b"mdia", box(b"mdhd", mdhd) + box(b"hdlr", hdlr) + box(b"minf", stbl))
    return ftyp + mdat + box(b"moov", box(b"trak", mdia))


def write_three_colours(path: Path) -> "list[bytes]":
    """Thirty frames at ten a second: red, green, blue; the frames, for the oracle."""
    frames = [hap_section(0xAB, solid_dxt1(WIDTH, HEIGHT, COLOURS[n // FPS])) for n in range(3 * FPS)]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hap_movie(WIDTH, HEIGHT, frames, FPS))
    return frames


# =============================================================================
# A movie's frames read back - enough of QuickTime to find them
# =============================================================================

def boxes_in(data: bytes, start: int, end: int):
    at = start

    while at + 8 <= end:
        size = struct.unpack(">I", data[at:at + 4])[0]
        kind = data[at + 4:at + 8]
        head = 8

        if size == 1:
            size = struct.unpack(">Q", data[at + 8:at + 16])[0]
            head = 16
        elif size == 0:
            size = end - at

        if size < head:
            return

        yield kind, at + head, at + size
        at += size


def find_box(data: bytes, start: int, end: int, path: "list[bytes]"):
    for kind, a, b in boxes_in(data, start, end):
        if kind == path[0]:
            return (a, b) if len(path) == 1 else find_box(data, a, b, path[1:])

    return None


def frames_of(path: Path) -> "list[bytes]":
    data = path.read_bytes()
    stbl = find_box(data, 0, len(data), [b"moov", b"trak", b"mdia", b"minf", b"stbl"])

    if stbl is None:
        return []

    stsz = find_box(data, stbl[0], stbl[1], [b"stsz"])
    chunks = find_box(data, stbl[0], stbl[1], [b"stco"])
    wide = False

    if chunks is None:
        chunks = find_box(data, stbl[0], stbl[1], [b"co64"])
        wide = True

    if stsz is None or chunks is None:
        return []

    fixed, count = struct.unpack(">II", data[stsz[0] + 4:stsz[0] + 12])
    sizes = [fixed] * count if fixed else [struct.unpack(">I", data[stsz[0] + 12 + 4 * n:stsz[0] + 16 + 4 * n])[0]
                                           for n in range(count)]
    chunk_count = struct.unpack(">I", data[chunks[0] + 4:chunks[0] + 8])[0]
    each = 8 if wide else 4
    offsets = [struct.unpack(">Q" if wide else ">I", data[chunks[0] + 8 + each * n:chunks[0] + 8 + each * (n + 1)])[0]
               for n in range(chunk_count)]

    if chunk_count == count:
        return [data[offset:offset + size] for offset, size in zip(offsets, sizes)]

    at = offsets[0] if offsets else 0
    frames = []

    for size in sizes:
        frames.append(data[at:at + size])
        at += size

    return frames


# =============================================================================
# Driving
# =============================================================================

def text_of(server, address: str) -> str:
    return media_edit.text_of(server, address)


def render_row(server, cue: str) -> "list[str]":
    for line in (text_of(server, "/godot/engine/editRender") or "").split("\n"):
        fields = line.split("\t")

        if fields and fields[0] == cue:
            return fields

    return []


def wait_rendered(server, cue: str, timeout: float = 90.0) -> "list[str]":
    row = common.wait_until(lambda: (lambda r: r if len(r) >= 2 and r[1] == "done" else None)(render_row(server, cue)),
                            timeout=timeout)
    return row if row is not None else render_row(server, cue)


def send(server, address: str, args) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args))


def number_of(server, address: str) -> float:
    """A number off the tree; nought is a number too."""
    value = common.http_json(server.http_port, address).get("VALUE", [])

    try:
        return float(value[0]) if value else float("nan")
    except (TypeError, ValueError):
        return float("nan")


def section_bounds(server, ids: "list[str]") -> "list[tuple[float, float]]":
    return [(number_of(server, f"/godot/section/{i}/in"), number_of(server, f"/godot/section/{i}/out")) for i in ids]


def run(locale: str) -> int:
    report = common.Report(f"a movie edited ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-movie-edit-") as scratch:
        room = Path(scratch)
        bundle = room / "Edit"
        media = bundle / "media"
        media.mkdir(parents=True)
        (bundle / "Edit.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
        (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
        source_frames = write_three_colours(media / "three.mov")
        media_edit.write_ramp(media / "ramp.wav")

        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        movie = f"/godot/cue/{MOVIE}/"
        sound = f"/godot/cue/{SOUND}/"

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            report.equal(first_sound.wait_for(server, movie + "duration", 3.0), 3.0,
                         "the movie's length is read when the show opens")
            report.equal(first_sound.wait_for(server, movie + "frameRate", float(FPS), timeout=60), float(FPS),
                         "and its frame rate once the analyser has read it")
            report.equal(first_sound.value_of(server, movie + "codec"), "Hap1", "which says it is HAP")

            # --- 1. The sound's dip, two cuts, the shots reordered: the sound in step ----
            send(server, "/godot/cmd/node/set", [sound + "levelLane", "2 -10 2.5 -10"])
            report.equal(media_edit.wait_for_numbers(server, sound + "levelLane", [2.0, -10.0, 2.5, -10.0]),
                         [2.0, -10.0, 2.5, -10.0], "a dip is drawn on the sound over the movie's third second")

            send(server, "/godot/cmd/section/split", [MOVIE, 1.02])   # on the grid: one second
            send(server, "/godot/cmd/section/split", [MOVIE, 2.0])
            sections = common.wait_until(lambda: (lambda s: s if len(s) == 3 else None)(text_of(server, movie + "sections").split()),
                                         timeout=20)
            report.check(sections is not None, "two splits make three sections on the movie", text_of(server, movie + "sections"))

            if sections is None:
                return report.finish()

            report.equal(section_bounds(server, sections), [(0.0, 1.0), (1.0, 2.0), (2.0, 3.0)],
                         "cut on the nearest frames: at one and two seconds")

            copies = common.wait_until(lambda: (lambda s: s if len(s) == 3 else None)(text_of(server, sound + "sections").split()),
                                       timeout=20)
            report.check(copies is not None, "the sound's sections are the movie's, copied", text_of(server, sound + "sections"))

            if copies is not None:
                report.equal(section_bounds(server, copies), [(0.0, 1.0), (1.0, 2.0), (2.0, 3.0)], "at the same seconds")
                report.check(all(c not in sections for c in copies), "under identifiers of their own")

            # Blue, green, red: the third shot first, then the second between.
            send(server, "/godot/cmd/section/move", [sections[2], 0])
            order = " ".join([sections[2], sections[0], sections[1]])
            report.equal(first_sound.wait_for(server, movie + "sections", order), order, "the blue shot moved first")
            send(server, "/godot/cmd/section/move", [sections[1], 1])
            order = " ".join([sections[2], sections[1], sections[0]])
            report.equal(first_sound.wait_for(server, movie + "sections", order), order, "then the green between")

            if copies is not None:
                # The copies are matched by position: each keeps its place and takes the movie's seconds there.
                followed = common.wait_until(lambda: (lambda b: b if b == [(2.0, 3.0), (1.0, 2.0), (0.0, 1.0)] else None)
                                             (section_bounds(server, copies)), timeout=20)
                report.equal(followed, [(2.0, 3.0), (1.0, 2.0), (0.0, 1.0)],
                             "and the sound's copies follow, each taking the movie's seconds at its place",
                             repr(section_bounds(server, copies)))

            report.equal(first_sound.value_of(server, movie + "duration"), 3.0, "the movie is still three seconds long")
            report.equal(media_edit.wait_for_numbers(server, sound + "levelLane", [0.0, -10.0, 0.5, -10.0]),
                         [0.0, -10.0, 0.5, -10.0], "and the sound's dip is on the first half second, over the same sound")

            # --- 2. A dissolve at the join into the green shot, and the renders ----------
            send(server, "/godot/cmd/node/set", [f"/godot/section/{sections[1]}/crossfade", "0.4"])
            report.equal(first_sound.wait_for(server, f"/godot/section/{sections[1]}/crossfade", 0.4), 0.4,
                         "a dissolve of four frames at the join into the green shot")

            if copies is not None:
                report.equal(first_sound.wait_for(server, f"/godot/section/{copies[1]}/crossfade", 0.4), 0.4,
                             "which the sound's copy carries too")

            edits = media / ".edits"
            settled = common.wait_until(lambda: (lambda m, s, movs: (m, s) if len(m) >= 2 and m[1] == "done"
                                                                     and len(s) >= 2 and s[1] == "done" and len(movs) == 1 else None)
                                        (render_row(server, MOVIE), render_row(server, SOUND),
                                         sorted(edits.glob("*.mov")) if edits.is_dir() else []),
                                        timeout=120)
            report.check(settled is not None, "both renders say done and one movie render stands under media/.edits",
                         repr((render_row(server, MOVIE), render_row(server, SOUND),
                               [e.name for e in edits.glob("*")] if edits.is_dir() else [])))

            if settled is not None:
                report.equal(settled[0][4] if len(settled[0]) > 4 else "", "movie", "the readout says the movie's kind")
                report.equal(settled[1][4] if len(settled[1]) > 4 else "", "sound", "and the sound's")

            movs = sorted(edits.glob("*.mov")) if edits.is_dir() else []
            wavs = sorted(edits.glob("*.wav")) if edits.is_dir() else []
            first_write = movs[0].stat().st_mtime_ns if movs else 0

            if movs:
                frames = frames_of(movs[0])
                report.equal(len(frames), 3 * FPS, "the movie's render is thirty frames on the file's grid")

                if len(frames) == 3 * FPS:
                    # Blue 0..9 from 20..29, green 10..19 from 10..19, red 20..29 from 0..9; frames 8 to 11 dissolve.
                    expected = {k: (20 + k if k < 10 else k if k < 20 else k - 20) for k in range(30)}
                    plain = [k for k in range(30) if k < 8 or k > 11]
                    report.check(all(frames[k] == source_frames[expected[k]] for k in plain),
                                 "every frame outside the dissolve is the source's own bytes, in the new order")
                    report.check(all(frames[k] != source_frames[expected[k]] for k in range(8, 12)),
                                 "and the four frames inside it are a blend of their own")

            if wavs:
                channels, samples = first_sound.read_render(wavs[0])
                report.equal(channels, 1, "the sound's render has the file's channels")
                report.equal(len(samples[0]), RATE * 3, "and the movie's length, its sections being the movie's")

                if len(samples[0]) == RATE * 3:
                    report.check(abs(samples[0][int(RATE * 0.5)] - media_edit.ramp_at(2.5)) < 0.01,
                                 "its first half second is the ramp's third second", str(samples[0][int(RATE * 0.5)]))

            # --- 3. Freeze: the pair, one record; a GO plays both -------------------
            send(server, "/godot/cmd/media/freeze", [MOVIE])
            report.equal(first_sound.wait_for(server, movie + "file", "three (edit).mov", timeout=90), "three (edit).mov",
                         "media.freeze on the movie: it plays its bounce")
            report.equal(first_sound.value_of(server, movie + "editSource"), "three.mov", "and keeps the file it was made from")
            report.equal(first_sound.wait_for(server, sound + "file", "ramp (edit).wav"), "ramp (edit).wav",
                         "its sound plays its own bounce, in the same record")
            report.equal(first_sound.value_of(server, sound + "editSource"), "ramp.wav", "and keeps its source too")
            report.check((media / "three (edit).mov").is_file() and (media / "ramp (edit).wav").is_file(),
                         "both bounces are beside their sources")
            report.equal(len(frames_of(media / "three (edit).mov")), 3 * FPS, "the movie's bounce is the render, frame for frame")

            send(server, "/godot/cmd/node/set", [f"/godot/list/{LIST}/standby", MOVIE])
            first_sound.wait_for(server, f"/godot/list/{LIST}/standby", MOVIE)
            first_sound.go(server)
            movie_run = first_sound.run_for_cue(server, MOVIE)
            sound_run = first_sound.run_for_cue(server, SOUND)
            report.check(movie_run != "", "GO starts the frozen movie")
            report.check(sound_run != "", "and the sound locked to it")

            if movie_run:
                report.equal(first_sound.wait_for_run_state(server, movie_run, "playing"), "playing", "the movie plays")

            if sound_run:
                report.equal(first_sound.wait_for_run_state(server, sound_run, "playing"), "playing", "and so does the sound")

            send(server, "/godot/cmd/run/stopAll", [])

            for run_id in (movie_run, sound_run):
                if run_id:
                    first_sound.wait_for_run_state(server, run_id, "done")

            # --- 4. Unfreeze: both back, the renders found again -------------------
            send(server, "/godot/cmd/media/unfreeze", [MOVIE])
            report.equal(first_sound.wait_for(server, movie + "file", "three.mov"), "three.mov",
                         "media.unfreeze on the movie: it is on its file again")
            report.equal(first_sound.wait_for(server, sound + "file", "ramp.wav"), "ramp.wav", "and so is its sound")
            report.equal(first_sound.value_of(server, movie + "editSource"), "", "with nothing frozen")

            again = wait_rendered(server, MOVIE)
            report.check(len(again) >= 2 and again[1] == "done", "the movie's render is there again", repr(again))
            movs_again = sorted(edits.glob("*.mov")) if edits.is_dir() else []
            report.equal(len(movs_again), 1, "the same render, not a second one")

            if movs and movs_again:
                report.equal(movs_again[0].stat().st_mtime_ns, first_write, "found on disk, not made again")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""

        for record in (" section.split", " section.move", " media.freeze", " media.frozen", " media.unfreeze"):
            report.check(record in text, f"the log holds{record}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` reproduces the session record for record", (out + err).strip()[-2000:])

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
