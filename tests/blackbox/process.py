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

"""A process cue's patch, end to end (namespace draft 51, PC.12).

WHAT THIS IS FOR. The unit suite runs patches in the same process and hands
them what a device said as records. This drives the shipped `wfg serve` -
Pure Data inside it, Go.dot's ready-made patches beside it - with nothing but
datagrams and named commands, against `mock_target.py`, a device written from
the specification in another language, and reads back what the device
RECEIVED and what the engine SAYS. Then it replays the session.

THE SHOW is tests/fixtures/bundles/process: a device under /dev that is heard
(`rx`), a process cue whose patch doubles what the device reports at /dev/in
and sends it back to /dev/out, and whose `[go.edge 0.5]` fires a memo, Bell,
when the report rises past a half.

THE SESSION:

 1. GO on the process cue: its patch opens and runs.
 2. The device reports at /dev/in, ten times: each doubled value arrives at
    /dev/out (M57, the time from the report leaving to the double arriving,
    judged off CI only).
 3. A report past a half fires Bell, under the patch's own origin.
 4. Esc: the run ends and the patch is closed; a report then is answered by
    nothing.
 5. The session's log replays record for record - with no patch run, since
    what the patch made Go.dot do is in the log as its own records.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "process"

PROCESS = "PRCS0001"
BELL = "B3NK0001"


class Device(first_sound.MockTarget):
    """`mock_target.py`, asked what arrived when."""

    def timed(self) -> list:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{self.query_port}/_mock/timed", timeout=4) as answer:
                return json.loads(answer.read().decode("utf-8"))["VALUE"] if answer.status == 200 else []
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            return []


def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def report_at(server: Server, value: float) -> None:
    """The device saying something, from its own host (this machine)."""
    common.send_udp(server.osc_port, common.osc_encode("/dev/in", [value]))


def point_mount_at(bundle: Path, device: Device) -> None:
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('port="9000"', f'port="{device.osc_port}"')
    show.write_text(text, encoding="utf-8", newline="\n")


def run_of(server: Server, cue: str) -> "str | None":
    for run in first_sound.runs_in(server):
        if first_sound.value_of(server, f"/godot/run/{run}/cue") == cue:
            return run
    return None


def doubled(device: Device, value: float) -> bool:
    for message in device.timed():
        if message[2] == "/dev/out" and message[3] and abs(message[3][0] - 2 * value) < 1e-4:
            return True
    return False


def session(report: Report, server: Server, device: Device, facts: dict) -> None:
    #  1. GO: the patch opens and runs.
    command(server, "cue.fire", [PROCESS])
    report.check(bool(common.wait_until(lambda: run_of(server, PROCESS) is not None, timeout=10.0)),
                 "GO: the process cue runs")
    run = run_of(server, PROCESS) or ""
    facts["run"] = run
    state = first_sound.wait_for(server, f"/godot/run/{run}/processState", "running", timeout=20.0)
    report.equal(state, "running", "its patch opens in Pure Data and runs")

    #  2. M57: a report doubled and back at the device.
    latencies = []
    for step in range(10):
        value = 0.01 * (step + 1)          # all under the edge's half
        sent_at = time.monotonic()
        report_at(server, value)
        arrived = common.wait_until(lambda v=value: doubled(device, v), timeout=5.0)
        latencies.append(time.monotonic() - sent_at)
        report.check(bool(arrived), f"the device's {value:.2f} comes back doubled at /dev/out")
        time.sleep(0.05)

    worst = max(latencies)
    lane_level.timed(report, worst < 0.1, "M57: a report is doubled and back at the device within 100 ms",
                     f"worst {worst * 1000:.0f} ms, " + ", ".join(f"{l * 1000:.0f}" for l in latencies))
    print(f"  M57: report to answer {', '.join(f'{l * 1000:.0f} ms' for l in latencies)}")

    report.check(run_of(server, BELL) is None, "nothing under a half has fired Bell")

    #  3. Past a half: Bell, once.
    report_at(server, 0.8)
    report.check(bool(common.wait_until(lambda: run_of(server, BELL) is not None, timeout=5.0)),
                 "a report past a half fires Bell through go.edge")

    #  4. Esc: the run ends and nothing answers any more.
    command(server, "run.stopAll")
    ended = common.wait_until(lambda: first_sound.value_of(server, f"/godot/run/{run}/state") in ("done", "stopped", None),
                              timeout=10.0)
    report.check(bool(ended), "Esc ends the process cue's run",
                 f"{first_sound.value_of(server, f'/godot/run/{run}/state')}")
    time.sleep(0.2)
    report_at(server, 0.33)
    time.sleep(0.5)
    report.check(not doubled(device, 0.33), "and a report after it is answered by nothing")


def run(locale: "str | None", keep_log: "Path | None") -> int:
    report = Report(f"process cue, a patch end to end ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-process-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "process")
        log = room / "session.wfglog"
        replayed = room / "replayed"
        facts: dict = {}

        with Device() as device:
            point_mount_at(bundle, device)

            with Server(bundle, log=log, locale=locale) as server:
                session(report, server, device, facts)

        if keep_log is not None:
            keep_log.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(log, keep_log)
            print(f"  (the session's log is kept at {keep_log})")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        report.check(" mount.heard " in text, "what the device reported is in the log as mount.heard")
        report.check(f"process:{facts.get('run', '?')}" in text and " cue.fire " in text,
                     "and Bell's firing, under the patch's own origin")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))
        report.equal(code, 0, "`wfg replay` reproduces the session record for record", (out + err).strip()[-2000:])
        report.check("reproduced exactly" in out, "saying so in as many words")

    return report.finish()


def main(argv: "list[str]") -> int:
    locale = None
    keep_log = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]
        elif argument.startswith("--keep-log="):
            keep_log = Path(argument.split("=", 1)[1])

    try:
        common.find_binary()
        return run(locale, keep_log)
    except HarnessError as error:
        print(f"process: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
