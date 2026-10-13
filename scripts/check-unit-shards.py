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

"""Every unit case runs exactly once per locale, whatever the shards say.

WHY THIS EXISTS. The unit suite is one binary run as several ctest entries -
`unit.C.<shard>` and `unit.fr_FR.<shard>` - each handed a slice of the cases by
doctest's own filters, so that the slices can run side by side (2026-10-13: the
whole suite in one process was ten minutes of a forty-minute run, alone on one
core). A slice is a list of test FILES, or a range of one file's cases, plus
one slice for every file nobody named. Three mistakes would each make a case
silently stop running, or run twice, and none of them is visible from a green
ctest: a file named in two shards, a file renamed so that its name in the
shards matches nothing (its cases then fall into the rest - or nowhere, if the
rest's exclusion still names it), and a pattern loose enough to catch a second
file (`*RunTests.cpp` is also TimecodeRunTests.cpp). A slice that reports
green over nothing is the failure TestMain.cpp's locale check exists to
prevent, one level up.

SO IT ASKS THE BINARY, not the CMake lists: each shard's own arguments with
doctest's --list-test-cases, which applies exactly the filters a run applies -
--source-file, --source-file-exclude, --first and --last, the common exclusion
of the GPU cases - and reports each case with its file and line. Every case of
the whole list must come back from exactly one shard; every file a shard names
must hold a case the shard lists. What goes wrong is said case by case.

USAGE
    check-unit-shards.py <wfg_tests> --common=<arg> ... --shard=<name>|<arg>|<arg> ...

`--common` is an argument every shard and the whole list are run with (the GPU
exclusion). Each `--shard` is a name, then the shard's doctest arguments, `|`
between them; a `--source-file=` among them names files the shard must hold.

EXIT CODES
    0  every case in exactly one shard, every named file found
    1  a case in no shard or in two, or a named file holding nothing; each said
    2  the binary could not be run or its list could not be read
"""

from __future__ import annotations

import collections
import subprocess
import sys
import xml.etree.ElementTree as ET


def listed(binary: str, arguments: "list[str]") -> "list[tuple[str, int, str]]":
    """(file, line, name) of every case these arguments would run."""
    done = subprocess.run([binary, *arguments, "--list-test-cases", "--reporters=xml"],
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)

    if done.returncode != 0:
        raise RuntimeError(f"{binary} {' '.join(arguments)} exited {done.returncode}:\n"
                           f"{(done.stdout + done.stderr).strip()[:2000]}")

    start = done.stdout.find("<?xml")

    if start < 0:
        raise RuntimeError(f"{binary} {' '.join(arguments)} printed no list")

    root = ET.fromstring(done.stdout[start:])
    return [(case.get("filename", "").replace("\\", "/"), int(case.get("line", "0")), case.get("name", ""))
            for case in root.iter("TestCase")]


def base_name(path: str) -> str:
    return path.rsplit("/", 1)[-1]


def named_files(arguments: "list[str]") -> "list[str]":
    """The files a shard's --source-file= patterns name, as the last part of
    each pattern: `*tests?AudioTests.cpp` names AudioTests.cpp."""
    names = []

    for argument in arguments:
        if argument.startswith("--source-file="):
            for pattern in argument[len("--source-file="):].split(","):
                names.append(pattern.replace("\\", "/").rsplit("?", 1)[-1].rsplit("/", 1)[-1])

    return names


def main(argv: "list[str]") -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2

    binary = argv[1]
    common: "list[str]" = []
    shards: "list[tuple[str, list[str]]]" = []

    for argument in argv[2:]:
        if argument.startswith("--common="):
            common.append(argument[len("--common="):])
        elif argument.startswith("--shard="):
            name, *rest = argument[len("--shard="):].split("|")
            shards.append((name, [part for part in rest if part]))
        else:
            print(f"check-unit-shards: not understood: {argument}")
            return 2

    try:
        whole = listed(binary, common)
        per_shard = {name: listed(binary, [*arguments, *common]) for name, arguments in shards}
    except (OSError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as error:
        print(f"check-unit-shards: {error}")
        return 2

    problems: "list[str]" = []
    homes: "dict[tuple[str, int, str], list[str]]" = collections.defaultdict(list)

    for name, cases in per_shard.items():
        if not cases:
            problems.append(f"shard {name} runs no case at all")

        for case in cases:
            homes[case].append(name)

    for case in whole:
        where = homes.get(case, [])

        if len(where) != 1:
            file, line, title = case
            said = "in no shard" if not where else "in " + " and ".join(where)
            problems.append(f"{base_name(file)}({line}) \"{title}\" is {said}")

    known = set(whole)

    for case, where in homes.items():
        if case not in known:
            file, line, title = case
            problems.append(f"{base_name(file)}({line}) \"{title}\" runs in {', '.join(where)} "
                            f"but is not in the whole list")

    for name, arguments in shards:
        held = {base_name(file) for file, _, _ in per_shard[name]}

        for file in named_files(arguments):
            if file not in held:
                problems.append(f"shard {name} names {file}, which holds no case it runs "
                                f"(renamed, removed, or misspelled in tests/CMakeLists.txt)")

    files = collections.Counter(base_name(file) for file, _, _ in whole)

    for name, cases in per_shard.items():
        mine = collections.Counter(base_name(file) for file, _, _ in cases)
        print(f"  {name}: {len(cases)} cases from {len(mine)} file(s)")

    if problems:
        for problem in problems:
            print(f"FAIL {problem}")
        print(f"\ncheck-unit-shards: {len(problems)} problem(s) over {len(whole)} cases "
              f"in {len(files)} files and {len(shards)} shards")
        return 1

    print(f"check-unit-shards: ok - {len(whole)} cases in {len(files)} files, "
          f"each in exactly one of {len(shards)} shards")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
