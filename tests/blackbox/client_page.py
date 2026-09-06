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

"""The engine serves its own client, and serves nothing else.

WHAT THIS IS FOR. `wfg serve --ui=<dir>` answers `/ui` with a page from disk,
on the same port the OSCQuery tree is on. Same origin, so the page needs no
CORS and a tablet reaches it by the address it already has to know.

That is a file server inside an engine that binds an interface and answers
anybody who can reach it, so the interesting assertions here are the refusals:
a path that climbs out of the directory, a file that is not there, and — the
one that would be easy to lose — the tree still answering on the same port.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import sys
import tempfile
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
        return max(run(locale), refuses_a_bad_directory())
    except HarnessError as problem:
        print(f"client_page: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
