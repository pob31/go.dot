#!/usr/bin/env python3
"""M34 - the set at load: buildEdit to every child loaded, and the memory it takes.

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

A MEASUREMENT, NOT A GATE (namespace draft §17.9, PRD §6.11): decision AE's
cost in numbers. A show with N voices and P entries of a real plugin is served
hosted; the script times the moment every entry reads `loaded` against the
moment the ports were printed, and reads each child's working set off the
operating system. Run by hand on a quiet machine:

    WFG_BINARY=<wfg> WFG_REAL_VST3=<identifier from wfg plugins --list> \\
        python tests/blackbox/m34_set_load.py [voices...]

The identifier must be one this machine's scan knows; `godot:test-gain` gives
the transport's own cost with no plugin at all.
"""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
from common import HarnessError, Server  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "fx"


def value_of(server, address):
    status, body = common.http_get(server.http_port, f"{address}?VALUE")
    if status != 200:
        return None
    try:
        return common.json.loads(body)["VALUE"][0]
    except Exception:
        return None


def working_sets(name="wfg"):
    """{pid: bytes} for every process of that name, off the OS."""
    out = {}
    if os.name == "nt":
        script = ("Get-Process -Name %s -ErrorAction SilentlyContinue | "
                  "ForEach-Object { \"$($_.Id) $($_.WorkingSet64)\" }" % name)
        done = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                              capture_output=True, text=True)
        for line in done.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2:
                out[int(parts[0])] = int(parts[1])
    else:
        done = subprocess.run(["ps", "-o", "pid=,rss=,comm="], capture_output=True, text=True)
        for line in done.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2].endswith(name):
                out[int(parts[0])] = int(parts[1]) * 1024
    return out


def show_with(voices, identifier, entries):
    plugins = "".join(
        f'      <Plugin id="M34P000{n + 1}" format="VST3" identifier="{identifier}" name="Real {n + 1}"/>\n'
        for n in range(entries))
    return (f'<Show>\n'
            f'  <Lists>\n'
            f'    <List id="M34X0001" name="Sound">\n'
            f'      <Media id="M34M0001" file="tone.wav" name="Steady" number="1">\n'
            f'        <Route id="M34R0001" bus="M34B0001" gains="1 0 0 1"/>\n'
            f'      </Media>\n'
            f'    </List>\n'
            f'  </Lists>\n'
            f'  <Mounts/>\n'
            f'  <Audio tracks="{voices}">\n'
            f'    <Bus id="M34B0001" name="Main L/R" width="2"/>\n'
            f'    <Plugins>\n{plugins}    </Plugins>\n'
            f'  </Audio>\n'
            f'  <MidiPorts/>\n  <Network/>\n  <Surfaces/>\n  <Dcas/>\n</Show>\n')


def measure(voices, entries, identifier):
    with tempfile.TemporaryDirectory(prefix="wfg-m34-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "fx")
        (bundle / "show.xml").write_text(show_with(voices, identifier, entries), encoding="utf-8")
        before = set(working_sets())
        started = time.monotonic()
        with Server(bundle, hosted=True, sample_rate=48000, buffer_size=64) as server:
            ports_at = time.monotonic()
            ids = [f"M34P000{n + 1}" for n in range(entries)]

            def all_loaded():
                return all(value_of(server, f"/godot/plugin/{pid}/state") == "loaded" for pid in ids)

            ok = common.wait_until(all_loaded, timeout=60.0, interval=0.02)
            loaded_at = time.monotonic()
            states = {pid: value_of(server, f"/godot/plugin/{pid}/state") for pid in ids}
            time.sleep(1.0)
            sets = working_sets()
            children = {pid: size for pid, size in sets.items() if pid not in before and pid != server.process.pid}
            parent = sets.get(server.process.pid, 0)
        return {
            "voices": voices, "entries": entries, "loaded": ok, "states": states,
            "start_to_ports_ms": (ports_at - started) * 1000.0,
            "ports_to_loaded_ms": (loaded_at - ports_at) * 1000.0,
            "parent_mb": parent / 1e6,
            "children_mb": [size / 1e6 for size in children.values()],
        }


def main(argv):
    identifier = os.environ.get("WFG_REAL_VST3", "godot:test-gain")
    voices_list = [int(a) for a in argv[1:]] or [1, 8, 16]
    print(f"M34 - the set at load, plugin {identifier}")
    print("voices | entries | loaded | start->ports ms | ports->loaded ms | parent MB | children MB")
    for voices in voices_list:
        for entries in (1, 3):
            try:
                r = measure(voices, entries, identifier)
            except HarnessError as error:
                print(f"{voices:6} | {entries:7} | harness: {error}")
                continue
            kids = " ".join(f"{m:.0f}" for m in sorted(r["children_mb"]))
            print(f"{r['voices']:6} | {r['entries']:7} | {str(r['loaded']):6} | {r['start_to_ports_ms']:15.0f} | "
                  f"{r['ports_to_loaded_ms']:16.0f} | {r['parent_mb']:9.0f} | {kids}  {r['states'] if not r['loaded'] else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
