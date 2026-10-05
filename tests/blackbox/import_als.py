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

"""An Ableton Live set imported by the shipped binary (namespace draft §29.3).

WHAT THIS IS FOR. The unit suite builds the session fixture into a document in
memory. What it cannot say is that `wfg import-als`, the verb a person or a
script runs, writes a bundle that both opinions of the grammar accept - the
engine's own `wfg validate` and `scripts/validate-show.py`'s RELAX NG through
somebody else's validator - that the same set imports to the same bytes twice
(QQ), that a folder already holding a show is refused untouched, and that the
report is written beside the show and says what the import could not carry.

The set is `tests/fixtures/als/session.als.xml`, imported with `--no-media`: its
sounds are not in the repository, so the import reports them as not found,
which is one of the things it is checked to say. The exit code is therefore 1,
"imported, with something to read" - 0 is a set with nothing to report.

Exit codes: 0 everything held, 1 something did not, 2 the harness could not run.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common
from common import HarnessError, Report


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SET = REPO_ROOT / "tests" / "fixtures" / "als" / "session.als.xml"
VALIDATE_SHOW = REPO_ROOT / "scripts" / "validate-show.py"


def run(*args: str) -> "tuple[int, str]":
    """The binary once, its output read as the UTF-8 it writes."""
    done = subprocess.run([str(common.find_binary()), *args], capture_output=True,
                          encoding="utf-8", errors="replace", timeout=120)
    return done.returncode, done.stdout + done.stderr


def main(argv: "list[str]") -> int:
    locale = None

    for argument in argv[1:]:
        if argument.startswith("--wfg-locale="):
            locale = argument.split("=", 1)[1]

    report = Report(f"an Ableton Live set, imported ({locale or 'C'})")
    with_locale = [f"--wfg-locale={locale}"] if locale else []

    try:
        with tempfile.TemporaryDirectory(prefix="wfg-als-") as scratch:
            room = Path(scratch)
            first, second = room / "Session", room / "Again"

            code, said = run("import-als", str(SET), f"--into={first}", "--no-media", *with_locale)
            report.equal(code, 1, "the set imports, with something in its report to read", said.strip()[-300:])
            report.check("2 GO(s), 2 sound(s)" in said, "two GOs and two sounds, the unnamed scene left out",
                         said.strip()[-300:])

            for name in ("Session.wfg", "show.xml", "state.xml", "import-report.md"):
                report.check((first / name).exists(), f"the folder holds {name}")

            code, said = run("validate", str(first), *with_locale)
            report.check(code in (0, 1) and ("is valid" in said or "loaded" in said),
                         "the engine opens the show it wrote", said.strip()[-300:])

            done = subprocess.run([sys.executable, str(VALIDATE_SHOW), str(first)], capture_output=True,
                                  encoding="utf-8", errors="replace", timeout=120)
            report.equal(done.returncode, 0, "and somebody else's validator accepts it against show.rng",
                         (done.stdout + done.stderr).strip()[-300:])

            # The same set, again: the same show, byte for byte (QQ).
            run("import-als", str(SET), f"--into={second}", "--no-media", *with_locale)
            report.check((first / "show.xml").read_bytes() == (second / "show.xml").read_bytes(),
                         "the same set imports to the same show.xml twice")

            # A folder that holds a show is refused, and left as it was.
            before = (first / "show.xml").read_bytes()
            code, said = run("import-als", str(SET), f"--into={first}", "--no-media", *with_locale)
            report.equal(code, 2, "a folder already holding a show is refused", said.strip()[-200:])
            report.check((first / "show.xml").read_bytes() == before, "and its show is untouched")

            # The report says what the import could not carry.
            text = (first / "import-report.md").read_text(encoding="utf-8")
            for words in ("## The hands", "CC 120, channel 7", "## Not imported", "panned", "## Sounds not found",
                          "Arrangement view"):
                report.check(words in text, f"the report says \"{words}\"")

            show = (first / "show.xml").read_text(encoding="utf-8")
            report.check("timeline" in show and "<Fade" in show and "<Transport" in show,
                         "the show holds a timeline GO, a fade and a stop")
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2

    return report.finish()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
