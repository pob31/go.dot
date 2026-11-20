#!/usr/bin/env python3
"""A timeline group lines up for its plugins' delay, heard (namespace draft 140).

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
one to the right through a plugin that gives its sound back 480 samples late
and says so - the test gain with a look-ahead in its state. Fired with the
group's `lineUp` off, the right is heard 480 samples after the left, which is
what a plugin's delay has always done. Fired with it on, the two are heard on
the same sample: the plain one was held back by what the other's plugin
declared. And the group's run says what it held its members to.

A STEP, not a tone: a second of nothing and then a constant, so where each
side begins is one sample that can be counted.
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
from phase9a_fx import Hand, value_of, wait_for, wait_for_frames  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "fx"

RATE = 48000
BLOCK = 64

GROUP = "FX000010"
PLAIN = "FX000011"
LATE = "FX000013"
PLUGIN = "FX000006"
LOOK_AHEAD = 480
STATE_NAME = f"state/{PLUGIN}-0000000000000001.state"
LEVEL = 0.25
QUIET_SECONDS = 1.0

SHOW = """<Show>
  <Lists>
    <List id="FX000001" name="Sound">
      <Group id="{group}" name="Scene" number="1" mode="timeline"{line_up}>
        <Media id="{plain}" file="step.wav" name="Plain">
          <Route id="FX000012" bus="FX000004" gains="1 0 0 0"/>
        </Media>
        <Media id="{late}" file="step.wav" name="Through a look-ahead">
          <Route id="FX000014" bus="FX000004" gains="0 0 0 1"/>
          <Fx id="FX000015" plugin="{plugin}" stateFile="{state}"/>
        </Media>
      </Group>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="2">
    <Bus id="FX000004" name="Main L/R" width="2"/>
    <Plugins>
      <Plugin id="{plugin}" format="VST3" identifier="godot:test-gain" name="Test gain"/>
    </Plugins>
  </Audio>
  <MidiPorts/>
  <Network/>
  <Surfaces/>
  <Dcas/>
</Show>
"""

STATE = """<State formatVersion="1">
  <List id="FX000001" standby="{group}"/>
</State>
"""


def write_step(path: Path, seconds: float = 4.0) -> None:
    quiet = int(RATE * QUIET_SECONDS)
    loud = int(RATE * seconds) - quiet
    sample = int(LEVEL * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<" + "h" * (quiet * 2), *([0] * (quiet * 2))))
        out.writeframes(struct.pack("<" + "h" * (loud * 2), *([sample] * (loud * 2))))


def fire(report: Report, room: Path, locale: "str | None", line_up: bool) -> "tuple[int, int, object]":
    """One session: the group fired once. Where the left and the right begin in
    the render, and what the group's run said it held its members to."""
    word = "on" if line_up else "off"
    bundle = common.copy_bundle(FIXTURE, room / word / "fx")
    render = room / word / "out.wav"
    log = room / word / "session.wfglog"

    (bundle / "show.xml").write_text(SHOW.format(group=GROUP, plain=PLAIN, late=LATE, plugin=PLUGIN, state=STATE_NAME,
                                                 line_up=' lineUp="true"' if line_up else ""))
    (bundle / "state.xml").write_text(STATE.format(group=GROUP))
    (bundle / "media").mkdir(exist_ok=True)
    write_step(bundle / "media" / "step.wav")
    (bundle / "plugins" / "state").mkdir(parents=True, exist_ok=True)
    (bundle / "plugins" / STATE_NAME).write_text(f"gain=1\ndie=0\npad=0\nlookAhead={LOOK_AHEAD}\n")

    held = None

    # A DEADLINE A CI RUNNER CAN MEET, as phase9a_fx.py asks for one: this
    # driver is about where a sound is placed, not about the round trip.
    with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=BLOCK,
                hosted=True, render=render, proxy_deadline_us=20000) as server:
        hand = Hand(server)
        try:
            report.equal(wait_for(server, "/godot/audio/status", "running"), "running",
                         f"line-up {word}: the audio side comes up running")
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/state", "loaded"), "loaded",
                         "the test-gain child comes up and the entry reads loaded")
            report.equal(value_of(server, f"/godot/cue/{GROUP}/lineUp"), line_up,
                         "the group's lineUp reads as the show wrote it")

            # THE GROUP AT STANDBY ARMS ITS SOUNDS, the late one with its state:
            # the plugin says its look-ahead once that is loaded, and the entry
            # shows it - which is what the line-up reads when the group is fired.
            report.equal(wait_for(server, f"/godot/plugin/{PLUGIN}/latencySamples", LOOK_AHEAD), LOOK_AHEAD,
                         "armed at standby, the plugin declares its look-ahead and the entry shows it")

            went_at = first_sound.frames_on_disk(render)
            hand.send("/godot/cmd/go")

            run = first_sound.run_for_cue(server, GROUP)
            report.check(wait_for_frames(render, went_at + int(RATE * (QUIET_SECONDS + 1.5))),
                         "the render runs past where both sounds begin")
            held = value_of(server, f"/godot/run/{run}/lineUp")

            report.equal(value_of(server, f"/godot/plugin/{PLUGIN}/state"), "loaded",
                         "the plugin answered throughout", value_of(server, f"/godot/plugin/{PLUGIN}/problem") or "")
            report.equal(value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")
            first_sound.wait_for_render_tail(render)
        finally:
            hand.close()

    channels, data = first_sound.read_render(render)
    report.check(channels >= 2, "the render is stereo", f"{channels} channels")

    if channels < 2:
        return -1, -1, held

    return first_sound.first_above(data[0], LEVEL * 0.1), first_sound.first_above(data[1], LEVEL * 0.1), held


def run(locale: "str | None") -> int:
    report = Report(f"a timeline group lines up for its plugins' delay ({locale or 'C'})")
    with tempfile.TemporaryDirectory(prefix="wfg-lineup-") as scratch:
        room = Path(scratch)

        left, right, held = fire(report, room, locale, line_up=False)
        report.check(left >= 0 and right >= 0, "not asked: both sounds are heard", f"left from {left}, right from {right}")
        report.equal(right - left, LOOK_AHEAD,
                     "not asked: the one through the plugin is heard its look-ahead after the other",
                     f"left from frame {left}, right from frame {right}")
        report.equal(held, 0, "and the group's run holds its members to nothing")

        left, right, held = fire(report, room, locale, line_up=True)
        report.check(left >= 0 and right >= 0, "asked: both sounds are heard", f"left from {left}, right from {right}")
        report.equal(right - left, 0,
                     "asked: the two are heard on the same sample",
                     f"left from frame {left}, right from frame {right}")
        report.equal(held, LOOK_AHEAD, "and the group's run says what it held its members to, in samples")
    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None
    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
    try:
        return run(locale)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
