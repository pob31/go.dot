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
"""A sound edited inside Go.dot: sections, a move carrying the lane, the render, Freeze and Unfreeze
(namespace draft section 55).

The author, 2026-10-10: "split, move parts around, adjust crossfades between sections and adjust the
trim of each section for audio ... Once the edit is confirmed, it can be frozen and bounced to a new
file ... defreeze, recover the edits and make changes ... lock the automation curves to the media".

Driven through the shipped serve, hosted on the dummy device, with nothing but named commands over
OSC and the tree:

 1. A four-second ramp - every sample says where in the file it came from - split at one and three
    seconds, its lane drawn over the second between two and two and a half. The third section moved
    first: the cue is still four seconds long, and the lane's points are a second later, over the
    same sound.
 2. The render under media/.edits says `done`, and its samples are the ramp's, in the new order.
 3. `media.freeze`: the cue's file becomes "ramp (edit).wav" beside the source, `editSource` keeps
    "ramp.wav", and a GO plays it.
 4. `media.unfreeze`: the file is the source again, the render is found rather than made again.
 5. The handles' verbs (namespace draft 55.9): a selection deleted leaving silence, everything after
    where it was; the section after the silence fading in from it, alone, then its curve bent; the
    silence closed up with ripple, the dip a half second earlier with its sound. Each heard so in the
    render, sample by sample.
 6. The session's log holds every record and replays record for record.
"""
import struct
import sys
import tempfile
import wave
from pathlib import Path

import common
import first_sound
from common import Server

RATE = first_sound.RATE
BLOCK = first_sound.BLOCK
LIST = "7K2QM9X4"
CUE = "ED000001"
SECONDS = 4

SHOW = ('<Show><Lists goDebounce="0"><List id="7K2QM9X4" name="Edit">'
        '<Media id="ED000001" file="ramp.wav" name="Ramp" number="1" channels="1">'
        '<Route id="ED000002" bus="J3MT5XYA" gains="1 1"/></Media></List></Lists><Mounts/>'
        '<Audio tracks="2"><Bus id="J3MT5XYA" width="2"/></Audio></Show>')


def write_ramp(path: Path, seconds: int = SECONDS) -> None:
    """A mono 24-bit file whose sample n is 0.8 * n / total: the oracle for where a sample came from."""
    path.parent.mkdir(parents=True, exist_ok=True)
    total = RATE * seconds
    out = bytearray()

    for n in range(total):
        value = int(0.8 * n / total * 8388607)
        out += struct.pack("<i", value)[:3]

    with wave.open(str(path), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(3)
        file.setframerate(RATE)
        file.writeframes(bytes(out))


def ramp_at(seconds: float) -> float:
    return 0.8 * seconds / SECONDS


def text_of(server, address: str) -> str:
    value = common.http_json(server.http_port, address).get("VALUE", [""])
    return value[0] if value else ""


def numbers_of(server, address: str) -> "list[float]":
    """A list row, as the numbers the tree serves it as."""
    value = common.http_json(server.http_port, address).get("VALUE", [])
    try:
        return [float(v) for v in value]
    except (TypeError, ValueError):
        return []


def wait_for_numbers(server, address: str, expected: "list[float]", timeout: float = 20.0) -> "list[float]":
    found = common.wait_until(lambda: (lambda got: got if got == expected else None)(numbers_of(server, address)),
                              timeout=timeout)
    return found if found is not None else numbers_of(server, address)


def render_row(server) -> "list[str]":
    for line in (text_of(server, "/godot/engine/editRender") or "").split("\n"):
        fields = line.split("\t")
        if fields and fields[0] == CUE:
            return fields
    return []


def send(server, address: str, args) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args))


def run(locale: str) -> int:
    report = common.Report(f"a sound edited ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-edit-") as scratch:
        room = Path(scratch)
        bundle = room / "Edit"
        media = bundle / "media"
        media.mkdir(parents=True)
        (bundle / "Edit.wfg").write_text('<Bundle formatVersion="1"/>', encoding="utf-8")
        (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
        write_ramp(media / "ramp.wav")

        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        cue = f"/godot/cue/{CUE}/"

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render) as server:
            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            report.equal(first_sound.wait_for(server, cue + "duration", float(SECONDS)), float(SECONDS),
                         "the ramp's length is read when the show opens")

            # --- 1. Two cuts, a lane, the chorus first -----------------------------
            send(server, "/godot/cmd/node/set", [cue + "levelLane", "2 -10 2.5 -10"])
            report.equal(wait_for_numbers(server, cue + "levelLane", [2.0, -10.0, 2.5, -10.0]), [2.0, -10.0, 2.5, -10.0],
                         "a dip is drawn over the second between two and two and a half")

            send(server, "/godot/cmd/section/split", [CUE, 1.0])
            send(server, "/godot/cmd/section/split", [CUE, 3.0])
            sections = common.wait_until(lambda: (lambda s: s if len(s) == 3 else None)(text_of(server, cue + "sections").split()),
                                         timeout=20)
            report.check(sections is not None, "two splits make three sections", text_of(server, cue + "sections"))

            if sections is None:
                return report.finish()

            report.equal(float(text_of(server, f"/godot/section/{sections[1]}/in")), 1.0, "the second begins at one")
            report.equal(float(text_of(server, f"/godot/section/{sections[2]}/in")), 3.0, "the third at three")

            send(server, "/godot/cmd/section/move", [sections[2], 0])
            report.equal(first_sound.wait_for(server, cue + "sections", " ".join([sections[2], sections[0], sections[1]])),
                         " ".join([sections[2], sections[0], sections[1]]), "the third section moved first")
            report.equal(first_sound.value_of(server, cue + "duration"), float(SECONDS), "the cue is still four seconds long")
            report.equal(wait_for_numbers(server, cue + "levelLane", [3.0, -10.0, 3.5, -10.0]), [3.0, -10.0, 3.5, -10.0],
                         "and the dip is a second later, over the same sound")

            # --- 2. The render ---------------------------------------------------
            done = common.wait_until(lambda: (lambda row: row if len(row) >= 2 and row[1] == "done" else None)(render_row(server)),
                                     timeout=60)
            report.check(done is not None, "the edit is rendered and the readout says done", repr(render_row(server)))

            edits = sorted((media / ".edits").glob("*.wav")) if (media / ".edits").is_dir() else []
            report.equal(len(edits), 1, "one render under media/.edits", repr([e.name for e in edits]))

            if edits:
                channels, samples = first_sound.read_render(edits[0])
                report.equal(channels, 1, "the render has the file's channels")
                report.equal(len(samples[0]), RATE * SECONDS, "and the edit's length")

                if len(samples[0]) == RATE * SECONDS:
                    # Twenty milliseconds in: past the moved section's ten-millisecond fade from silence (55.9).
                    first = samples[0][int(RATE * 0.02)]
                    middle = samples[0][int(RATE * 1.5)]
                    later = samples[0][int(RATE * 2.5)]
                    report.check(abs(first - ramp_at(3.02)) < 0.01, "twenty milliseconds in it is the file at three seconds and as much",
                                 str(first))
                    report.check(abs(middle - ramp_at(0.5)) < 0.01, "at a second and a half, the file's at half a second", str(middle))
                    report.check(abs(later - ramp_at(1.5)) < 0.01, "at two and a half, the file's at a second and a half", str(later))

                first_write = edits[0].stat().st_mtime_ns

            # --- 3. Freeze, and a GO on the bounce --------------------------------
            send(server, "/godot/cmd/media/freeze", [CUE])
            report.equal(first_sound.wait_for(server, cue + "file", "ramp (edit).wav", timeout=60), "ramp (edit).wav",
                         "media.freeze: the cue plays the bounce")
            report.equal(first_sound.value_of(server, cue + "editSource"), "ramp.wav", "and keeps the file it was made from")
            report.check((media / "ramp (edit).wav").is_file(), "the bounce is beside its source")
            report.equal(first_sound.value_of(server, cue + "sections").split(), [sections[2], sections[0], sections[1]],
                         "the sections wait, as they were")

            send(server, "/godot/cmd/node/set", [f"/godot/list/{LIST}/standby", CUE])
            first_sound.wait_for(server, f"/godot/list/{LIST}/standby", CUE)
            first_sound.go(server)
            run_id = first_sound.run_for_cue(server, CUE)
            report.check(run_id != "", "GO starts the frozen cue")

            if run_id:
                report.equal(first_sound.wait_for_run_state(server, run_id, "playing"), "playing", "and it plays")

            send(server, "/godot/cmd/run/stopAll", [])

            if run_id:
                first_sound.wait_for_run_state(server, run_id, "done")

            # --- 4. Unfreeze: the render found again -----------------------------
            send(server, "/godot/cmd/media/unfreeze", [CUE])
            report.equal(first_sound.wait_for(server, cue + "file", "ramp.wav"), "ramp.wav",
                         "media.unfreeze: the cue is on its file again")
            report.equal(first_sound.value_of(server, cue + "editSource"), "", "with nothing frozen")

            again = common.wait_until(lambda: (lambda row: row if len(row) >= 2 and row[1] == "done" else None)(render_row(server)),
                                      timeout=60)
            report.check(again is not None, "the render is there again")
            edits_again = sorted((media / ".edits").glob("*.wav"))
            report.equal(len(edits_again), 1, "the same render, not a second one")

            if edits and edits_again:
                report.equal(edits_again[0].stat().st_mtime_ns, first_write, "found on disk, not made again")

            # --- 5. The handles' verbs, heard in the render (55.9) ------------
            def newest_render(frames: int, after: int) -> "Path | None":
                """The newest render under .edits that is `frames` long and written after `after`."""
                def found():
                    for path in sorted((media / ".edits").glob("*.wav"), key=lambda p: p.stat().st_mtime_ns, reverse=True):
                        try:
                            if path.stat().st_mtime_ns > after and first_sound.frames_on_disk(path) == frames:
                                return path
                        except OSError:
                            continue
                    return None

                ready = common.wait_until(lambda: found() if len(render_row(server)) >= 2 and render_row(server)[1] == "done" else None,
                                          timeout=60)
                return ready

            def sample_at(path: Path, seconds: float) -> float:
                _, samples = first_sound.read_render(path)
                return samples[0][int(RATE * seconds)]

            mark = max((p.stat().st_mtime_ns for p in (media / ".edits").glob("*.wav")), default=0)

            # A selection from 2.5 to 3 s deleted, leaving silence: the last section stays where it was.
            send(server, "/godot/cmd/section/deleteSpan", [CUE, 2.5, 3.0])
            pieces = common.wait_until(lambda: (lambda s: s if len(s) == 4 else None)(text_of(server, cue + "sections").split()),
                                       timeout=20)
            report.check(pieces is not None, "the selection's two ends cut, its middle taken out", text_of(server, cue + "sections"))

            if pieces is None:
                return report.finish()

            last = pieces[-1]
            report.equal(first_sound.wait_for(server, f"/godot/section/{last}/gap", 0.5), 0.5,
                         "half a second of silence before the last section")
            report.equal(first_sound.value_of(server, cue + "duration"), float(SECONDS), "the cue is still four seconds long")
            report.equal(wait_for_numbers(server, cue + "levelLane", [3.0, -10.0, 3.5, -10.0]), [3.0, -10.0, 3.5, -10.0],
                         "and the dip where it was, over the same sound")

            silent = newest_render(RATE * SECONDS, mark)
            report.check(silent is not None, "the render follows")

            if silent is not None:
                report.check(abs(sample_at(silent, 2.75)) < 1e-6, "the selection is silence in the render",
                             str(sample_at(silent, 2.75)))
                report.check(abs(sample_at(silent, 3.5) - ramp_at(2.5)) < 0.01, "and the last section where it was",
                             str(sample_at(silent, 3.5)))
                mark = silent.stat().st_mtime_ns

            # The last section fades in from the silence over 300 ms, alone (it has no partner beside a gap).
            send(server, "/godot/cmd/section/fade", [last, "in", 0.3, 1])
            # Sent as OSC's 32-bit float, so within a millionth.
            report.check(common.wait_until(lambda: abs((first_sound.value_of(server, f"/godot/section/{last}/fadeIn") or 0.0) - 0.3) < 1e-6,
                                           timeout=20) is not None,
                         "section.fade: a fade in from silence")
            faded = newest_render(RATE * SECONDS, mark)

            if faded is not None:
                report.check(abs(sample_at(faded, 3.15) - ramp_at(2.15) * 0.70710678) < 0.005,
                             "halfway through it, equal power: three decibels down", str(sample_at(faded, 3.15)))
                mark = faded.stat().st_mtime_ns

            # Its curve bent up: halfway, a decibel and a half down.
            send(server, "/godot/cmd/section/curve", [last, "in", 1.0])
            report.equal(first_sound.wait_for(server, f"/godot/section/{last}/fadeInCurve", 1.0), 1.0,
                         "section.curve: the fade's curve")
            bent = newest_render(RATE * SECONDS, mark)

            if bent is not None:
                report.check(abs(sample_at(bent, 3.15) - ramp_at(2.15) * 0.84089642) < 0.005,
                             "halfway through it, lifted by the curve", str(sample_at(bent, 3.15)))
                mark = bent.stat().st_mtime_ns

            # The silence closed up with ripple: everything after it half a second earlier, the dip with its sound.
            send(server, "/godot/cmd/section/deleteSpan", [CUE, 2.5, 3.0, 1])
            report.equal(first_sound.wait_for(server, f"/godot/section/{last}/gap", 0.0), 0.0,
                         "with ripple the silence closes")
            report.equal(first_sound.wait_for(server, cue + "duration", 3.5), 3.5, "the cue three and a half seconds long")
            report.equal(wait_for_numbers(server, cue + "levelLane", [2.5, -10.0, 3.0, -10.0]), [2.5, -10.0, 3.0, -10.0],
                         "and the dip half a second earlier, with its sound")
            closed = newest_render(RATE * 7 // 2, mark)
            report.check(closed is not None, "the render follows, three and a half seconds long")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""

        for record in (" section.split", " section.move", " media.freeze", " media.frozen", " media.unfreeze",
                       " section.deleteSpan", " section.fade", " section.curve"):
            report.check(record in text, f"the log holds{record}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        f"--wfg-locale={locale}")
        report.equal(code, 0, "`wfg replay` reproduces the session record for record", (out + err).strip()[-2000:])

    return report.finish()


if __name__ == "__main__":
    locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")
    sys.exit(run(locale))
