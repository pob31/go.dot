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

"""A main and its backup start a sound on the same sample - driven from outside
(namespace draft 58, RD.9; the author's AFS; AGI, AGQ; M61).

WHAT THIS FILE IS FOR. The unit suite composes the clock map from simulated
clocks and joins two engines in one process, each on a simulated clock. What
none of it can prove is two shipped binaries doing it: each stamping the
blocks of its own hosted pump, the two host clocks compared over the link's
own pings, the leader's lead said in its stream, a GO at the main and the same
click written into two renders at the same moment.

  - a main and a backup, the backup following; the backup says its launches
    are on the main's samples (`pair/mapped`), the drift between the two sound
    clocks near nought - both run on this machine's one clock - and how well
    the map is known; the main says the extra lead its launches take (AFS),
    each side the other's output latency (AGQ);
  - a GO at the main: each render's click onset, read on that machine's sample
    count, put on the steady clock through its `engine/sampleZeroAt`, the
    two onsets within a millisecond of each other here - within a block on CI,
    where a Debug runner's ticks run late;
  - M61 printed: the distance between the two onsets, and Δ.

TWO SERVES ON ONE MACHINE, hosted and rendering, on COPIES of the phase 2
fixture whose show is rewritten for this driver, each with its own temporary
engine folder and an explicit role. The click is generated here.
"""
import os
import struct
import sys
import tempfile
import time
import wave
from pathlib import Path

import common
from common import HarnessError, Report, Server

ON_CI = os.environ.get("GITHUB_ACTIONS") == "true"

FIXTURE = Path(__file__).resolve().parent.parent / "fixtures" / "bundles" / "phase2"

LIST = "7K2QM9X4"
CLICK = "C1K7P9X3"

TIMEOUT = 1.0
RATE = 48000
BLOCK = 128

# WITHIN A MILLISECOND HERE; a block on CI, where a Debug runner's tick can
# land a launch one block later than its sample asked.
WITHIN = (BLOCK / RATE) if ON_CI else 0.001

# How near nought the drift between two clocks that are one must come.
DRIFT_PPM = 5.0

SHOW = f"""<Show>
  <Lists goDebounce="0">
    <List id="{LIST}" name="Show">
      <Media id="{CLICK}" file="click.wav" name="Click" number="1">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1 1"/>
      </Media>
      <Cue id="Q6RS8TW2" name="After" number="2"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="4">
    <Bus id="J3MT5XYA" name="Main L/R" width="2"/>
  </Audio>
  <MidiPorts/>
  <Network/>
  <Surfaces/>
  <Dcas/>
</Show>
"""

locale = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--wfg-locale=")), "C")


def write_click(path: Path) -> None:
    """A step to half scale for ten milliseconds, then silence: its first sample is its onset."""
    path.parent.mkdir(parents=True, exist_ok=True)
    level = int(0.5 * 32767)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(struct.pack("<hh", level, level) * (RATE // 100)
                        + struct.pack("<hh", 0, 0) * (RATE // 5))


def machine(scratch: Path, name: str):
    bundle = common.copy_bundle(FIXTURE, scratch / name / "show")
    (bundle / "show.xml").write_text(SHOW, encoding="utf-8")
    (bundle / "state.xml").unlink(missing_ok=True)
    write_click(bundle / "media" / "click.wav")
    engine = scratch / name / "machine" / "engine"
    engine.mkdir(parents=True)
    return bundle, engine, scratch / name / "render.wav"


def wait_for(predicate, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(0.05)
    return predicate()


def reader(server):
    def read(address):
        """A node's first value as text, "" when it cannot be read; a flag as true or false."""
        try:
            values = common.http_json(server.http_port, address + "?VALUE").get("VALUE", [])
        except (HarnessError, OSError, ValueError):
            return ""
        if not values:
            return ""
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return str(values[0])
    return read


def sender(server):
    def send(command, args=None):
        common.send_udp(server.osc_port,
                        common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or []))
    return send


def runs_of(read, cue):
    return [run for run in read("/godot/run/order").split() if read(f"/godot/run/{run}/cue") == cue]


def as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def read_render(path: Path):
    """The first channel of a render as floats: 32-bit float, Go.dot's own (first_sound.py says why by hand)."""
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise HarnessError(f"{path} is not a RIFF/WAVE file")
    fmt_tag = bits = channels = 0
    data = b""
    at = 12
    while at + 8 <= len(raw):
        name = raw[at:at + 4]
        size = struct.unpack_from("<I", raw, at + 4)[0]
        body = raw[at + 8:at + 8 + size]
        if name == b"fmt ":
            fmt_tag, channels = struct.unpack_from("<HH", body, 0)
            bits = struct.unpack_from("<H", body, 14)[0]
            if fmt_tag == 0xFFFE and len(body) >= 26:
                fmt_tag = struct.unpack_from("<H", body, 24)[0]
        elif name == b"data":
            data = body
        at += 8 + size + (size & 1)
    if fmt_tag != 3 or bits != 32 or channels == 0:
        raise HarnessError(f"{path} is format {fmt_tag} at {bits} bits, which this does not read")
    frame = 4 * channels
    return [struct.unpack_from("<f", data, n * frame)[0] for n in range(len(data) // frame)]


def onset(samples, floor=0.05):
    for n, value in enumerate(samples):
        if abs(value) > floor:
            return n
    return -1


def run() -> int:
    report = Report(f"redundancy: the same sample on both machines ({locale})")

    with tempfile.TemporaryDirectory(prefix="wfg-pair-sample-") as scratch:
        scratch = Path(scratch)
        main_bundle, main_engine, main_render = machine(scratch, "main")
        backup_bundle, backup_engine, backup_render = machine(scratch, "backup")
        servers = []
        rows = {}

        try:
            main = Server(main_bundle, locale=locale, engine_folder=main_engine, role="main", hosted=True,
                          buffer_size=BLOCK, render=main_render, extra_args=[f"--link-timeout={TIMEOUT:g}"])
            servers.append(main)
            read_main, send_main = reader(main), sender(main)

            backup = Server(backup_bundle, locale=locale, engine_folder=backup_engine, role="backup", hosted=True,
                            buffer_size=BLOCK, render=backup_render,
                            extra_args=[f"--peer=127.0.0.1:{main.http_port}", f"--link-timeout={TIMEOUT:g}"])
            servers.append(backup)
            read_backup = reader(backup)

            # === FOLLOWING, AND ON THE MAIN'S SAMPLES =============================
            report.check(wait_for(lambda: read_backup("/godot/pair/state") == "following", 10.0),
                         "the backup follows its main", f"state {read_backup('/godot/pair/state')!r}")
            report.check(wait_for(lambda: read_backup("/godot/pair/mapped") == "true", 15.0),
                         "its launches are placed on the main's samples (pair/mapped)",
                         f"mapped {read_backup('/godot/pair/mapped')!r}, residual "
                         f"{read_backup('/godot/pair/clockResidual')!r} us, drift {read_backup('/godot/pair/drift')!r} ppm")

            # A second for the lines to settle past their first stamps.
            time.sleep(1.0)

            rows["delta"] = as_int(read_backup("/godot/pair/delay"))
            rows["extra"] = as_int(read_main("/godot/pair/delay"))
            rows["drift"] = as_float(read_backup("/godot/pair/drift"))
            rows["residual"] = as_float(read_backup("/godot/pair/clockResidual"))
            rows["backup peer latency"] = as_float(read_backup("/godot/pair/peerOutputLatency"))
            rows["main peer latency"] = as_float(read_main("/godot/pair/peerOutputLatency"))
            print(f"  note Δ {rows['delta']} tick(s) on the backup, the main's extra lead {rows['extra']},"
                  f" drift {rows['drift']:.3f} ppm, residual {rows['residual']:.1f} us,"
                  f" output latency of the other: {rows['backup peer latency']} s on the backup,"
                  f" {rows['main peer latency']} s on the main")

            report.check(rows["delta"] >= 1, "the backup measured its delay", f"pair/delay {rows['delta']}")
            report.equal(rows["extra"], rows["delta"], "the main places its launches that much further ahead (AFS)")
            report.check(0.0 < rows["residual"] < 5000.0, "and the map is known to well under five milliseconds",
                         f"pair/clockResidual {rows['residual']} us")
            report.equal(read_main("/godot/pair/mapped"), "false", "the main, which leads, maps nothing")
            report.check(rows["backup peer latency"] >= 0.0 and rows["main peer latency"] >= 0.0,
                         "each side shows the other's output latency, as information (AGQ)")

            # === A GO AT THE MAIN ================================================
            send_main("node.set", [f"/godot/list/{LIST}/standby", CLICK])
            report.check(wait_for(lambda: read_main(f"/godot/list/{LIST}/standby") == CLICK, 3.0),
                         "the standby on the click")
            time.sleep(0.5)
            send_main("go")
            report.check(wait_for(lambda: len(runs_of(read_main, CLICK)) >= 1 and len(runs_of(read_backup, CLICK)) >= 1,
                                  5.0), "the click runs on both")
            time.sleep(1.0)

            # Where each engine's sample nought sits, read before it is stopped.
            zero_main = as_int(read_main("/godot/engine/sampleZeroAt"))
            zero_backup = as_int(read_backup("/godot/engine/sampleZeroAt"))
            report.check(zero_main > 0 and zero_backup > 0, "each engine says where its sample nought sits",
                         f"main {zero_main}, backup {zero_backup}")
            late = as_int(read_backup("/godot/pair/late"))
            print(f"  note pair/late on the backup {late}")

            # === THE DRIFT, ONCE THE PINGS HAVE SAID IT ============================
            # Both serves count on this machine's one clock, so nought - but a few
            # seconds of pings over a loopback socket say a drift only to tens of
            # parts per million; the line through them knows it to one within half
            # a minute (pair/ClockMap.h). Watched until it does.
            began = time.monotonic()
            settled = wait_for(lambda: abs(as_float(read_backup("/godot/pair/drift"))) <= DRIFT_PPM, 40.0)
            drift = as_float(read_backup("/godot/pair/drift"))
            rows["drift"] = drift
            print(f"  note the drift read {drift:.3f} ppm, {time.monotonic() - began:.1f} s after the GO")
            report.check(settled, f"the two sound clocks drift by nearly nothing - one machine's one clock - within"
                                  f" {DRIFT_PPM:g} ppm", f"pair/drift {drift} ppm")

            for path in (main_render, backup_render):
                covered = common.wait_until(lambda path=path: path.is_file() and path.stat().st_size > 200_000, timeout=5.0)
                if not covered:
                    print(f"  note {path.name} is short")
            time.sleep(1.5)
        finally:
            for server in reversed(servers):
                try:
                    server.stop()
                except HarnessError as error:
                    report.check(False, "a serve stopped cleanly", str(error))

        # === THE TWO RENDERS, ON THE STEADY CLOCK ==================================
        if not report.check(main_render.is_file() and backup_render.is_file(), "both renders were written"):
            return report.finish()

        onset_main = onset(read_render(main_render))
        onset_backup = onset(read_render(backup_render))
        report.check(onset_main > 0 and onset_backup > 0, "the click is in both renders",
                     f"main frame {onset_main}, backup frame {onset_backup}")

        if onset_main > 0 and onset_backup > 0 and zero_main > 0 and zero_backup > 0:
            at_main = zero_main + onset_main * 1.0e9 / RATE
            at_backup = zero_backup + onset_backup * 1.0e9 / RATE
            apart = (at_backup - at_main) / 1.0e9
            print(f"M61: the click on the main at frame {onset_main}, on the backup at frame {onset_backup};"
                  f" on the steady clock {apart * 1.0e6:+.0f} us apart (backup less main); Δ {rows.get('delta')} tick(s),"
                  f" the main's extra lead {rows.get('extra')}, drift {rows.get('drift', float('nan')):.3f} ppm,"
                  f" residual {rows.get('residual', float('nan')):.1f} us")
            report.check(abs(apart) <= WITHIN,
                         f"the two onsets are within {WITHIN * 1000:.1f} ms of each other on the steady clock",
                         f"{apart * 1000:+.3f} ms apart")

    return report.finish()


if __name__ == "__main__":
    sys.exit(run())
