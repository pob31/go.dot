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

"""`serve --device=` opens a real interface, and is still there afterwards.

WHAT THIS IS FOR, and it is the plainest possible thing: the engine used to
print its whole banner — both ports, the client URL, the granted device
settings — and then die of a segmentation fault before the first tick. Every
line on the terminal said it had started. A browser opening the address it had
just been handed got a refused connection, and the only clue was an exit code
nobody looks at.

The cause was a decision written as a pair when there are three: `--hosted`, or
else a dummy clock. `--device` is neither, so it fell down the `else` and called
`start()` through a null pointer. Nothing caught it because nothing in the suite
had ever run `serve --device` end to end — the device layer has unit tests that
open a card, and the serve wiring around it had none.

WHAT IT DOES WHERE THERE IS NO USABLE CARD, which is every CI runner and is the
case that has to be got right or this test is noise. A runner *lists* a device —
"Apple Virtual Sound Device", "Primary Sound Driver" — and cannot open it for
playback. So the driver tries each device the machine offers, in order, and
takes the first that opens; only when none of them will does it report that
there is nothing here to test, and say what it tried.

REFUSING AND CRASHING ARE TOLD APART, which is the whole reason the skip is safe.
A device that will not open is a deliberate refusal: exit code 2, with a line on
stderr naming the flag. The failure this test exists for is not that — it is an
access violation or a signal, which never exits 2. So a refusal is a machine
without a card, and anything else that is not alive is the defect.

Exit codes: 0 everything held or nothing to test, 1 something did not,
2 the harness could not run.
"""

from __future__ import annotations

import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "groups"
CLIENT = REPO_ROOT / "clients" / "console"


def available_devices() -> "list[str]":
    """Every device `wfg devices` lists, in the order it lists them.

    Parsed off the verb's own output rather than guessed at, because the name
    `--device=` wants is exactly the name that verb prints: a device list is a
    fact about the machine, and the two have to agree or neither is useful.
    """
    code, out, err = common.run_wfg("devices")

    if code != 0:
        raise HarnessError(f"wfg devices exited {code}: {(out + err).strip()[:200]}")

    found = []

    for line in out.splitlines():
        # A type heading is unindented; a device is indented four spaces; its
        # channel counts and rates are indented eight.
        if line.startswith("    ") and not line.startswith("        "):
            found.append(line.strip())

    return found


def value_at(port: int, address: str):
    status, body = common.http_get(port, address + "?VALUE")

    if status != 200:
        raise HarnessError(f"GET {address}?VALUE returned {status}")

    return json.loads(body)["VALUE"][0]


def wait_until_ticking(server) -> bool:
    """True once the engine has ticked, false if it left first.

    WAITED FOR RATHER THAN SLEPT THROUGH. The ports are printed before the
    device is opened, and opening one takes as long as the driver takes — the
    better part of two seconds for DirectSound on a Windows box. A fixed sleep
    either races that or is a guess padded until it stops racing, and the first
    version of this test did the former and reported a healthy engine as broken.
    What says the engine is up is the tick advancing, so that is what is waited
    on.
    """
    deadline = time.monotonic() + 20.0

    while time.monotonic() < deadline:
        if server.process.poll() is not None:
            return False

        try:
            if int(value_at(server.http_port, "/godot/engine/tick")) > 0:
                return True
        except (HarnessError, OSError):
            # A refused connection is one of the failures this test is FOR: the
            # engine died with its banner already on the screen. It has to
            # arrive as a check that failed rather than as a traceback.
            pass

        time.sleep(0.05)

    return False


def refusal_from(server) -> "str | None":
    """The engine's own words when it declined to open the device, or None.

    Exit code 2 with the flag named on stderr is `serve` saying it could not
    have the card. Anything else — a signal, an access violation, silence — is
    not a refusal and must not be read as one.
    """
    if server.process.poll() != 2:
        return None

    try:
        text = server.process.stderr.read() or ""
    except (ValueError, OSError):
        return None

    for line in text.splitlines():
        if "--device" in line:
            return line.strip()

    return None


def check_on(device: str, locale: "str | None", report: Report) -> bool:
    """Runs every check against one device. False if it would not open."""
    with tempfile.TemporaryDirectory(prefix="wfg-device-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "groups")

        try:
            server = Server(bundle, locale=locale, sample_rate=44100,
                            device=device, ui=CLIENT)
        except HarnessError:
            # It did not get far enough to report its ports, which on this path
            # means it never opened the card.
            return False

        with server:
            ticking = wait_until_ticking(server)

            if not ticking:
                if refusal_from(server) is not None:
                    return False

                """STILL ALIVE IS THE WHOLE ASSERTION. The banner had already
                been printed when it used to die, so reading the ports off
                stdout is evidence of nothing: the question is whether it is
                there afterwards."""
                report.check(False, "it is still running after it said it had started",
                             f"exit code {server.process.poll()}")
                return True

            report.check(True, "it is still running after it said it had started")
            report.check(True, "and the tick is advancing")

            report.equal(value_at(server.http_port, "/godot/audio/status"), "running",
                         "and the audio side says so")

            status, _ = common.http_get(server.http_port, "/ui")
            report.equal(status, 200, "and the client is being served")

            told = [line for line in server.notices if "/ui" in line]
            report.check(bool(told), "and the address it printed is one to open",
                         "\n".join(server.notices))

            """AND THE CLOCK IS THE DEVICE'S, which is what --device is for. A
            dummy clock would tick too, and a test that only asked whether ticks
            happened would have passed on the wrong one."""
            report.equal(value_at(server.http_port, "/godot/engine/clock"), "device",
                         "and the clock being counted is the device's")

            first = int(value_at(server.http_port, "/godot/engine/tick"))
            time.sleep(0.5)

            report.check(int(value_at(server.http_port, "/godot/engine/tick")) > first,
                         "and it keeps ticking")

            return True


def run(locale: "str | None") -> int:
    report = Report(f"serve --device stays up and serves ({locale or 'C'})")

    devices = available_devices()

    if not devices:
        print("device_serve: this machine has no audio device; nothing to test")
        return 0

    for device in devices:
        print(f"device_serve: trying \"{device}\"")

        if check_on(device, locale, report):
            return report.finish()

        print(f"device_serve:   would not open")

    print("device_serve: none of this machine's devices will open for playback;"
          " nothing to test")

    for device in devices:
        print(f"device_serve:   tried \"{device}\"")

    return 0


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"device_serve: {problem}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return run(locale)
    except HarnessError as problem:
        print(f"device_serve: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
