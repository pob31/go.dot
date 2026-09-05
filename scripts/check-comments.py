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

"""No comment delimiter hidden inside a comment.

WHY THIS EXISTS. Go.dot's comments explain OSC addresses, and OSC address
PATTERNS use `*`. Two of those spellings are booby-trapped inside a C block
comment, and this project has hit both:

    /godot/cue/*/name     the `*/` ENDS the comment early. The prose after it
                          becomes code, and the compiler reports a syntax error
                          somewhere further down that names nothing relevant.

    /godot/cue/*          the `/*` inside the comment is -Wcomment, which is an
                          ERROR under the strict preset and appears on GCC only.

The first cost three separate debugging sessions in Phase 1 and the second cost
a CI round trip. Both are invisible while writing and obvious once named, which
is exactly what a gate is for.

THE FIX IS ALWAYS TO REWORD, never to escape. "a star where a cue identifier
belongs" reads better than the literal path anyway, and a comment that needs a
backslash to survive is a comment that will be broken again by the next person
who tidies it.

Scans our own sources only — ThirdParty/ is somebody else's problem and is
compiled with -isystem for that reason.

EXIT CODES
    0  clean
    1  a delimiter is hidden inside a comment; the file, line and text are printed
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ("src", "tests", "spikes")
SUFFIXES = (".h", ".hpp", ".cpp", ".mm")


def offences(text: str) -> "list[tuple[int, str]]":
    """(line, why) for every delimiter that is inside a comment.

    A real scanner rather than a regex: it tracks string literals, character
    literals and both comment forms, because `"/*"` in a string is fine and a
    regex cannot tell the difference.
    """
    found: list[tuple[int, str]] = []
    i, n, line = 0, len(text), 1
    quote = chr(34)
    apostrophe = chr(39)
    backslash = chr(92)

    while i < n:
        c = text[i]

        if c == "\n":
            line += 1
            i += 1
            continue

        # --- inside a block comment: look for a nested opener ---------------
        if text.startswith("/*", i):
            start_line = line
            i += 2

            while i < n and not text.startswith("*/", i):
                if text[i] == "\n":
                    line += 1
                elif text.startswith("/*", i):
                    found.append((line, "'/*' inside a block comment "
                                        f"(opened on line {start_line}) — -Wcomment"))
                    i += 1
                    continue
                i += 1

            i += 2
            continue

        # --- a line comment runs to the newline ----------------------------
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue

        # --- string and character literals are not comments ----------------
        if c == quote or c == apostrophe:
            terminator = c
            i += 1

            while i < n and text[i] != terminator:
                if text[i] == backslash:
                    i += 1
                elif text[i] == "\n":
                    line += 1
                i += 1

            i += 1
            continue

        i += 1

    return found


def unterminated(text: str) -> bool:
    """True when a block comment is never closed — the `*/` trap's signature."""
    depth = 0
    i, n = 0, len(text)
    quote = chr(34)
    backslash = chr(92)

    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                return True
            i = end + 2
            continue

        if text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
            continue

        if text[i] == quote:
            i += 1
            while i < n and text[i] != quote:
                if text[i] == backslash:
                    i += 1
                i += 1

        i += 1

    return depth != 0


def main() -> int:
    print(f"check-comments: {REPO_ROOT}")

    failures: list[str] = []
    scanned = 0

    for directory in SCAN_DIRS:
        root = REPO_ROOT / directory

        if not root.is_dir():
            continue

        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in {"build", "__pycache__"}]

            for name in sorted(filenames):
                if not name.endswith(SUFFIXES):
                    continue

                path = Path(dirpath) / name
                text = path.read_text(encoding="utf-8", errors="replace")
                scanned += 1

                rel = path.relative_to(REPO_ROOT).as_posix()

                for line, why in offences(text):
                    failures.append(f"{rel}:{line}: {why}")

                if unterminated(text):
                    failures.append(
                        f"{rel}: a block comment is never closed — most likely a "
                        "'*/' inside one, such as an OSC pattern written as "
                        "'/godot/cue/*' followed by '/name'")

    if failures:
        print("\ncheck-comments: FAILED\n", file=sys.stderr)

        for i, f in enumerate(failures, start=1):
            print(f"  {i}. {f}", file=sys.stderr)

        print("\n  Reword the prose; do not escape the delimiter. 'a star where a cue\n"
              "  identifier belongs' says the same thing and cannot break.",
              file=sys.stderr)
        return 1

    print(f"  ok  {scanned} files, no comment delimiter hidden inside a comment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
