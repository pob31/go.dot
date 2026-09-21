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

"""A real interface plays media before and after stopped reconfiguration.

Also saves and reopens the show's interface and output patch. The media probe
is a quiet sine; final device-buffer signal and channel isolation are measured
separately by DeviceTests.cpp. Hardware rates are observed, never forced here.

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
import math
import struct
import sys
import tempfile
import time
import wave
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server
from first_sound import runs_in, wait_for_run_state


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "groups"
CLIENT = REPO_ROOT / "clients" / "console"


def add_media_probe(bundle: Path) -> None:
    """Make the first cue a quiet routed sine, safe to send to a real device."""
    root = ET.parse(bundle / "show.xml")
    cue = root.find("./Lists/List/Cue")
    cue.tag = "Media"
    cue.set("file", "device-probe.wav")
    ET.SubElement(cue, "Route", id="Z04EH7PH", bus="J3MT5XYA", gains="1 0")
    audio = root.find("Audio")
    audio.set("tracks", "1")
    ET.SubElement(audio, "Bus", id="J3MT5XYA", name="Main", width="2")
    root.write(bundle / "show.xml", encoding="utf-8")
    media = bundle / "media"
    media.mkdir(exist_ok=True)
    with wave.open(str(media / "device-probe.wav"), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(44100)
        out.writeframes(b"".join(struct.pack("<h", int(32 * math.sin(2 * math.pi * 440 * n / 44100)))
                                 for n in range(44100 * 5)))


def available_devices() -> "list[tuple[str, str]]":
    """Every device `wfg devices` lists, in the order it lists them.

    Parsed off the verb's own output rather than guessed at, because the name
    `--device=` wants is exactly the name that verb prints: a device list is a
    fact about the machine, and the two have to agree or neither is useful.
    """
    code, out, err = common.run_wfg("devices")

    if code != 0:
        raise HarnessError(f"wfg devices exited {code}: {(out + err).strip()[:200]}")

    found = []
    device_type = ""

    for line in out.splitlines():
        # A type heading is unindented; a device is indented four spaces; its
        # channel counts and rates are indented eight.
        if line and not line.startswith(" "):
            device_type = line.removesuffix(" (default)").strip()
        elif line.startswith("    ") and not line.startswith("        "):
            found.append((device_type, line.strip()))

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


def check_on(device: str, locale: "str | None", report: Report, device_type: str = "") -> bool:
    """Runs every check against one device. False if it would not open."""
    with tempfile.TemporaryDirectory(prefix="wfg-device-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "groups")
        add_media_probe(bundle)

        try:
            server = Server(bundle, locale=locale, sample_rate=None, buffer_size=None,
                            device=device, device_type=device_type, ui=CLIENT)
        except HarnessError as error:
            # An explicit refusal can mean unavailable hardware. A crash or a
            # startup timeout must fail this test, even before ports appeared.
            if "serve exited 2 before it was ready:" in str(error):
                print(f"device_serve: {error}")
                return False
            raise

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

            #  Waited for rather than slept for: what is being asked is whether
            #  the tick moves, and a fixed half-second is a guess about how fast
            #  a loaded runner publishes.
            moved = common.wait_until(
                lambda: int(value_at(server.http_port, "/godot/engine/tick")) > first)

            report.check(bool(moved), "and it keeps ticking")

            # A callback and advancing ticks can both work while GO stays silent:
            # the hardware launch path must also configure Runner's tick size.
            play_and_kill(server, report)

            revision = value_at(server.http_port, "/godot/audio/settingsRevision")
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/audio/setup",
                            [True, device_type, device, "", 0, "-1 -1", "1 0"]))
            applied = common.wait_until(
                lambda: value_at(server.http_port, "/godot/audio/settingsRevision") > revision,
                timeout=20.0)
            report.check(bool(applied), "Audio settings finishes switching while stopped")
            report.equal(value_at(server.http_port, "/godot/audio/settingsError"), "",
                         "the selected interface and swapped patch are accepted")
            report.equal(value_at(server.http_port, "/godot/engine/clock"), "device",
                         "the switched interface drives the clock")
            if applied:
                play_and_kill(server, report)
            rate = value_at(server.http_port, "/godot/audio/actualSampleRate")
            buffer = value_at(server.http_port, "/godot/audio/actualBufferSize")
            common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/document/save"))
            saved = common.wait_until(
                lambda: value_at(server.http_port, "/godot/document/dirty") is False)
            report.check(bool(saved), "the show saves its audio settings")

        # No --device argument: the reopened show must select its own interface.
        with Server(bundle, locale=locale, sample_rate=rate, buffer_size=buffer) as reopened:
            report.check(wait_until_ticking(reopened), "the saved show reopens on its selected interface")
            report.equal(value_at(reopened.http_port, "/godot/engine/clock"), "device",
                         "the saved selection starts hardware playback")
            report.equal(value_at(reopened.http_port, "/godot/audio/outputPatch"), "1 0",
                         "the saved output patch is restored")
            play_and_kill(reopened, report)
        return True


def play_and_kill(server: Server, report: Report) -> None:
    before = set(runs_in(server))
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/cue/fire", ["B3N8R5TW"]))
    created = common.wait_until(lambda: [run for run in set(runs_in(server)) - before
                               if value_at(server.http_port, f"/godot/run/{run}/cue") == "B3N8R5TW"],
                               timeout=8.0)
    report.check(bool(created), "the media cue has a new run")
    if not created:
        return
    run = sorted(created)[0]
    state = wait_for_run_state(server, run, "playing", timeout=8.0)
    report.equal(state, "playing", "GO launches media on the device clock")
    if state != "playing":
        print("device_serve: last engine error:", value_at(server.http_port, "/godot/engine/lastError"))
    if state == "playing":
        advanced = common.wait_until(
            lambda: float(value_at(server.http_port, f"/godot/run/{run}/position")) > 0.1)
        report.check(bool(advanced), "the media playhead advances")
        common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/audio/reconnect"))
        paused = common.wait_until(lambda: value_at(server.http_port, "/godot/audio/status") == "noClock")
        report.check(bool(paused), "reconnecting reports that cues are paused")
        paused_position = float(value_at(server.http_port, f"/godot/run/{run}/position"))
        paused_tick = int(value_at(server.http_port, "/godot/engine/tick"))
        # Silent clock validation lasts 250 ms; query twice without waiting for
        # an audio tick, proving the control plane remains available while held.
        if value_at(server.http_port, "/godot/audio/status") == "noClock":
            report.equal(int(value_at(server.http_port, "/godot/engine/tick")), paused_tick,
                         "the show tick stays frozen during recovery")
            report.equal(float(value_at(server.http_port, f"/godot/run/{run}/position")), paused_position,
                         "the existing cue retains its position")
        restored = common.wait_until(lambda: value_at(server.http_port, "/godot/audio/status") == "running",
                                     timeout=15.0)
        report.check(bool(restored), "the original device and clock recover automatically")
        resumed = common.wait_until(lambda: float(value_at(server.http_port, f"/godot/run/{run}/position")) > paused_position)
        report.check(bool(resumed), "the same run resumes after reconnection")
        report.equal(value_at(server.http_port, f"/godot/run/{run}/state"), "playing",
                     "recovery does not replace or restart the cue")
    common.send_udp(server.osc_port,
                    common.osc_encode("/godot/cmd/run/kill", [run]))
    report.equal(wait_for_run_state(server, run, "done", timeout=5.0), "done",
                 "the media cue can still be killed")


def run(locale: "str | None") -> int:
    report = Report(f"serve --device stays up and serves ({locale or 'C'})")

    devices = available_devices()

    if not devices:
        print("device_serve: this machine has no audio device; nothing to test")
        return 0

    for device_type, device in devices:
        print(f"device_serve: trying {device_type} / \"{device}\"")

        if check_on(device, locale, report, device_type):
            return report.finish()

        print(f"device_serve:   would not open")

    print("device_serve: none of this machine's devices will open for playback;"
          " nothing to test")

    for device_type, device in devices:
        print(f"device_serve:   tried {device_type} / \"{device}\"")

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
