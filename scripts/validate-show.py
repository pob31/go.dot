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
"""Validates Go.dot bundles and show documents against the committed RELAX NG.

Produces: nothing on success.
Exits:   0 everything validated, 1 something did not, 2 there was nothing to
         read or the grammar itself is broken.
Usage:   python3 scripts/validate-show.py <bundle-or-file> [...]
         python3 scripts/validate-show.py            # every fixture in the tree
Build requirements: python3 and lxml. Both are hard requirements of the test
         build, checked at configure time — see the note on skipping below.

WHY THIS EXISTS WHEN `wfg validate` ALREADY DOES

Because `wfg validate` is the engine checking its own homework. It runs the
engine's schema against a document the engine's own reader parsed, so it is
exactly as wrong as the engine is: a mistake the reader and the schema share is
invisible to it, and that is the class of mistake that reaches a show.

This runs a grammar in a published language through somebody else's validator.
When the two disagree, one of them is wrong and we get to find out which —
which is the whole reason docs/schema/show.rng is generated and committed
rather than being an implementation detail.

It is also what anyone outside this repository can run. A show file is XML with
a grammar; nobody should have to build the engine to find out whether theirs is
well formed.

WHY IT NEVER SKIPS

If lxml is missing this exits 2 and says so. It does not pass. The locale rule
set the precedent and the reasoning is the same: a check that quietly reports
success when it did not run is worse than no check, because it also removes the
pressure to fix it.
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GRAMMAR = REPO / "docs" / "schema" / "show.rng"

# The XML a bundle contains. namespaces/*.json is not ours to have an opinion
# about — those files describe other people's programs and arrive as OSCQuery
# JSON, which this grammar says nothing about.
BUNDLE_FILES = ("show.xml", "state.xml")
MANIFEST_SUFFIX = ".wfg"


def targets_in(path: Path):
    """The XML files to check for one argument: a bundle's, or the file itself."""
    if path.is_dir():
        found = [path / name for name in BUNDLE_FILES if (path / name).is_file()]
        found += sorted(path.glob("*" + MANIFEST_SUFFIX))
        return found

    return [path]


def default_targets():
    """Every fixture in the tree, so the no-argument form is the useful one."""
    fixtures = REPO / "tests" / "fixtures"
    found = []

    for bundle in sorted((fixtures / "bundles").glob("*")):
        if bundle.is_dir():
            found += targets_in(bundle)

    found += sorted((fixtures / "documents").glob("*.xml"))
    return found


def main() -> int:
    print(f"validate-show: {REPO}")

    try:
        from lxml import etree
    except ImportError:
        print("  FAIL  lxml is not installed, so nothing was validated.", file=sys.stderr)
        print("        pip install lxml   (or apt install python3-lxml)", file=sys.stderr)
        return 2

    if not GRAMMAR.is_file():
        print(f"  FAIL  no grammar at {GRAMMAR.relative_to(REPO)}", file=sys.stderr)
        print("        generate it:  wfg schema --out=docs/schema/show.rng", file=sys.stderr)
        return 2

    # A grammar that is not itself valid RELAX NG is a generator bug, and it is
    # worth its own exit code: every document below would fail for a reason that
    # has nothing to do with the documents.
    try:
        grammar = etree.RelaxNG(etree.parse(str(GRAMMAR)))
    except (etree.RelaxNGParseError, etree.XMLSyntaxError) as error:
        print(f"  FAIL  {GRAMMAR.relative_to(REPO)} is not usable as RELAX NG:", file=sys.stderr)
        print(f"        {error}", file=sys.stderr)
        return 2

    print(f"  ok    grammar: {GRAMMAR.relative_to(REPO)}")

    if len(sys.argv) > 1:
        targets = []
        for argument in sys.argv[1:]:
            path = Path(argument)
            if not path.exists():
                print(f"  FAIL  no such path: {argument}", file=sys.stderr)
                return 2
            targets += targets_in(path)
    else:
        targets = default_targets()

    if not targets:
        print("  FAIL  nothing to validate", file=sys.stderr)
        return 2

    failures = 0

    for target in targets:
        try:
            shown = target.relative_to(REPO)
        except ValueError:
            shown = target

        try:
            document = etree.parse(str(target))
        except etree.XMLSyntaxError as error:
            print(f"  FAIL  {shown}: not well-formed XML: {error}", file=sys.stderr)
            failures += 1
            continue

        if grammar.validate(document):
            print(f"  ok    {shown}")
            continue

        failures += 1
        print(f"  FAIL  {shown}:", file=sys.stderr)

        for entry in grammar.error_log:
            print(f"        line {entry.line}: {entry.message}", file=sys.stderr)

    print(f"validate-show: {len(targets) - failures}/{len(targets)} valid")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
