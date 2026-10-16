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

"""M80 - a control in the room to what it does: how long (namespace draft §74, BD.12).

AN INSTRUMENT, NOT A GATE, in the idiom of M26 (m26_pad_latency.py): it prints
and asserts nothing, it is not registered as a ctest, and the number that counts
is the one taken on a Release build on a machine nobody else is using.

TWO ROADS.

  A CONTROLLER TO THE LEVEL HEARD. Controller 7 on the port "Desk", played in by
  `--midi-play` every two seconds, full and nought by turns, moves the bed's
  level between unity and -20 dB through its binding (the gate's fixture,
  tests/fixtures/bundles/bindings). The log says the tick each write was applied
  on; the render says the sample where the level crossed halfway after it. What
  it cannot see is the time between the controller's arrival and the tick that
  took it - up to one tick, 20 ms at 50 Hz, the bridge draining at each - printed
  as the bound it is.

  A TRIGGER TO THE WIRE. A datagram /pad/1 from this driver fires a cart's OSC
  cue through its trigger (AQU), whose message reaches the mock device. The
  driver stamps the send and the device the arrival on one clock - the machine's
  monotonic one, which every process on it shares - so the difference is the
  whole road, socket to socket. A MIDI note's road to a trigger differs from
  this only on its input thread, where it is matched and submitted as a datagram
  is, so the datagram stands for both.

    WFG_BINARY=build/vs/src/Release/wfg.exe python tests/blackbox/m80_bindings.py [--hits 20] [--moves 12] [--buffer 128]
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bindings                                  # noqa: E402
import common                                    # noqa: E402
import first_sound                               # noqa: E402
import lane_level                                # noqa: E402
from common import HarnessError, Server          # noqa: E402

RATE = lane_level.RATE
SAMPLES_PER_TICK = RATE // 50
BY_MIDI = "BD0MD001"
FIRST_MOVE_S = 8.0
MOVE_EVERY_S = 2.0

CART = """    <List id="BD0CRT01" kind="cart" name="Cart">
      <Osc id="BD0HT001" address="/hit/go" name="Hit" number="C1" value="f:1">
        <Trigger id="BD0TG001" address="/pad/1" kind="osc"/>
      </Osc>
    </List>
  </Lists>"""


def play_file(moves: int) -> str:
    lines = ["0 0 80 3C 00"]

    for n in range(moves):
        ns = int((FIRST_MOVE_S + n * MOVE_EVERY_S) * 1e9)
        lines.append(f"{ns} {ns} B0 07 {'7F' if n % 2 else '00'}")

    return "\n".join(lines) + "\n"


def ticks_written(log: Path, origin: str) -> "list[int]":
    out = []

    for line in log.read_text(encoding="utf-8").splitlines():
        parts = line.split()

        if len(parts) >= 5 and parts[0] == "A" and parts[3] == origin and parts[4] == "node.set":
            out.append(int(parts[1]))

    return out


def envelope_db(samples: "list[float]", at: int, width: int = 64) -> float:
    """One period of the tone around `at`, as its peak in dBFS."""
    part = samples[max(0, at - width // 2):max(0, at - width // 2) + width]

    if len(part) < width:
        return -400.0

    rms = math.sqrt(sum(x * x for x in part) / len(part))
    return 20.0 * math.log10(rms * math.sqrt(2.0)) if rms > 0.0 else -400.0


def crossing(samples: "list[float]", tick: int, rising: bool, halfway: float) -> "int | None":
    """The first sample, from two ticks before the write's tick, where the level is past halfway."""
    start = max(0, (tick - 2) * SAMPLES_PER_TICK)

    for at in range(start, min(len(samples), start + RATE), 16):
        level = envelope_db(samples, at)

        if (rising and level >= halfway) or (not rising and level <= halfway):
            return at

    return None


def spread(name: str, values: "list[float]") -> None:
    if not values:
        print(f"  {name}: nothing measured")
        return

    ordered = sorted(values)
    p90 = ordered[min(len(ordered) - 1, int(round(0.9 * (len(ordered) - 1))))]
    print(f"  {name}: median {statistics.median(ordered):.1f} ms, 90% {p90:.1f} ms, "
          f"min {ordered[0]:.1f}, max {ordered[-1]:.1f}  ({len(ordered)} of them)")


def run(hits: int, moves: int, buffer: int) -> int:
    with tempfile.TemporaryDirectory(prefix="wfg-m80-") as scratch, first_sound.MockTarget() as device:
        room = Path(scratch)
        bundle = common.copy_bundle(bindings.FIXTURE, room / "bindings")
        render = room / "out.wav"
        log = room / "session.wfglog"
        played = room / "desk.txt"

        lane_level.write_tone(bundle / "media" / "tone.wav")
        played.write_text(play_file(moves), encoding="ascii", newline="\n")

        show = bundle / "show.xml"
        text = show.read_text(encoding="utf-8")
        text = text.replace("  </Lists>", CART, 1)
        text = text.replace("<Mounts/>", f'<Mounts>\n    <Mount id="BD0DV001" name="Hits" port="{device.osc_port}"'
                                         f' prefix="/hit"/>\n  </Mounts>', 1)
        show.write_text(text, encoding="utf-8", newline="\n")

        sent: "list[float]" = []

        with Server(bundle, log=log, sample_rate=RATE, buffer_size=buffer, hosted=True, render=render,
                    midi_play={"Desk": str(played)}) as server:
            if first_sound.wait_for(server, "/godot/audio/status", "running") != "running":
                raise HarnessError("the audio side did not come up")

            first_sound.go(server)
            time.sleep(1.0)

            for _ in range(hits):
                sent.append(time.monotonic())
                common.send_udp(server.osc_port, common.osc_encode("/pad/1", [1.0]))
                time.sleep(0.25)

            until = FIRST_MOVE_S + moves * MOVE_EVERY_S + 2.0
            ticks_now = lambda: first_sound.value_of(server, "/godot/engine/tick") or 0  # noqa: E731
            deadline = time.monotonic() + until + 20.0

            while time.monotonic() < deadline and len(ticks_written(log, f"binding:{BY_MIDI}")) < moves:
                time.sleep(0.25)

            last = ticks_now()
            lane_level.wait_for_frames(render, int(last) * SAMPLES_PER_TICK + RATE)

            _, started = common.http_get(device.query_port, "/_mock/started")
            _, timed = common.http_get(device.query_port, "/_mock/timed")
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/run/stopAll"))
            first_sound.wait_for_render_tail(render)

        # --- a trigger to the wire ------------------------------------------------
        origin = common.json.loads(started)["VALUE"][0]
        arrivals = [origin + entry[0] for entry in common.json.loads(timed)["VALUE"] if str(entry[2]).endswith("/go")]
        wire = [1000.0 * (arrived - left) for left, arrived in zip(sent, arrivals)]

        # --- a controller to the level heard ---------------------------------------
        _, data = first_sound.read_render(render)
        left = data[0] if data else []
        unity = 20.0 * math.log10(lane_level.AMPLITUDE)
        halfway = unity - 10.0
        heard = []

        for n, tick in enumerate(ticks_written(log, f"binding:{BY_MIDI}")[:moves]):
            rising = n % 2 == 1
            at = crossing(left, tick, rising, halfway)

            if at is not None:
                heard.append(1000.0 * (at - tick * SAMPLES_PER_TICK) / RATE)

        print(f"M80 - {hits} triggers, {moves} controller moves, buffer {buffer}, {RATE} Hz")
        spread("a datagram on a trigger to its cart cue's message at the device", wire)
        spread("a controller's binding write (its tick) to the level halfway there, heard", heard)
        print("  and before that tick: up to one tick (20 ms) between the controller's arrival and the drain")
        print(f"  arrivals at the device: {len(arrivals)} of {hits}")

    return 0


def main(argv: "list[str]") -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hits", type=int, default=20)
    parser.add_argument("--moves", type=int, default=12)
    parser.add_argument("--buffer", type=int, default=128)
    options = parser.parse_args(argv[1:])

    try:
        return run(options.hits, options.moves, options.buffer)
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
