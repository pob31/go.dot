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
"""The compiled client reads one door and names nothing behind it.

Namespace draft §14.16 gives the desktop client three rules, and the second -
the client reads only ParameterTree::snapshot() - is the one a careless
afternoon breaks without noticing: the document is on the same heap, and a
reach past the door compiles. This reads the source under src/wfg/client and
says so, the way check-no-openssl.py reads the binary rather than trusting a
CMake property to have stayed edited.

  (a) no childrenOf() anywhere in the client - it calls all(), which allocates
      a vector of the whole tree, and per row that is quadratic
  (b) nothing the tick thread owns is named: ShowDocument, doc::, ValueTree,
      EngineState, UndoManager, ChangeListener
  (c) exactly ONE snapshot() call site in the whole client, in the root timer
  (d) the model half and the public header name no JUCE type at all - they are
      std only, as Engine.h is, so they can be tested with no window

Comments are stripped before any of it is counted, so a comment may say
"childrenOf is banned" without tripping (a). Standard library only.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
CLIENT = REPO_ROOT / "src" / "wfg" / "client"

FORBIDDEN_EVERYWHERE = ["ShowDocument", "doc::", "ValueTree", "EngineState",
                        "UndoManager", "ChangeListener"]

_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def code_of(path):
    """The file with its comments removed; strings are left alone on purpose,
    because a string that names a forbidden type is still a finding."""
    return _COMMENT.sub(" ", path.read_text(encoding="utf-8"))


def sources():
    return sorted(p for p in CLIENT.rglob("*") if p.suffix in (".h", ".cpp", ".mm"))


def is_model(path):
    rel = path.relative_to(CLIENT).as_posix()
    return rel.startswith("model/") or rel == "Client.h"


def main():
    files = sources()
    if not files:
        print("check-client-boundary: nothing under %s yet" % CLIENT)
        return 0

    failures = []
    code = {p: code_of(p) for p in files}
    word = lambda token: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])")

    # (a)
    hits = [p for p in files if word("childrenOf").search(code[p])]
    if hits:
        failures.append("(a) childrenOf() is called in: " + ", ".join(str(p.relative_to(REPO_ROOT)) for p in hits))
    else:
        print("  ok  (a) no childrenOf() under src/wfg/client")

    # (b)
    for token in FORBIDDEN_EVERYWHERE:
        pattern = word(token) if "::" not in token else re.compile(r"(?<![A-Za-z0-9_])" + re.escape(token))
        hits = [p for p in files if pattern.search(code[p])]
        if hits:
            failures.append("(b) %s is named in: %s" % (token, ", ".join(str(p.relative_to(REPO_ROOT)) for p in hits)))
    if not any(f.startswith("(b)") for f in failures):
        print("  ok  (b) nothing the tick thread owns is named")

    # (c)
    sites = []
    for p in files:
        for m in re.finditer(r"snapshot\s*\(\s*\)", code[p]):
            line = code[p].count("\n", 0, m.start()) + 1
            sites.append("%s:%d" % (p.relative_to(REPO_ROOT), line))
    if len(sites) != 1:
        failures.append("(c) snapshot() is called at %d sites, not one: %s" % (len(sites), ", ".join(sites) or "none"))
    else:
        print("  ok  (c) one snapshot() call site: %s" % sites[0])

    # (d)
    hits = [p for p in files if is_model(p) and word("juce").search(code[p])]
    if hits:
        failures.append("(d) the model names juce in: " + ", ".join(str(p.relative_to(REPO_ROOT)) for p in hits))
    else:
        print("  ok  (d) the model half and Client.h name no JUCE type")

    if failures:
        for f in failures:
            print("  FAIL " + f)
        print("check-client-boundary: FAILED")
        return 1

    print("check-client-boundary: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
