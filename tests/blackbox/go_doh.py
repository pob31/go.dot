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

"""Doh! end to end (PRD §3.32; namespace draft §24.15): the GO pressed too
soon, taken back, and the GO the operator meant, asked of the SHIPPED BINARY.

WHAT THIS IS FOR. The unit suite drives every road of `go.doh` against a fake
audio side and a fake wire. This drives `wfg serve` with a real Tracktion graph,
two devices on the other end of real sockets and nothing but named commands over
OSC - the way a desk, a footswitch or the Doh! button reach the engine - and
reads back what was HEARD (the render), what each device RECEIVED (the mock's
own account, message by message) and what the engine SAYS (its readouts and its
log). Then it replays the session.

THE SHOW is tests/fixtures/bundles/doh: a bed; a ramp on a bus of its own; a
timeline scene whose sound is the same ramp on another bus, whose cues write a
console that takes back (`Mount/@doh` "takeBack") and fire a lighting desk left
to its operator (the default, "Meh") - one cue at once and one three seconds
in - and whose last cue stops the bed; and a last ramp. Five buses' worth of
evidence, one per channel pair, so the render says which cue is which.

THE RAMP IS THE CLOCK. `ramp.wav` rises from 0.1 to 0.9 over eight seconds, so a
sample IS a position - (value - 0.1) / 0.1 seconds into the file - and it never
starts from silence: a launch is a loud edge, a pause is a gain falling under a
line that keeps rising, and where a resume landed is a reading off the file
rather than a claim about a clock.

THE SESSION, in the order an evening might have it:

 1. A hand puts the console's fader at 0.2 (the value from before any GO).
 2. GO: the bed. GO, too soon: the ramp. Doh!: the ramp paused over the panic
    fade, the pointer back on it, the resume published. GO: the ramp carries on
    from where it was, over the 0.1 s de-click (D2).
 3. GO, too soon: the scene - the console to 0.8, Q12 to the lights, the bed
    stopped. Doh! part-way, before Q13: the console back to 0.2, the lights sent
    NOTHING, the bed made again fading in, the report naming the lights "left to
    its operator" (D3, D4). GO: the scene re-seated where it was - the console
    sent 0.8 again (it takes back), the lights NOT sent Q12 again, Q13 at its own
    time on the corrected GO's clock, the bed stopped again.
 4. GO: the last ramp. Doh!, Doh! again: the second press forgets the resume,
    and the next GO starts it from its top. Past the window, Doh! is refused
    too-late.
 5. The session's log replays record for record.

TIMING. What a stall cannot move is judged everywhere. A check that compares
where something was heard with when the engine did it goes through
`lane_level.timed`, voided on CI in words (the author, 2026-09-28). Everything
else compares engine-side facts: the log, the readouts, what the devices got and
the render's own frames.

`--keep-log=<path>` copies the session's log out: that is how
tests/fixtures/logs/doh.wfglog was recorded.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import array
import json
import shutil
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.request
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "doh"

RATE = 48000
BLOCK = 128
TICK = RATE // 50

LIST = "DHSHW001"
BED = "DHBED001"
RAMP = "DHRMP001"
SCENE = "DHSCN001"
SOUND = "DHSND001"
DESK_CUE = "DHDSK001"
Q12 = "DHX12001"
Q13 = "DHX13001"
LAST = "DHEND001"

FADER = "/desk/fader"
LIGHTS_GO = "/lx/go"

# The channels the buses start on (the show's Audio element).
BED_CHANNEL = 0
RAMP_CHANNEL = 2
SCENE_CHANNEL = 4
LAST_CHANNEL = 6

# The media, written by the driver into the copy's media/.
BED_LEVEL = 0.25
BED_SECONDS = 40.0
RAMP_FROM = 0.1
RAMP_SLOPE = 0.1                  # per second of the file
RAMP_SECONDS = 8.0

DOH_WINDOW_TICKS = 5 * 50         # the show's list/dohWindow
PANIC_FADE = 1.0                  # audio/panicFade, the default
Q13_AT_TICKS = 3 * 50             # Q13's offset in the timeline

POSITION_TOLERANCE = 0.1          # seconds of the file

#  A TICK THREAD BEHIND ITS AUDIO. Where a launch lands and how a de-click
#  ramps are placed by the tick: a tick processed late lands its launch late,
#  and ticks processed back to back to catch up squeeze a five-tick ramp. On a
#  laptop whose scheduler moves the tick thread to its slow cores a Debug build
#  falls seconds behind (2026-10-03: `engine/lateness` past eight seconds twenty
#  seconds in). A reading off by no more than the lateness measured around it
#  is then voided in words - never a lateness that cannot explain the miss, and
#  a reading on time always counts: a stall may excuse a miss, never hide a pass.
LAG_EXCUSES = 0.04                # two ticks


# =============================================================================
# The media
# =============================================================================

def write_constant(path: Path, level: float, seconds: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sample = int(level * 32767)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<hh", sample, sample) * int(RATE * seconds))


def write_ramp(path: Path) -> None:
    """0.1 rising to 0.9 over eight seconds: a sample is a second of the file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = int(RATE * RAMP_SECONDS)
    body = bytearray()

    for n in range(frames):
        sample = int((RAMP_FROM + RAMP_SLOPE * n / RATE) * 32767)
        body += struct.pack("<hh", sample, sample)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(body))


def second_of(value: float) -> float:
    return (abs(value) - RAMP_FROM) / RAMP_SLOPE


# =============================================================================
# The engine
# =============================================================================

def value_of(server: Server, address: str):
    return first_sound.value_of(server, address)


def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def tick_of(server: Server) -> int:
    now = value_of(server, "/godot/engine/tick")
    return now if isinstance(now, int) else -1


def wait_until_tick(server: Server, tick: int, seconds: float = 60.0) -> bool:
    return bool(common.wait_until(lambda: tick_of(server) >= tick, timeout=seconds))


def runs_of(server: Server, cue: str) -> "list[str]":
    return [run for run in first_sound.runs_in(server) if value_of(server, f"/godot/run/{run}/cue") == cue]


def fresh_run(server: Server, cue: str, known: "set[str]", seconds: float = 20.0) -> str:
    found = common.wait_until(lambda: next((run for run in runs_of(server, cue) if run not in known), None),
                              timeout=seconds)
    return found or ""


def resume_of(server: Server) -> "tuple[str, float]":
    """`list/resume` as "<cue> <seconds>" back into its halves - the number read,
    not compared as text: the suite runs under fr_FR too."""
    text = str(value_of(server, f"/godot/list/{LIST}/resume") or "")
    cue, _, seconds = text.partition(" ")

    try:
        return cue, float(seconds.replace(",", "."))
    except ValueError:
        return cue, -1.0


def report_of(server: Server) -> str:
    return str(value_of(server, "/godot/list/dohReport") or "")


def lateness_of(server: Server) -> float:
    """How far behind its audio the engine processed its last tick, in seconds."""
    late = value_of(server, "/godot/engine/lateness")
    return late / RATE if isinstance(late, int) else 0.0


def lag_until(server: Server, predicate, timeout: float = 20.0) -> "tuple[object, float]":
    """Waits for `predicate` as `common.wait_until` does, and says the worst
    lateness of the tick seen while it waited."""
    worst = [lateness_of(server)]

    def watched():
        worst.append(lateness_of(server))
        return predicate()

    return common.wait_until(watched, timeout=timeout), max(worst)


def judged(report: Report, ok: bool, words: str, detail: str, lag: float, miss: float = 0.0) -> None:
    """A reading the tick places: judged, or - off CI, off by no more than the
    tick's own lateness around it - voided with the lateness said (LAG_EXCUSES)."""
    if ok:
        report.check(True, words, detail)
    elif lane_level.ON_CI:
        report.void(words, f"{detail}; {lane_level.CI_REASON}")
    elif lag > LAG_EXCUSES and miss <= lag + POSITION_TOLERANCE:
        report.void(words, f"{detail}; the tick thread ran {lag * 1000:.0f} ms behind the audio here "
                           f"(engine/lateness) - judged where the tick keeps time")
    else:
        report.check(False, words, detail + (f"; the tick ran {lag * 1000:.0f} ms late" if lag > 0 else ""))


# =============================================================================
# The devices
# =============================================================================

class Device(first_sound.MockTarget):
    """`mock_target.py`, asked what it holds and what it was sent."""

    def ask(self, path: str):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{self.query_port}{path}", timeout=4) as answer:
                if answer.status != 200:
                    return None
                return json.loads(answer.read().decode("utf-8"))["VALUE"]
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            return None

    def holds(self, address: str):
        got = self.ask(f"{address}?VALUE")
        return got[0] if got else None

    def messages(self, address: "str | None" = None) -> "list[tuple[str, list]]":
        got = self.ask("/_mock/messages") or []
        return [(message[0], message[1]) for message in got if address is None or message[0] == address]


def point_mounts_at(bundle: Path, desk: Device, lights: Device) -> None:
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('port="9000"', f'port="{desk.osc_port}"')
    text = text.replace('queryPort="5005"', f'queryPort="{desk.query_port}"')
    text = text.replace('port="9001"', f'port="{lights.osc_port}"')
    show.write_text(text, encoding="utf-8", newline="\n")


def faders(desk: Device) -> "list[float]":
    return [round(float(values[0]), 3) for _, values in desk.messages(FADER) if values]


def lights_sent(lights: Device) -> "list[int]":
    return [values[0] for _, values in lights.messages(LIGHTS_GO) if values]


# =============================================================================
# The log
# =============================================================================

def records(log: Path) -> "list[list[str]]":
    """Every record of the session's log, split on spaces: kind, tick, sequence,
    origin, [the reason, for a refusal,] the command and its arguments."""
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []

    return [line.split(" ") for line in lines if line[:2] in ("A ", "R ")]


def ticks_of(log: Path, command_name: str, argument: "str | None" = None) -> "list[int]":
    out = []

    for record in records(log):
        if record[0] == "A" and len(record) > 4 and record[4] == command_name \
                and (argument is None or f's:"{argument}"' in record[5:]):
            out.append(int(record[1]))

    return out


# =============================================================================
# The render
# =============================================================================

def read_channels(path: Path, wanted: "list[int]") -> "dict[int, array.array]":
    """The channels asked for, as float arrays - the render is 32-bit float,
    eight channels and a minute long, and a list of Python floats per sample of
    every channel would be half a gigabyte."""
    raw = path.read_bytes()

    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise HarnessError(f"{path} is not a RIFF/WAVE file")

    channels = bits = tag = 0
    data = memoryview(b"")
    at = 12

    while at + 8 <= len(raw):
        name = raw[at:at + 4]
        size = struct.unpack_from("<I", raw, at + 4)[0]

        if name == b"fmt ":
            tag, channels = struct.unpack_from("<HH", raw, at + 8)
            bits = struct.unpack_from("<H", raw, at + 22)[0]

            if tag == 0xFFFE and size >= 26:
                tag = struct.unpack_from("<H", raw, at + 32)[0]
        elif name == b"data":
            data = memoryview(raw)[at + 8:at + 8 + size]

        at += 8 + size + (size & 1)

    if tag != 3 or bits != 32 or channels == 0:
        raise HarnessError(f"{path} is format {tag} at {bits} bits; this reads 32-bit float")

    frames = len(data) // (4 * channels)
    floats = data[:frames * channels * 4].cast("f")
    return {c: array.array("f", floats[c::channels]) for c in wanted if c < channels}


def block_means(samples: "array.array", width: int = 480) -> "list[float]":
    return [sum(abs(v) for v in samples[i:i + width]) / width
            for i in range(0, len(samples) - width + 1, width)]


def loud_edges(samples: "array.array", floor: float = 0.03) -> "list[int]":
    """Every frame where a channel goes from silence to sound."""
    out = []
    quiet = True

    for n, value in enumerate(samples):
        if quiet and abs(value) > floor:
            out.append(n)
            quiet = False
        elif not quiet and abs(value) < 0.0005:
            quiet = True

    return out


def fade_after(samples: "array.array", start: int, width: int = 480) -> "tuple[int, int]":
    """Where a ramp that was playing begins to fall under its own line, and
    where it reaches silence: the Doh fade, read off the render. (-1, -1) when
    it does not fall. A block the recorder dropped is a hole that comes back up,
    and is passed over: a fade keeps falling."""
    means = block_means(samples[start:], width)

    for k in range(2, len(means) - 6):
        if means[k] < means[k - 1] and means[k + 1] < means[k] and means[k + 5] < means[k] * 0.9:
            end = next((j for j in range(k, len(means)) if means[j] < 0.002), -1)
            return start + (k - 1) * width, -1 if end < 0 else start + end * width

    return -1, -1


def first_sound_after_silence(samples: "array.array", start: int, quiet: int = RATE // 10) -> int:
    """The first frame that sounds after a tenth of a second of silence at or
    after `start` - a fade's tail is not silence - or -1."""
    still = 0

    for n in range(start, len(samples)):
        if abs(samples[n]) < 0.0002:
            still += 1
        elif still >= quiet:
            return n
        else:
            still = 0

    return -1


# =============================================================================
# The session
# =============================================================================

def run(locale: "str | None", keep_log: "Path | None", keep_render: "Path | None" = None) -> int:
    report = Report(f"Doh! end to end ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-doh-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "doh")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"

        write_constant(bundle / "media" / "bed.wav", BED_LEVEL, BED_SECONDS)
        write_ramp(bundle / "media" / "ramp.wav")

        facts: "dict[str, object]" = {}

        with Device("agree") as desk, Device("agree") as lights:
            point_mounts_at(bundle, desk, lights)

            with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK,
                        hosted=True, render=render) as server:
                report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                             "the audio side comes up running")
                report.equal(value_of(server, f"/godot/mount/DHMNT001/doh"), "takeBack",
                             "the console takes back (Undo(h))")
                report.equal(value_of(server, f"/godot/mount/DHMNT002/doh"), "leave",
                             "and the lighting desk is left to its operator - the default (Meh)")

                session(report, server, desk, lights, facts)

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "Go.dot's own code allocated nothing on the audio thread")

                first_sound.wait_for_render_tail(render)

        if keep_log is not None:
            keep_log.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(log, keep_log)
            print(f"  (the session's log is kept at {keep_log})")

        if keep_render is not None and render.is_file():
            shutil.copyfile(render, keep_render)

        logged(report, log, facts)
        heard(report, render, facts)

        # --- and the session replays ------------------------------------------
        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))
        report.equal(code, 0, "`wfg replay` reproduces the session record for record",
                     (out + err).strip()[-2000:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def session(report: Report, server: Server, desk: Device, lights: Device, facts: dict) -> None:
    # --- 1. the console before any GO ------------------------------------------
    command(server, "node.set", [FADER, 0.2])
    report.check(bool(common.wait_until(lambda: abs((desk.holds(FADER) or 0.0) - 0.2) < 0.001, timeout=15.0)),
                 "a hand sets the console's fader to 0.2 before any GO", f"it holds {desk.holds(FADER)!r}")

    # --- 2. the bed, then the ramp too soon --------------------------------------
    first_sound.go(server)
    bed = common.wait_until(lambda: next((r for r in runs_of(server, BED)
                                          if value_of(server, f"/godot/run/{r}/state") == "playing"), None))
    report.check(bool(bed), "GO: the bed plays")
    facts["bed"] = bed
    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/standby", RAMP), RAMP,
                 "and the pointer moves on to the ramp")
    wait_until_tick(server, tick_of(server) + 25)

    first_sound.go(server)
    ramp = common.wait_until(lambda: next((r for r in runs_of(server, RAMP)
                                           if value_of(server, f"/godot/run/{r}/state") == "playing"), None))
    report.check(bool(ramp), "GO, too soon: the ramp plays")
    offered = str(value_of(server, "/godot/list/doh") or "").split()
    report.check(len(offered) == 3 and offered[0] == LIST and offered[1] == RAMP,
                 "and /godot/list/doh offers to take that GO back", f"{offered}")
    early = int(offered[2]) if len(offered) == 3 else tick_of(server)

    wait_until_tick(server, early + 75)

    # --- the Doh: the ramp paused ---------------------------------------------
    facts["press lag"] = lateness_of(server)
    command(server, "go.doh")
    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/standby", RAMP), RAMP,
                 "Doh!: the pointer is back on the ramp")
    report.equal(first_sound.wait_for(server, "/godot/list/doh", ""), "",
                 "and nothing is left for a press to take back")
    resumed = common.wait_until(lambda: resume_of(server) if resume_of(server)[0] == RAMP
                                and resume_of(server)[1] > 0.0 else None, timeout=15.0)
    report.check(bool(resumed), "list/resume names the ramp and the second it carries on from",
                 f"{value_of(server, f'/godot/list/{LIST}/resume')!r}")
    press_second = resumed[1] if resumed else -1.0
    facts["ramp press"] = press_second
    report.check(1.0 <= press_second <= 2.5, "about a second and a half into the file - where the press found it",
                 f"{press_second:.3f} s")
    report.equal(first_sound.wait_for(server, "/godot/list/dohForget", f"{LIST} {RAMP}", 5.0), f"{LIST} {RAMP}",
                 "and a second press would forget it (list/dohForget)")

    if ramp:
        down, lag = lag_until(server, lambda: value_of(server, f"/godot/run/{ramp}/state") == "done")
        facts["fade lag"] = max(lag, float(facts["press lag"]))
        report.check(bool(down), "the ramp comes down over the panic fade and its run ends",
                     f"{value_of(server, f'/godot/run/{ramp}/state')!r}")

    wait_until_tick(server, tick_of(server) + 50)

    # --- the corrected GO: carried on -------------------------------------------
    known = set(runs_of(server, RAMP)) - set(r for r in runs_of(server, RAMP)
                                             if value_of(server, f"/godot/run/{r}/state") == "armed")
    lags = [lateness_of(server)]
    first_sound.go(server)

    def playing_again():
        lags.append(lateness_of(server))
        return next((r for r in runs_of(server, RAMP) if r not in known and
                     value_of(server, f"/godot/run/{r}/state") == "playing"), None)

    carried = common.wait_until(playing_again)
    facts["resume lag"] = max(lags)
    report.check(bool(carried), "the corrected GO: the ramp plays again")

    if carried:
        position = value_of(server, f"/godot/run/{carried}/position")
        #  Read as soon as it plays: up to a launch latency short of the point
        #  while the launch is still placed ahead (L3), and nowhere near its top.
        report.check(isinstance(position, (int, float)) and position >= press_second - 0.15,
                     "carried on from where it was, not from its top (run/position)",
                     f"position {position!r} against the press's {press_second:.3f} s")

    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/resume", ""), "",
                 "and the resume is spent")
    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/standby", SCENE), SCENE,
                 "the pointer walks on to the scene")
    wait_until_tick(server, tick_of(server) + 30)

    # --- 3. the scene too soon ---------------------------------------------------
    report.equal(lights_sent(lights), [], "the lighting desk has been sent nothing yet")
    first_sound.go(server)
    sound = common.wait_until(lambda: next((r for r in runs_of(server, SOUND)
                                            if value_of(server, f"/godot/run/{r}/state") == "playing"), None))
    report.check(bool(sound), "GO, too soon: the scene's sound plays")
    offered = str(value_of(server, "/godot/list/doh") or "").split()
    scene_go = int(offered[2]) if len(offered) == 3 else tick_of(server)
    facts["scene go"] = scene_go

    report.check(bool(common.wait_until(lambda: lights_sent(lights) == [12], timeout=10.0)),
                 "the lights get Q12 at once", f"{lights.messages()}")
    report.check(bool(common.wait_until(lambda: abs((desk.holds(FADER) or 0.0) - 0.8) < 0.001, timeout=10.0)),
                 "the console goes to 0.8", f"it holds {desk.holds(FADER)!r}")
    if bed:
        report.equal(first_sound.wait_for_run_state(server, bed, "done", 10.0), "done",
                     "and the scene's last cue stops the bed")

    wait_until_tick(server, scene_go + 75)
    report.equal(lights_sent(lights), [12], "a second and a half in, Q13 is not due yet")

    # --- the Doh, part-way ---------------------------------------------------------
    beds_before = set(runs_of(server, BED))
    command(server, "go.doh")
    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/standby", SCENE), SCENE,
                 "Doh!: the pointer is back on the scene")

    report.check(bool(common.wait_until(lambda: abs((desk.holds(FADER) or 0.0) - 0.2) < 0.001, timeout=10.0)),
                 "the console, which takes back, gets its 0.2 from before the GO",
                 f"it holds {desk.holds(FADER)!r}")

    said = common.wait_until(lambda: report_of(server) if "left to its operator" in report_of(server) else None,
                             timeout=10.0) or report_of(server)
    facts["report"] = said
    report.check(said.startswith(f"{LIST} "), "/godot/list/dohReport is the scene's list's", said)
    report.check("Lights: " in said and "left to its operator, not sent again" in said,
                 "and names the lighting desk: left to its operator, not sent again", said)
    report.check("Desk" not in said.split("left to its operator")[0],
                 "the console is not among what was left", said)

    relaunched = fresh_run(server, BED, beds_before)
    report.check(bool(relaunched), "the bed the GO stopped is made again")
    facts["bed again"] = relaunched

    if relaunched:
        report.equal(first_sound.wait_for_run_state(server, relaunched, "playing", 10.0), "playing",
                     "and plays")

    scene_resume = common.wait_until(lambda: resume_of(server) if resume_of(server)[0] == SCENE else None,
                                     timeout=10.0)
    report.check(bool(scene_resume) and 1.0 <= scene_resume[1] <= 2.5,
                 "list/resume names the scene and its own second", f"{value_of(server, f'/godot/list/{LIST}/resume')!r}")

    doh_tick = tick_of(server)
    sent_at_doh = list(lights.messages())
    _, facts["bed lag"] = lag_until(server, lambda: tick_of(server) >= doh_tick + 100, 60.0)
    report.equal(lights.messages(), sent_at_doh,
                 "THE LIGHTING DESK RECEIVES NOTHING AT THE DOH, nor in the two seconds after it")
    report.equal(faders(desk), [0.2, 0.8, 0.2], "and the console got exactly the put-back")

    # --- the corrected GO: re-seated -------------------------------------------------
    sounds_before = set(runs_of(server, SOUND))
    q13_before = set(runs_of(server, Q13))
    first_sound.go(server)

    seated = fresh_run(server, SOUND, sounds_before)
    report.check(bool(seated), "the corrected GO: the scene's sound plays again")
    if seated:
        report.equal(first_sound.wait_for_run_state(server, seated, "playing", 10.0), "playing", "and sounds")
        position = value_of(server, f"/godot/run/{seated}/position")
        report.check(isinstance(position, (int, float)) and position >= 1.0,
                     "carried on at its second, not from its top", f"position {position!r}")

    #  THE BED THE DOH MADE AGAIN GOES AGAIN: what the scene had fired - its
    #  stop among it - is fired again from the scene's start (L7). Over, or no
    #  longer published: either is a run that has ended.
    if relaunched:
        ended = common.wait_until(lambda: value_of(server, f"/godot/run/{relaunched}/state") in ("done", None),
                                  timeout=10.0)
        report.check(bool(ended), "the scene's stop fires again from its start: the bed goes again",
                     f"{value_of(server, f'/godot/run/{relaunched}/state')!r}")

    #  Q12 IS PLANNED OVER, not run again: a re-seated scene does not send
    #  again what it had sent to a device left to its operator (HJ, L7) - the
    #  lights' own account below says so message by message.
    q13 = fresh_run(server, Q13, q13_before, 10.0)
    facts["q13"] = q13

    report.check(bool(common.wait_until(lambda: abs((desk.holds(FADER) or 0.0) - 0.8) < 0.001, timeout=10.0)),
                 "the console is sent 0.8 again - it takes back, and the corrected GO is a first GO for it",
                 f"it holds {desk.holds(FADER)!r}")

    report.check(bool(common.wait_until(lambda: lights_sent(lights) == [12, 13], timeout=15.0)),
                 "Q13 reaches the lights on the corrected GO's clock", f"{lights.messages()}")
    wait_until_tick(server, tick_of(server) + 50)
    report.equal(lights_sent(lights), [12, 13],
                 "THE LIGHTING DESK GOT EACH CUE ONCE IN ALL: Q12 from the early GO, Q13 from the corrected one")
    report.equal(faders(desk), [0.2, 0.8, 0.2, 0.8], "the console: 0.2, 0.8, put back to 0.2, 0.8 again")

    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/standby", LAST), LAST,
                 "the pointer walks on to the last cue")

    # --- 4. a second press forgets --------------------------------------------------
    first_sound.go(server)
    last = common.wait_until(lambda: next((r for r in runs_of(server, LAST)
                                           if value_of(server, f"/godot/run/{r}/state") == "playing"), None))
    report.check(bool(last), "GO: the last ramp plays")
    wait_until_tick(server, tick_of(server) + 50)

    command(server, "go.doh")
    report.check(bool(common.wait_until(lambda: resume_of(server)[0] == LAST and resume_of(server)[1] > 0.0,
                                        timeout=10.0)),
                 "Doh!: the last ramp paused, its resume published", f"{value_of(server, f'/godot/list/{LIST}/resume')!r}")
    report.equal(first_sound.wait_for(server, "/godot/list/dohForget", f"{LIST} {LAST}"), f"{LIST} {LAST}",
                 "and offered to the second press")

    command(server, "go.doh")
    report.equal(first_sound.wait_for(server, f"/godot/list/{LIST}/resume", ""), "",
                 "Doh! again: the resume is forgotten")
    report.equal(first_sound.wait_for(server, "/godot/list/dohForget", ""), "", "and nothing is offered")
    report.equal(value_of(server, f"/godot/list/{LIST}/standby"), LAST, "the pointer stays on the last cue")
    wait_until_tick(server, tick_of(server) + 75)

    lasts_before = set(r for r in runs_of(server, LAST) if value_of(server, f"/godot/run/{r}/state") != "armed")
    first_sound.go(server)
    again = common.wait_until(lambda: next((r for r in runs_of(server, LAST) if r not in lasts_before and
                                            value_of(server, f"/godot/run/{r}/state") == "playing"), None))
    if report.check(bool(again), "the next GO plays the last ramp"):
        position = value_of(server, f"/godot/run/{again}/position")
        report.check(isinstance(position, (int, float)) and position < 0.5,
                     "from its top", f"position {position!r}")

    offered = str(value_of(server, "/godot/list/doh") or "").split()
    last_go = int(offered[2]) if len(offered) == 3 else tick_of(server)
    wait_until_tick(server, last_go + DOH_WINDOW_TICKS + 10)

    errors_before = value_of(server, "/godot/engine/errorCount")
    command(server, "go.doh")
    common.wait_until(lambda: value_of(server, "/godot/engine/errorCount") != errors_before, timeout=10.0)
    refusal = str(value_of(server, "/godot/engine/lastError") or "")
    report.check(" too-late " in f" {refusal} " and "go.doh" in refusal,
                 "past the window Doh! is refused too-late "
                 "(\"Doh! ignored: the last GO is too long ago to take back\" on the desktop)", refusal)
    report.equal(value_of(server, f"/godot/list/{LIST}/resume"), "", "and nothing is resumed")
    facts["last go"] = last_go


def logged(report: Report, log: Path, facts: dict) -> None:
    """What the log says the engine did - its own ticks, nothing of the wall clock."""
    applied = ticks_of(log, "go.doh")
    report.equal(len(applied), 4, "four Doh! presses applied: two take-backs, one more, and its forget",
                 f"at ticks {applied}")

    refused = [r for r in records(log) if r[0] == "R" and len(r) > 5 and r[4] == "too-late" and r[5] == "go.doh"]
    report.equal(len(refused), 1, "and one refused too-late, in the log")

    playheads = ticks_of(log, "go.dohPlayhead")
    report.check(len(playheads) >= 3, "each pause's playhead is a record (go.dohPlayhead)", f"{playheads}")

    reports = [r for r in records(log) if r[0] == "A" and len(r) > 4 and r[4] == "list.dohReport"]
    report.check(any("left" in " ".join(r) for r in reports),
                 "the report reaches the log as the engine's own list.dohReport record",
                 f"{len(reports)} records")

    q13 = facts.get("q13")
    scene_go = facts.get("scene go")
    gos = ticks_of(log, "go")

    if q13 and isinstance(scene_go, int) and len(applied) >= 2:
        doh = applied[1]
        corrected = next((t for t in gos if t > doh), None)
        fired = ticks_of(log, "run.fire", q13)

        if corrected is not None and fired:
            due = corrected + Q13_AT_TICKS - (doh - scene_go)
            lane_level.timed(report, abs(fired[0] - due) <= 10,
                             "Q13 fires on the corrected GO's clock - three seconds into the scene, less the "
                             "second and a half already played",
                             f"fired at {fired[0]}, due about {due} (GO {scene_go}, Doh {doh}, GO {corrected})")
        else:
            report.check(False, "Q13's wait is in the log", f"corrected GO {corrected}, run.fire {fired}")


def heard(report: Report, render: Path, facts: dict) -> None:
    """What came out, read off the render's own frames."""
    if not report.check(render.is_file(), "the render was written"):
        return

    channels = read_channels(render, [BED_CHANNEL, RAMP_CHANNEL, SCENE_CHANNEL, LAST_CHANNEL])

    if not report.check(len(channels) == 4, "the render carries the show's four buses", f"{sorted(channels)}"):
        return

    # --- the ramp: paused over the panic fade, carried on over the de-click ---------
    ramp = channels[RAMP_CHANNEL]
    edges = loud_edges(ramp)

    if report.check(len(edges) >= 2, "the ramp is heard twice: the early GO and the corrected one", f"{edges}"):
        first, second = edges[0], edges[1]
        report.check(abs(second_of(ramp[first + RATE // 100])) < 0.1,
                     "the early GO plays it from its top", f"{second_of(ramp[first + RATE // 100]):.3f} s")

        begin, end = fade_after(ramp, first + RATE // 2)

        if report.check(0 <= begin < second and 0 <= end <= second, "Doh! brings it down before it is heard again",
                        f"fade {begin}..{end}, again at {second}"):
            #  The panic fade is moved at the tick: judged where the tick kept time.
            fade_lag = float(facts.get("fade lag", 0.0))
            length = (end - begin) / RATE
            judged(report, 0.3 <= length <= PANIC_FADE + 0.3,
                   "over the panic fade - faded, never cut", f"{length:.3f} s", fade_lag)
            report.check(length > 0.02, "and never in one block", f"{length:.3f} s")

            middle = begin + int(0.25 * RATE)
            line = ramp[begin] + RAMP_SLOPE * (middle - begin) / RATE
            share = abs(ramp[middle]) / line if line > 0 else 0.0
            judged(report, 0.05 < share < 0.95, "a quarter of a second into the fade it is part-way down",
                   f"{share:.2f} of the line", fade_lag)

            press = facts.get("ramp press", -1.0)
            miss = abs(second_of(ramp[begin]) - press)
            judged(report, miss <= 0.15, "the fade begins where the engine says the press found the file",
                   f"heard falling at {second_of(ramp[begin]):.3f} s, the press at {press:.3f} s",
                   float(facts.get("press lag", 0.0)), miss)

        #  THE ARRIVAL, from its first sound to the line: K8's de-click is five
        #  ticks from silence, so the ramp climbs to its own line in about a
        #  tenth of a second and then rises only as the file does. Where it
        #  reached the line, less how long it took, is where it landed.
        start = first_sound_after_silence(ramp, max(0, end))
        full = next((n for n in range(start, len(ramp) - RATE // 5)
                     if abs(ramp[n]) >= 0.98 * (abs(ramp[n + RATE // 5]) - RAMP_SLOPE * 0.2)), -1)             if start >= 0 else -1

        if report.check(start >= 0 and full >= 0, "the corrected GO is heard arriving", f"{start}..{full}"):
            rise = (full - start) / RATE
            arrived = second_of(ramp[full]) - rise
            press = facts.get("ramp press", -1.0)
            lag = float(facts.get("resume lag", 0.0))
            miss = abs(arrived - press)
            judged(report, miss <= POSITION_TOLERANCE,
                   "THE CORRECTED GO CARRIES IT ON FROM WHERE IT WAS - the render reads the press's second",
                   f"arrived at {arrived:.3f} s of the file, the press at {press:.3f} s", lag, miss)
            judged(report, 0.05 <= rise <= 0.25,
                   "over the de-click: about a tenth of a second from silence to its line, never a cut",
                   f"{rise * 1000:.0f} ms", lag)
            report.check(rise > 0.002, "it never arrives at full level in one sample", f"{rise * 1000:.1f} ms")

    # --- the bed: stopped by the early GO, back fading in, stopped again -----------
    bed = channels[BED_CHANNEL]
    edges = loud_edges(bed, floor=0.01)

    if report.check(len(edges) >= 2, "the bed is heard twice: before the scene and when Doh! makes it again",
                    f"{edges}"):
        #  From its first sound to its level: the panic fade, moved at the tick (NG).
        start = first_sound_after_silence(bed, max(0, edges[1] - RATE))
        level = next((n for n in range(max(0, start), len(bed)) if abs(bed[n]) >= 0.98 * BED_LEVEL), -1)

        if report.check(start >= 0 and level > start, "the bed made again reaches its level",
                        f"{start}..{level}"):
            rise = (level - start) / RATE
            judged(report, 0.3 <= rise <= PANIC_FADE + 0.5,
                   "fading in over the panic fade, never at once", f"{rise:.3f} s",
                   float(facts.get("bed lag", 0.0)))
            report.check(rise > 0.02, "and never in one block", f"{rise:.3f} s")

    # --- the scene's sound: the same pause, re-seated --------------------------------
    scene = channels[SCENE_CHANNEL]
    edges = loud_edges(scene)

    if report.check(len(edges) >= 2, "the scene's sound is heard twice", f"{edges}"):
        settled = edges[1] + int(0.15 * RATE)
        report.check(second_of(scene[settled]) - 0.15 >= 1.0,
                     "and the second time from its second, not its top",
                     f"{second_of(scene[settled]) - 0.15:.3f} s of the file")

    # --- the last ramp: paused, forgotten, from its top --------------------------------
    last = channels[LAST_CHANNEL]
    edges = loud_edges(last)

    if report.check(len(edges) >= 2, "the last ramp is heard twice", f"{edges}"):
        top = edges[-1]
        at = top + int(0.02 * RATE)
        report.check(abs(second_of(last[at])) < 0.1,
                     "after the forget the next GO plays it from its top", f"{second_of(last[at]):.3f} s")
        report.check(abs(abs(last[at]) - (RAMP_FROM + RAMP_SLOPE * 0.02)) < 0.01,
                     "at its full level - nothing armed silent at the old point", f"{last[at]:.4f}")


def main(argv: "list[str]") -> int:
    locale = None
    keep_log = None
    keep_render = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
        elif argument.startswith("--keep-log="):
            keep_log = Path(argument.split("=", 1)[1])
        elif argument.startswith("--keep-render="):
            keep_render = Path(argument.split("=", 1)[1])

    try:
        common.find_binary()
        return run(locale, keep_log, keep_render)
    except HarnessError as error:
        print(f"doh: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
