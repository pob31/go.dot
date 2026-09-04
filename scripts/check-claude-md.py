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
"""CLAUDE.md drift gate for Go.dot.

Produces: nothing. Exits 0 when CLAUDE.md's constraints section is byte-identical
         to the PRD's, 1 with a unified diff when it has drifted.
Usage:   python3 scripts/check-claude-md.py
Build requirements: python3. Nothing else — this runs in the `pins` job, which
         has no compiler and is deliberately the cheapest thing in CI.

WHY THIS EXISTS

The devplan makes CLAUDE.md "= PRD §4" and the review criterion for every PR.
Two documents saying the same thing is two documents that can disagree, and the
one that drifts silently is the one people actually read. CLAUDE.md says of
itself that the PRD wins and that it must be fixed by copying, never by editing.
That sentence is worth nothing unless something checks it.

WHY IT ANCHORS ON THE HEADING, NOT LINE NUMBERS

§4 sat at docs/godot-prd-draft-0.7.md:1183-1217 when this was written. It will not tomorrow:
the spike reports amend §3.24, §3.25 and §6.1, and every one of those edits
moves §4. A line-number gate would fail on the first amendment and be deleted by
the third. So both sides are extracted the same way — from the heading line to
the next horizontal rule — and the PRD is found by glob, so the 0.7 -> 0.8
rename does not touch this file either.
"""

import difflib
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

HEADING = "## 4. Constraints as law"


def extract_section(path: Path) -> list[str] | None:
    """The block from HEADING up to (not including) the next `---` rule.

    Identical logic on both sides, so a mismatch is always a real difference in
    the text and never an artefact of two different extractors.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index(HEADING)
    except ValueError:
        return None
    body = []
    for line in lines[start:]:
        if line.rstrip() == "---" and body:
            break
        body.append(line.rstrip("\n"))
    while body and not body[-1].strip():
        body.pop()
    return body


def find_prd(failures: list[str]) -> Path | None:
    matches = sorted((REPO_ROOT / "docs").glob("godot-prd-draft-*.md"))
    if not matches:
        failures.append("no docs/godot-prd-draft-*.md found - is the PRD still in docs/?")
        return None
    if len(matches) > 1:
        names = ", ".join(m.name for m in matches)
        failures.append(
            f"more than one PRD draft in docs/ ({names}).\n"
            "    A version bump should `git mv` the old one, not leave both: with two\n"
            "    drafts present there is no single answer to what CLAUDE.md must match."
        )
        return None
    return matches[0]


def main() -> int:
    print(f"check-claude-md: {REPO_ROOT}")
    failures: list[str] = []

    prd = find_prd(failures)
    claude = REPO_ROOT / "CLAUDE.md"

    if prd is not None:
        print(f"  ok  (a) PRD located: docs/{prd.name}")

        if not claude.is_file():
            failures.append(
                "CLAUDE.md does not exist.\n"
                "    The devplan's Phase 0 scaffold requires it, = PRD section 4 verbatim."
            )
        else:
            want = extract_section(prd)
            got = extract_section(claude)

            if want is None:
                failures.append(
                    f"docs/{prd.name} has no line reading exactly:\n"
                    f"        {HEADING}\n"
                    "    Either the section was renamed or renumbered. If it was renamed\n"
                    "    deliberately, update HEADING in this script and re-copy CLAUDE.md."
                )
            elif got is None:
                failures.append(
                    f"CLAUDE.md has no line reading exactly:\n        {HEADING}\n"
                    "    CLAUDE.md keeps the PRD's section number on purpose, so that this\n"
                    "    comparison is exact. Do not renumber it."
                )
            else:
                print(f"  ok  (b) section found in both ({len(want)} lines)")
                if want == got:
                    print("  ok  (c) CLAUDE.md matches the PRD byte-for-byte")
                else:
                    diff = "\n".join(
                        "      " + ln
                        for ln in difflib.unified_diff(
                            want, got,
                            fromfile=f"docs/{prd.name} (authoritative)",
                            tofile="CLAUDE.md (stale)",
                            lineterm="",
                        )
                    )
                    failures.append(
                        "CLAUDE.md has drifted from the PRD.\n"
                        "    The PRD wins. Fix this by RE-COPYING the section, never by\n"
                        "    editing CLAUDE.md to taste - if the constraint itself needs to\n"
                        "    change, change it in the PRD and copy the result.\n\n"
                        f"{diff}"
                    )

    if failures:
        print("\ncheck-claude-md: FAILED\n")
        for i, f in enumerate(failures, start=1):
            print(f"  {i}. {f}\n")
        return 1

    print("\ncheck-claude-md: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
