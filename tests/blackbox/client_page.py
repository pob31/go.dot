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

"""The engine serves its own client, and answers what that client sends.

WHAT THIS IS FOR. `wfg serve --ui=<dir>` answers `/ui` with a page from disk,
on the same port the OSCQuery tree is on. Same origin, so the page needs no
CORS and a tablet reaches it by the address it already has to know.

That is a file server inside an engine that binds an interface and answers
anybody who can reach it, so the first assertions here are the refusals: a path
that climbs out of the directory, a file that is not there, and — the one that
would be easy to lose — the tree still answering on the same port.

AND WHAT COMES BACK IS THE FILE. A client is not only its page: it ships icons,
fonts and — once it is split into ES modules — a directory of `.mjs`. None of
those survive a UTF-8 round trip, so one case here serves a PNG this driver
builds itself and compares the reply with the bytes on disk, which is the only
assertion that can tell a served file from a mangled one.

The second half is the other direction. The page reads over HTTP and writes by
sending binary OSC on the WebSocket that answers on the same port, so an edit in
its inspector is `node.set` and a new cue is `cue.create` — every one of them a
named command (§4.11), arriving exactly as it would from a hardware surface.
Driven here over the harness's own socket, which sends the same bytes.

INCLUDING THE REFUSAL, which is the one that was actually broken: OSC has no
reply channel, so a client that writes a word to a number learns of it only by
reading `/godot/engine/lastError` — and the serve loop published the COUNT of
refusals while never publishing their text. Every refusal in a running engine
arrived as an empty string. Nothing in the unit suite could see it; a client
found it on its first edit.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import json
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report, Server


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "bundles" / "minimal"
CLIENT = REPO_ROOT / "clients" / "console"


def run(locale: "str | None") -> int:
    report = Report(f"the engine serves its client ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-client-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "minimal")

        with Server(bundle, locale=locale, ui=CLIENT) as server:
            status, body = common.http_get(server.http_port, "/ui")

            """AND THE TERMINAL SAID WHERE IT IS, which is the assertion that
            would have saved an evening. `wfg: http 5010` is a fact and not an
            instruction: the obvious thing to do with a client sitting in the
            repository is to double-click it, which opens it as a `file:` URL
            with no engine behind it and no cue list in it. So the engine prints
            an address, spelled the way it has to be typed, ending in `/ui`."""
            told = [line for line in server.notices if "/ui" in line]

            report.check(bool(told), "starting with --ui prints an address to open",
                         "\n".join(server.notices))

            if told:
                report.check(told[0].endswith("/ui"),
                             "and it ends in /ui, which is the part that is easy to lose",
                             told[0])
                report.check("http://" in told[0],
                             "and it is a URL rather than a port number", told[0])

            report.equal(status, 200, "GET /ui answers with the page")
            report.check("<title>Go.dot</title>" in body,
                         "and it is the client rather than something else")

            # The same file by its own name, which is what a bookmark holds.
            named, _ = common.http_get(server.http_port, "/ui/index.html")
            report.equal(named, 200, "GET /ui/index.html answers too")

            # A file that is not there is a 404 and not a 500.
            missing, _ = common.http_get(server.http_port, "/ui/nothing.js")
            report.equal(missing, 404, "a file that is not there is refused")

            """A PATH THAT CLIMBS OUT IS REFUSED, and it is checked with the
            escape a request parser has already unescaped rather than with a
            literal `..`, because that is the form that walks past a check
            written against the text of the request. The guard asks whether the
            file it resolved is a descendant of the directory, which is the
            question that cannot be talked around."""
            for climb in ("/ui/../CLAUDE.md",
                          "/ui/..%2f..%2fCLAUDE.md",
                          "/ui/....//CLAUDE.md"):
                code, text = common.http_get(server.http_port, climb)
                report.check(code == 404 and "GPL" not in text,
                             f"{climb} does not escape the client directory",
                             f"answered {code}")

            """AND THE TREE IS STILL THERE, on the same port, which is the whole
            point of serving the page from it. A route added in front of the
            namespace is exactly the kind of change that could shadow it."""
            code, _ = common.http_get(server.http_port, "/godot/list/order?VALUE")
            report.equal(code, 200, "the OSCQuery tree still answers on this port")

            code, _ = common.http_get(server.http_port, "/godot")
            report.equal(code, 200, "and so does the whole tree")

    return report.finish()


def one_pixel_png() -> bytes:
    """A valid one-pixel PNG, built here rather than committed as a blob.

    A dozen lines of stdlib, and the bytes are then visible to whoever reads
    this test rather than sitting in a file nobody in a review can open.
    Greyscale, eight bits, one row of one sample behind its
    zero filter byte; a chunk is a length, a type, its data and the CRC-32 of
    the last two (PNG 1.2 §5). Sixty-seven bytes, and a decoder accepts it.

    WHY A PNG AND NOT A TEXT FILE, which is the whole point of the assertion
    below. Its first byte is 0x89, which begins no UTF-8 sequence, and its
    ninth is a zero because the length field in front of IHDR is 00 00 00 0D.
    A server that read the file into a string and re-encoded it hands back a
    replacement character wherever the decoder gave up; one that built a string
    from a pointer with no length stops at that zero and serves eight bytes.
    Both answer 200 with the right content type in front of them, so only a
    driver that compares the bytes can see either.
    """
    def chunk(kind: bytes, data: bytes) -> bytes:
        return (len(data).to_bytes(4, "big") + kind + data
                + (zlib.crc32(kind + data) & 0xFFFFFFFF).to_bytes(4, "big"))

    signature = bytes([137, 80, 78, 71, 13, 10, 26, 10])

    # Width, height, then bit depth, colour type, compression, filter and
    # interlace — the thirteen bytes of IHDR, in that order.
    header = (1).to_bytes(4, "big") + (1).to_bytes(4, "big") + bytes([8, 0, 0, 0, 0])

    row = bytes([0, 128])           # the zero filter byte, then the pixel

    return (signature
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(row))
            + chunk(b"IEND", b""))


def serves_files(locale: "str | None") -> int:
    """A file comes back as the bytes on disk, whatever kind of file it is.

    ITS OWN DIRECTORY, and not `clients/console`: this driver writes the files
    it asks for, and a test that writes into the repository is a test that
    leaves somebody's working tree dirty and their next `git status` confusing.
    The directory is a client as far as the engine is concerned once it holds
    the one file that makes it one: `wfg serve` refuses a `--ui` with no
    `index.html` in it, and says so before it opens a port, so a page nobody
    wrote is not a page anybody can be sent to. Beyond that file nothing says
    what may live there, which is the property under test.
    """
    report = Report(f"the client directory is served as bytes ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-bytes-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "minimal")

        ui = Path(scratch) / "ui"
        ui.mkdir()

        (ui / "index.html").write_text("<!doctype html><title>Go.dot</title>\n",
                                       encoding="utf-8")

        picture = ui / "pixel.png"
        picture.write_bytes(one_pixel_png())

        module = ui / "panel.mjs"
        module.write_text("export const nothing = 0;\n", encoding="utf-8")

        with Server(bundle, locale=locale, ui=ui) as server:
            status, headers, body = common.http_get_bytes(server.http_port,
                                                          "/ui/pixel.png")

            report.equal(status, 200, "GET /ui/pixel.png answers")
            report.equal(body, picture.read_bytes(),
                         "and it is byte for byte the file on disk",
                         f"{len(body)} bytes back, "
                         f"{picture.stat().st_size} on disk")
            report.equal(headers.get("content-type", ""), "image/png",
                         "and it says what it is")

            """NOT CACHED, which §14.5 will assert of the media route as well:
            the client directory is edited while the engine runs, and a stale
            copy after a refresh is a minute of somebody wondering why their
            change did nothing."""
            report.equal(headers.get("cache-control", ""), "no-store",
                         "and tells the browser not to keep it")

            """AN ES MODULE IS `text/javascript`, and a browser refuses to
            import one served as anything else - it blocks the script before it
            parses it and says so in a console nobody has open, which reads as
            a blank page rather than as a MIME table missing a line. The console
            becomes modules loaded from this directory with no build step, so
            the extension has to be in that table before it can."""
            status, headers, body = common.http_get_bytes(server.http_port,
                                                          "/ui/panel.mjs")

            report.equal(status, 200, "GET /ui/panel.mjs answers")
            report.check(headers.get("content-type", "").startswith("text/javascript"),
                         "and .mjs is served as text/javascript, as .js is",
                         headers.get("content-type", "<no content type>"))
            report.equal(body, module.read_bytes(),
                         "and its bytes are the file's too")

    return report.finish()


def value_at(port: int, address: str):
    """The value at one address, or None when the node carries none."""
    status, body = common.http_get(port, address + "?VALUE")

    if status == 204:
        return None

    if status != 200:
        raise HarnessError(f"GET {address}?VALUE returned {status}")

    return json.loads(body)["VALUE"][0]


def settles(port: int, address: str, wanted, timeout: float = 5.0) -> bool:
    """Waits for a value to arrive, because a command is applied on a tick.

    Polled rather than slept through: the engine applies on its own clock, and
    how many ticks pass between the send and the publish is not this driver's
    business to know.
    """
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if value_at(port, address) == wanted:
            return True

        time.sleep(0.02)

    return value_at(port, address) == wanted


def edits(locale: "str | None") -> int:
    report = Report(f"the client's gestures reach the engine ({locale or 'C'})")

    with tempfile.TemporaryDirectory(prefix="wfg-edits-") as scratch:
        bundle = common.copy_bundle(FIXTURE, Path(scratch) / "minimal")

        with Server(bundle, locale=locale, ui=CLIENT) as server:
            port = server.http_port
            link = common.WSClient(port)

            try:
                """A VALUE, which is what every field in the inspector sends,
                and it is sent as TEXT whatever the node's type is: `node.set`
                declares its value argument as "whatever the target says" and
                turns what arrives into canonical text before the schema parses
                it. A client sending "2.5" and one sending the double produce
                the identical document, and the page leans on that rather than
                guessing a type it could get wrong."""
                link.send_osc("/godot/cue/B3N8R5TW/name", ["House to a quarter"])
                report.check(settles(port, "/godot/cue/B3N8R5TW/name", "House to a quarter"),
                             "an edit to a name lands in the document")

                link.send_osc("/godot/cue/F7HR8TVD/postWait", ["2.5"])
                report.check(settles(port, "/godot/cue/F7HR8TVD/postWait", 2.5),
                             "and a number sent as text is read as a number")

                link.send_osc("/godot/cue/E4GP6QSC/enabled", ["false"])
                report.check(settles(port, "/godot/cue/E4GP6QSC/enabled", False),
                             "and a tick box is a boolean")

                """AND A REFUSAL COMES BACK, which is the whole of what a client
                gets: nothing replies on this socket, so a write the engine
                would not take is indistinguishable from one it took unless this
                node says otherwise."""
                before = value_at(port, "/godot/engine/errorCount")
                link.send_osc("/godot/cue/F7HR8TVD/postWait", ["not a number"])

                settles(port, "/godot/engine/errorCount", before + 1)
                answer = value_at(port, "/godot/engine/lastError")
                refusal = answer if isinstance(answer, str) else ""

                report.check(bool(refusal), "a refused write says so at lastError",
                             f"lastError is {refusal!r}")
                report.check("node.set" in refusal,
                             "and names the command that was refused", refusal)
                report.check(settles(port, "/godot/cue/F7HR8TVD/postWait", 2.5),
                             "and the value it aimed at did not change")

                """STRUCTURE. The inspector's `+ cue` puts a new one after the
                cue being looked at, which is `cue.create <parent> <index>`."""
                order = value_at(port, "/godot/list/7K2QM9X4/order").split()
                link.send_osc("/godot/cmd/cue/create",
                              ["7K2QM9X4", 1, "osc", "Made by the client"])

                grew = lambda: len(
                    value_at(port, "/godot/list/7K2QM9X4/order").split()) == len(order) + 1

                deadline = time.monotonic() + 5.0

                while time.monotonic() < deadline and not grew():
                    time.sleep(0.02)

                report.check(grew(), "cue.create added exactly one cue",
                             value_at(port, "/godot/list/7K2QM9X4/order"))

                grown = value_at(port, "/godot/list/7K2QM9X4/order").split()
                fresh = grown[1] if len(grown) > 1 else ""

                report.check(fresh not in order, "and put it where it was asked to",
                             " ".join(grown))
                report.equal(value_at(port, f"/godot/cue/{fresh}/kind"), "osc",
                             "of the kind it was asked for")

                """AND IT MOVES, which is the inspector's two arrows: the same
                `object.move`, given the parent it already has, so the command
                that reparents is the command that reorders."""
                link.send_osc("/godot/cmd/object/move", [fresh, "7K2QM9X4", 0])
                report.check(settles(port, "/godot/list/7K2QM9X4/order",
                                     " ".join([fresh] + order)),
                             "object.move takes a cue one place earlier")

                link.send_osc("/godot/cmd/object/move", [fresh, "7K2QM9X4", 1])
                report.check(settles(port, "/godot/list/7K2QM9X4/order",
                                     " ".join([order[0], fresh] + order[1:])),
                             "and back again")

                """THE POINTER, which the page moves from a row's gutter and
                from the transport bar - and never from selecting a row, which
                §3.5 is explicit about."""
                link.send_osc("/godot/cmd/standby/set", ["B3N8R5TW"])
                report.check(settles(port, "/godot/list/7K2QM9X4/standby", "B3N8R5TW"),
                             "standby.set parks the pointer")

                link.send_osc("/godot/cmd/standby/next")
                report.check(settles(port, "/godot/list/7K2QM9X4/standby", fresh),
                             "standby.next moves it on")

                link.send_osc("/godot/cmd/object/delete", [fresh])
                report.check(settles(port, "/godot/list/7K2QM9X4/order", " ".join(order)),
                             "and object.delete takes the cue away again")
            finally:
                link.close()

    return report.finish()


def refuses_a_bad_directory() -> int:
    """`--ui` pointed at nothing is a sentence at startup, not a 404 later.

    A mistyped path is the ordinary mistake here, and the moment to say so is
    while somebody is still looking at the terminal they typed it into.
    """
    report = Report("--ui says so at once when the directory is wrong")

    code, out, err = common.run_wfg("serve", str(FIXTURE),
                                    "--sample-rate=48000", "--buffer=128",
                                    "--http-port=0", "--osc-port=0",
                                    "--ui=" + str(REPO_ROOT / "no" / "such" / "place"))

    report.equal(code, 2, "it refuses to start")
    report.check("--ui" in (out + err), "and the message names the flag",
                 (out + err).strip()[:200])

    return report.finish()


def main() -> int:
    try:
        common.find_binary()
    except HarnessError as problem:
        print(f"client_page: {problem}", file=sys.stderr)
        return 2

    if not CLIENT.is_dir():
        print(f"client_page: no client at {CLIENT}", file=sys.stderr)
        return 2

    locale = None

    for argument in sys.argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    try:
        return max(run(locale), serves_files(locale), edits(locale),
                   refuses_a_bad_directory())
    except HarnessError as problem:
        print(f"client_page: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
