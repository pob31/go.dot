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

"""Assert that the shipped binary links no OpenSSL.

WHY THIS EXISTS. juce_simpleweb declares OpenSSL as a link dependency on all
three platforms, in two different spellings (juce_simpleweb.h:25-27):

    linuxLibs:    ssl,crypto            <- bare
    OSXLibs:      libssl,libcrypto,z    <- lib-prefixed
    windowsLibs:  libssl,libcrypto      <- lib-prefixed

Go.dot compiles that module with SIMPLEWEB_SECURE_SUPPORTED=0 and calls no TLS
code at all, so those libraries must not be on the link line. WfgThirdParty.cmake
clears the module's INTERFACE_LINK_LIBRARIES outright to make that true.

The upstream recipe this project transcribed - spatcore's
spatcore_add_juce_simpleweb() - instead REMOVE_ITEMs the list `libssl libcrypto
z`, which matches macOS and Windows and misses Linux entirely, because there the
items are spelled `ssl` and `crypto`. That is a gate that fails open, and it is
exactly the failure this script is here to catch: it asks the BINARY what it
actually depends on, rather than trusting a CMake property to have been edited
correctly.

An OpenSSL dependency would mean a shipped Go.dot that refuses to start on a
machine without the runtime, in service of a feature it does not have.

EXIT CODES
    0  the binary depends on no OpenSSL library
    1  it does - the names found are printed
    2  the check could not run (missing binary, missing tool, unreadable format)

Two, never zero, when the check cannot run. A gate that reports success without
having looked is worse than no gate: it also removes the pressure to fix it.
"""

from __future__ import annotations

import argparse
import platform
import struct
import subprocess
import sys
from pathlib import Path

# Matched case-insensitively as substrings of a dependency's file name. `ssleay`
# and `libeay` are the pre-1.1 Windows names, kept because a stray old import
# library on a build machine is exactly the kind of thing this should catch.
FORBIDDEN = ("libssl", "libcrypto", "ssleay", "libeay")


def fail(message: str) -> "int":
    print(f"check-no-openssl: CANNOT RUN: {message}", file=sys.stderr)
    return 2


def run_tool(argv: list[str]) -> "tuple[int, str]":
    try:
        done = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    except FileNotFoundError:
        return 2, f"{argv[0]} is not installed"
    except subprocess.TimeoutExpired:
        return 2, f"{argv[0]} timed out"

    if done.returncode != 0:
        return 2, f"{argv[0]} exited {done.returncode}: {done.stderr.strip()}"

    return 0, done.stdout


def pe_imports(path: Path) -> "tuple[list[str], str]":
    """The DLL names in a PE binary's import table.

    Parsed here rather than shelled out to `dumpbin /DEPENDENTS`, which lives
    inside the Visual Studio installation and is not on PATH in a plain shell.
    Requiring it would mean this gate could only run from a developer command
    prompt - and per the exit codes above, "could not run" is a failure, so it
    would fail for everyone else. The format is fixed and the walk is short.
    """
    data = path.read_bytes()

    if len(data) < 0x40 or data[:2] != b"MZ":
        return [], "not a PE binary (no MZ header)"

    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]

    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return [], "not a PE binary (no PE signature)"

    coff = pe_offset + 4
    num_sections, = struct.unpack_from("<H", data, coff + 2)
    opt_size, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20

    magic, = struct.unpack_from("<H", data, opt)

    if magic == 0x20B:                      # PE32+
        dir_offset = opt + 112
    elif magic == 0x10B:                    # PE32
        dir_offset = opt + 96
    else:
        return [], f"unknown optional-header magic 0x{magic:x}"

    num_dirs, = struct.unpack_from("<I", data, dir_offset - 4)

    if num_dirs < 2:
        return [], "no import directory"

    import_rva, import_size = struct.unpack_from("<II", data, dir_offset + 8)

    if import_rva == 0 or import_size == 0:
        return [], ""                       # genuinely imports nothing

    # Section table, for the RVA -> file offset mapping.
    sections = []
    sec = opt + opt_size

    for i in range(num_sections):
        base = sec + i * 40

        if base + 40 > len(data):
            return [], "truncated section table"

        virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from(
            "<IIII", data, base + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_ptr))

    def to_offset(rva: int) -> "int | None":
        for virtual_address, size, raw_ptr in sections:
            if virtual_address <= rva < virtual_address + size:
                return raw_ptr + (rva - virtual_address)
        return None

    def read_c_string(offset: int) -> str:
        end = data.find(b"\0", offset)
        return data[offset:end if end != -1 else len(data)].decode(
            "ascii", errors="replace")

    table = to_offset(import_rva)

    if table is None:
        return [], "import directory RVA is outside every section"

    names: list[str] = []

    # IMAGE_IMPORT_DESCRIPTOR is 20 bytes; an all-zero one ends the array.
    for i in range(4096):
        base = table + i * 20

        if base + 20 > len(data):
            return [], "truncated import descriptor array"

        fields = struct.unpack_from("<IIIII", data, base)

        if not any(fields):
            break

        name_offset = to_offset(fields[3])

        if name_offset is not None:
            names.append(read_c_string(name_offset))

    return names, ""


def dependencies_of(path: Path) -> "tuple[list[str], str]":
    system = platform.system()

    if system == "Linux":
        code, out = run_tool(["ldd", str(path)])
        return ([], out) if code else ([line.strip() for line in out.splitlines()], "")

    if system == "Darwin":
        code, out = run_tool(["otool", "-L", str(path)])
        return ([], out) if code else ([line.strip() for line in out.splitlines()], "")

    if system == "Windows":
        return pe_imports(path)

    return [], f"unsupported platform {system!r}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="the executable to inspect")
    args = parser.parse_args()

    path = Path(args.binary)

    if not path.is_file():
        return fail(f"no such file: {path}")

    print(f"check-no-openssl: {path}")

    entries, problem = dependencies_of(path)

    if problem:
        return fail(problem)

    offenders = [e for e in entries
                 if any(name in e.lower() for name in FORBIDDEN)]

    if offenders:
        print("check-no-openssl: FAILED — the binary links OpenSSL:",
              file=sys.stderr)

        for entry in offenders:
            print(f"    {entry}", file=sys.stderr)

        print("\n  Go.dot compiles juce_simpleweb with SIMPLEWEB_SECURE_SUPPORTED=0 and\n"
              "  calls no TLS code. Check that WfgThirdParty.cmake still CLEARS the\n"
              "  module's INTERFACE_LINK_LIBRARIES rather than filtering it: the module\n"
              "  spells the libraries `ssl,crypto` on Linux and `libssl,libcrypto` on\n"
              "  macOS and Windows, so a REMOVE_ITEM list misses one platform.",
              file=sys.stderr)
        return 1

    print(f"  ok  {len(entries)} dependencies, none of them OpenSSL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
