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

"""M81 — what your presets and the network's devices cost the tick (namespace draft 76).

AN INSTRUMENT, NOT A GATE, in M25's idiom: it prints and asserts nothing, is
not a ctest, and wants a quiet machine. The author runs it and reads it.

WHAT IT IS FOR. Reading your folder again and saving a preset happen on the
scanner's thread, and finding and fetching a device on the discovery and
fetcher threads, so the tick should not see them - but the round before this
one found a thread parsing twenty megabytes beside the tick starving it
through the Debug heap's one lock (namespace draft 57.7, DP.11). So:

  A   idle               serve as every driver runs it, nothing asked
  B   rescans            your folder holding a copy of every shipped preset,
                         twenty megabytes, read again every half second
  C   saves              a device made from the largest shipped preset (three
                         megabytes) saved as a preset every half second
  D   picks              a device made from the mock's description five times:
                         the time from mount.describeAt to the device loaded

Each of A-C samples `/godot/engine/lateness` every 20 ms for five seconds and
reports the median, the 95th percentile and how far `latenessMax` moved.

    python tests/blackbox/m81_presets_discovery.py [--binary=<wfg>]
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import statistics
import sys
import tempfile
import time
from pathlib import Path

import common
from discovery import DESCRIPTION, Mock

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "minimal"
SHIPPED = REPO_ROOT / "presets" / "devices"
RATE = 48000


def read_int(server, address: str) -> int:
    reply = common.http_json(server.http_port, address + "?VALUE")
    values = (reply or {}).get("VALUE", [0])
    try:
        return int(values[0])
    except (TypeError, ValueError, IndexError):
        return 0


def sample(server, seconds: float, every=None) -> dict:
    """Lateness every 20 ms for `seconds`, calling `every` each half second."""
    before = read_int(server, "/godot/engine/latenessMax")
    readings = []
    start = time.time()
    next_call = start

    while time.time() - start < seconds:
        if every is not None and time.time() >= next_call:
            every()
            next_call += 0.5
        readings.append(read_int(server, "/godot/engine/lateness"))
        time.sleep(0.02)

    readings.sort()
    return {
        "median": statistics.median(readings) if readings else 0,
        "p95": readings[int(len(readings) * 0.95) - 1] if readings else 0,
        "maxMoved": read_int(server, "/godot/engine/latenessMax") - before,
        "samples": len(readings),
    }


def ms(samples: float) -> float:
    return samples * 1000.0 / RATE


def say(name: str, reading: dict) -> None:
    print(f"  {name:<10} median {ms(reading['median']):6.2f} ms   p95 {ms(reading['p95']):6.2f} ms   "
          f"latenessMax moved {ms(reading['maxMoved']):7.2f} ms   ({reading['samples']} samples)")


def main(argv) -> int:
    for argument in argv[1:]:
        if argument.startswith("--binary="):
            os.environ["WFG_BINARY"] = argument.split("=", 1)[1]

    print(f"M81 with {common.find_binary()}")

    with tempfile.TemporaryDirectory(prefix="wfg-m81-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "show")
        yours = room / "yours"
        yours.mkdir()

        for preset in SHIPPED.glob("*.json"):
            shutil.copyfile(preset, yours / preset.name)

        description = room / "mockdesk.json"
        description.write_text(json.dumps(DESCRIPTION), encoding="utf-8")

        with Mock(description) as mock, common.Server(
                bundle, extra_args=["--mdns-port=0", f"--mdns-peer=127.0.0.1:{mock.mdns_port}",
                                    f"--user-presets={yours}"]) as server:
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

            def command(name: str, args: list) -> None:
                sender.sendto(common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args),
                              ("127.0.0.1", server.osc_port))

            time.sleep(3.0)                                 # past the start's own lateness

            say("A idle", sample(server, 5.0))
            say("B rescans", sample(server, 5.0, lambda: command("preset.rescan", [])))

            command("mount.createFromPreset", ["digico-sd-osc", "M81SAVE1"])
            common.wait_until(lambda: common.http_json(server.http_port, "/godot/mount/M81SAVE1/preset?VALUE"),
                              timeout=20.0)
            saved = [0]

            def save() -> None:
                saved[0] += 1
                command("preset.save", ["M81SAVE1", f"m81-copy-{saved[0]}"])

            say("C saves", sample(server, 5.0, save))

            times = []

            def row(address: str) -> str:
                return str((common.http_json(server.http_port, address + "?VALUE") or {}).get("VALUE", [""])[0])

            #  The first pick makes the device; each after it moves that one to
            #  the new description (ARX), which its namespace row says.
            for pick in range(5):
                device = f"M81PCK{pick:02d}"
                started = time.time()
                command("mount.describeAt", ["127.0.0.1", mock.query_port, "Mock", device])

                if pick == 0:
                    common.wait_until(lambda: row("/godot/mount/M81PCK00/loaded") == "True", timeout=10.0)
                else:
                    common.wait_until(lambda: row("/godot/mount/M81PCK00/namespace") == f"namespaces/{device}.json",
                                      timeout=10.0)

                times.append((time.time() - started) * 1000.0)

            print(f"  D picks    {', '.join(f'{t:.0f}' for t in times)} ms "
                  f"(median {statistics.median(times):.0f} ms, from the command to the device loaded)")
            sender.close()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
