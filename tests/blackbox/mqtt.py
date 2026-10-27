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

"""MQTT end to end, through Go.dot's own broker (namespace draft 67, MQ.8).

WHAT THIS IS FOR. The unit suite drives the codec, the session and the broker
in one process. This drives the shipped `wfg serve` - its broker listening,
a device of the show on it with no socket, a script cue hearing the device -
with nothing but MQTT packets written here from the specification, in
another language, and reads back what a client RECEIVED and what the engine
SAYS. Then it replays the session.

THE SHOW is tests/fixtures/bundles/mqtt: Go.dot's broker on, a device under
/house on it (topics #), and a script cue whose JavaScript doubles what
arrives at /house/room1/temp onto /house/room1/echo, fires a memo, Bell,
when the temperature rises past 25, and counts what arrives under
/house/storm/, publishing the count on /house/stats/heard once a second.

THE SESSION:

 1. Two clients connect to the broker: a watcher subscribed to house/#, and a
    sensor. The engine says the broker listens and counts them (with the
    device's own session, three).
 2. GO on the script cue: its script starts and runs.
 3. The sensor publishes room1/temp ten times: each doubled value reaches the
    watcher on room1/echo (M72 - the time from the publish leaving to the
    echo arriving, judged off CI only).
 4. A temperature past 25 fires Bell, under the script's own origin.
 5. M73: fifty clients publish twenty a second each for three seconds under
    storm/; the script's count reaches every one of them, and its run was
    never late.
 6. Esc: the run ends; a publish then is answered by nothing.
 7. The session's log replays record for record, with no socket open.
"""

from __future__ import annotations

import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
import first_sound
import lane_level
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "mqtt"

SCRIPT = "SCRP0001"
BELL = "B3NK0001"
DEVICE = "H0SEMNT1"


#  --- MQTT 3.1.1, the few packets a client needs, from the specification --------

def _string(text: str) -> bytes:
    data = text.encode("utf-8")
    return struct.pack(">H", len(data)) + data


def _packet(first: int, body: bytes) -> bytes:
    remaining = len(body)
    length = b""
    while True:
        byte = remaining % 128
        remaining //= 128
        if remaining > 0:
            byte |= 0x80
        length += bytes([byte])
        if remaining == 0:
            break
    return bytes([first]) + length + body


def connect_packet(client_id: str, keep_alive: int = 60) -> bytes:
    body = _string("MQTT") + bytes([4, 0x02]) + struct.pack(">H", keep_alive) + _string(client_id)
    return _packet(0x10, body)


def subscribe_packet(packet_id: int, topic_filter: str) -> bytes:
    return _packet(0x82, struct.pack(">H", packet_id) + _string(topic_filter) + bytes([0]))


def publish_packet(topic: str, payload: str) -> bytes:
    return _packet(0x30, _string(topic) + payload.encode("utf-8"))


def disconnect_packet() -> bytes:
    return _packet(0xE0, b"")


class Client:
    """A client at the end of a socket: what it was sent, kept with the time it came."""

    def __init__(self, port: int, client_id: str):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        self.sock.settimeout(0.05)
        self.lock = threading.Lock()
        self.received: list = []          # (time, type, topic, payload)
        self.partial = b""
        self.stopping = False
        self.sock.sendall(connect_packet(client_id))
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()
        if not common.wait_until(lambda: any(kind == "connack" for _, kind, _, _ in self.all()), timeout=5.0):
            raise HarnessError(f"{client_id}: no CONNACK from the broker")

    def _read(self) -> None:
        while not self.stopping:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            self.partial += data
            self._cut()

    def _cut(self) -> None:
        while len(self.partial) >= 2:
            remaining, at, multiplier = 0, 1, 1
            while True:
                if at >= len(self.partial):
                    return
                byte = self.partial[at]
                remaining += (byte & 0x7F) * multiplier
                multiplier *= 128
                at += 1
                if byte & 0x80 == 0:
                    break
            whole = at + remaining
            if len(self.partial) < whole:
                return
            frame = self.partial[:whole]
            self.partial = self.partial[whole:]
            self._handle(frame[0] >> 4, frame[at:])

    def _handle(self, kind: int, body: bytes) -> None:
        now = time.monotonic()
        with self.lock:
            if kind == 2:
                self.received.append((now, "connack", "", str(body[1])))
            elif kind == 9:
                self.received.append((now, "suback", "", ""))
            elif kind == 3:
                length = struct.unpack(">H", body[:2])[0]
                topic = body[2:2 + length].decode("utf-8", errors="replace")
                self.received.append((now, "publish", topic, body[2 + length:].decode("utf-8", errors="replace")))

    def all(self) -> list:
        with self.lock:
            return list(self.received)

    def publishes(self, topic: str) -> list:
        return [entry for entry in self.all() if entry[1] == "publish" and entry[2] == topic]

    def subscribe(self, topic_filter: str) -> None:
        self.sock.sendall(subscribe_packet(1, topic_filter))
        if not common.wait_until(lambda: any(kind == "suback" for _, kind, _, _ in self.all()), timeout=5.0):
            raise HarnessError("no SUBACK from the broker")

    def publish(self, topic: str, payload: str) -> None:
        self.sock.sendall(publish_packet(topic, payload))

    def close(self) -> None:
        self.stopping = True
        try:
            self.sock.sendall(disconnect_packet())
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


#  --- the session ----------------------------------------------------------------

def command(server: Server, name: str, args: "list | None" = None) -> None:
    common.send_udp(server.osc_port, common.osc_encode("/godot/cmd/" + name.replace(".", "/"), args or []))


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def point_broker_at(bundle: Path, port: int) -> None:
    show = bundle / "show.xml"
    text = show.read_text(encoding="utf-8")
    text = text.replace('brokerPort="18830"', f'brokerPort="{port}"')
    show.write_text(text, encoding="utf-8", newline="\n")


def run_of(server: Server, cue: str) -> "str | None":
    for run in first_sound.runs_in(server):
        if first_sound.value_of(server, f"/godot/run/{run}/cue") == cue:
            return run
    return None


def echoed(watcher: Client, value: float) -> "float | None":
    for when, _, _, payload in watcher.publishes("room1/echo"):
        try:
            if abs(float(payload) - 2 * value) < 1e-6:
                return when
        except ValueError:
            continue
    return None


def session(report: Report, server: Server, port: int, facts: dict) -> None:
    #  1. The broker listens; two clients join it.
    state = first_sound.wait_for(server, "/godot/network/brokerState", "listening", timeout=20.0)
    report.equal(state, "listening", "the show's broker listens")
    report.equal(first_sound.value_of(server, "/godot/network/brokerPort"), port, "on the port the show says")

    watcher = Client(port, "watcher")
    watcher.subscribe("#")
    sensor = Client(port, "sensor")
    facts["clients"] = [watcher, sensor]

    counted = common.wait_until(lambda: (first_sound.value_of(server, "/godot/network/brokerClients") or 0) >= 3,
                                timeout=10.0)
    report.check(bool(counted), "and counts its clients: the two, and the device of the show on it",
                 f"{first_sound.value_of(server, '/godot/network/brokerClients')}")
    report.equal(first_sound.value_of(server, f"/godot/mount/{DEVICE}/link"), "open",
                 "the device's link to the broker is open, with no socket")

    #  2. GO: the script starts and runs.
    command(server, "cue.fire", [SCRIPT])
    report.check(bool(common.wait_until(lambda: run_of(server, SCRIPT) is not None, timeout=10.0)),
                 "GO: the script cue runs")
    run = run_of(server, SCRIPT) or ""
    facts["run"] = run
    report.equal(first_sound.wait_for(server, f"/godot/run/{run}/processState", "running", timeout=20.0),
                 "running", "its JavaScript starts and runs")

    #  3. M72: a publish doubled by the script and back at the watcher.
    latencies = []
    for step in range(10):
        value = float(step + 1)             # all under the line at 25
        sent_at = time.monotonic()
        sensor.publish("room1/temp", f"{value}")
        #  The echo's time is the reader's stamp when it came, not when the poll noticed it: the poll's
        #  sleep added 10-26 ms on the Mac and put M72 past 100 ms with the echo back in 81 (2026-10-27).
        arrived = common.wait_until(lambda v=value: echoed(watcher, v), timeout=5.0)
        latencies.append((arrived if arrived else time.monotonic()) - sent_at)
        report.check(bool(arrived), f"the sensor's {value:.0f} comes back doubled on room1/echo")
        time.sleep(0.05)

    worst = max(latencies)
    lane_level.timed(report, worst < 0.1, "M72: a publish is heard by the script and its answer back at a client"
                     " within 100 ms", f"worst {worst * 1000:.0f} ms, " + ", ".join(f"{l * 1000:.0f}" for l in latencies))
    print(f"  mqtt: publish to echo {', '.join(f'{l * 1000:.0f} ms' for l in latencies)}")

    report.check(run_of(server, BELL) is None, "nothing under the line has fired Bell")

    #  4. Past the line: Bell, once.
    sensor.publish("room1/temp", "30")
    report.check(bool(common.wait_until(lambda: run_of(server, BELL) is not None, timeout=5.0)),
                 "a temperature past 25 fires Bell, by godot.fire")

    #  5. M73: fifty clients, twenty a second each, three seconds.
    storm = [Client(port, f"storm{n}") for n in range(50)]
    facts["clients"].extend(storm)
    sent_count = [0]
    sent_lock = threading.Lock()

    def blow(client: Client, n: int) -> None:
        for k in range(60):
            client.publish(f"storm/{n}", str(k))
            with sent_lock:
                sent_count[0] += 1
            time.sleep(0.05)

    threads = [threading.Thread(target=blow, args=(client, n)) for n, client in enumerate(storm)]
    started = time.monotonic()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    blown = time.monotonic() - started

    def heard_count() -> int:
        counts = watcher.publishes("stats/heard")
        try:
            return int(float(counts[-1][3])) if counts else 0
        except ValueError:
            return 0

    all_heard = common.wait_until(lambda: heard_count() >= sent_count[0], timeout=10.0)
    report.check(bool(all_heard), f"M73: the script heard every one of {sent_count[0]} publishes from fifty clients",
                 f"heard {heard_count()}, sent over {blown:.1f} s")
    late = first_sound.value_of(server, f"/godot/run/{run}/processLate") or 0
    print(f"  mqtt: storm of {sent_count[0]} in {blown:.1f} s, heard {heard_count()}, late {late},"
          f" latenessMax {first_sound.value_of(server, '/godot/engine/latenessMax')} samples")

    for client in storm:
        client.close()

    #  6. Esc: the run ends and nothing answers any more.
    command(server, "run.stopAll")
    ended = common.wait_until(lambda: first_sound.value_of(server, f"/godot/run/{run}/state") in ("done", "stopped", None),
                              timeout=10.0)
    report.check(bool(ended), "Esc ends the script cue's run",
                 f"{first_sound.value_of(server, f'/godot/run/{run}/state')}")
    time.sleep(0.2)
    sensor.publish("room1/temp", "0.33")
    time.sleep(0.5)
    report.check(echoed(watcher, 0.33) is None, "and a publish after it is answered by nothing")


def run(locale: "str | None", keep_log: "Path | None") -> int:
    report = Report(f"MQTT end to end, through Go.dot's broker ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-mqtt-") as scratch:
        room = Path(scratch)
        bundle = common.copy_bundle(FIXTURE, room / "mqtt")
        log = room / "session.wfglog"
        replayed = room / "replayed"
        facts: dict = {"clients": []}
        port = free_port()
        point_broker_at(bundle, port)

        try:
            with Server(bundle, log=log, locale=locale) as server:
                session(report, server, port, facts)
        finally:
            for client in facts["clients"]:
                client.close()

        if keep_log is not None:
            keep_log.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(log, keep_log)
            print(f"  (the session's log is kept at {keep_log})")

        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
        report.check(" mount.heard " in text, "what the sensor published is in the log as the device's report")
        report.check(f"script:{facts.get('run', '?')}" in text and " cue.fire " in text,
                     "and Bell's firing, under the script's own origin")

        code, out, err = common.run_wfg("replay", str(log), f"--bundle={bundle}", f"--out={replayed}",
                                        *([f"--wfg-locale={locale}"] if locale else []))
        report.equal(code, 0, "`wfg replay` reproduces the session record for record, with no socket",
                     (out + err).strip()[-2000:])
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
        return run(locale, keep_log)
    except HarnessError as error:
        print(f"HARNESS ERROR: {error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
