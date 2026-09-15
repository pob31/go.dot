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

"""M24: performance.now() around the web console's render() on a 500-cue show.

Usage:   python3 scripts/measure-console-render.py [--binary P] [--ui DIR |
             --ui-rev REV] [--cues N] [--polls N] [--browser P] [--wrap NAME]
             [--wait S] [--go-wait S] [--json] [--keep]
Produces: a readable report on stdout - or, with --json, the report on stderr
         and one JSON object on stdout.
Exits:   0 measured, 1 the instrument could not do something it needed (it
         says what, and how far it got), 2 bad arguments.

A HAND INSTRUMENT, NOT A TEST. Nothing registers this with ctest and nothing
should: §14.15 refuses browser automation on CI - a browser download on three
runners, on every job, to assert what two cheaper checks cover - and a wall
clock on a shared runner is a flaky test that teaches people to re-run the
suite (§14.14). What this prints is milliseconds for a person to quote in a
pull request, which is where M24 says the number goes. It asserts nothing.

WHAT IT MEASURES (§14.14's M24, for the render and its split; §14.3 for the
two defects and the trigger index). It writes the 500-cue show with
tests/fixtures/make_large_show.py - M18's shape plus what a console draws -
into a temporary folder, serves it with `wfg serve --ui`, opens the page in a
headless Chromium (Edge, else Chrome, else chromium) and speaks the Chrome
DevTools Protocol to it over a WebSocket, with the RFC 6455 client below.
Then, inside the page:

  (a) RENDER. The page's own `render` is wrapped, and each of `renderStrip`,
      `renderLists`, `renderAim`, `renderRuns`, `renderInspector`,
      `tree.triggersOf` and `tree.overlaps` - by name, through the global
      binding `poll()` and `render()` look them up by, so the page runs its
      own code and nothing else - and the page is left to poll on its own. Per
      render: the total's median and worst, and each part's median. The two
      render costs M24 asks to see apart are reported APART, because keying
      the rows fixes only the first: the rows (§14.3's first defect -
      `renderLists` less the `tree.triggersOf` and `tree.overlaps` calls made
      inside it, however deep) and the trigger scan (§14.3's index -
      `triggersOf`, milliseconds and calls per render, from every caller).
      `overlaps` is neither, and is reported beside them. A name added with
      `--wrap` is the rows' own work and is not taken out of the rows' figure.
      What the poll costs OUTSIDE render() is timed too and labelled as not
      M24's number: the reply's size, download and JSON.parse, the work
      between the parsed reply and render() (`flatten`, and whatever else a
      page does there), the style and layout a render leaves behind (forced
      straight after it), and HOW OLD the tree it drew was.
  (b) THE SCROLL, the one §14.3's first defect was said to lose. #cues is
      scrolled to the middle, three polls go by, and it is read back: kept,
      or snapped to the top. Twice: once by setting scrollTop, and once with
      real mouse-wheel events, so that an answer cannot be an artefact of
      scrolling from script.
  (c) THE FOCUS, §14.3's second defect. First the operator's path: the aim
      slider is clicked, then Space is pressed; GO reaching the engine is
      read over HTTP, from the focused list's `history`, which records a GO
      as a GO. A page that lets go of the slider on `change` - PR 5.9's does,
      and a click that moves the thumb is a change - has taken the focus off
      it before Space is pressed, so that answer says whether the operator's
      GO got through and nothing about the guard, and the report says which
      it was. Then a CONTROL: focus taken off everything
      and Space again, which must fire, or no answer here says anything. Then
      THE GUARD ITSELF: the slider given the focus from script with its value
      untouched, so that nothing lets go of it; Space, which must be GO; and
      an arrow, which must move the standby and leave the slider's value
      where it was - ArrowDown, or ArrowUp when the slider sits at its
      minimum, where ArrowDown would leave it there whoever took the key.
      Last, a BUTTON holding the focus (the transport's `standby.clear`):
      Space is GO, and the button's own command must not be sent as well -
      it would leave the list with no standby, which is what is read.
  (d) ESCAPE. A cue row is clicked, its name field in the inspector focused,
      a few characters typed with Input.insertText, Escape pressed; two polls
      later: where the focus is, whether the field holds the typing or the
      tree's value, and whether the engine's name changed.

INSTALLED BEFORE THE PAGE RUNS. The instrument goes in with
Page.addScriptToEvaluateOnNewDocument and the page is navigated to after, so
its very first request is timed and its functions are wrapped at
DOMContentLoaded, before any reply has come back. Installing into a page
already running did not work on the page before PR 5.9 (clients/console at
7154bb2, the commit PR 5.9 sits on), and the reason was that page's: its
`setInterval` started a poll every 100 ms whether or not the last one had
finished, so once a reply took longer than that - an 8.6 MB tree at 500 cues
did - requests queued in the browser, and every reply drawn afterwards had
been asked for before the instrument existed. `flatten` is not wrapped: the
gap between the parsed reply and render() times the same work without
depending on its name. (Installing into that running page would have had a
second flaw there: its poll read `flatten(await reply.json(), {})`, which
looks the callee up before the await, so a wrapper put in during a download
was bypassed at the root and entered by every child.)

WHAT THE PAGE BEFORE PR 5.9 DID AT 500 CUES (clients/console at 7154bb2), so
that nobody serving it with --ui-rev mistakes it for the instrument failing
(PR 5.9's before-measurement, Edge 153, the Debug engine). Its first rows took
from 10 s to 173 s to appear, its early requests ending in "Failed to fetch";
the tree it drew had been requested 14 s to 191 s before it was drawn; and in
four runs of seven the poll never recovered - about 1350 requests left
unanswered, every new one failing at once, one render or none in four
minutes. A page that does that is reported as doing it: a render count short
of --polls, and "not tested" for a scroll no poll redrew. --wait bounds each
wait (240 s by default).

BEFORE AND AFTER. `--ui` serves any folder, so a committed page can be
measured from a copy; `--ui-rev REV` makes the copy itself, from git -
`--ui-rev HEAD` is "before" while the working tree is "after". To reproduce
PR 5.9's before-measurement, use --ui-rev 7154bb2. Functions are found by
name, so a page that renames one reports it as absent rather than failing;
`--wrap NAME` (repeatable, `a.b` for a method) adds one the after page grew,
so its cost is not quietly moved out of the number.

WHAT IT DOES NOT CHANGE. The page and the engine are the shipped ones; the
wrappers call the originals with the same arguments and `this`. The one
liberty is the poll's reply: `response.json()` is answered as
`JSON.parse(await response.text())` - the same thing, in two steps, so the
download and the parse can be timed apart.

CLEAN-UP IS THE INSTRUMENT'S JOB, error or not: the engine, every browser
process started with this run's profile (on Windows the browser's launcher
exits and leaves its tree behind, so they are found by the temporary folder's
name and the browser's image; elsewhere by the profile's full path, which the
engine's command line never holds), and the temporary folder. An engine or a
browser that fails to start is stopped before the error is raised. `--keep`
leaves the folder for a look, and it is never removed automatically - that is
yours to do. A hard kill from outside runs no `finally`, so on Windows the
processes are tied to this one by a job object and die with it however it
dies, and a folder left by a killed run is swept by the next run once its
maker is gone.

STDLIB ONLY, for tests/blackbox/common.py's reasons. NOTHING HERE IMPORTS THE
ENGINE, and the WebSocket client is written again rather than imported from
the black-box suite: that one speaks OSC to the engine and this one speaks
JSON to a browser, and neither should grow the other's habits.
"""

from __future__ import annotations

import argparse
import base64
import collections
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAKE_SHOW = REPO / "tests" / "fixtures" / "make_large_show.py"
HOST = "127.0.0.1"

STARTUP_TIMEOUT = 60.0
CALL_TIMEOUT = 60.0

# The functions M24 names, in the order render() calls them. `render` is the
# frame every other one is attributed to and must be first.
DEFAULT_WRAP = ["render", "renderStrip", "renderLists", "renderAim", "renderRuns",
                "renderInspector", "tree.triggersOf", "tree.overlaps"]
RENDER_PARTS = DEFAULT_WRAP[1:6]

TYPED = "-m24"
WORKSPACE_PREFIX = "go.dot-m24-"


class HarnessError(RuntimeError):
    """Something the instrument could not do. Distinct from a slow page."""


def say(text: str) -> None:
    """Progress, always on stderr, so stdout is the report or the JSON."""
    print(f"m24: {text}", file=sys.stderr, flush=True)


# =============================================================================
# Leaving nothing behind
# =============================================================================

def tie_children_to_this_process():
    """On Windows, every process this one starts dies with it - however it dies.

    A `finally` runs when the instrument fails and when it is interrupted; it
    does not run when something outside kills it, and a killed run of this
    left nine headless browser processes behind. So this process puts itself
    in a job object that kills everything in it when its last handle closes.
    Children join the job as they are created, the handle is this process's
    alone, and Windows closes it at exit whatever the exit was. ctypes is
    stdlib; nothing is installed. Elsewhere, and wherever the job cannot be
    had (a process already in a job that forbids nesting), it does nothing
    and the `finally` is the only clean-up - which is what it was before."""
    if sys.platform != "win32":
        return None

    try:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                     ctypes.c_void_p, wintypes.DWORD]
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]

        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                        ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD),
                        ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t),
                        ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t),
                        ("PriorityClass", wintypes.DWORD),
                        ("SchedulingClass", wintypes.DWORD)]

        class Counters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                         "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

        class Extended(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", Basic), ("IoInfo", Counters),
                        ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]

        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            return None

        limits = Extended()
        limits.BasicLimitInformation.LimitFlags = 0x2000      # KILL_ON_JOB_CLOSE
        extended_limit_information = 9

        if not kernel32.SetInformationJobObject(job, extended_limit_information,
                                                ctypes.byref(limits), ctypes.sizeof(limits)):
            return None

        if not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
            return None

        return job                       # kept by the caller, never closed by hand
    except (OSError, AttributeError, ValueError):
        return None


def process_alive(pid: int) -> bool:
    """Whether a process exists, asked without touching it.

    NOT os.kill(pid, 0): on Windows every signal but the two console ones is
    TerminateProcess, so the POSIX idiom for "are you there" kills the process
    it asks about."""
    if pid <= 0:
        return False

    if sys.platform == "win32":
        import ctypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(0x1000, False, pid)     # QUERY_LIMITED_INFORMATION

        if not handle:
            return False

        code = ctypes.c_ulong()
        kernel32.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(code))
        kernel32.CloseHandle(ctypes.c_void_p(handle))
        return code.value == 259                             # STILL_ACTIVE

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def sweep_abandoned_workspaces(ours: Path) -> None:
    """Removes the temporary folders of runs whose process is gone.

    Only those: each folder says which process made it, and a folder whose
    maker is still running is another run of this instrument, mid-measurement,
    that deleting would break. A folder that does not say is left alone too."""
    for folder in Path(tempfile.gettempdir()).glob(WORKSPACE_PREFIX + "*"):
        if folder == ours or not folder.is_dir():
            continue

        try:
            owner = int((folder / "owner.pid").read_text().strip())
        except (OSError, ValueError):
            continue

        if not process_alive(owner):
            shutil.rmtree(folder, ignore_errors=True)


# =============================================================================
# Finding things
# =============================================================================

def find_binary(given: "str | None") -> Path:
    candidates = []

    if given:
        candidates.append(Path(given))
    elif os.environ.get("WFG_BINARY"):
        candidates.append(Path(os.environ["WFG_BINARY"]))
    else:
        name = "wfg.exe" if sys.platform == "win32" else "wfg"
        candidates.append(REPO / "build" / "vs" / "src" / "Debug" / name)
        candidates.extend(sorted(REPO.glob(f"build/*/src/**/{name}")))

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    raise HarnessError("no wfg binary at " + ", ".join(str(c) for c in candidates[:3])
                       + " - build one, or pass --binary or set WFG_BINARY")


def find_browser(given: "str | None") -> Path:
    if given:
        path = Path(given)
        if path.is_file():
            return path
        found = shutil.which(given)
        if found:
            return Path(found)
        raise HarnessError(f"--browser {given} is not a file or a command on PATH")

    local = os.environ.get("LOCALAPPDATA", "")
    candidates = [
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.join(local, r"Google\Chrome\Application\chrome.exe") if local else "",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
    ]

    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)

    for name in ("msedge", "microsoft-edge", "google-chrome", "google-chrome-stable",
                 "chrome", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return Path(found)

    raise HarnessError("no Edge, Chrome or Chromium found - pass --browser")


def copy_ui_at(rev: str, into: Path) -> Path:
    """clients/console/ as it was at `rev`, every file, written as bytes."""
    listing = subprocess.run(["git", "-C", str(REPO), "ls-tree", "-r", "--name-only", rev,
                              "clients/console"], capture_output=True, text=True)

    if listing.returncode != 0 or not listing.stdout.strip():
        raise HarnessError(f"git has no clients/console at {rev}: {listing.stderr.strip()}")

    for name in listing.stdout.split("\n"):
        if not name.strip():
            continue

        blob = subprocess.run(["git", "-C", str(REPO), "show", f"{rev}:{name}"],
                              capture_output=True)
        if blob.returncode != 0:
            raise HarnessError(f"git show {rev}:{name} failed")

        target = into / Path(name).relative_to("clients/console")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(blob.stdout)

    return into


# =============================================================================
# HTTP to the engine
# =============================================================================

def http_get(port: int, target: str, timeout: float = CALL_TIMEOUT) -> "tuple[int, bytes]":
    """(status, body), over a raw socket, as tests/blackbox/common.py does it."""
    request = (f"GET {target} HTTP/1.1\r\nHost: {HOST}:{port}\r\n"
               f"Connection: close\r\n\r\n").encode("ascii")

    with socket.create_connection((HOST, port), timeout=timeout) as sock:
        sock.sendall(request)
        chunks = []
        while True:
            block = sock.recv(65536)
            if not block:
                break
            chunks.append(block)

    head, _, body = b"".join(chunks).partition(b"\r\n\r\n")

    try:
        status = int(head.split(b"\r\n", 1)[0].split(b" ")[1])
    except (IndexError, ValueError):
        raise HarnessError(f"GET {target}: unparseable reply")

    return status, body


def node_value(port: int, address: str, fallback=None):
    """The first VALUE of one node, read from the engine now.

    RETRIED, for a page before PR 5.9 served with --ui-rev, which was not a
    polite neighbour. At 500 cues the page at 7154bb2 kept several 8.6 MB
    requests in flight at once, the server built each on one of a few
    threads, and a request whose headers were not read soon enough had its
    connection closed with nothing sent back. That was the page's problem to
    report, not this reading's to die of. PR 5.9's page keeps one request in
    flight, and against it the retry costs nothing."""
    for attempt in range(12):
        try:
            status, body = http_get(port, address, timeout=15)
            break
        except (HarnessError, OSError):
            if attempt == 11:
                raise HarnessError(f"GET {address} failed twelve times running")
            time.sleep(0.5)

    if status != 200:
        return fallback

    try:
        value = json.loads(body).get("VALUE")
    except (json.JSONDecodeError, AttributeError):
        return fallback

    return value[0] if isinstance(value, list) and value else fallback


def id_list(text) -> "list[str]":
    return text.split() if isinstance(text, str) else []


# =============================================================================
# The engine
# =============================================================================

class Engine:
    """`wfg serve` on ports the OS chose, read back off stdout the way
    tests/blackbox/common.py's Server reads them - and both pipes drained for
    as long as it runs, so a chatty engine never blocks on a full pipe."""

    def __init__(self, binary: Path, bundle: Path, ui: Path):
        self.argv = [str(binary), "serve", str(bundle), "--sample-rate=48000", "--buffer=128",
                     "--http-port=0", "--osc-port=0", f"--ui={ui}"]
        self.tail: "collections.deque[str]" = collections.deque(maxlen=80)
        self.http_port = 0
        self.osc_port = 0
        self._ready = threading.Event()

        self.process = subprocess.Popen(self.argv, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True,
                                        encoding="utf-8", errors="replace")

        # STOPPED BEFORE ANYTHING IS RAISED, as common.py's Server does: the
        # caller holds no Engine until this returns, so its `finally` cannot
        # stop one that timed out or was interrupted here - and on POSIX,
        # where no job object ties it to this process, it would outlive the
        # run holding its ports.
        try:
            for stream, ports in ((self.process.stdout, True), (self.process.stderr, False)):
                threading.Thread(target=self._pump, args=(stream, ports), daemon=True).start()

            deadline = time.monotonic() + STARTUP_TIMEOUT

            while not self._ready.wait(0.1):
                if self.process.poll() is not None:
                    time.sleep(0.2)
                    raise HarnessError(f"wfg serve exited {self.process.returncode} before it "
                                       "was ready:\n    " + "\n    ".join(self.tail))
                if time.monotonic() > deadline:
                    raise HarnessError(f"wfg serve reported no ports within {STARTUP_TIMEOUT}s")
        except BaseException:
            self.stop()
            raise

    def _pump(self, stream, ports: bool) -> None:
        try:
            for line in stream:
                self.tail.append(line.rstrip())

                if not ports:
                    continue

                parts = line.split()

                if len(parts) == 3 and parts[0] == "wfg:" and parts[2].isdigit():
                    if parts[1] == "http":
                        self.http_port = int(parts[2])
                    elif parts[1] == "osc":
                        self.osc_port = int(parts[2])

                if self.http_port and self.osc_port:
                    self._ready.set()
        except (OSError, ValueError):
            return

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)


# =============================================================================
# WebSocket, RFC 6455, client side, text frames - what the DevTools protocol needs
# =============================================================================

class WebSocket:
    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, url: str):
        parts = urllib.parse.urlsplit(url)

        if parts.scheme != "ws" or not parts.hostname or not parts.port:
            raise HarnessError(f"not a ws:// address with a port: {url}")

        self.sock = socket.create_connection((parts.hostname, parts.port), timeout=CALL_TIMEOUT)

        # No Origin header, deliberately: the DevTools endpoint refuses a
        # connection that names an origin it was not told to allow, and one
        # that names none is a tool rather than a web page.
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        path = parts.path + ("?" + parts.query if parts.query else "")
        self.sock.sendall((f"GET {path} HTTP/1.1\r\n"
                           f"Host: {parts.hostname}:{parts.port}\r\n"
                           f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\n"
                           f"Sec-WebSocket-Version: 13\r\n\r\n").encode("ascii"))

        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise HarnessError("the DevTools endpoint closed during the handshake")
            response += chunk

        head, _, self.buffer = response.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")

        if " 101 " not in lines[0] + " ":
            raise HarnessError(f"the DevTools endpoint refused the upgrade: {lines[0]}")

        accept = base64.b64encode(hashlib.sha1((key + self.GUID).encode("ascii")).digest())
        headers = {name.strip().lower(): value.strip()
                   for name, _, value in (line.partition(":") for line in lines[1:])}

        if headers.get("sec-websocket-accept") != accept.decode("ascii"):
            raise HarnessError("the DevTools endpoint answered with the wrong accept key")

    def _send(self, opcode: int, payload: bytes) -> None:
        mask = secrets.token_bytes(4)
        header = bytes([0x80 | opcode])
        n = len(payload)

        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)

        # Masked, as RFC 6455 section 5.3 requires of every client frame.
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_text(self, text: str) -> None:
        self._send(0x1, text.encode("utf-8"))

    def _exact(self, n: int, deadline: float) -> bytes:
        while len(self.buffer) < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HarnessError("the page did not answer in time")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                raise HarnessError("the page did not answer in time")
            if not chunk:
                raise HarnessError("the DevTools connection closed")
            self.buffer += chunk

        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def receive(self, deadline: float) -> str:
        """One whole text message, continuation frames joined, pings answered."""
        pieces: "list[bytes]" = []

        while True:
            first = self._exact(2, deadline)
            final = bool(first[0] & 0x80)
            opcode = first[0] & 0x0F
            length = first[1] & 0x7F

            if length == 126:
                length = struct.unpack(">H", self._exact(2, deadline))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._exact(8, deadline))[0]

            if first[1] & 0x80:                 # a server may not mask; unmask if it did
                mask = self._exact(4, deadline)
                masked = self._exact(length, deadline)
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(masked))
            else:
                payload = self._exact(length, deadline) if length else b""

            if opcode == 0x8:
                raise HarnessError("the DevTools connection was closed by the browser")
            if opcode == 0x9:
                self._send(0xA, payload)
                continue
            if opcode == 0xA:
                continue

            pieces.append(payload)

            if final:
                return b"".join(pieces).decode("utf-8", "replace")

    def close(self) -> None:
        try:
            self._send(0x8, b"")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class DevTools:
    """One DevTools target, one call at a time. Events are read past and
    dropped: nothing here enables a domain that would send any worth keeping."""

    def __init__(self, url: str):
        self.ws = WebSocket(url)
        self.counter = 0

    def call(self, method: str, params: "dict | None" = None, timeout: float = CALL_TIMEOUT):
        self.counter += 1
        ident = self.counter
        self.ws.send_text(json.dumps({"id": ident, "method": method, "params": params or {}}))
        deadline = time.monotonic() + timeout

        while True:
            message = json.loads(self.ws.receive(deadline))

            if message.get("id") != ident:
                continue

            if "error" in message:
                raise HarnessError(f"{method}: {message['error'].get('message', message['error'])}")

            return message.get("result", {})

    def evaluate(self, expression: str, timeout: float = CALL_TIMEOUT):
        """The value of an expression in the page, promises awaited."""
        result = self.call("Runtime.evaluate",
                           {"expression": expression, "awaitPromise": True,
                            "returnByValue": True, "userGesture": True}, timeout)

        if "exceptionDetails" in result:
            details = result["exceptionDetails"]
            text = (details.get("exception") or {}).get("description") or details.get("text")
            raise HarnessError(f"the page threw: {text}")

        return (result.get("result") or {}).get("value")

    def click(self, x: float, y: float) -> None:
        for kind, buttons in (("mouseMoved", 0), ("mousePressed", 1), ("mouseReleased", 0)):
            params = {"type": kind, "x": x, "y": y, "buttons": buttons}
            if kind != "mouseMoved":
                params.update({"button": "left", "clickCount": 1})
            self.call("Input.dispatchMouseEvent", params)

    def wheel(self, x: float, y: float, dy: float) -> None:
        self.call("Input.dispatchMouseEvent",
                  {"type": "mouseWheel", "x": x, "y": y, "deltaX": 0, "deltaY": dy})

    def press(self, key: str, code: str, keycode: int, text: "str | None" = None) -> None:
        down = {"type": "keyDown", "key": key, "code": code,
                "windowsVirtualKeyCode": keycode, "nativeVirtualKeyCode": keycode}
        if text is not None:
            down.update({"text": text, "unmodifiedText": text})
        self.call("Input.dispatchKeyEvent", down)
        self.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": key, "code": code,
                                             "windowsVirtualKeyCode": keycode,
                                             "nativeVirtualKeyCode": keycode})

    def close(self) -> None:
        self.ws.close()


# =============================================================================
# The browser
# =============================================================================

class Browser:
    """A headless Chromium on a profile of its own, and every process whose
    command line carries that profile - which is how it is found again at the
    end."""

    def __init__(self, executable: Path, profile: Path):
        self.executable = executable
        self.profile = profile

        # What each of its processes carries: the temporary folder's own name,
        # which mkdtemp made unique, inside the profile's path. Never the
        # profile folder's name alone - "profile" is on half the command lines
        # of a Windows box. The engine's command line carries the temporary
        # folder too (its bundle and its --ui copy live there), which is why
        # the marker is never the whole test: see _ours.
        self.marker = profile.parent.name

        if not self.marker.startswith(WORKSPACE_PREFIX):
            raise HarnessError(f"refusing to hunt processes by {self.marker!r}")

        self.port = 0
        self.browser_path = ""
        self.stderr: "collections.deque[str]" = collections.deque(maxlen=40)

        profile.mkdir(parents=True, exist_ok=True)

        # about:blank first: the page is navigated to once the instrument is
        # registered, so that it is there before the page's first line runs.
        argv = [str(executable), "--headless=new", "--remote-debugging-port=0",
                f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
                "--disable-extensions", "--disable-sync", "--disable-component-update",
                "--disable-background-networking", "--window-size=1400,1000",
                # The operator's page is the tab in front. A headless one is
                # nobody's tab, and these keep its timers and its renderer from
                # being treated as a background page's - which would slow the
                # poll and measure a throttle instead of the page.
                "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
                "--disable-backgrounding-occluded-windows",
                "about:blank"]

        self.process = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # Stopped before anything is raised, for the reason Engine gives.
        try:
            threading.Thread(target=self._pump, daemon=True).start()

            # Chrome prints "DevTools listening on ws://..." on stderr. Edge on
            # Windows prints nothing there, and both write DevToolsActivePort
            # into the profile - port on the first line, the browser's path on
            # the second - so that is asked first and the line is the fallback.
            deadline = time.monotonic() + STARTUP_TIMEOUT
            active = profile / "DevToolsActivePort"

            while time.monotonic() < deadline and not self.port:
                if active.is_file():
                    lines = active.read_text(encoding="utf-8", errors="replace").split("\n")
                    if lines and lines[0].strip().isdigit():
                        self.port = int(lines[0].strip())
                        self.browser_path = lines[1].strip() if len(lines) > 1 else ""
                        break

                for line in list(self.stderr):
                    found = re.search(r"DevTools listening on ws://[^:/]+:(\d+)(/\S*)", line)
                    if found:
                        self.port = int(found.group(1))
                        self.browser_path = found.group(2)
                        break

                time.sleep(0.1)

            if not self.port:
                raise HarnessError("the browser gave no DevTools port within "
                                   f"{STARTUP_TIMEOUT}s; its stderr said:\n    "
                                   + "\n    ".join(self.stderr))
        except BaseException:
            self.stop()
            raise

    def _pump(self) -> None:
        try:
            for raw in self.process.stderr:
                self.stderr.append(raw.decode("utf-8", "replace").rstrip())
        except (OSError, ValueError):
            return

    def json(self, path: str):
        with urllib.request.urlopen(f"http://{HOST}:{self.port}{path}", timeout=10) as reply:
            return json.loads(reply.read())

    def blank_page(self) -> dict:
        deadline = time.monotonic() + STARTUP_TIMEOUT

        while time.monotonic() < deadline:
            try:
                for target in self.json("/json/list"):
                    if target.get("type") == "page" and target.get("webSocketDebuggerUrl"):
                        return target
            except (OSError, ValueError):
                pass
            time.sleep(0.2)

        raise HarnessError("the browser opened no page")

    # -- the end -------------------------------------------------------------

    def _ours(self) -> "list[int]":
        """Every live browser process of this run, and never the engine,
        whose command line holds the temporary folder as well.

        ON WINDOWS, by the marker AND the browser's image name. The image
        keeps out the engine (wfg.exe) and the PowerShell asking the question,
        whose own command line holds the marker; the marker rather than the
        profile's path because it survives however the path was spelt to a
        child.

        ELSEWHERE, by the profile's full path - `--user-data-dir=<profile>` on
        the browser and its children, `--database=<profile>/...` on the crash
        handler - and not by the image, which is a wrapper script on Linux
        (google-chrome execs chrome) and "... Helper (Renderer)" on macOS. The
        engine's command line names the bundle and the --ui copy, never the
        profile, so it is not found here and is not killed as a straggler.
        The resolved path is asked too, for a temporary folder behind a
        symbolic link that the browser resolved."""
        if sys.platform == "win32":
            image = self.executable.name.replace("'", "")
            script = (f"Get-CimInstance Win32_Process -Filter \"Name='{image}'\" | "
                      "Where-Object { $_.CommandLine -and "
                      f"$_.CommandLine.Contains('{self.marker}') }} | "
                      "ForEach-Object { $_.ProcessId }")
            done = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive",
                                   "-Command", script], capture_output=True, text=True,
                                  timeout=60)
            return [int(x) for x in done.stdout.split() if x.isdigit()]

        paths = {str(self.profile), str(self.profile.resolve())}
        done = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True, text=True,
                              timeout=30)
        found = []
        for line in done.stdout.split("\n"):
            pid, _, args = line.strip().partition(" ")
            if (any(path in args for path in paths) and pid.isdigit()
                    and int(pid) != os.getpid()):
                found.append(int(pid))
        return found

    def stop(self) -> None:
        # Politely first: Browser.close on the browser's own endpoint takes
        # the whole tree down the way the browser means it to go.
        if self.port and self.browser_path:
            try:
                session = DevTools(f"ws://{HOST}:{self.port}{self.browser_path}")
                try:
                    session.call("Browser.close", timeout=5)
                except HarnessError:
                    pass
                session.close()
            except (HarnessError, OSError):
                pass

        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()

        # Then whatever is left, found by the folder's name.
        deadline = time.monotonic() + 10

        while True:
            try:
                left = self._ours()
            except (OSError, subprocess.SubprocessError):
                left = []

            if not left:
                return

            if time.monotonic() > deadline:
                for pid in left:
                    try:
                        if sys.platform == "win32":
                            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                                           capture_output=True, timeout=30)
                        else:
                            os.kill(pid, signal.SIGKILL)
                    except (OSError, subprocess.SubprocessError):
                        pass
                return

            time.sleep(0.5)


# =============================================================================
# The instrument, as it runs inside the page
# =============================================================================

# Registered to run before the page's own script. The reply timing goes in at
# once, so the first request is timed; the wrappers go in at DOMContentLoaded,
# when the page's script has declared what they wrap and no reply has yet been
# drawn. `targets` is written by `wrap_targets` below: for each name, how to
# read the binding and how to replace it. A top-level function declaration in
# a classic script is a writable global binding, which is what lets a wrapper
# stand where poll() and render() look the name up.
INSTRUMENT = r"""
(function (targets) {
  "use strict";
  if (window.__m24) return;

  const M = window.__m24 = {
    renderCount: 0, recording: false, installed: false,
    records: [], outside: {}, wrapped: [], absent: [], refused: {},
    issued: 0, answered: 0, succeeded: 0, failed: 0, lastReply: null
  };

  let current = null;
  const running = Object.create(null);
  const stack = [];

  /* THE POLL'S REPLY, from the page's very first request. json() is answered
     as JSON.parse over text(), which is what it is, in two timed halves. The
     reply is remembered until the next render() takes it, which is how a
     render knows how old the tree it is drawing is. */
  if (typeof window.fetch === "function") {
    const originalFetch = window.fetch;

    window.fetch = function (resource) {
      const url = typeof resource === "string" ? resource
                                               : String((resource && resource.url) || "");

      if (!/\/godot$/.test(url.split("?")[0])) return originalFetch.apply(this, arguments);

      const began = performance.now();
      M.issued += 1;

      return originalFetch.apply(this, arguments).then((response) => {
        const answered = performance.now();
        M.answered += 1;

        response.json = async function () {
          const t0 = performance.now();
          let text, value;

          try {
            text = await response.text();
          } catch (problem) { M.failed += 1; throw problem; }

          const t1 = performance.now();

          try {
            value = JSON.parse(text);
          } catch (problem) { M.failed += 1; throw problem; }

          const t2 = performance.now();
          M.succeeded += 1;
          M.lastReply = { began: began, answered: answered, bodyStart: t0, bodyEnd: t1,
                          parsed: t2, chars: text.length };
          return value;
        };

        return response;
      }, (problem) => { M.answered += 1; M.failed += 1; throw problem; });
    };
  }

  function tally(into, label, ms) {
    const part = into[label] || (into[label] = { ms: 0, calls: 0 });
    part.ms += ms;
    part.calls += 1;
  }

  function wrapRender(original) {
    return function () {
      M.renderCount += 1;
      const reply = M.lastReply;
      M.lastReply = null;

      if (!M.recording || current) return original.apply(this, arguments);

      const start = performance.now();
      const record = { start: start, total: 0, layout: null, parts: {}, within: {},
                       inFlight: M.issued - M.answered, reply: null };

      if (reply) {
        record.reply = { age: start - reply.began, sinceAnswer: start - reply.answered,
                         served: reply.answered - reply.began,
                         download: reply.bodyEnd - reply.bodyStart,
                         parse: reply.parsed - reply.bodyEnd,
                         untilRender: start - reply.parsed, chars: reply.chars };
      }

      current = record;
      stack.push("render");

      try { return original.apply(this, arguments); }
      finally {
        stack.pop();
        record.total = performance.now() - start;
        current = null;

        /* The style and layout this render left for the next frame, done now
           so that it can be timed. It moves that work, and adds none. */
        const before = performance.now();
        void document.body.getBoundingClientRect();
        record.layout = performance.now() - before;

        M.records.push(record);
      }
    };
  }

  function wrapPart(label, original) {
    return function () {
      /* A recursive call is timed once, at the outside. */
      if (running[label]) return original.apply(this, arguments);

      running[label] = true;
      stack.push(label);
      const began = performance.now();

      try { return original.apply(this, arguments); }
      finally {
        const ms = performance.now() - began;
        stack.pop();
        running[label] = false;

        if (M.recording) {
          if (current) {
            tally(current.parts, label, ms);

            /* Charged to every wrapped frame it ran inside, not only the
               nearest: with a name added by --wrap between renderLists and
               triggersOf, the nearest frame would be that name, and the
               rows' figure - renderLists less what ran inside it - would
               quietly take the scan back. A label is on the stack once at
               most, which `running` sees to. */
            for (const outer of stack) {
              if (outer === "render") continue;
              const within = current.within[outer] || (current.within[outer] = {});
              within[label] = (within[label] || 0) + ms;
            }
          } else {
            (M.outside[label] || (M.outside[label] = [])).push(ms);
          }
        }
      }
    };
  }

  function wrap(target) {
    let original;
    try { original = target.get(); } catch (e) { original = undefined; }
    if (typeof original !== "function") { M.absent.push(target.label); return; }

    const wrapper = target.label === "render" ? wrapRender(original)
                                              : wrapPart(target.label, original);

    try { target.set(wrapper); }
    catch (e) { M.refused[target.label] = String((e && e.message) || e); return; }

    let now;
    try { now = target.get(); } catch (e) { now = undefined; }
    if (now !== wrapper) {
      M.refused[target.label] = "the binding did not take the wrapper";
      return;
    }

    M.wrapped.push(target.label);
  }

  function install() {
    if (M.installed) return;
    M.installed = true;
    for (const target of targets) wrap(target);
  }

  if (document.readyState === "loading")
    document.addEventListener("DOMContentLoaded", install, { once: true });
  else
    install();

  M.measure = function (count, timeoutMs) {
    M.records = []; M.outside = {};
    M.recording = true;
    const began = performance.now();
    const issuedBefore = M.issued, answeredBefore = M.answered;
    const succeededBefore = M.succeeded, failedBefore = M.failed;

    return new Promise((resolve) => {
      const check = () => {
        const done = M.records.length >= count;

        if (done || performance.now() - began > timeoutMs) {
          M.recording = false;
          resolve({ records: M.records.slice(0, count), outside: M.outside,
                    wrapped: M.wrapped, absent: M.absent, refused: M.refused,
                    timedOut: !done, elapsed: performance.now() - began,
                    issued: M.issued - issuedBefore, answered: M.answered - answeredBefore,
                    succeeded: M.succeeded - succeededBefore, failed: M.failed - failedBefore,
                    inFlightAtEnd: M.issued - M.answered,
                    rows: document.querySelectorAll("#cues .row").length });
          return;
        }

        setTimeout(check, 25);
      };
      check();
    });
  };

  M.waitRenders = function (count, timeoutMs) {
    const start = M.renderCount;
    const began = performance.now();

    return new Promise((resolve) => {
      const check = () => {
        const seen = M.renderCount - start;

        if (seen >= count || performance.now() - began > timeoutMs) {
          resolve({ renders: seen, timedOut: seen < count, waited: performance.now() - began });
          return;
        }

        setTimeout(check, 25);
      };
      check();
    });
  };
})(__TARGETS__);
"""

STATUS = r"""
(() => {
  const M = window.__m24;
  const link = document.getElementById("link-text");
  return { rows: document.querySelectorAll("#cues .row").length,
           instrument: !!M, installed: !!(M && M.installed),
           wrapped: M ? M.wrapped : [], absent: M ? M.absent : [],
           refused: M ? M.refused : {}, renders: M ? M.renderCount : 0,
           issued: M ? M.issued : 0, succeeded: M ? M.succeeded : 0,
           failed: M ? M.failed : 0, link: link ? link.textContent : null };
})()
"""

ACTIVE = r"""
(() => {
  const a = document.activeElement;
  if (!a) return null;
  return { tag: a.tagName, id: a.id || "", type: a.type || "",
           set: (a.dataset && a.dataset.set) || "" };
})()
"""

SCROLL_BY_SCRIPT = r"""
(async () => {
  const pane = document.getElementById("cues");
  if (!pane) return { error: "the page has no #cues" };

  const height = pane.scrollHeight;
  const visible = pane.clientHeight;
  const target = Math.max(0, Math.round((height - visible) / 2));

  pane.scrollTop = target;
  const set = pane.scrollTop;

  /* A row held across the polls says whether the rows under the scroll were
     replaced or kept - so that "kept" can be told apart from "nothing was
     redrawn", which would be no test of the defect at all. */
  const held = pane.querySelector(".row");
  const waited = await window.__m24.waitRenders(3, __WAIT__);

  return { scrollHeight: height, clientHeight: visible, target: target, set: set,
           after: pane.scrollTop, renders: waited.renders, timedOut: waited.timedOut,
           rowsReplaced: !!held && !held.isConnected };
})()
"""

# Back to the top by script, a listener to see what the wheel does, and where
# on the screen the pane is - its middle, where a wheel over it scrolls it.
WHEEL_PREPARE = r"""
(() => {
  const pane = document.getElementById("cues");
  if (!pane) return null;
  pane.scrollTop = 0;

  const seen = window.__m24wheel = { most: 0, last: 0, events: 0 };
  pane.addEventListener("scroll", () => {
    seen.events += 1;
    seen.last = pane.scrollTop;
    if (seen.last > seen.most) seen.most = seen.last;
  }, { passive: true });

  const r = pane.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2,
           room: pane.scrollHeight - pane.clientHeight, top: pane.scrollTop };
})()
"""

WHEEL_READ = r"""
(async () => {
  const pane = document.getElementById("cues");
  const seen = window.__m24wheel;
  const settled = pane.scrollTop;
  const held = pane.querySelector(".row");
  const waited = await window.__m24.waitRenders(3, __WAIT__);
  return { most: seen.most, events: seen.events, set: settled, after: pane.scrollTop,
           scrollHeight: pane.scrollHeight, renders: waited.renders,
           timedOut: waited.timedOut, rowsReplaced: !!held && !held.isConnected };
})()
"""

SLIDER = r"""
(() => {
  const s = document.getElementById("aim-offset");
  if (!s) return null;
  const r = s.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2, width: r.width,
           shown: r.width > 0 && r.height > 0, value: s.value };
})()
"""

# The slider given the focus and nothing else: focus() moves no thumb and
# fires no `input` or `change`, so a page that lets go of the slider on
# `change` has nothing to let go on, and the guard is what the keys meet.
FOCUS_SLIDER = r"""
(() => {
  const s = document.getElementById("aim-offset");
  if (!s) return null;
  s.focus();
  const a = document.activeElement;
  return { focused: a === s, value: s.value, min: s.min, max: s.max,
           active: a ? { tag: a.tagName, id: a.id || "" } : null };
})()
"""

SLIDER_STATE = r"""
(() => {
  const s = document.getElementById("aim-offset");
  const a = document.activeElement;
  return { value: s ? s.value : null, focused: !!s && a === s,
           active: a ? { tag: a.tagName, id: a.id || "" } : null };
})()
"""

# The transport's clear button, focused: a button a Space could press, whose
# own command leaves a mark nothing else here would - a list with no standby.
FOCUS_BUTTON = r"""
(() => {
  const b = document.querySelector('#transport [data-cmd="standby.clear"]');
  if (!b) return null;
  b.focus();
  const a = document.activeElement;
  return { focused: a === b, disabled: !!b.disabled,
           active: a ? { tag: a.tagName, id: a.id || "",
                         cmd: (a.dataset && a.dataset.cmd) || "" } : null };
})()
"""

BLUR = r"""
(() => {
  const a = document.activeElement;
  if (a && a !== document.body && typeof a.blur === "function") a.blur();
  return document.activeElement ? document.activeElement.tagName : null;
})()
"""

# A media cue's row that is not the standby's, centred in the pane, and the
# point on its name to click - not the gutter, which parks the standby, and not
# the twist, which folds.
PICK_ROW = r"""
(() => {
  const rows = [...document.querySelectorAll("#cues .row[data-pick]")]
    .filter((r) => !r.classList.contains("derived") && r.dataset.standby !== "yes");
  const media = rows.filter((r) => {
    const k = r.querySelector(".kind");
    return k && k.textContent.trim() === "media";
  });
  const row = media[2] || media[0] || rows[0];
  if (!row) return null;

  row.scrollIntoView({ block: "center" });
  const where = row.querySelector(".name .text") || row.querySelector(".name") || row;
  const r = where.getBoundingClientRect();
  const x = r.left + Math.min(r.width / 2, 30), y = r.top + r.height / 2;

  /* What a click there would land on, asked before it is made. */
  const hit = document.elementFromPoint(x, y);
  const hitRow = hit && hit.closest ? hit.closest("[data-pick]") : null;

  return { id: row.dataset.pick, x: x, y: y,
           lands: hitRow ? hitRow.dataset.pick
                         : (hit ? hit.tagName + "." + hit.className : null) };
})()
"""

# What the page made of the click: the row it picked, and the inspector's head.
PICKED = r"""
(() => {
  const inspect = document.getElementById("inspect");
  return { picked: typeof picked !== "undefined" ? picked : "(no global picked)",
           inspector: inspect ? inspect.textContent.replace(/\s+/g, " ").trim().slice(0, 120)
                              : null,
           nameFields: [...document.querySelectorAll('[data-set$="/name"]')]
                         .map((f) => f.dataset.set) };
})()
"""

FIELD = r"""
(() => {
  const id = __ID__;
  const field = [...document.querySelectorAll('[data-set$="/name"]')]
    .find((f) => f.dataset.set.indexOf("/" + id + "/") >= 0);
  if (!field) return null;
  return { address: field.dataset.set, value: field.value,
           focused: document.activeElement === field };
})()
"""

FOCUS_FIELD = r"""
(() => {
  const address = __ADDRESS__;
  const field = document.querySelector('[data-set="' + address + '"]');
  if (!field) return false;
  field.focus();
  const end = field.value.length;
  try { field.setSelectionRange(end, end); } catch (e) { /* not a text control */ }
  return document.activeElement === field;
})()
"""

FIELD_STATE = r"""
(() => {
  const address = __ADDRESS__;
  const field = document.querySelector('[data-set="' + address + '"]');
  const a = document.activeElement;
  return { present: !!field, value: field ? field.value : null,
           focused: !!field && a === field,
           active: a ? { tag: a.tagName, id: a.id || "", set: (a.dataset && a.dataset.set) || "" }
                     : null };
})()
"""


def wrap_targets(names: "list[str]") -> str:
    """The JavaScript array of {label, get, set} for the names given."""
    entries = []

    for name in names:
        if not re.fullmatch(r"[A-Za-z_$][\w$]*(\.[A-Za-z_$][\w$]*)*", name):
            raise HarnessError(f"--wrap {name!r} is not a name or a dotted path")

        parts = name.split(".")
        root = parts[0]
        label = json.dumps(name)

        if len(parts) == 1:
            get = f'() => (typeof {root} === "function" ? {root} : undefined)'
            put = f"(f) => {{ {root} = f; }}"
        else:
            holder = ".".join(parts[:-1])
            key = json.dumps(parts[-1])
            get = (f'() => ((typeof {root} !== "undefined" && {holder}) '
                   f'? {holder}[{key}] : undefined)')
            put = f"(f) => {{ {holder}[{key}] = f; }}"

        entries.append(f"{{ label: {label}, get: {get}, set: {put} }}")

    return "[" + ", ".join(entries) + "]"


# =============================================================================
# The measurements
# =============================================================================

def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def worst(values):
    values = [v for v in values if v is not None]
    return max(values) if values else None


def spread(values) -> dict:
    return {"median_ms": median(values), "worst_ms": worst(values)}


def summarise_render(raw: dict, names: "list[str]") -> dict:
    records = raw.get("records") or []
    starts = [r["start"] for r in records]

    components = {}

    for name in names:
        if name == "render" or name not in raw.get("wrapped", []):
            continue

        ms = [(r["parts"].get(name) or {}).get("ms", 0.0) for r in records]
        calls = [(r["parts"].get(name) or {}).get("calls", 0) for r in records]

        if not any(calls):
            continue                     # wrapped, and never called inside a render

        components[name] = {**spread(ms), "calls_per_render": median(calls)}

    # THE ROWS: renderLists less the two named calls made inside it, and
    # only those two. The scan is the second cost M24 asks for apart, and
    # overlaps is neither; a name added with --wrap is the rows' own work
    # (reconcile, say), wrapped so that its cost shows, not so that it leaves.
    rows = None

    if "renderLists" in components:
        lists = [(r["parts"].get("renderLists") or {}).get("ms", 0.0) for r in records]

        def inside(name):
            return [(r["within"].get("renderLists") or {}).get(name, 0.0) for r in records]

        scan, overlaps = inside("tree.triggersOf"), inside("tree.overlaps")
        rows = {**spread([a - b - c for a, b, c in zip(lists, scan, overlaps)]),
                "triggersOf_inside_median_ms": median(scan),
                "overlaps_inside_median_ms": median(overlaps)}

    replies = [r["reply"] for r in records if r.get("reply")]

    def of(key):
        return [reply[key] for reply in replies]

    return {
        "renders": len(records),
        "timed_out": raw.get("timedOut", False),
        "rows": raw.get("rows"),
        "total": {**spread([r["total"] for r in records]),
                  "samples_ms": [round(r["total"], 2) for r in records]},
        "components": components,
        "the_rows": rows,
        "the_scan": components.get("tree.triggersOf"),
        "layout_after_render": spread([r.get("layout") for r in records]),
        "between_renders": spread([b - a for a, b in zip(starts, starts[1:])]),
        "outside_render": {name: {**spread(samples), "calls": len(samples)}
                           for name, samples in (raw.get("outside") or {}).items() if samples},
        "reply": {
            "replies": len(replies),
            "chars": median(of("chars")),
            "download": spread(of("download")),
            "parse": spread(of("parse")),
            "until_render": spread(of("untilRender")),
            "served": spread(of("served")),
            "age": spread(of("age")),
            "since_answer": spread(of("sinceAnswer")),
        } if replies else None,
        "requests": {"issued": raw.get("issued"), "answered": raw.get("answered"),
                     "succeeded": raw.get("succeeded"), "failed": raw.get("failed"),
                     "seconds": (raw.get("elapsed") or 0) / 1000,
                     "in_flight_at_start": records[0].get("inFlight") if records else None,
                     "in_flight_at_end": raw.get("inFlightAtEnd")},
        "wrapped": raw.get("wrapped", []),
        "absent": raw.get("absent", []),
        "refused": raw.get("refused", {}),
    }


def list_state(port: int, list_id: str) -> "tuple[str, list[str]]":
    """(standby, history) of one list, read from the engine now."""
    standby = node_value(port, f"/godot/list/{list_id}/standby", "") or ""
    history = node_value(port, f"/godot/list/{list_id}/history", "") or ""
    return standby, history.split() if isinstance(history, str) else []


def wait_for_go(port: int, list_id: str, before: "tuple[str, list[str]]",
                seconds: float) -> dict:
    """Whether a GO reached the engine, asked of the one node that says so.

    THE LIST'S HISTORY, and not the standby or the run table. `history`
    records each thing the list did as `<tick>:<cue>:<g|f|t>`, and `g` is GO
    and nothing else - not a trigger, not a cue fired by name. The two
    cheaper signs both lie at the edges: a GO's run can be published a poll
    before the standby moves, so reading the two one after the other can see
    the run and not the move; and a run of the standby cue is not proof of a
    GO at all. The standby is still read, after, because it is what the
    operator watches - but it decides nothing here."""
    standby_before, history_before = before
    known = set(history_before)
    deadline = time.monotonic() + seconds

    while True:
        standby, history = list_state(port, list_id)
        gos = [entry for entry in history if entry not in known and entry.endswith(":g")]

        if gos or time.monotonic() > deadline:
            break

        time.sleep(0.25)

    if gos:
        # Give the standby the moment it takes to follow, for the report.
        follow = time.monotonic() + 3
        while standby == standby_before and time.monotonic() < follow:
            time.sleep(0.25)
            standby, _history = list_state(port, list_id)

    return {"reached": bool(gos), "go_entries": gos,
            "standby_before": standby_before, "standby_after": standby}


def wait_for_standby_move(port: int, list_id: str, before: str, seconds: float) -> str:
    """The list's standby once it is no longer `before`, or when time is up."""
    deadline = time.monotonic() + seconds

    while True:
        standby = node_value(port, f"/godot/list/{list_id}/standby", "") or ""
        if standby != before or time.monotonic() > deadline:
            return standby
        time.sleep(0.25)


def guard_case(page: DevTools, port: int, list_id: str, seconds: float) -> dict:
    """The narrowed guard, met with the slider holding the focus.

    The operator's click cannot ask this of a page that lets go of the slider
    on `change`, because by the time Space is pressed nothing holds the focus.
    So the focus is put there from script, with the value untouched, and the
    keys are pressed with the slider provably holding it - asked again before
    each key, because a key that took the focus away would make the next one
    a question about the body."""
    out: dict = {}
    held = page.evaluate(FOCUS_SLIDER)
    out["held_for_space"] = held

    if not held or not held.get("focused"):
        active = (held or {}).get("active") or {}
        out["error"] = ("the slider would not take the focus from script (it went to "
                        f"{active.get('tag', '?')}#{active.get('id', '')})")
        return out

    before = list_state(port, list_id)
    page.press(" ", "Space", 32, text=" ")
    out["space"] = wait_for_go(port, list_id, before, seconds)

    # The arrow. ArrowDown is the standby's next; at the slider's minimum a
    # slider that took the key would not move either, so the value would say
    # nothing, and ArrowUp - the standby's previous - asks the same question
    # with an answer on both sides.
    held = page.evaluate(FOCUS_SLIDER)
    out["held_for_arrow"] = held

    if not held or not held.get("focused"):
        out["arrow"] = {"error": "the slider would not take the focus again for the arrow"}
        return out

    try:
        at_minimum = float(held["value"]) <= float(held["min"])
    except (KeyError, TypeError, ValueError):
        at_minimum = False

    key, keycode = ("ArrowUp", 38) if at_minimum else ("ArrowDown", 40)
    standby_before, _history = list_state(port, list_id)
    page.press(key, key, keycode)
    standby_after = wait_for_standby_move(port, list_id, standby_before, seconds)
    after = page.evaluate(SLIDER_STATE) or {}

    out["arrow"] = {"key": key, "standby_before": standby_before,
                    "standby_after": standby_after,
                    "standby_moved": standby_after != standby_before,
                    "slider_before": held.get("value"), "slider_after": after.get("value"),
                    "slider_moved": after.get("value") != held.get("value"),
                    "still_focused": after.get("focused")}
    return out


def button_case(page: DevTools, port: int, list_id: str, seconds: float) -> dict:
    """Space with a button holding the focus: GO, and not the button as well.

    A button is pressed on the release of Space, and only when the press was
    not refused, so a page that answers the keydown with GO and refuses it
    sends GO alone. The button is the transport's clear, because its command
    leaves a list with no standby, where a GO leaves the next cue there."""
    out: dict = {}
    held = page.evaluate(FOCUS_BUTTON)
    out["held"] = held

    if not held or not held.get("focused"):
        why = "it is disabled" if (held or {}).get("disabled") else "it would not take it"
        out["error"] = f"the transport's clear button could not be given the focus ({why})"
        return out

    before = list_state(port, list_id)
    page.press(" ", "Space", 32, text=" ")
    out["space"] = wait_for_go(port, list_id, before, seconds)

    # A button's command sent on the key's release would land a moment after
    # the GO, so the standby is read once more after it has had that moment.
    time.sleep(1.0)
    out["standby_final"] = list_state(port, list_id)[0]
    out["cleared"] = out["standby_final"] == ""
    return out


def verdict(reading: dict) -> dict:
    """Kept, snapped to the top, or moved - of a pane that was scrolled, and
    only once a poll has redrawn it: a scroll that no render touched was not
    tested, whatever it reads."""
    set_to = reading.get("set") or 0
    after = reading.get("after") or 0

    if not set_to:
        reading["verdict"] = "the pane was not scrolled"
        reading["kept"] = None
    elif not reading.get("renders"):
        reading["verdict"] = "not tested - no poll redrew the list while waiting"
        reading["kept"] = None
    elif abs(after - set_to) <= 1:
        reading["verdict"] = "kept"
        reading["kept"] = True
    else:
        reading["verdict"] = "snapped to the top" if after == 0 else "moved"
        reading["kept"] = False

    return reading


def measure(args, page: DevTools, engine: Engine, names: "list[str]") -> dict:
    result: dict = {}
    port = engine.http_port

    lists = id_list(node_value(port, "/godot/list/order", ""))
    list_id = node_value(port, "/godot/list/focus", "") or (lists[0] if lists else "")
    wait_ms = int(args.wait * 1000)

    # -- (a) render ------------------------------------------------------------
    say(f"measuring {args.polls} renders")
    raw = page.evaluate(f"window.__m24.measure({int(args.polls)}, {wait_ms})",
                        timeout=wait_ms / 1000 + 30)
    result["render"] = summarise_render(raw, names)
    result["render"]["records"] = raw.get("records")

    # -- (b) the scroll --------------------------------------------------------
    say("the scroll: middle of the list, then three polls - by script, then by wheel")
    by_script = page.evaluate(SCROLL_BY_SCRIPT.replace("__WAIT__", str(wait_ms)),
                              timeout=wait_ms / 1000 + 30)
    result["scroll"] = verdict(by_script) if by_script and "error" not in by_script else by_script

    where = page.evaluate(WHEEL_PREPARE)

    if where and where.get("room", 0) > 0:
        step = 400
        for _ in range(max(1, min(40, int(where["room"] / 2 / step)))):
            page.wheel(where["x"], where["y"], step)
        time.sleep(1.0)                  # the wheel's smooth scroll, played out
        by_wheel = page.evaluate(WHEEL_READ.replace("__WAIT__", str(wait_ms)),
                                 timeout=wait_ms / 1000 + 30)
        result["scroll_by_wheel"] = verdict(by_wheel)
    else:
        result["scroll_by_wheel"] = {"error": "the pane has nothing to scroll"}

    # -- (c) the focus ---------------------------------------------------------
    say("the focus: click the aim slider, then Space")
    focus: dict = {"list": list_id}
    slider = page.evaluate(SLIDER)

    if not slider or not slider.get("shown"):
        focus["error"] = "no visible #aim-offset on the page"
    else:
        # THE OPERATOR'S PATH. Whether it asked anything of the guard depends
        # on where the focus is once the click is over, which is recorded and
        # reported: a page that lets go of the slider on `change` has Space
        # pressed with nothing focused, which is the control's question.
        before = list_state(port, list_id)
        page.click(slider["x"], slider["y"])
        focus["active_after_click"] = page.evaluate(ACTIVE)
        focus["slider_focused"] = (focus["active_after_click"] or {}).get("id") == "aim-offset"
        page.press(" ", "Space", 32, text=" ")
        focus["space"] = wait_for_go(port, list_id, before, args.go_wait)

        # THE CONTROL. Without it, "Space did not fire" could be a socket that
        # is down or a key the page never saw, and would say nothing.
        page.evaluate(BLUR)
        focus["active_for_control"] = page.evaluate(ACTIVE)
        before = list_state(port, list_id)
        page.press(" ", "Space", 32, text=" ")
        focus["control"] = wait_for_go(port, list_id, before, args.go_wait)

        # THE GUARD ITSELF, and then a button - each left with nothing focused
        # after it, so that what follows starts where the control did.
        say("the guard: the slider focused from script, then Space and an arrow")
        focus["guard"] = guard_case(page, port, list_id, args.go_wait)
        page.evaluate(BLUR)

        say("a button holding the focus, then Space")
        focus["button"] = button_case(page, port, list_id, args.go_wait)
        page.evaluate(BLUR)

    result["focus"] = focus

    # -- (d) escape ------------------------------------------------------------
    say("escape: type into a name field, then Escape")
    escape: dict = {}
    row = page.evaluate(PICK_ROW)

    if not row:
        escape["error"] = "no cue row to pick"
    else:
        escape["cue"] = row["id"]
        escape["click_lands_on"] = row.get("lands")
        field = None

        # Twice at most, and said: a click the page did not take is worth
        # knowing about, and a second is what an operator would do.
        for attempt in (1, 2):
            escape["clicks"] = attempt
            page.click(row["x"], row["y"])
            deadline = time.monotonic() + 15

            while time.monotonic() < deadline:
                field = page.evaluate(FIELD.replace("__ID__", json.dumps(row["id"])))
                if field:
                    break
                time.sleep(0.2)

            if field:
                break

            escape[f"after_click_{attempt}"] = page.evaluate(PICKED)
            row = page.evaluate(PICK_ROW) or row

        if not field:
            escape["error"] = (f"the inspector showed no name field for {escape['cue']} after two "
                               f"clicks (the click was to land on {escape['click_lands_on']}; "
                               f"then the page said {escape.get('after_click_2')})")
        else:
            address = field["address"]
            escape.update({"address": address, "original": field["value"],
                           "engine_before": node_value(port, address)})

            quoted = json.dumps(address)
            escape["focused"] = page.evaluate(FOCUS_FIELD.replace("__ADDRESS__", quoted))
            page.call("Input.insertText", {"text": TYPED})
            escape["typed"] = page.evaluate(FIELD_STATE.replace("__ADDRESS__", quoted))["value"]

            page.press("Escape", "Escape", 27)
            escape["renders_waited"] = page.evaluate(
                f"window.__m24.waitRenders(2, {wait_ms})", timeout=wait_ms / 1000 + 30)

            after = page.evaluate(FIELD_STATE.replace("__ADDRESS__", quoted))
            escape["after"] = after
            escape["still_in_field"] = after["focused"]
            escape["restored"] = after["value"] == field["value"]
            escape["kept_typing"] = after["value"] == escape["typed"]
            escape["engine_after"] = node_value(port, address)
            escape["engine_changed"] = escape["engine_after"] != escape["engine_before"]

    result["escape"] = escape

    result["page_link"] = page.evaluate(
        '(() => { const t = document.getElementById("link-text"); '
        'return t ? t.textContent : null; })()')

    return result


# =============================================================================
# Reporting
# =============================================================================

def ms(value) -> str:
    return "-" if value is None else f"{value:.1f}"


LABEL_WIDTH = 53                # the rows' label, the longest


def line(label: str, part: "dict | None", calls=None) -> str:
    if part is None:
        return f"      {label}"
    text = (f"      {label:<{LABEL_WIDTH}} {ms(part.get('median_ms')):>9}  "
            f"{ms(part.get('worst_ms')):>9}")
    return text + (f"   {calls:g}" if calls is not None else "")


def scroll_line(name: str, reading: dict) -> str:
    if not reading or "error" in reading:
        return f"      {name}: {(reading or {}).get('error', 'not taken')}"
    rows = ("the rows under it were replaced" if reading.get("rowsReplaced")
            else "the rows under it were kept, not replaced")
    return (f"      {name}: set to {reading.get('set')} of {reading.get('scrollHeight')} px; "
            f"after {reading.get('renders')} polls it reads {reading.get('after')} - "
            f"{reading.get('verdict', '?').upper()}; {rows}")


def go_words(reading: dict, reached: str = "REACHED the engine",
             missed: str = "did NOT reach the engine") -> str:
    """"GO reached the engine (history ...; standby a -> b)", from wait_for_go."""
    return ((reached if reading["reached"] else missed)
            + f" (history {' '.join(reading['go_entries']) or 'unchanged'}; standby "
              f"{reading['standby_before'] or '(none)'} -> "
              f"{reading['standby_after'] or '(none)'})")


def focus_lines(focus: dict) -> "list[str]":
    """Section (c) of the report: the operator's click, the control, the guard
    met with the slider holding the focus, and a button."""
    out = ["  (c) the focus"]

    active = focus.get("active_after_click") or {}
    on = f"{active.get('tag', '?')}#{active.get('id', '')}"
    out.append(f"      the operator's path: slider clicked, focus then on {on}")

    if focus.get("slider_focused"):
        out.append("      Space, the slider holding the focus -> GO " + go_words(focus["space"]))
    else:
        out.append("      Space -> GO " + go_words(focus["space"]))
        out.append("      - the slider had let go of the focus on the click's change, so this "
                   "did not exercise the guard")

    out.append("      control, nothing focused: Space -> GO "
               + go_words(focus["control"], "reached the engine",
                          "did NOT reach the engine either - no answer here says anything"))

    guard = focus.get("guard") or {}
    if "error" in guard:
        out.append(f"      the guard: {guard['error']}")
    elif guard:
        held = guard.get("held_for_space") or {}
        out.append(f"      the guard: #aim-offset focused from script at {held.get('value')}, "
                   "no value changed, and holding the focus")
        out.append("      Space -> GO " + go_words(guard["space"]))

        arrow = guard.get("arrow") or {}
        if "error" in arrow:
            out.append(f"      the arrow: {arrow['error']}")
        elif arrow:
            standby = (f"MOVED the standby ({arrow['standby_before'] or '(none)'} -> "
                       f"{arrow['standby_after'] or '(none)'})" if arrow["standby_moved"]
                       else f"did NOT move the standby (it stayed on "
                            f"{arrow['standby_before'] or '(none)'})")
            value = (f"the slider moved, {arrow['slider_before']} -> {arrow['slider_after']}"
                     if arrow["slider_moved"]
                     else f"the slider stayed at {arrow['slider_after']}")
            out.append(f"      {arrow['key']} -> {standby}; {value}")

    button = focus.get("button") or {}
    if "error" in button:
        out.append(f"      a button: {button['error']}")
    elif button:
        own = ("and its own command was sent as well - the list has no standby"
               if button["cleared"] else "and its own command was not sent")
        out.append("      a focused button (standby.clear): Space -> GO "
                   + go_words(button["space"], "reached the engine")
                   + f", {own}")

    return out


def report(meta: dict, result: dict) -> str:
    out = []
    render = result["render"]

    out.append("M24 - the web console's render() on a large show")
    out.append(f"  page      {meta['ui']}")
    out.append(f"  browser   {meta['browser']} ({meta.get('browser_version', '?')})")
    out.append(f"  engine    {meta['binary']}")
    out.append(f"  show      {meta['show']}")
    out.append(f"  rows      {render.get('rows')} drawn in #cues")
    between = render["between_renders"]
    out.append(f"  renders   {render['renders']} measured"
               + (" - TIMED OUT before the count" if render["timed_out"] else "")
               + f"; {ms(between['median_ms'])} ms apart at the median, "
                 f"{ms(between['worst_ms'])} worst")
    out.append("")
    out.append(f"{'  (a) render(), per poll':<{LABEL_WIDTH + 7}}"
               "median ms   worst ms   calls")
    out.append(line("total", render["total"]))

    for name in RENDER_PARTS + [n for n in render["components"] if n not in DEFAULT_WRAP]:
        part = render["components"].get(name)
        if part is not None:
            out.append(line(name, part, part["calls_per_render"]))
        elif name in render["absent"]:
            out.append(line(f"{name}: absent from the page", None))
        elif name in render["refused"]:
            out.append(line(f"{name}: could not be wrapped - {render['refused'][name]}", None))
        else:
            out.append(line(f"{name}: not called inside render()", None))

    out.append("")
    out.append("      the two render costs M24 asks to see apart - the rows (section 14.3's")
    out.append("      first defect) and the trigger scan (section 14.3's index):")

    rows = render.get("the_rows")
    out.append(line("1. the rows: renderLists less triggersOf and overlaps", rows) if rows
               else line("1. the rows: renderLists was not measured", None))

    scan = render.get("the_scan")
    if scan:
        out.append(line("2. the scan: tree.triggersOf, every call", scan,
                        scan["calls_per_render"]))
        if rows:
            out.append(line("   of which inside renderLists",
                            {"median_ms": rows["triggersOf_inside_median_ms"]}))
    else:
        why = ("absent from the page" if "tree.triggersOf" in render["absent"]
               else "not called inside render()")
        out.append(line(f"2. the scan: tree.triggersOf {why}", None))

    # Beside the two and counted in neither: what of it ran inside renderLists
    # has been taken out of the rows' figure, and is printed so that the
    # subtraction can be checked, as the scan's is above.
    overlaps = render["components"].get("tree.overlaps")
    if overlaps:
        out.append(line("(neither) tree.overlaps, every call", overlaps,
                        overlaps["calls_per_render"]))
        if rows:
            out.append(line("   of which inside renderLists",
                            {"median_ms": rows["overlaps_inside_median_ms"]}))

    out.append("")
    out.append("      what the poll costs outside render(), and not M24's number:")
    out.append(line("style and layout render() left behind", render["layout_after_render"]))

    reply = render.get("reply")
    if reply:
        out.append(line("JSON.parse of the reply", reply["parse"]))
        out.append(line("parsed reply to render() (flatten, etc.)", reply["until_render"]))
        out.append(line("the reply's download", reply["download"]))
        out.append(f"      the reply: {reply['chars'] / 1e6:.1f} M characters")
        age = reply["age"]
        out.append(f"      the tree drawn was asked for {ms(age['median_ms'])} ms before it "
                   f"was drawn (median; {ms(age['worst_ms'])} worst), and answered "
                   f"{ms(reply['since_answer']['median_ms'])} ms before")
    else:
        out.append("      no reply was timed")

    requests = render["requests"]
    out.append(f"      polls while measuring ({requests['seconds']:.0f} s): {requests['issued']} "
               f"requests issued, {requests['succeeded']} replies parsed, "
               f"{requests['failed']} failed; {requests['in_flight_at_start']} unanswered at "
               f"the first render, {requests['in_flight_at_end']} at the end")
    out.append(f"      the first rows were drawn {meta.get('first_rows_s', 0):.1f} s after the "
               "page was opened")

    for name, part in render["outside_render"].items():
        out.append(line(f"{name} (outside render, per call)", part, part["calls"]))

    out.append("")
    out.append("  (b) the scroll, #cues scrolled to the middle and three polls let go by")
    out.append(scroll_line("by script (scrollTop)", result.get("scroll")))
    out.append(scroll_line("by the mouse wheel   ", result.get("scroll_by_wheel")))

    out.append("")
    focus = result.get("focus") or {}
    if "error" in focus:
        out.append(f"  (c) the focus: {focus['error']}")
    else:
        out.extend(focus_lines(focus))

    out.append("")
    escape = result.get("escape") or {}
    if "error" in escape:
        out.append(f"  (d) escape: {escape['error']}")
    else:
        after = escape["after"]
        now_on = after["active"]["tag"] if after.get("active") else "?"
        where = "still in the field" if escape["still_in_field"] else f"left it (now {now_on})"
        value = ("restored to the tree's value" if escape["restored"]
                 else "still holds the typing" if escape["kept_typing"]
                 else f"reads {after['value']!r}")
        engine_word = ("CHANGED to " + repr(escape["engine_after"]) if escape["engine_changed"]
                       else "unchanged")
        clicks = (" (the row took two clicks to pick)" if escape.get("clicks") == 2 else "")
        out.append(f"  (d) escape: typed {TYPED!r} into {escape['address']} and pressed Escape"
                   + clicks)
        out.append(f"      focus {where}; the field {value}; the engine's name {engine_word}")

    out.append("")
    out.append(f"  the page's link readout said: {result.get('page_link')}")

    return "\n".join(out)


# =============================================================================
# The run
# =============================================================================

def main(argv: "list[str]") -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        description="M24: the web console's render() on a large show (a hand instrument).")
    parser.add_argument("--binary", help="wfg to serve with (default $WFG_BINARY, else "
                                         "build/vs/src/Debug/wfg.exe)")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--ui", help="the client folder to serve (default clients/console)")
    source.add_argument("--ui-rev", help="serve clients/console as it was at this git revision, "
                                         "copied into the temporary folder")
    parser.add_argument("--cues", type=int, default=500, help="media cues in the show (500)")
    parser.add_argument("--polls", type=int, default=10, help="renders to measure (10)")
    parser.add_argument("--browser", help="Chromium-family browser (default Edge, then Chrome, "
                                          "then chromium on PATH)")
    parser.add_argument("--wrap", action="append", default=[],
                        help="another function to time, by name or a.b path (repeatable)")
    parser.add_argument("--go-wait", type=float, default=10.0,
                        help="seconds to wait for a GO to show over HTTP (10)")
    parser.add_argument("--wait", type=float, default=240.0,
                        help="seconds to wait for the page - its first rows, the renders "
                             "measured, the polls let go by (240)")
    parser.add_argument("--json", action="store_true",
                        help="the JSON object on stdout, the readable report on stderr")
    parser.add_argument("--keep", action="store_true",
                        help="leave the temporary folder behind and say where; a kept folder "
                             "is never removed automatically, so delete it yourself")
    args = parser.parse_args(argv)

    if args.polls < 1 or args.cues < 1:
        parser.error("--polls and --cues must be at least 1")

    names = DEFAULT_WRAP + [n for n in args.wrap if n not in DEFAULT_WRAP]

    # SIGTERM and SIGBREAK become the same exit Ctrl-C is, so the finally
    # below runs for every way this can be asked to stop.
    def stop(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")

    for name in ("SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), stop)
            except (OSError, ValueError):
                pass

    job = tie_children_to_this_process()   # noqa: F841 - held for the life of the process

    workspace = Path(tempfile.mkdtemp(prefix=WORKSPACE_PREFIX))
    (workspace / "owner.pid").write_text(str(os.getpid()))
    sweep_abandoned_workspaces(workspace)

    engine = browser = page = None
    reached = "nothing yet"
    code = 0

    try:
        instrument = INSTRUMENT.replace("__TARGETS__", wrap_targets(names))
        binary = find_binary(args.binary)
        chromium = find_browser(args.browser)

        if args.ui_rev:
            ui = copy_ui_at(args.ui_rev, workspace / "ui")
            ui_label = f"clients/console at {args.ui_rev}"
        else:
            ui = Path(args.ui).resolve() if args.ui else REPO / "clients" / "console"
            ui_label = str(ui)
            if not (ui / "index.html").is_file():
                raise HarnessError(f"{ui} has no index.html")

        reached = "writing the show"
        bundle = workspace / "large-show"
        made = subprocess.run([sys.executable, str(MAKE_SHOW), str(bundle),
                               "--cues", str(args.cues)], capture_output=True, text=True)
        if made.returncode != 0:
            raise HarnessError("make_large_show.py failed:\n" + made.stderr)

        show = made.stdout.strip().split(": ", 1)[-1]
        say(show)

        reached = "starting the engine"
        engine = Engine(binary, bundle, ui)
        say(f"engine on http {engine.http_port}")

        reached = "starting the browser"
        browser = Browser(chromium, workspace / "profile")
        version = browser.json("/json/version").get("Browser", "?")
        say(f"{version} on DevTools port {browser.port}")

        reached = "attaching to the page"
        page = DevTools(browser.blank_page()["webSocketDebuggerUrl"])

        for method, params in (("Emulation.setFocusEmulationEnabled", {"enabled": True}),
                               ("Page.bringToFront", {})):
            try:
                page.call(method, params)
            except HarnessError:
                pass                     # a nicety; the measurements do not need it

        # The Page domain on first: without it the browser accepts the script,
        # hands back an identifier, and never runs it.
        reached = "registering the instrument"
        page.call("Page.enable")
        page.call("Page.addScriptToEvaluateOnNewDocument", {"source": instrument})

        reached = "opening the page"
        url = f"http://{HOST}:{engine.http_port}/ui"
        opened = time.monotonic()
        page.call("Page.navigate", {"url": url})

        # SAID WHILE IT HAPPENS, because at 500 cues it could take a while: on
        # the page before PR 5.9 (7154bb2, served with --ui-rev) the first
        # reply had to survive the queue that page's own polls built.
        reached = "waiting for the page to draw the rows"
        deadline = opened + args.wait
        told = opened
        status: dict = {}

        while time.monotonic() < deadline:
            try:
                status = page.evaluate(STATUS) or {}
            except HarnessError:
                status = {}              # between documents, for a moment
            if status.get("rows"):
                break
            if time.monotonic() - told > 15:
                told = time.monotonic()
                say(f"no rows yet after {told - opened:.0f} s: {status.get('issued')} requests "
                    f"issued, {status.get('succeeded')} parsed, {status.get('failed')} failed; "
                    f"the page says {status.get('link')!r}")
            time.sleep(0.25)

        first_rows = time.monotonic() - opened

        if not status.get("rows"):
            raise HarnessError(f"the page drew no rows in #cues within {args.wait:.0f} s "
                               f"({status.get('issued')} requests issued, "
                               f"{status.get('succeeded')} parsed, {status.get('failed')} "
                               f"failed; it said {status.get('link')!r})")

        say(f"rows drawn {first_rows:.1f} s after opening")

        if "render" not in status.get("wrapped", []):
            present = "present" if status.get("instrument") else "missing"
            raise HarnessError(f"render() could not be wrapped - instrument {present}, "
                               f"absent {status.get('absent')}, refused {status.get('refused')}")

        # A second's grace, so the renders measured are not the first ones the
        # JIT ever saw.
        time.sleep(1.0)

        reached = "measuring"
        meta = {"ui": ui_label, "browser": str(chromium), "browser_version": version,
                "binary": str(binary), "show": show, "cues": args.cues, "polls": args.polls,
                "first_rows_s": round(first_rows, 2)}
        result = measure(args, page, engine, names)
        reached = "done"

        text = report(meta, result)

        if args.json:
            print(text, file=sys.stderr)
            print(json.dumps({"instrument": "M24", **meta, **result}, indent=2))
        else:
            print(text)

    except HarnessError as problem:
        say(f"could not finish - got as far as {reached}: {problem}")
        if engine is not None and engine.process.poll() is not None:
            say("the engine's last words:\n    " + "\n    ".join(engine.tail))
        code = 1
    except KeyboardInterrupt:
        say(f"interrupted while {reached}")
        code = 1
    finally:
        # The engine before the browser. It is reaped through its own handle
        # and needs finding by nobody; stopped second, it would still be
        # running while the browser's stragglers are hunted, and its command
        # line names the temporary folder too - a hunt that ever matched on
        # the folder alone would take it for one and kill it.
        if page is not None:
            page.close()
        if engine is not None:
            engine.stop()
        if browser is not None:
            browser.stop()

        if args.keep:
            # Without its owner.pid the folder says nobody made it, and the
            # next run's sweep leaves such a folder alone - which is what
            # --keep promised. A run killed from outside never gets here, so
            # its folder still says who made it, and is still swept.
            try:
                (workspace / "owner.pid").unlink(missing_ok=True)
            except OSError as problem:
                say(f"could not unmark {workspace} ({problem}); the next run may sweep it")
            say(f"kept {workspace}")
        else:
            # Retried, because a browser that has only just exited on Windows
            # can still hold a file in its profile for a moment.
            for _attempt in range(20):
                shutil.rmtree(workspace, ignore_errors=True)
                if not workspace.exists():
                    break
                time.sleep(0.5)
            if workspace.exists():
                say(f"could not remove {workspace}")

    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
