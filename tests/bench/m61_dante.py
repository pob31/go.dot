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

"""M61 over Dante: how far apart a main and its backup start the same sound,
heard where it comes back (namespace draft 58, RD.9b; AFS, AGQ).

WHAT THIS IS. A bench tool, not a test: it is not registered in ctest and it
opens the author's real interfaces. RD.9's own M61 (tests/blackbox/
pair_sample.py) runs two hosted serves on one machine and lays their renders
side by side on one steady clock; this one runs a main and a backup on two
machines, each on its own interface, and listens to both clicks come back into
ONE interface, on one sound clock, where nothing has to be converted:

  - the PC (Windows, RME Digiface Dante, ASIO "ASIO MADIface USB") has its own
    Dante outputs 1+2 patched back to its inputs 5+6, and the Mac mini's
    outputs 1+2 (CoreAudio "Dante Virtual Soundcard") on its inputs 3+4;
  - the PC's serve captures inputs 3-6 (`serve --capture`, audio/
    InputCapture.h), the sidecar tying the capture's first frame to the count
    the PC places launches on, which `/godot/run/<id>/launchSample` reads;
  - each GO plays a click - one sample at -20 dBFS, the rest silence - on
    outputs 1+2 of both machines; per GO, the click's onset on 5+6 (the PC)
    and on 3+4 (the Mac) are found in the capture, and their difference is
    THE CHANGEOVER JUMP an operator would hear switching a console from one
    machine to the other;
  - AGQ: nothing compensates the two interfaces' output latencies, which both
    machines publish (`audio/outputLatency`, `pair/outputLatency`,
    `pair/peerOutputLatency`). The jump less their difference is the ENGINE
    ALIGNMENT - how well RD.9 put both launches on one sample.

MODES.
  --main-only   the PC alone (role main, no backup): its clicks heard back on
                5+6 through its own Dante loopback - the capture path proved,
                and the PC's round trip against what its driver reports.
  --pair        the PC main and the Mac backup, then - if that ran cleanly -
                the roles swapped (the Mac main, the PC backup; the capture is
                still the PC's). Run only when the Mac's CoreAudio lists
                "Dante Virtual Soundcard": the driver looks first and stops,
                saying the bench owes it, when it does not.
  --mac-hosted  with --pair: a rehearsal of the plumbing - the Mac's serve on
                its hosted pump instead of Dante Virtual Soundcard, so the
                link, Δ, the map and the drift are measured over the real LAN
                while nothing reaches inputs 3+4. No M61.

SAFETY, because these are the author's real interfaces: the show plays one
click at -20 dBFS on outputs 1+2 and nothing else (one mono file, one bus of
two channels, no mounts, no MIDI, no network devices, no video); serve runs
headless (`--no-video-window`, never `--window`); each run is under two
minutes; no other device is named; every serve this driver started - and only
those - is stopped whatever happens, on both machines, and the end says what
is left running.

Writes build/redundancy-bench/m61-<stamp>.txt (everything printed), and keeps
the capture (m61-<stamp>[-swapped].wav and its .json sidecar) and the PC's log
beside it. Stdlib only, Python 3.10+.
"""
from __future__ import annotations

import argparse
import array
import contextlib
import json
import math
import os
import shlex
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import wave
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tests" / "blackbox"))

import common  # noqa: E402
from common import HarnessError, Server  # noqa: E402

RATE = 48000
BLOCK = 128

# THE CLICK: one sample at -20 dBFS, the rest silence, two seconds of mono. It
# sits a tenth of a second into the file rather than on its first frame, clear
# of anything a reader does at its start (a new wave reader fades in, ae79022),
# and the analysis takes the tenth back off.
CLICK_LEVEL = 0.1
CLICK_AT = RATE // 10
CLICK_SECONDS = 2

GO_EVERY = 1.0
LIST = "7K2QM9X4"
BUS = "J3MT5XYA"

PC_DEVICE, PC_TYPE = "ASIO MADIface USB", "ASIO"
MAC_DEVICE, MAC_TYPE = "Dante Virtual Soundcard", "CoreAudio"
MAC = "pierre-olivierboulant@172.22.1.27"
MAC_HOST = "172.22.1.27"
MAC_FARM_WFG = "Documents/go.dot-farm/build/ci-macos/src/Debug/wfg"     # under the Mac's home
MAC_SHOWS = "Documents/m61-bench"                                       # under the Mac's home
MAC_TEMP = "m61-tmp"                                                    # under the Mac's home

# The capture's channels: the PC's inputs 3-6, so the file's 0-1 are the Mac's
# outputs 1+2 and its 2-3 the PC's own, back through Dante.
CAPTURE_INPUTS = "3-6"
MAC_CHANNELS = (0, 1)
PC_CHANNELS = (2, 3)

SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]


# =============================================================================
# Output, kept for the report file
# =============================================================================

class Out:
    def __init__(self):
        self.lines: list[str] = []

    def __call__(self, text: str = "") -> None:
        print(text, flush=True)
        self.lines.append(text)


say = Out()


# =============================================================================
# The show
# =============================================================================

def cue_id(n: int) -> str:
    # Crockford base32 (no I, L, O, U), eight characters.
    return f"C{n:02d}K7P9X"


def route_id(n: int) -> str:
    return f"R{n:02d}H7PHZ"


def show_text(count: int) -> str:
    cues = "\n".join(
        f"""      <Media id="{cue_id(n)}" file="click.wav" name="Click {n}" number="{n}">
        <Route id="{route_id(n)}" bus="{BUS}" gains="1 1"/>
      </Media>""" for n in range(1, count + 1))

    return f"""<Show>
  <Lists goDebounce="0">
    <List id="{LIST}" name="M61">
{cues}
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="6">
    <Bus id="{BUS}" name="Main L/R" width="2"/>
  </Audio>
  <MidiPorts/>
  <Network/>
  <Surfaces/>
  <Dcas/>
</Show>
"""


def write_click(path: Path) -> None:
    """Mono, 16-bit: one sample of 3277 (-20.0 dBFS), the rest silence."""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray(2 * RATE * CLICK_SECONDS)
    struct.pack_into("<h", frames, 2 * CLICK_AT, round(CLICK_LEVEL * 32767))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(frames))


def make_show(folder: Path, count: int) -> Path:
    bundle = common.copy_bundle(REPO / "tests" / "fixtures" / "bundles" / "phase2", folder)
    (bundle / "show.xml").write_text(show_text(count), encoding="utf-8")
    (bundle / "state.xml").unlink(missing_ok=True)
    shutil.rmtree(bundle / "namespaces", ignore_errors=True)
    write_click(bundle / "media" / "click.wav")
    return bundle


# =============================================================================
# The Mac, over ssh
# =============================================================================

def ssh(command: str, timeout: float = 30.0) -> "tuple[int, str]":
    done = subprocess.run(SSH + [MAC, command], capture_output=True, text=True, timeout=timeout)
    return done.returncode, (done.stdout + done.stderr).strip()


def mac_home() -> str:
    code, text = ssh("echo $HOME")
    if code != 0 or not text.startswith("/"):
        raise HarnessError(f"the Mac did not say where its home is: {text}")
    return text.splitlines()[-1].strip()


def mac_has_dante() -> bool:
    code, text = ssh("system_profiler SPAudioDataType 2>/dev/null | grep -i 'Dante Virtual Soundcard'", 60.0)
    return code == 0 and "dante virtual soundcard" in text.lower()


class MacServe:
    """`wfg serve` on the Mac, started over ssh, its ports read off its stdout.

    THE SHELL EXECS THE SERVE after saying its own pid, so that pid is the
    serve's, and stopping it is one `kill` of exactly the process this driver
    started - never anything another session on the Mac runs.
    """

    def __init__(self, wfg: str, show: str, folder: str, role: str, peer: str | None, hosted: bool):
        args = [wfg, "serve", show, "--http-port=0", "--osc-port=0", f"--engine-folder={folder}/engine",
                f"--log={folder}/serve.log", f"--role={role}", "--link-timeout=1", "--no-video-window",
                f"--sample-rate={RATE}", f"--buffer={BLOCK}"]
        if hosted:
            args.append("--hosted")
        else:
            args += [f"--device={MAC_DEVICE}", f"--device-type={MAC_TYPE}"]
        if peer:
            args.append(f"--peer={peer}")

        remote = (f"mkdir -p {shlex.quote(folder + '/engine')} && echo wfg-pid $$ && exec "
                  + " ".join(shlex.quote(a) for a in args))
        self.argv = args
        self.process = subprocess.Popen(SSH + [MAC, remote], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True)
        self.pid = 0
        self.http_port = 0
        self.osc_port = 0
        self.notices: list[str] = []
        self.errors: list[str] = []
        threading.Thread(target=self._read_errors, daemon=True).start()

        deadline = time.monotonic() + 40.0
        while (self.http_port == 0 or self.osc_port == 0) and time.monotonic() < deadline:
            line = self.process.stdout.readline()
            if not line:
                if self.process.poll() is not None:
                    break
                continue
            parts = line.split()
            if parts[:1] == ["wfg-pid"] and len(parts) == 2:
                self.pid = int(parts[1])
            elif parts[:1] == ["wfg:"]:
                self.notices.append(line.strip())
                if len(parts) == 3 and parts[1] == "http":
                    self.http_port = int(parts[2])
                elif len(parts) == 3 and parts[1] == "osc":
                    self.osc_port = int(parts[2])
        threading.Thread(target=self._drain, daemon=True).start()

        if self.http_port == 0:
            time.sleep(0.5)
            self.stop()
            raise HarnessError("the Mac's serve did not report its ports: " + " | ".join(self.errors[-5:]))

    def _read_errors(self) -> None:
        for line in self.process.stderr:
            self.errors.append(line.rstrip())

    def _drain(self) -> None:
        for line in self.process.stdout:
            if line.startswith("wfg:"):
                self.notices.append(line.strip())

    def stop(self) -> None:
        if self.pid:
            ssh(f"kill -TERM {self.pid} 2>/dev/null; for i in $(seq 1 50); do kill -0 {self.pid} 2>/dev/null || exit 0;"
                f" sleep 0.1; done; kill -KILL {self.pid} 2>/dev/null; exit 0")
        with contextlib.suppress(Exception):
            self.process.wait(timeout=10)
        if self.process.poll() is None:
            self.process.kill()


def mac_sweep(marker: str) -> str:
    """Stops any serve on the Mac whose command line names this run's folder, and says what wfg is left."""
    ssh(f"pkill -TERM -f {shlex.quote('serve .*' + marker)} 2>/dev/null; sleep 1;"
        f" pkill -KILL -f {shlex.quote('serve .*' + marker)} 2>/dev/null; exit 0")
    _, left = ssh("pgrep -fl 'wfg' || true")
    return left


# =============================================================================
# Reading and commanding a serve, wherever it is
# =============================================================================

class Node:
    """A serve's rows over HTTP and its commands over OSC/UDP, at a host and ports."""

    def __init__(self, host: str, http_port: int, osc_port: int, name: str):
        self.host, self.http_port, self.osc_port, self.name = host, http_port, osc_port, name

    def read(self, address: str) -> str:
        url = f"http://{self.host}:{self.http_port}{address}?VALUE"
        try:
            with urllib.request.urlopen(url, timeout=3.0) as reply:
                values = json.loads(reply.read().decode("utf-8")).get("VALUE", [])
        except (urllib.error.URLError, OSError, ValueError):
            return ""
        if not values:
            return ""
        if isinstance(values[0], bool):
            return "true" if values[0] else "false"
        return str(values[0])

    def reachable(self) -> bool:
        return self.read("/godot/engine/tick") != ""

    def send(self, command: str, args=None) -> None:
        packet = common.osc_encode("/godot/cmd/" + command.replace(".", "/"), args or [])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(packet, (self.host, self.osc_port))


def tunnel_to_mac(port: int) -> "tuple[subprocess.Popen, int]":
    """An ssh tunnel to a port on the Mac's loopback, for a LAN that does not let us in."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        local = probe.getsockname()[1]
    process = subprocess.Popen(SSH + ["-N", "-L", f"{local}:127.0.0.1:{port}", MAC],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    return process, local


def as_float(text: str) -> float:
    try:
        return float(text)
    except (TypeError, ValueError):
        return float("nan")


def as_int(text: str) -> int:
    try:
        return int(text)
    except (TypeError, ValueError):
        return -1


def wait_for(predicate, seconds: float, every: float = 0.1) -> bool:
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(every)
    return bool(predicate())


ROWS = ["/godot/audio/device", "/godot/audio/outputLatency", "/godot/audio/inputLatency",
        "/godot/engine/sampleRate", "/godot/engine/blockSize", "/godot/engine/launchLatencyTicks",
        "/godot/engine/latenessMax", "/godot/engine/audioGaps", "/godot/engine/audioGapMax",
        "/godot/engine/audioCallbackMax", "/godot/engine/sampleZeroAt",
        "/godot/pair/role", "/godot/pair/state", "/godot/pair/linked", "/godot/pair/delay",
        "/godot/pair/roundTrip", "/godot/pair/mapped", "/godot/pair/inStep", "/godot/pair/differs",
        "/godot/pair/drift", "/godot/pair/clockResidual", "/godot/pair/late",
        "/godot/pair/outputLatency", "/godot/pair/peerOutputLatency", "/godot/pair/showMatch",
        "/godot/pair/showDiffers", "/godot/pair/alarm"]


def rows_of(node: Node) -> dict:
    return {address.removeprefix("/godot/"): node.read(address) for address in ROWS}


# =============================================================================
# The capture
# =============================================================================

def read_capture(path: Path) -> "tuple[array.array, int]":
    """The capture's samples, interleaved, and its channel count: 32-bit float, Go.dot's own."""
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
    samples = array.array("f")
    samples.frombytes(data[:len(data) - len(data) % (4 * channels)])
    if sys.byteorder != "little":
        samples.byteswap()
    return samples, channels


def onset(samples: array.array, channels: int, channel: int, start: int, end: int) -> "tuple[int, float]":
    """The first frame in [start, end) at half the window's peak or more, and the peak; (-1, peak) when
    the peak is under a fifth of the click's level - nothing heard."""
    frames = len(samples) // channels
    start, end = max(0, start), min(frames, end)
    if end <= start:
        return -1, 0.0
    window = samples[start * channels + channel:end * channels:channels]
    peak = max(max(window), -min(window))
    if peak < CLICK_LEVEL / 5:
        return -1, peak
    for offset, value in enumerate(window):
        if abs(value) >= 0.5 * peak:
            return start + offset, peak
    return -1, peak


def describe(values: "list[float]", unit: str, digits: int = 3) -> str:
    if not values:
        return "none"
    spread = max(values) - min(values)
    deviation = statistics.pstdev(values) if len(values) > 1 else 0.0
    return (f"mean {statistics.fmean(values):+.{digits}f} {unit}, min {min(values):+.{digits}f}, "
            f"max {max(values):+.{digits}f}, spread {spread:.{digits}f}, sd {deviation:.{digits}f} (n={len(values)})")


# =============================================================================
# One run
# =============================================================================

def find_run(node: Node, cue: str) -> str:
    for run in node.read("/godot/run/order").split():
        if node.read(f"/godot/run/{run}/cue") == cue:
            return run
    return ""


def fire(main: Node, pc: Node, count: int, send) -> "list[dict]":
    """N GOs at the main, a second apart; each one's run on the PC and its launch sample there."""
    send("node.set", [f"/godot/list/{LIST}/standby", cue_id(1)])
    if not wait_for(lambda: main.read(f"/godot/list/{LIST}/standby") == cue_id(1), 5.0):
        raise HarnessError(f"the {main.name}'s standby did not reach the first click")
    time.sleep(1.0)

    gos = []
    begun = time.monotonic()
    for n in range(1, count + 1):
        due = begun + (n - 1) * GO_EVERY
        time.sleep(max(0.0, due - time.monotonic()))
        send("go")
        cue = cue_id(n)
        found = {"n": n, "cue": cue, "run": "", "launch": 0, "late": ""}

        def placed():
            if not found["run"]:
                found["run"] = find_run(pc, cue)
            if found["run"]:
                found["launch"] = as_int(pc.read(f"/godot/run/{found['run']}/launchSample"))
            return found["launch"] > 0

        wait_for(placed, 0.8, 0.05)
        if found["run"]:
            found["late"] = pc.read(f"/godot/run/{found['run']}/late")
        gos.append(found)
        if n == 1 and not found["run"]:
            raise HarnessError(f"the first GO made no run of {cue} on the PC within 0.8 s")
    return gos


def analyse(capture: Path, gos: "list[dict]", pc_rows: dict, mac_rows: dict | None, pc_is_main: bool) -> dict:
    sidecar = json.loads(Path(str(capture) + ".json").read_text(encoding="utf-8"))
    first = sidecar.get("firstSample")
    say(f"  capture: {capture.name}, inputs {sidecar.get('inputs')}, first sample {first},"
        f" {sidecar.get('frames')} frames ({(sidecar.get('frames') or 0) / RATE:.1f} s),"
        f" dropped {sidecar.get('droppedFrames')}, closed {sidecar.get('closed')},"
        f" ended {sidecar.get('ended')!r}")
    if first is None:
        raise HarnessError("the capture's sidecar has no first sample: nothing was captured")

    samples, channels = read_capture(capture)
    window_before, window_after = int(0.3 * RATE), int(0.6 * RATE)
    pc_round, mac_round, jumps, rows = [], [], [], []

    for go in gos:
        if go["launch"] <= 0:
            rows.append(f"    GO {go['n']:2d}: no launch sample on the PC (run {go['run'] or 'none'})")
            continue
        at = go["launch"] - first + CLICK_AT
        heard = {}
        for label, pair in (("pc", PC_CHANNELS), ("mac", MAC_CHANNELS)):
            found = [onset(samples, channels, c, at - window_before, at + window_after) for c in pair]
            heard[label] = found
        pc_l, pc_r = heard["pc"][0][0], heard["pc"][1][0]
        mac_l, mac_r = heard["mac"][0][0], heard["mac"][1][0]
        line = f"    GO {go['n']:2d}: launch {go['launch']}, late {go['late'] or '0'} block(s);"
        if pc_l >= 0:
            trip = pc_l - at
            pc_round.append(trip)
            line += f" PC heard +{trip} samples ({trip * 1000 / RATE:.3f} ms, peak {heard['pc'][0][1]:.4f}"
            line += "" if pc_r == pc_l else f", right at {pc_r - at:+d}"
            line += ")"
        else:
            line += f" PC not heard (peak {heard['pc'][0][1]:.4f})"
        if mac_l >= 0:
            trip = mac_l - at
            mac_round.append(trip)
            line += f"; Mac heard +{trip} samples ({trip * 1000 / RATE:.3f} ms, peak {heard['mac'][0][1]:.4f}"
            line += "" if mac_r == mac_l else f", right at {mac_r - at:+d}"
            line += ")"
        else:
            line += f"; Mac not heard (peak {heard['mac'][0][1]:.4f})"
        if pc_l >= 0 and mac_l >= 0:
            jump = (mac_l - pc_l) * 1000.0 / RATE
            jumps.append(jump)
            line += f"; jump (Mac less PC) {jump:+.3f} ms"
        rows.append(line)

    for line in rows:
        say(line)

    result = {"pc_round_ms": [v * 1000 / RATE for v in pc_round], "mac_round_ms": [v * 1000 / RATE for v in mac_round],
              "jumps_ms": jumps, "sidecar": sidecar}
    say()
    say(f"  the PC's click back on inputs 5+6, from its launch sample: {describe(result['pc_round_ms'], 'ms')}")
    out_lat = as_int(pc_rows.get("audio/outputLatency"))
    in_lat = as_int(pc_rows.get("audio/inputLatency"))
    if out_lat >= 0 and in_lat >= 0:
        reported = out_lat + in_lat
        say(f"  the PC's driver reports output {out_lat} + input {in_lat} = {reported} samples"
            f" ({reported * 1000 / RATE:.3f} ms)")
        if pc_round:
            mean_trip = statistics.fmean(pc_round)
            say(f"  measured round trip less reported: {mean_trip - reported:+.1f} samples"
                f" ({(mean_trip - reported) * 1000 / RATE:+.3f} ms) - what the Dante network and anything"
                f" the driver does not report add")

    if mac_rows is not None:
        say(f"  the Mac's click on inputs 3+4, from the PC's launch sample: {describe(result['mac_round_ms'], 'ms')}")
        say(f"  M61, THE CHANGEOVER JUMP (the Mac's onset less the PC's): {describe(jumps, 'ms')}")
        pc_out = as_float(pc_rows.get("pair/outputLatency"))
        mac_out = as_float(mac_rows.get("pair/outputLatency"))
        pc_says_mac = as_float(pc_rows.get("pair/peerOutputLatency"))
        mac_says_pc = as_float(mac_rows.get("pair/peerOutputLatency"))
        say(f"  output latencies reported: PC {pc_out * 1000:.3f} ms (the Mac hears {mac_says_pc * 1000:.3f}),"
            f" Mac {mac_out * 1000:.3f} ms (the PC hears {pc_says_mac * 1000:.3f}); Mac less PC"
            f" {(mac_out - pc_out) * 1000:+.3f} ms")
        if jumps and not math.isnan(pc_out) and not math.isnan(mac_out):
            alignment = [j - (mac_out - pc_out) * 1000 for j in jumps]
            result["alignment_ms"] = alignment
            say(f"  ENGINE ALIGNMENT (the jump less the latencies' difference): {describe(alignment, 'ms')}")
        follower = mac_rows if pc_is_main else pc_rows
        leader = pc_rows if pc_is_main else mac_rows
        say(f"  the follower ({'Mac' if pc_is_main else 'PC'}): pair/drift {follower.get('pair/drift')} ppm,"
            f" pair/delay {follower.get('pair/delay')} tick(s), pair/clockResidual {follower.get('pair/clockResidual')} us,"
            f" mapped {follower.get('pair/mapped')}, inStep {follower.get('pair/inStep')}, late {follower.get('pair/late')}")
        say(f"  the leader ({'PC' if pc_is_main else 'Mac'}): pair/delay (extra lead) {leader.get('pair/delay')} tick(s),"
            f" pair/state {leader.get('pair/state')}")
    return result


def print_rows(name: str, rows: dict) -> None:
    say(f"  {name}:")
    for key, value in rows.items():
        if value != "":
            say(f"    {key} = {value}")


def run_order(args, stamp: str, bundle: Path, mac_show: str | None, pc_is_main: bool, out_dir: Path) -> dict:
    """One order of roles: start, link, fire, read, stop, analyse."""
    label = ("main-only" if mac_show is None else
             ("PC main, Mac backup" if pc_is_main else "Mac main, PC backup"))
    suffix = "" if pc_is_main else "-swapped"
    capture = out_dir / f"m61-{stamp}{suffix}.wav"
    pc_log = out_dir / f"m61-{stamp}{suffix}-pc.log"
    say()
    say(f"=== {label} ===")

    machine = Path(tempfile.mkdtemp(prefix="wfg-m61-")) / "engine"
    machine.mkdir(parents=True)
    mac_folder = f"{MAC_TEMP}/{stamp}{suffix}"
    pc_server = mac_serve = tunnel = None
    pc = mac = None
    result: dict = {"label": label}
    begun = time.monotonic()

    def start_pc(role: str, peer: str | None) -> Server:
        extra = ["--no-video-window", f"--device-type={PC_TYPE}", f"--capture={capture}",
                 f"--capture-inputs={CAPTURE_INPUTS}", "--link-timeout=1"]
        if peer:
            extra.append(f"--peer={peer}")
        return Server(bundle, log=pc_log, locale=args.locale, sample_rate=RATE, buffer_size=BLOCK,
                      device=PC_DEVICE, engine_folder=machine, role=role, extra_args=extra)

    def start_mac(role: str, peer: str | None) -> MacServe:
        wfg = args.mac_wfg if args.mac_wfg.startswith("/") else f"{args.home}/{args.mac_wfg}"
        return MacServe(wfg, f"{args.home}/{mac_show}", f"{args.home}/{mac_folder}", role, peer, args.mac_hosted)

    try:
        if mac_show is None or pc_is_main:
            pc_server = start_pc("main", None)
            pc = Node("127.0.0.1", pc_server.http_port, pc_server.osc_port, "PC")
            wait_for(lambda: pc.read("/godot/audio/device") != "", 5.0)
            say(f"  PC serve: pid {pc_server.process.pid}, http {pc_server.http_port}, osc {pc_server.osc_port},"
                f" interface {pc.read('/godot/audio/device')!r}")
            if mac_show is not None:
                url = f"http://{args.pc_host}:{pc_server.http_port}/godot/engine/tick?VALUE"
                code, text = ssh(f"curl -s -m 3 {shlex.quote(url)}")
                if code != 0 or "VALUE" not in text:
                    raise HarnessError(f"the Mac cannot reach the PC's serve at {args.pc_host}:{pc_server.http_port}"
                                       f" (a firewall?): {text[:200]}")
                mac_serve = start_mac("backup", f"{args.pc_host}:{pc_server.http_port}")
        else:
            mac_serve = start_mac("main", None)

        if mac_serve is not None:
            mac = Node(MAC_HOST, mac_serve.http_port, mac_serve.osc_port, "Mac")
            if not wait_for(mac.reachable, 3.0):
                tunnel, local = tunnel_to_mac(mac_serve.http_port)
                mac = Node("127.0.0.1", local, mac_serve.osc_port, "Mac")
                if not wait_for(mac.reachable, 5.0):
                    raise HarnessError("the Mac's serve answers neither over the LAN nor through a tunnel")
                say("  (the Mac's rows read through an ssh tunnel: the LAN did not let us in)")
            wait_for(lambda: mac.read("/godot/audio/device") != "", 5.0)
            say(f"  Mac serve: pid {mac_serve.pid}, http {mac_serve.http_port}, osc {mac_serve.osc_port},"
                f" interface {mac.read('/godot/audio/device')!r}")

        if mac_show is not None and not pc_is_main:
            pc_server = start_pc("backup", f"{MAC_HOST}:{mac_serve.http_port}")
            pc = Node("127.0.0.1", pc_server.http_port, pc_server.osc_port, "PC")
            wait_for(lambda: pc.read("/godot/audio/device") != "", 5.0)
            say(f"  PC serve: pid {pc_server.process.pid}, http {pc_server.http_port}, osc {pc_server.osc_port},"
                f" interface {pc.read('/godot/audio/device')!r}")

        main = pc if (mac_show is None or pc_is_main) else mac
        follower = None if mac_show is None else (mac if pc_is_main else pc)

        if follower is not None:
            linked = wait_for(lambda: follower.read("/godot/pair/state") == "following", 30.0)
            in_step = linked and wait_for(lambda: follower.read("/godot/pair/inStep") == "true", 30.0)
            mapped = in_step and wait_for(lambda: follower.read("/godot/pair/mapped") == "true", 30.0)
            say(f"  the {follower.name} follows: {linked}, in step: {in_step}, mapped: {mapped}"
                f" (after {time.monotonic() - begun:.1f} s; state {follower.read('/godot/pair/state')!r},"
                f" alarm {follower.read('/godot/pair/alarm')!r}, show {follower.read('/godot/pair/showDiffers')!r})")
            if not in_step:
                raise HarnessError(f"the {follower.name} never came into step with its main")
            time.sleep(2.0)     # the lines a few more stamps past their first

        send = main.send
        if main is mac:
            # Over the LAN to the Mac's UDP first; a Mac that drops it gets the commands from the PC
            # backup's WebSocket, which passes them on to its main (AFT).
            main.send("node.set", [f"/godot/list/{LIST}/standby", cue_id(1)])
            if not wait_for(lambda: main.read(f"/godot/list/{LIST}/standby") == cue_id(1), 3.0):
                client = common.WSClient(pc.http_port, "m61")

                def send(command, arguments=None):
                    client.send_osc("/godot/cmd/" + command.replace(".", "/"), arguments or [])
                say("  (the commands go to the PC backup's WebSocket and on to the Mac: its UDP did not answer)")

        gos = fire(main, pc, args.gos, send)
        result["gos"] = gos
        placed = sum(1 for go in gos if go["launch"] > 0)
        say(f"  {len(gos)} GOs fired at the {main.name}, {placed} placed on the PC")

        # The last click a second and a half past its launch, and a flush of the capture after it.
        time.sleep(2.5)
        sidecar = Path(str(capture) + ".json")
        last = max((go["launch"] for go in gos), default=0)

        def covered():
            try:
                data = json.loads(sidecar.read_text(encoding="utf-8"))
                return (data.get("firstSample") or 0) + (data.get("frames") or 0) > last + CLICK_AT + RATE // 2
            except (OSError, ValueError):
                return False

        wait_for(covered, 5.0, 0.2)

        result["pc_rows"] = rows_of(pc)
        result["mac_rows"] = rows_of(mac) if mac is not None else None
        print_rows("the PC's rows", result["pc_rows"])
        if result["mac_rows"] is not None:
            print_rows("the Mac's rows", result["mac_rows"])
    finally:
        # The backup first, so it never takes over from a main that went first.
        for stop in ([lambda: mac_serve and mac_serve.stop(), lambda: pc_server and pc_server.stop()] if pc_is_main
                     else [lambda: pc_server and pc_server.stop(), lambda: mac_serve and mac_serve.stop()]):
            try:
                stop()
            except HarnessError as error:
                say(f"  note: a serve's stop said: {error}")
        if tunnel is not None:
            tunnel.terminate()
        if mac_serve is not None:
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes", f"{MAC}:{mac_folder}/serve.log",
                            str(out_dir / f"m61-{stamp}{suffix}-mac.log")], capture_output=True, timeout=60)
        shutil.rmtree(machine.parent, ignore_errors=True)
        say(f"  stopped after {time.monotonic() - begun:.1f} s")

    result.update(analyse(capture, result["gos"], result["pc_rows"], result["mac_rows"], pc_is_main))
    return result


def our_pc_serves() -> str:
    """What wfg.exe is left on this PC; only this driver's serves are ever stopped."""
    if sys.platform != "win32":
        return ""
    done = subprocess.run(["tasklist", "/FI", "IMAGENAME eq wfg.exe", "/FO", "CSV", "/NH"],
                          capture_output=True, text=True)
    return done.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--main-only", action="store_true", help="the PC alone, heard back through its own loopback")
    mode.add_argument("--pair", action="store_true", help="the PC and the Mac, then swapped")
    parser.add_argument("--no-swap", action="store_true", help="with --pair: the PC main only, no swapped order")
    parser.add_argument("--mac-hosted", action="store_true", help="with --pair: the Mac on its hosted pump, a rehearsal")
    parser.add_argument("--gos", type=int, default=20)
    parser.add_argument("--wfg", default=str(REPO / "build" / "vs" / "src" / "Debug" / "wfg.exe"))
    parser.add_argument("--mac-wfg", default=MAC_FARM_WFG, help="the Mac's binary, under its home unless absolute")
    parser.add_argument("--pc-host", default="172.22.1.11", help="the PC's address the Mac dials")
    parser.add_argument("--locale", default="C")
    args = parser.parse_args()

    if not 1 <= args.gos <= 60:
        parser.error("--gos is 1 to 60: each run stays under two minutes")
    os.environ["WFG_BINARY"] = args.wfg

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = REPO / "build" / "redundancy-bench"
    out_dir.mkdir(parents=True, exist_ok=True)
    say(f"M61 over Dante, {stamp}: {'main only' if args.main_only else 'pair'}"
        f"{' (the Mac hosted: a rehearsal)' if args.mac_hosted else ''}, {args.gos} GOs a second apart,"
        f" click {20 * math.log10(CLICK_LEVEL):.0f} dBFS at frame {CLICK_AT} of a {CLICK_SECONDS} s file,"
        f" {RATE} Hz, {BLOCK}-frame blocks; PC binary {args.wfg}")

    if args.pair and not args.mac_hosted and not mac_has_dante():
        say("The Mac's CoreAudio lists no \"Dante Virtual Soundcard\": the two-machine M61 is owed to the bench."
            " Nothing was started.")
        write_report(out_dir, stamp)
        return 3

    scratch = Path(tempfile.mkdtemp(prefix="wfg-m61-show-"))
    show_name = f"m61-show-{stamp}"
    bundle = make_show(scratch / show_name, args.gos)
    mac_show = None
    results = []
    status = 0

    try:
        if args.pair:
            args.home = mac_home()
            code, text = ssh(f"mkdir -p {MAC_SHOWS} {MAC_TEMP}")
            if code != 0:
                raise HarnessError(f"the Mac would not make its folders: {text}")
            copied = subprocess.run(["scp", "-q", "-o", "BatchMode=yes", "-r", str(bundle),
                                     f"{MAC}:{MAC_SHOWS}/"], capture_output=True, text=True, timeout=60)
            if copied.returncode != 0:
                raise HarnessError(f"scp to the Mac failed: {copied.stderr.strip()}")
            mac_show = f"{MAC_SHOWS}/{show_name}"
            say(f"  the show copied to the Mac: ~/{mac_show}")

        results.append(run_order(args, stamp, bundle, mac_show, True, out_dir))

        if args.pair and not args.no_swap:
            clean = results[-1].get("jumps_ms") or args.mac_hosted
            if clean:
                results.append(run_order(args, stamp, bundle, mac_show, False, out_dir))
            else:
                say("  the swapped order not run: the first did not run cleanly")
    except (HarnessError, subprocess.TimeoutExpired, OSError) as error:
        say(f"FAILED: {error}")
        status = 1
    finally:
        if args.pair:
            left = mac_sweep(show_name)
            say(f"  the Mac's wfg processes left: {left or 'none'}")
            ssh(f"rm -rf {MAC_SHOWS}/{show_name} {MAC_TEMP}")
        say(f"  this PC's wfg.exe left: {our_pc_serves() or 'none'}")
        shutil.rmtree(scratch, ignore_errors=True)
        write_report(out_dir, stamp)

    return status


def write_report(out_dir: Path, stamp: str) -> None:
    report = out_dir / f"m61-{stamp}.txt"
    report.write_text("\n".join(say.lines) + "\n", encoding="utf-8")
    print(f"(written to {report})")


if __name__ == "__main__":
    sys.exit(main())
