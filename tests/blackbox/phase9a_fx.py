#!/usr/bin/env python3
"""Phase 9a, PR 9a.8 - a cue's insert, heard: the test-gain child on a voice.

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
(§17.6, §17.9): a show that declares the test gain in its set is served hosted,
the child comes up and the entry reads `loaded`; an insert made on the cue by
`fx.create` switches the plugin in, and the render of a constant source is the
source at a half - the child's baseline; a value written to `p0` through the
door moves the render on the next blocks; the kill switch, `p1` at one, takes
the child down mid-show and the voice is silent from there - never dry, the
author's decision of 2026-09-26 (CU) - through the relaunch the same switch
takes down again, with the entry reading `failed` in words; the engine's own
code allocated nothing on the audio thread throughout; and the session replays
exactly on a machine with no child at all.

A CONSTANT SOURCE, not a tone, because what the gain does is multiply: the
mean of the absolute sample over a second is the level, whatever block the
hosted render happened to drop (see phase9a_eq.py for that finding).
"""

import struct
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import first_sound  # noqa: E402
from common import HarnessError, Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "fx"

RATE = 48000
BLOCK = 64

MEDIA = "FX000002"
PLUGIN = "FX000006"
SOURCE = 0.25
TOLERANCE = 0.03


def write_constant(path: Path, seconds: float = 20.0) -> None:
    frames = int(RATE * seconds)
    sample = int(SOURCE * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * (frames * 2), *([sample] * (frames * 2))))


def level(samples: "list[float]", start_frame: int, seconds: float = 1.0) -> float:
    """The MEDIAN of the blocks' mean levels over the window: what a window
    that should be silent is at. A level the plugin makes is read from the
    blocks it answered in time instead (`common.answered_level`), since a late
    block is silent and its neighbours fade (decision CV)."""
    part = samples[start_frame:start_frame + int(RATE * seconds)]
    if len(part) < BLOCK:
        return 0.0
    blocks = [sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK)]
    blocks.sort()
    return blocks[len(blocks) // 2]


def shares(samples: "list[float]", start_frame: int, seconds: float = 1.0) -> str:
    """WHAT THE WINDOW'S BLOCKS WERE AT, each put with the level it is nearest.

    For a failure's detail line, never for the verdict. This driver has failed
    on CI three different ways at one of its windows - the source dry, the gain
    without the state's Pad, a parameter's next value arriving early - and a
    median alone cannot say which: the share of each, and when the window's
    first and last blocks sit at which level, can.
    """
    part = samples[start_frame:start_frame + int(RATE * seconds)]
    if len(part) < BLOCK:
        return "no blocks in the window"
    levels = {"dry": SOURCE, "three quarters": SOURCE * 0.75, "half": SOURCE * 0.5,
              "padded": SOURCE * 0.5 * 0.25, "silent": 0.0}
    blocks = [sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK)]
    named = [min(levels, key=lambda word: abs(levels[word] - b)) for b in blocks]
    counted = ", ".join(f"{named.count(word) / len(named):.0%} {word}" for word in levels if named.count(word))
    return f"{counted}; first block {named[0]}, last block {named[-1]}"


def dry_blocks(samples: "list[float]", start_frame: int, end_frame: int, processed: float) -> int:
    """How many blocks between two frames sit nearer the dry source than the
    processed level - none, ever, since decision CU: a plugin that cannot
    play the cue leaves it silent."""
    part = samples[max(0, start_frame):max(0, end_frame)]
    blocks = [sum(abs(s) for s in part[i:i + BLOCK]) / BLOCK for i in range(0, len(part) - BLOCK + 1, BLOCK)]
    return sum(1 for b in blocks if b > (SOURCE + processed) / 2)


class Hand:
    def __init__(self, server: Server):
        import socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.port = server.osc_port

    def send(self, address: str, args: "list | None" = None) -> None:
        self.sock.sendto(common.osc_encode(address, args), (common.HOST, self.port))

    def close(self) -> None:
        self.sock.close()


def value_of(server: Server, address: str):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def wait_for(server: Server, address: str, expected, timeout: float = 20.0):
    seen = None

    def ready():
        nonlocal seen
        seen = value_of(server, address)
        return seen == expected

    common.wait_until(ready, timeout=timeout)
    return seen


def wait_for_frames(render: Path, frames: int, timeout: float = 30.0) -> bool:
    return common.wait_until(lambda: first_sound.frames_on_disk(render) >= frames, timeout=timeout)


def plugin_said(server: Server) -> str:
    """WHAT THE ENTRY SAID AT A MOMENT, for a failure's detail line only.

    A window at the source level is either dry or processed at gain 1.0, and
    the render cannot tell which (handoff 2026-09-26, finding 1). A child
    that falls eight blocks behind in a row is marked failed at once, with a
    sentence - so reading the entry when each window was being played says
    whether the blocks were late, and a window read at the wrong value with
    the entry `loaded` is something else.
    """
    state = value_of(server, f"/godot/plugin/{PLUGIN}/state")
    problem = value_of(server, f"/godot/plugin/{PLUGIN}/problem") or ""
    return f"the entry {state}" + (f" ({problem})" if problem else "")


def stopped_answering(log: Path) -> str:
    """How many times the session logged a child that stopped answering -
    `plugin.failed` carries the sentence, and a miss is never logged alone."""
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "no session log to read"
    return f"{text.count('stopped answering')} 'stopped answering' in the session log"


def run(locale: "str | None") -> int:
    report = Report(f"phase 9a: a cue's insert, heard ({locale or 'C'})")
    with tempfile.TemporaryDirectory(prefix="wfg-phase9a-fx-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "fx")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_constant(bundle / "media" / "tone.wav")

        moved_at = 0
        killed_at = 0
        down_at = 0
        fx_id = None
        at_go = before_kill = "not read"

        # A DEADLINE A CI RUNNER CAN MEET. This driver proves the path, not the
        # round trip: at the 250 us default a slow shared box misses on and off,
        # and the render averages between dry and processed - which both Linux
        # and macOS CI showed. What the round trip costs is M31's question.
        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render,
                    proxy_deadline_us=20000) as server:
            hand = Hand(server)
            try:
                report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                             "the audio side comes up running")
                report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                             "the test-gain child comes up and the entry reads loaded")
                report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/paramCount"), 2,
                             "with its two parameters counted")

                hand.send("/godot/cmd/fx/create", [MEDIA, PLUGIN])
                fx_id = None

                def made():
                    nonlocal fx_id
                    listed = value_of(server, f"/godot/cue/{MEDIA}/fx")
                    if listed:
                        fx_id = str(listed).split()[0]
                    return bool(fx_id)

                report.check(common.wait_until(made, timeout=10.0),
                             "fx.create puts the entry on the cue, and the cue lists it", str(fx_id))
                if fx_id is None:
                    raise HarnessError("no Fx was made")

                report.equal(value_of(server, f"/godot/fx/{fx_id}/p0"), 0.5,
                             "p0 reads the catalogue's default before anybody writes it")
                report.equal(value_of(server, f"/godot/fx/{fx_id}/t0"), "-6.0 dB",
                             "and t0 says it in the plugin's words")

                # THREE SECONDS FROM WHERE THE RENDER IS AT GO, not from its first
                # frame: on macOS CI the render had run two seconds before GO, the
                # p0 write landed one second into the sound, and the window meant
                # for the baseline read three quarters (e20beff).
                went_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/go")
                report.check(wait_for_frames(render, went_at + int(RATE * 3.0)),
                             "the render runs three seconds past GO")
                at_go = plugin_said(server)

                moved_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/node/set", [f"/godot/fx/{fx_id}/p0", 0.75])
                report.equal(wait_for(server, f"/godot/fx/{fx_id}/p0", 0.75), 0.75,
                             "a write to p0 lands on the cue")
                report.equal(value_of(server, f"/godot/fx/{fx_id}/values"), "0:0.75",
                             "and the cue's row says so")
                report.check(wait_for_frames(render, moved_at + int(RATE * 3.0)),
                             "and the render runs three seconds past it")
                before_kill = plugin_said(server)

                killed_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/node/set", [f"/godot/fx/{fx_id}/p1", 1.0])
                report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "failed", timeout=5.0), "failed",
                             "p1 at one kills the child, and the entry reads failed within a poll")
                problem = value_of(server, f"/godot/plugin/{PLUGIN}/problem") or ""
                report.check("silent until it is back" in problem,
                             "with a sentence that says the voice is silent until it is back", problem)

                # THROUGH THE RELAUNCH. The host brings a failed child back once, two
                # seconds on, and the lane still asks it to die: it dies at its first
                # block and stays down. The voice is silent from the kill to the end,
                # across both children (decision CU) - measured over all of it.
                down = common.wait_until(
                    lambda: "stays down" in (value_of(server, f"/godot/plugin/{PLUGIN}/problem") or ""), timeout=10.0)
                report.check(down, "relaunched once, it dies again and stays down, saying so",
                             value_of(server, f"/godot/plugin/{PLUGIN}/problem") or "")
                down_at = first_sound.frames_on_disk(render)
                report.check(wait_for_frames(render, down_at + int(RATE * 1.5)),
                             "and the render runs on past it")

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "Go.dot's own code allocated nothing on the audio thread")
                first_sound.wait_for_render_tail(render)
            finally:
                hand.close()

        channels, data = first_sound.read_render(render)
        report.check(channels >= 2, "the render is stereo", f"{channels} channels")
        left = data[0] if data else []
        start = first_sound.first_above(left, 0.01)
        report.check(start >= 0, "the cue was heard at all")

        if start >= 0 and len(left) > down_at + int(RATE * 1.0):
            # THE BASELINE'S WINDOW ENDS BEFORE THE p0 WRITE: `moved_at` is what
            # was on disk when it was sent, and the write lands later than that.
            half_from, half_to = start + int(RATE * 0.25), min(start + int(RATE * 2.0), moved_at)
            moved_from, moved_to = moved_at + int(RATE * 1.5), moved_at + int(RATE * 2.5)
            dead_from, dead_to = killed_at + int(RATE * 1.5), down_at + int(RATE * 1.0)
            dead = level(left, dead_from, (dead_to - dead_from) / RATE)
            starved = common.logged_before(log, "plugin.failed", '/p1"', containing="stopped answering")
            common.check_level(report, common.answered_level(left, half_from, half_to, BLOCK), SOURCE * 0.5,
                               TOLERANCE, starved, "while the insert is in at its baseline, the source plays at a half",
                               f"{shares(left, half_from, (half_to - half_from) / RATE)}; moved at frame {moved_at},"
                               f" sound from {start}; {at_go} after it; {stopped_answering(log)}")
            common.check_level(report, common.answered_level(left, moved_from, moved_to, BLOCK), SOURCE * 0.75,
                               TOLERANCE, starved, "p0 at three quarters moves the render to three quarters",
                               f"{shares(left, moved_from)}; {before_kill} before the kill; {stopped_answering(log)}")
            report.check(dead <= 0.002,
                         "and with the child dead the voice is silent, through its relaunch",
                         f"{dead:.4f} from frame {dead_from} to {dead_to};"
                         f" {shares(left, dead_from, (dead_to - dead_from) / RATE)}")
            report.check(dry_blocks(left, dead_from, dead_to, SOURCE * 0.75) == 0,
                         "never dry: not one block at the source",
                         f"{dry_blocks(left, dead_from, dead_to, SOURCE * 0.75)} blocks at the source")
        else:
            report.check(False, "the render is long enough to read every window",
                         f"{len(left)} frames, moved at {moved_at}, killed at {killed_at}, down at {down_at}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}",
                                        f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session with no child at all",
                     (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")
    return report.finish()


STATE_NAME = f"state/{PLUGIN}-0000000000000001.state"
PADDED = SOURCE * 0.5 * 0.25
PAD_TOLERANCE = 0.008


def run_state(locale: "str | None") -> int:
    """THE WHOLE STATE PER CUE (the author's decision of 2026-09-25), heard.

    The test gain's Pad is no parameter - only state - and a quarter of the
    gain. A state file with Pad on is put in the bundle, as the editing helper
    would write it; `fx.capture` names it on the cue; GO, and the cue is heard
    at a quarter of a half. Then Undo takes the capture back, the cue is fired
    again, and it is heard at a half: the voice went back to the preset's own
    state, because no state is a state. `wfg replay` reproduces the session,
    with no child and no files read."""
    report = Report(f"phase 9a: a cue's whole plugin state, heard ({locale or 'C'})")
    with tempfile.TemporaryDirectory(prefix="wfg-phase9a-state-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "fx")
        render = room / "out.wav"
        log = room / "session.wfglog"
        replayed = room / "replayed"
        (bundle / "media").mkdir(exist_ok=True)
        write_constant(bundle / "media" / "tone.wav", seconds=30.0)
        (bundle / "plugins" / "state").mkdir(parents=True, exist_ok=True)
        (bundle / "plugins" / STATE_NAME).write_text("gain=0.5\ndie=0\npad=1\n")

        padded_at = 0
        plain_at = 0
        load_ms = None
        at_padded = at_plain = "not read"

        with Server(bundle, log=log, locale=locale, sample_rate=RATE,
                    buffer_size=BLOCK, hosted=True, render=render,
                    proxy_deadline_us=20000) as server:
            hand = Hand(server)
            try:
                report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                             "the test-gain child comes up")

                hand.send("/godot/cmd/fx/create", [MEDIA, PLUGIN])
                fx_id = None

                def made():
                    nonlocal fx_id
                    listed = value_of(server, f"/godot/cue/{MEDIA}/fx")
                    if listed:
                        fx_id = str(listed).split()[0]
                    return bool(fx_id)

                report.check(common.wait_until(made, timeout=10.0), "fx.create switches the entry in", str(fx_id))
                if fx_id is None:
                    raise HarnessError("no Fx was made")

                hand.send("/godot/cmd/fx/capture", [fx_id, STATE_NAME, "0:0.5 1:0"])
                report.equal(wait_for(server, f"/godot/fx/{fx_id}/stateFile", STATE_NAME), STATE_NAME,
                             "fx.capture names the state file on the cue")
                report.equal(value_of(server, f"/godot/fx/{fx_id}/values"), "0:0.5 1:0",
                             "and every value beside it")

                went_at = first_sound.frames_on_disk(render)
                hand.send("/godot/cmd/go")
                report.check(wait_for_frames(render, went_at + int(RATE * 3.0)), "the render runs three seconds past GO")
                padded_at = first_sound.frames_on_disk(render)
                at_padded = plugin_said(server)

                load_ms = value_of(server, f"/godot/plugin/{PLUGIN}/stateLoadMs")
                report.check(isinstance(load_ms, (int, float)) and load_ms >= 0,
                             "the entry says how long the state took to load", str(load_ms))
                report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/stateProblem") or "", "",
                             "and that it loaded")

                hand.send("/godot/cmd/run/killAll")

                # THE KILLED RUN FINISHED BEFORE THE CUE IS FIRED AGAIN. On a
                # one-track show a firing inside ~50 ms of a kill plays silence
                # while reporting `playing` - found by this driver, with no
                # plugin at all, and not this driver's to prove.
                def quiet():
                    status, body = common.http_get(server.http_port, "/godot/run")
                    if status != 200:
                        return False
                    contents = common.json.loads(body).get("CONTENTS") or {}
                    for node in contents.values():
                        leaves = node.get("CONTENTS") or {}
                        state = ((leaves.get("state") or {}).get("VALUE") or [None])[0]
                        if state in ("armed", "playing", "stopping"):
                            return False
                    return True

                report.check(common.wait_until(quiet, timeout=10.0), "the kill finishes the run")
                hand.send("/godot/cmd/undo")
                report.equal(wait_for(server, f"/godot/fx/{fx_id}/stateFile", ""), "",
                             "Undo takes the capture back")
                hand.send("/godot/cmd/cue/fire", [MEDIA])
                fired_at = first_sound.frames_on_disk(render)
                report.check(wait_for_frames(render, fired_at + int(RATE * 3.0)),
                             "the render runs three seconds past the second firing")
                plain_at = fired_at
                at_plain = plugin_said(server)

                report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                             "Go.dot's own code allocated nothing on the audio thread")
                first_sound.wait_for_render_tail(render)
            finally:
                hand.close()

        channels, data = first_sound.read_render(render)
        left = data[0] if data else []
        start = first_sound.first_above(left, 0.005)
        report.check(start >= 0, "the cue was heard at all")

        # THE SECOND WINDOW IS THE RENDER'S LAST SECOND, not one measured from
        # the frames on disk when the cue was fired: the writer flushes in its
        # own time, so that count lags the sound and a window placed by it can
        # land in the gap between the kill and the second firing. The second
        # run plays a thirty-second file past the end of the session.
        if start >= 0 and len(left) > plain_at + int(RATE * 2.5):
            # The first run's window ends where the kill was sent, and what was on
            # disk then is earlier than where it landed.
            padded_from, padded_to = start + int(RATE * 0.5), min(start + int(RATE * 2.5), padded_at)
            plain_from, plain_to = len(left) - int(RATE * 1.5), len(left) - int(RATE * 0.5)
            starved = common.logged_before(log, "plugin.failed", containing="stopped answering")
            common.check_level(report, common.answered_level(left, padded_from, padded_to, BLOCK), PADDED,
                               PAD_TOLERANCE, starved, "with the state's Pad on, the cue is heard at a quarter of a half",
                               f"{shares(left, padded_from, (padded_to - padded_from) / RATE)}; the whole first run:"
                               f" {shares(left, start, 2.5)}; the state took {load_ms} ms; {at_padded} three seconds"
                               f" in; {stopped_answering(log)}")
            common.check_level(report, common.answered_level(left, plain_from, plain_to, BLOCK), SOURCE * 0.5,
                               TOLERANCE, starved, "and after Undo, fired again, at a half - the preset's own state, back",
                               f"{shares(left, plain_from)}; {at_plain} three seconds in; {stopped_answering(log)}")
        else:
            report.check(False, "the render is long enough to read both windows",
                         f"{len(left)} frames, padded at {padded_at}, plain at {plain_at}")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}")
        report.equal(code, 0, "and `wfg replay` reproduces the session, reading no state file",
                     (out + err).strip()[-400:])
        report.check("reproduced exactly" in out, "saying so in as many words")

        # AND `wfg validate` SAYS WHEN THE FILE IS GONE: a copy of the fixture
        # with a cue naming a state, first with the file and then without it.
        checked = common.copy_bundle(FIXTURE, room / "checked" / "fx")
        (checked / "media").mkdir(exist_ok=True)
        write_constant(checked / "media" / "tone.wav", seconds=1.0)
        show = checked / "show.xml"
        route = '<Route id="FX000003" bus="FX000004" gains="1 0 0 1"/>'
        show.write_text(show.read_text().replace(
            route, route + f'\n        <Fx id="FX000007" plugin="{PLUGIN}" stateFile="{STATE_NAME}"/>', 1))
        (checked / "plugins" / "state").mkdir(parents=True, exist_ok=True)
        (checked / "plugins" / STATE_NAME).write_text("gain=0.5\ndie=0\npad=1\n")

        code, out, err = common.run_wfg("validate", str(checked))
        report.equal(code, 0, "wfg validate finds the state a cue names in the bundle", (out + err).strip()[-400:])

        (checked / "plugins" / STATE_NAME).unlink()
        code, out, err = common.run_wfg("validate", str(checked))
        report.check(code != 0 and "which this bundle does not have" in err,
                     "and says, in words, when it is not there", (out + err).strip()[-400:])
    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None
    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
    try:
        first = run(locale)
        second = run_state(locale)
        return first or second
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
