#!/usr/bin/env python3
"""A plugin too slow for the buffer is played later, in a served show (namespace draft 149).

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

WHAT THIS PROVES, end to end and through nothing but the product's own doors:
two sounds in a timeline group, the same file, one to the left output plain and
one to the right through a plugin that takes a millisecond and a little over
every block of 64 - the test gain with a `workDelayUs` in its state - where a
lane has eight tenths of one. Until namespace draft 149 that cue was silent, a block at
a time, for as long as it played.

  - Now the plugin is moved later instead: its entry says how many samples, the
    right side is heard that long after the left, and every silent sample after
    it began is one it was moved by - no holes.
  - The session's log says so in a note, and the machine keeps what the plugin
    needed: a second session in the same engine folder starts it that late
    before anything has gone through it.

NO LIMIT IS FIXED HERE (`proxy_deadline_us`), which every other plugin driver
does: a limit fixed by hand has no queue, and this driver is about the queue.

A STEP, not a tone: a second of nothing and then a constant, so where each side
begins is one sample that can be counted.
"""

import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
import first_sound  # noqa: E402
import lineup  # noqa: E402
from common import HarnessError, Report, Server  # noqa: E402
from phase9a_fx import Hand, value_of, wait_for, wait_for_frames  # noqa: E402

RATE = lineup.RATE
BLOCK = lineup.BLOCK
GROUP = lineup.GROUP
PLAIN = lineup.PLAIN
LATE = lineup.LATE
PLUGIN = lineup.PLUGIN
STATE_NAME = lineup.STATE_NAME
LEVEL = lineup.LEVEL
QUIET_SECONDS = lineup.QUIET_SECONDS

# A millisecond and a little: more than the 0.8 ms a lane has of a 1.33 ms block, less than the block - and,
# with the quarter a measure is planned at, just more than one block, so what the machine learns starts it two
# buffers later with a block and a half to spare. At 0.9 ms the measure made one buffer of it, 0.4 ms to spare,
# and a loaded runner's second session was late once more (the NUC, the whole suite beside it).
WORK_US = 1070
MOST = 8 * BLOCK


def wait_at_least(server: Server, path: str, least: int, seconds: float = 15.0):
    """The node's value once it is a number of at least `least`, or what it last read."""
    until = time.monotonic() + seconds
    value = None

    while time.monotonic() < until:
        value = value_of(server, path)

        if isinstance(value, (int, float)) and value >= least:
            return value

        time.sleep(0.05)

    return value


def session(report: Report, room: Path, locale: "str | None", name: str, taught: bool) -> "dict[str, object]":
    """One session: the group fired once. What the render and the entry said."""
    bundle = common.copy_bundle(lineup.FIXTURE, room / name / "fx")
    render = room / name / "out.wav"
    log = room / name / "session.wfglog"
    said: "dict[str, object]" = {"log": log}

    (bundle / "show.xml").write_text(lineup.SHOW.format(group=GROUP, plain=PLAIN, late=LATE, plugin=PLUGIN,
                                                        state=STATE_NAME, line_up=""))
    (bundle / "state.xml").write_text(lineup.STATE.format(group=GROUP))
    (bundle / "media").mkdir(exist_ok=True)
    lineup.write_step(bundle / "media" / "step.wav", seconds=6.0)
    (bundle / "plugins" / "state").mkdir(parents=True, exist_ok=True)
    (bundle / "plugins" / STATE_NAME).write_text(f"gain=1\ndie=0\npad=0\nworkDelayUs={WORK_US}\n")

    with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK,
                hosted=True, render=render, engine_folder=room / "engine") as server:
        hand = Hand(server)
        try:
            report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                         f"{name}: the audio side comes up running")
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                         "the test-gain child comes up and the entry reads loaded")

            if taught:
                # WHAT THE FIRST SESSION TAUGHT THE MACHINE: later from the start, before a block has gone through it.
                before = wait_at_least(server, f"/godot/plugin/{PLUGIN}/latencySamples", BLOCK)
                report.check(isinstance(before, (int, float)) and before >= BLOCK,
                             "a session after it starts the plugin as late as it was found to need",
                             f"the entry's delay before anything played: {before}")
                said["before"] = before

            went_at = first_sound.frames_on_disk(render)
            said["went"] = went_at
            hand.send("/godot/cmd/go")
            first_sound.run_for_cue(server, GROUP)

            # Past where both sounds begin, and long enough after it for a spell of late blocks to be over and said.
            report.check(wait_for_frames(render, went_at + int(RATE * (QUIET_SECONDS + 4.0))),
                         "the render runs four seconds past where both sounds begin")

            said["latency"] = value_of(server, f"/godot/plugin/{PLUGIN}/latencySamples")
            said["lateBlocks"] = value_of(server, f"/godot/plugin/{PLUGIN}/lateBlocks")
            report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/state"), "loaded",
                         "the plugin was never failed for being slow",
                         value_of(server, f"/godot/plugin/{PLUGIN}/problem") or "")
            report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")
            first_sound.wait_for_render_tail(render)
        finally:
            hand.close()

    channels, data = first_sound.read_render(render)
    report.check(channels >= 2, "the render is stereo", f"{channels} channels")

    if channels >= 2:
        said["left"] = first_sound.first_above(data[0], LEVEL * 0.1)
        said["right"] = first_sound.first_above(data[1], LEVEL * 0.1)

        if said["right"] >= 0:
            # Three seconds from where the right side begins: how much of it is not the step.
            window = data[1][said["right"]:said["right"] + 3 * RATE]
            said["quiet"] = sum(1 for sample in window if abs(sample) < LEVEL * 0.5)

    return said


def told(said: "dict[str, object]", name: str) -> None:
    """WHAT THE SESSION SAID OF ITS PLUGIN, printed when a check on what was heard fails: a runner that kept the
    child from its processor and a lane that lost its place read the same in two frame numbers, and not in these."""
    log = said.get("log")
    text = log.read_text(encoding="utf-8", errors="replace") if isinstance(log, Path) and log.exists() else ""
    lines = [line for line in text.splitlines() if "plugin" in line]
    print(f"         {name}: GO near frame {said.get('went')}, {said.get('lateBlocks')} late block(s); "
          f"the log's {len(lines)} line(s) about a plugin:")

    for line in lines[:60]:
        print(f"           {line[:240]}")


def heard(report: Report, said: "dict[str, object]", name: str) -> None:
    before = len(report.failures)

    try:
        heard_checks(report, said, name)
    finally:
        if len(report.failures) > before:
            told(said, name)


def heard_checks(report: Report, said: "dict[str, object]", name: str) -> None:
    left, right, latency = said.get("left", -1), said.get("right", -1), said.get("latency")
    report.check(left >= 0 and right >= 0, f"{name}: both sounds are heard", f"left from {left}, right from {right}")

    if left < 0 or right < 0 or not isinstance(latency, (int, float)):
        report.check(False, f"{name}: the entry says how much later the plugin is played", f"it read {latency}")
        return

    behind = right - left
    report.check(BLOCK <= latency <= MOST and latency % BLOCK == 0,
                 f"{name}: the entry says the plugin is played whole buffers later, eight at most",
                 f"{latency} samples")
    report.check(BLOCK <= behind <= latency and behind % BLOCK == 0,
                 f"{name}: the side through the plugin is heard that long after the other, to the buffer",
                 f"left from frame {left}, right from frame {right}: {behind} samples; the entry says {latency}")

    # Moved again after it began - a machine that kept the child from its processor - is a buffer of silence a time,
    # a little more with the fades either side of it; not moved, nothing.
    moved_after = int(latency - behind) // BLOCK
    quiet = said.get("quiet", -1)
    report.check(0 <= quiet <= moved_after * 2 * BLOCK,
                 f"{name}: once it began it is heard whole, but for a buffer each time it was moved again",
                 f"{quiet} samples short of the step in three seconds, moved {moved_after} time(s) after it began")


def run(locale: "str | None") -> int:
    report = Report(f"a plugin too slow for the buffer is played later ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-later-") as scratch:
        room = Path(scratch)

        first = session(report, room, locale, "first", taught=False)
        heard(report, first, "found out")

        text = first["log"].read_text(encoding="utf-8", errors="replace") if first["log"].exists() else ""
        notes = [line for line in text.splitlines() if line.startswith("# note plugin ")]
        report.check(any("samples later from now" in line for line in notes),
                     "the session's log says in a note that the plugin was moved later",
                     notes[0] if notes else "no note about a plugin in the log")
        report.check(any("longest in the plugin" in line for line in notes),
                     "and where the time went: how long its thread kept a buffer waiting, how long the plugin took")

        kept = room / "engine" / "plugins" / "measures.txt"
        lines = kept.read_text(encoding="utf-8").splitlines() if kept.exists() else []
        mine = [line for line in lines if line.endswith(" godot:test-gain")]
        report.check(len(mine) == 1 and " show " in mine[0] and int(mine[0].split()[0]) >= WORK_US * 0.9,
                     "the machine keeps what the plugin needed, as a show found it",
                     mine[0] if mine else f"{kept} holds no line for the test gain")

        second = session(report, room, locale, "second", taught=True)
        heard(report, second, "known")

        # Found out, it was late at least once; known, it need never be - and on a machine that held its child up
        # meanwhile, less often at any rate.
        was, now = first.get("lateBlocks"), second.get("lateBlocks")
        report.check(isinstance(was, (int, float)) and was >= 1,
                     "found out: it took a late block to find out", f"late blocks in the first session: {was}")
        report.check(isinstance(now, (int, float)) and isinstance(was, (int, float)) and (now == 0 or now < was),
                     "known: started that late, it is not late again to find it out",
                     f"late blocks: {was} in the first session, {now} in the second")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None

    for arg in argv[1:]:
        if arg.startswith("--wfg-locale="):
            locale = arg.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
