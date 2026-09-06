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

"""What a black-box driver needs, and nothing that knows how Go.dot works.

STDLIB ONLY. No requests, no websockets, no pytest. This runs on three CI
runners and on a contributor's machine, and every dependency is one more thing
that can be missing on one of them at the moment somebody needs the test most.
The RFC 6455 client below is ~80 lines and has no other cost.

BLACK BOX MEANS BLACK BOX. Nothing here imports the engine, links the library or
reads a header. It launches the shipped binary, speaks the protocols a real
client speaks, and reads what comes back. That is the point: the unit suite
already checks that the engine agrees with itself, and what it cannot check is
whether the thing we ship answers a socket.

A SEPARATE OSC CODEC, deliberately. It is small, it is written from the
specification, and it shares no code with the C++ one under test. A driver that
encoded through the implementation it is checking would agree with it about any
mistake they both made — which is the same reason the byte fixtures in
OscCodecTests.cpp are hand-written.
"""

from __future__ import annotations

import base64
import json
import os
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

HOST = "127.0.0.1"

# Generous, because these are CI runners under load rather than a workstation.
# A timeout that fires spuriously teaches people to re-run rather than to read.
STARTUP_TIMEOUT = 30.0
REPLY_TIMEOUT = 15.0


class HarnessError(RuntimeError):
    """Something the driver could not do. Distinct from a test FAILING."""


# =============================================================================
# OSC, from the specification
# =============================================================================

def _pad(n: int) -> int:
    return (n + 3) & ~3


def osc_string(text: str) -> bytes:
    raw = text.encode("ascii") + b"\0"
    return raw + b"\0" * (_pad(len(raw)) - len(raw))


def osc_encode(address: str, args: "list | None" = None) -> bytes:
    """One OSC message. Supports the types this harness actually sends."""
    args = args or []
    tags = ","
    payload = b""

    for value in args:
        if isinstance(value, bool):
            tags += "T" if value else "F"
        elif isinstance(value, int):
            tags += "i"
            payload += struct.pack(">i", value)
        elif isinstance(value, float):
            tags += "f"
            payload += struct.pack(">f", value)
        elif isinstance(value, str):
            tags += "s"
            payload += osc_string(value)
        else:
            raise HarnessError(f"cannot encode {type(value).__name__} as OSC")

    return osc_string(address) + osc_string(tags) + payload


def osc_decode(data: bytes):
    """(address, [values]) or None. Refuses rather than guesses, like the codec
    it is checking: a decoder that fabricates a zero for a truncated float is
    how three parameter changes nobody made reach a fader."""
    def read_string(pos: int):
        end = data.find(b"\0", pos)
        if end < 0:
            return None, pos
        return data[pos:end].decode("ascii", "replace"), _pad(end + 1 - pos) + pos

    address, pos = read_string(0)
    if address is None:
        return None

    tags, pos = read_string(pos)
    if tags is None or not tags.startswith(","):
        return None

    values = []
    for tag in tags[1:]:
        if tag == "i":
            if pos + 4 > len(data):
                return None
            values.append(struct.unpack_from(">i", data, pos)[0])
            pos += 4
        elif tag == "h":
            if pos + 8 > len(data):
                return None
            values.append(struct.unpack_from(">q", data, pos)[0])
            pos += 8
        elif tag == "f":
            if pos + 4 > len(data):
                return None
            values.append(struct.unpack_from(">f", data, pos)[0])
            pos += 4
        elif tag == "d":
            if pos + 8 > len(data):
                return None
            values.append(struct.unpack_from(">d", data, pos)[0])
            pos += 8
        elif tag == "s":
            text, pos = read_string(pos)
            if text is None:
                return None
            values.append(text)
        elif tag == "T":
            values.append(True)
        elif tag == "F":
            values.append(False)
        else:
            return None                 # an unknown tag takes the message with it

    return address, values


def send_udp(port: int, packet: bytes) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(packet, (HOST, port))


# =============================================================================
# HTTP, over a raw socket
# =============================================================================

def http_get(port: int, target: str) -> "tuple[int, str]":
    """(status, body).

    A raw socket rather than urllib, for the reason the C++ tests use one:
    OSCQuery's attribute queries are BARE keys — `?VALUE`, not `?VALUE=` — and a
    client library that normalises the query string asks a different question
    from the one intended. urllib also hides a 204's status behind an exception
    path, and 204 is a status this suite asserts.
    """
    request = (f"GET {target} HTTP/1.1\r\n"
               f"Host: {HOST}:{port}\r\n"
               f"Connection: close\r\n\r\n").encode("ascii")

    with socket.create_connection((HOST, port), timeout=REPLY_TIMEOUT) as sock:
        sock.sendall(request)
        chunks = []
        while True:
            block = sock.recv(65536)
            if not block:
                break
            chunks.append(block)

    response = b"".join(chunks)
    head, _, body = response.partition(b"\r\n\r\n")
    first = head.split(b"\r\n", 1)[0].decode("ascii", "replace")

    try:
        status = int(first.split(" ")[1])
    except (IndexError, ValueError):
        raise HarnessError(f"unparseable status line: {first!r}")

    return status, body.decode("utf-8", "replace")


def http_json(port: int, target: str):
    status, body = http_get(port, target)

    if status != 200:
        raise HarnessError(f"GET {target} returned {status}, expected 200")

    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise HarnessError(f"GET {target} returned unparseable JSON: {exc}")


# =============================================================================
# WebSocket, RFC 6455, client side only
# =============================================================================

class WSClient:
    """Text frames in and out, binary frames decoded as OSC.

    Started from WFS-DIY's oscquery_echo_check.py, which is the author's own
    RFC 6455 client and already proven against this same server module.
    """

    def __init__(self, port: int, name: str = "ws"):
        self.name = name
        self.pushes: "list[tuple[str, list]]" = []
        self.texts: "list[str]" = []
        self._lock = threading.Lock()
        self._closing = False

        self._sock = socket.create_connection((HOST, port), timeout=REPLY_TIMEOUT)

        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        self._sock.sendall(
            (f"GET / HTTP/1.1\r\n"
             f"Host: {HOST}:{port}\r\n"
             f"Upgrade: websocket\r\n"
             f"Connection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\n"
             f"Sec-WebSocket-Version: 13\r\n\r\n").encode("ascii"))

        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise HarnessError(f"[{name}] the server closed during the handshake")
            response += chunk

        status = response.split(b"\r\n", 1)[0].decode("ascii", "replace")

        if " 101 " not in status + " ":
            raise HarnessError(f"[{name}] handshake refused: {status}")

        self._buf = response.split(b"\r\n\r\n", 1)[1]
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # -- sending ------------------------------------------------------------

    def _frame(self, opcode: int, payload: bytes) -> None:
        mask = secrets.token_bytes(4)
        header = bytes([0x80 | opcode])
        n = len(payload)

        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)

        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(header + mask + masked)

    def send_text(self, text: str) -> None:
        self._frame(0x1, text.encode("utf-8"))

    def send_osc(self, address: str, args: "list | None" = None) -> None:
        self._frame(0x2, osc_encode(address, args))

    def listen(self, address: str) -> None:
        self.send_text(json.dumps({"COMMAND": "LISTEN", "DATA": address}))

    def ignore(self, address: str) -> None:
        self.send_text(json.dumps({"COMMAND": "IGNORE", "DATA": address}))

    # -- receiving ----------------------------------------------------------

    def _recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self._buf += chunk

        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _read_loop(self) -> None:
        try:
            while not self._closing:
                first = self._recv_exact(2)
                opcode = first[0] & 0x0F
                length = first[1] & 0x7F

                if length == 126:
                    length = struct.unpack(">H", self._recv_exact(2))[0]
                elif length == 127:
                    length = struct.unpack(">Q", self._recv_exact(8))[0]

                payload = self._recv_exact(length) if length else b""

                if opcode == 0x8:                       # close
                    return
                if opcode == 0x1:
                    with self._lock:
                        self.texts.append(payload.decode("utf-8", "replace"))
                elif opcode == 0x2:
                    decoded = osc_decode(payload)
                    if decoded is not None:
                        with self._lock:
                            self.pushes.append(decoded)
        except (OSError, ConnectionError):
            return

    def pushes_for(self, address: str) -> list:
        with self._lock:
            return [values for addr, values in self.pushes if addr == address]

    def wait_for_push(self, address: str, timeout: float = REPLY_TIMEOUT):
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            found = self.pushes_for(address)
            if found:
                return found[-1]
            time.sleep(0.02)

        return None

    def close(self) -> None:
        self._closing = True
        try:
            self._frame(0x8, b"")
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass


# =============================================================================
# The binary under test
# =============================================================================

def find_binary() -> Path:
    """The `wfg` this run should exercise.

    WFG_BINARY first, because ctest passes $<TARGET_FILE:wfg> and that is the
    only spelling that works on both the single-config Ninja jobs and the
    multi-config Windows one. The search below is for a person running this by
    hand.
    """
    override = os.environ.get("WFG_BINARY")

    if override:
        path = Path(override)
        if not path.is_file():
            raise HarnessError(f"WFG_BINARY is set to {path}, which is not a file")
        return path

    root = Path(__file__).resolve().parent.parent.parent
    name = "wfg.exe" if sys.platform == "win32" else "wfg"

    for candidate in sorted(root.glob(f"build/*/src/**/{name}")):
        if candidate.is_file():
            return candidate

    raise HarnessError(
        "cannot find a wfg binary. Build one, or set WFG_BINARY to its path.")


class Server:
    """A running `wfg serve`, on ports the OS chose.

    PORT 0 FOR BOTH, always. ctest runs in parallel and a fixed port makes a
    suite that cannot run twice at once; it also makes two developers on one
    build machine collide. The bound numbers are read back off stdout, which is
    why `serve` prints them in a fixed shape.
    """

    def __init__(self, bundle: Path, log: "Path | None" = None,
                 locale: "str | None" = None,
                 sample_rate: int = 48000, buffer_size: int = 128,
                 hosted: bool = False, render: "Path | None" = None):
        argv = [str(find_binary()), "serve", str(bundle),
                f"--sample-rate={sample_rate}", f"--buffer={buffer_size}",
                "--http-port=0", "--osc-port=0"]

        # --hosted puts a real Tracktion graph under the clock instead of a
        # dummy one, and --render writes what comes out of it to a WAV. That
        # pair is how "GO makes a sound" is asserted on a runner with no audio
        # interface: the same code path a device would drive, minus the device,
        # with the samples on disk afterwards for anybody to look at.
        if hosted:
            argv.append("--hosted")
        if render is not None:
            argv.append(f"--render={render}")

        if log is not None:
            argv.append(f"--log={log}")
        if locale is not None:
            argv.append(f"--wfg-locale={locale}")

        self.argv = argv
        self.process = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        self.http_port = 0
        self.osc_port = 0
        self._stderr: "list[str]" = []

        deadline = time.monotonic() + STARTUP_TIMEOUT

        while (self.http_port == 0 or self.osc_port == 0) \
                and time.monotonic() < deadline:
            line = self.process.stdout.readline()

            if not line:
                if self.process.poll() is not None:
                    raise HarnessError(
                        f"serve exited {self.process.returncode} before it was ready:\n"
                        + self.process.stderr.read())
                continue

            parts = line.split()

            if len(parts) == 3 and parts[0] == "wfg:":
                if parts[1] == "http":
                    self.http_port = int(parts[2])
                elif parts[1] == "osc":
                    self.osc_port = int(parts[2])

        if self.http_port == 0 or self.osc_port == 0:
            self.stop()
            raise HarnessError(
                f"serve did not report both ports within {STARTUP_TIMEOUT}s")

    def __enter__(self) -> "Server":
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)


def run_wfg(*args: str) -> "tuple[int, str, str]":
    """One shot of the binary. (exit code, stdout, stderr)."""
    done = subprocess.run([str(find_binary()), *args],
                          capture_output=True, text=True, timeout=120)
    return done.returncode, done.stdout, done.stderr


# =============================================================================
# Reporting
# =============================================================================

class Report:
    """Checks, counted, with every failure printed rather than the first.

    A driver that stops at the first failure makes each CI round trip worth one
    fact. This one runs everything it can and prints all of it.
    """

    def __init__(self, title: str):
        self.title = title
        self.failures: "list[str]" = []
        self.checks = 0
        print(f"=== {title} ===")

    def check(self, condition: bool, description: str, detail: str = "") -> bool:
        self.checks += 1

        if condition:
            print(f"  ok   {description}")
            return True

        line = f"{description}" + (f"\n         {detail}" if detail else "")
        print(f"  FAIL {line}")
        self.failures.append(line)
        return False

    def equal(self, actual, expected, description: str, detail: str = "") -> bool:
        note = f"expected {expected!r}, got {actual!r}"

        if detail:
            note += "\n         " + detail

        return self.check(actual == expected, description, note)

    def finish(self) -> int:
        print()

        if self.failures:
            print(f"{self.title}: FAILED — "
                  f"{len(self.failures)} of {self.checks} checks")
            return 1

        print(f"{self.title}: ok — {self.checks} checks")
        return 0


def copy_bundle(source: Path, destination: Path) -> Path:
    """A writable copy, because a session that saves must not edit the fixture."""
    if destination.exists():
        shutil.rmtree(destination)

    shutil.copytree(source, destination)
    return destination
