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

"""Bindings, heard (namespace draft §74, BD.12).

WHAT THIS IS FOR. The unit suite proves the laws, the matching, pickup, a cue's
binding against its run, feedback and Learn, against a bridge driven by hand.
What none of it says is that the SHIPPED BINARY wires them: a datagram on Go.dot's
own UDP port and a controller on a declared port's input each reaching a
binding, the binding writing a cue's level as `binding:<id>`, and the level
HEARD - and a control learned by moving it.

THE SHOW IS tests/fixtures/bundles/bindings: one cue, the lane driver's tone
(a 750 Hz sine at a quarter of full scale), looping for ever; two bindings that
move its level along a straight line from -20 dB to unity (ownRange off) - the
OSC address /desk/fader/1, and controller 7 on the port "Desk" - and a third on
its pre-wait that nothing moves until it is learned.

THE CONTROLLER ARRIVES BY `--midi-play` (TG.4), into the port's input as a
cable's would: a note-off at once, which no binding hears and Learn does not
take, then controller 7 at nought, full, nought ... every two seconds from the
fifteenth second on. So everything the driver does by datagram - Learn first,
while nothing else moves, then the OSC binding - is done before the first
controller arrives, and nothing here races the file.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import math
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common                                    # noqa: E402
import first_sound                               # noqa: E402
import lane_level                                # noqa: E402
from common import HarnessError, Report, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "bindings"

RATE = lane_level.RATE
BED = "BD0BED01"
LEVEL = f"/godot/cue/{BED}/level"
PRE_WAIT = f"/godot/cue/{BED}/preWait"
BY_OSC = "BD0SC001"
BY_MIDI = "BD0MD001"
LEARNED = "BD0KN001"

UNITY = 20.0 * math.log10(lane_level.AMPLITUDE)     # the tone at the cue's own level, in dBFS
DOWN = UNITY - 20.0
LEVEL_TOLERANCE_DB = 1.0

FIRST_CONTROLLER_S = 15.0
CONTROLLER_EVERY_S = 2.0
CONTROLLERS = 6


def play_file() -> str:
    """The `--midi-render` lines `--midi-play` reads: `sent_ns due_ns bytes`."""
    lines = ["0 0 80 3C 00"]

    for n in range(CONTROLLERS):
        ns = int((FIRST_CONTROLLER_S + n * CONTROLLER_EVERY_S) * 1e9)
        lines.append(f"{ns} {ns} B0 07 {'7F' if n % 2 else '00'}")

    return "\n".join(lines) + "\n"


def send(server: Server, address: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode(address, args or []))


def close(actual, wanted: float, within: float = 1.0e-6) -> bool:
    return isinstance(actual, (int, float)) and abs(actual - wanted) <= within


def settles(server: Server, address: str, wanted: float, timeout: float = 10.0):
    deadline = time.monotonic() + timeout
    actual = None

    while time.monotonic() < deadline:
        actual = first_sound.value_of(server, address)

        if close(actual, wanted, 1.0e-3):
            return actual

        time.sleep(0.02)

    return actual


def written_by(log: Path, origin: str, command: str = "node.set") -> int:
    """How many applied records of `command` the log has from `origin`."""
    count = 0

    for line in log.read_text(encoding="utf-8").splitlines():
        parts = line.split()

        if len(parts) >= 5 and parts[0] == "A" and parts[3] == origin and parts[4] == command:
            count += 1

    return count


def run(locale: "str | None") -> int:
    report = Report(f"a control in the room moves a number, heard ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-bindings-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "bindings")
        render = room / "out.wav"
        log = room / "session.wfglog"
        played = room / "desk.txt"

        lane_level.write_tone(bundle / "media" / "tone.wav")
        played.write_text(play_file(), encoding="ascii", newline="\n")

        marks: "list[tuple[str, int, float]]" = []      # what moved it, the render frame, the level drawn
        midi_seen: "list[float]" = []

        with Server(bundle, log=log, locale=locale, sample_rate=RATE, buffer_size=lane_level.BLOCK,
                    hosted=True, render=render, midi_play={"Desk": str(played)}) as server:
            started = time.monotonic()

            report.equal(first_sound.wait_for(server, "/godot/audio/status", "running"), "running",
                         "the audio side comes up running")
            errors_before = first_sound.value_of(server, "/godot/engine/errorCount")

            first_sound.go(server)

            # --- Learn, by a datagram, while nothing else moves ---------------------
            send(server, "/godot/cmd/binding/learn", [LEARNED])
            report.equal(first_sound.wait_for(server, "/godot/engine/bindingLearn", LEARNED), LEARNED,
                         "Learn listens for the pre-wait's binding, and says so")

            send(server, "/pedal/2", ["held", 0.0])
            report.equal(first_sound.wait_for(server, f"/godot/binding/{LEARNED}/address", "/pedal/2"), "/pedal/2",
                         "the next datagram's address is learned")
            report.equal(first_sound.value_of(server, f"/godot/binding/{LEARNED}/source"), "osc",
                         "as an OSC binding")
            report.equal(first_sound.value_of(server, f"/godot/binding/{LEARNED}/argument"), 1,
                         "its argument the first that carries a number")
            report.equal(first_sound.wait_for(server, "/godot/engine/bindingLearn", ""), "",
                         "and Learn stops listening")

            #  Picked up where the pre-wait is - nought - and then moved by it.
            send(server, "/pedal/2", ["held", 0.0])
            send(server, "/pedal/2", ["held", 1.0])
            report.check(close(settles(server, PRE_WAIT, 1.0), 1.0, 1.0e-3),
                         "the learned control moves the pre-wait", f"{first_sound.value_of(server, PRE_WAIT)}")

            # --- the OSC binding: twenty decibels down, then unity again -------------
            for value, drawn in ((0.0, -20.0), (1.0, 0.0)):
                send(server, "/desk/fader/1", [value])
                landed = settles(server, LEVEL, drawn)
                report.check(close(landed, drawn, 1.0e-3), f"/desk/fader/1 at {value} puts the level at {drawn:g} dB",
                             f"{landed}")
                marks.append(("/desk/fader/1", lane_level.clock_frame(server), drawn))
                time.sleep(0.8)

            report.check(time.monotonic() - started < FIRST_CONTROLLER_S - 0.5,
                         "the datagrams were done before the first controller came",
                         f"{time.monotonic() - started:.1f} s")

            # --- the controller, played into "Desk" ----------------------------------
            deadline = started + FIRST_CONTROLLER_S + CONTROLLERS * CONTROLLER_EVERY_S + 10.0
            last = first_sound.value_of(server, LEVEL)

            while time.monotonic() < deadline and len(midi_seen) < 3:
                now = first_sound.value_of(server, LEVEL)

                if isinstance(now, (int, float)) and isinstance(last, (int, float)) and abs(now - last) > 1.0e-3:
                    midi_seen.append(now)
                    marks.append(("controller 7", lane_level.clock_frame(server), now))

                last = now
                time.sleep(0.02)

            report.check(len(midi_seen) >= 3, "controller 7 on Desk moves the level, back and forth",
                         f"{midi_seen}")
            report.check(all(close(v, 0.0, 1.0e-3) or close(v, -20.0, 1.0e-3) for v in midi_seen),
                         "to the ends of its line", f"{midi_seen}")

            report.check(lane_level.wait_for_frames(render, (marks[-1][1] if marks else 0) + int(RATE * 1.0)),
                         "the render runs past the last move")

            report.equal(first_sound.value_of(server, "/godot/engine/errorCount"), errors_before,
                         "a datagram only a binding listens for is no error")
            report.equal(first_sound.value_of(server, "/godot/engine/rtViolations"), 0,
                         "Go.dot's own code allocated nothing on the audio thread")

            send(server, "/godot/cmd/run/stopAll")
            first_sound.wait_for_render_tail(render)

        # --- what the log says ---------------------------------------------------
        report.check(written_by(log, f"binding:{BY_OSC}") >= 2, "the OSC binding's writes are its own in the log",
                     f"{written_by(log, f'binding:{BY_OSC}')}")
        report.check(written_by(log, f"binding:{BY_MIDI}") >= 3, "and the controller's",
                     f"{written_by(log, f'binding:{BY_MIDI}')}")
        report.equal(written_by(log, f"binding:{LEARNED}", "node.setMany"), 1,
                     "what Learn heard is one edit, by the binding that listened")

        # --- what came out --------------------------------------------------------
        channels, data = first_sound.read_render(render)
        left = data[0] if data else []
        report.check(channels >= 2 and first_sound.first_above(left, 0.005) >= 0, "the cue was heard at all")

        for what, frame, drawn in marks:
            at = frame + int(RATE * 0.4)

            if at + RATE // 10 >= len(left):
                report.check(False, f"the render reaches {what}'s move", f"{len(left)} frames, wanted {at}")
                continue

            heard = lane_level.level_db(left, at)
            wanted = UNITY + drawn
            report.check(abs(heard - wanted) <= LEVEL_TOLERANCE_DB,
                         f"{what} at {drawn:g} dB is heard there", f"{heard:.2f} dBFS, wanted {wanted:.2f}")

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
