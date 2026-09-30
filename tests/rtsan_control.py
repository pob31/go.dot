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

"""Runs the planted violation and passes only if the sanitizer reported it.

The real-time safety job fails when the sanitizer's name appears anywhere in the
test output (.github/workflows/ci.yml reads ctest's LastTest.log). The control's
own report must therefore never reach that output: this wrapper reads it from
the child, checks it, and says what it saw in words that do not trip the gate.
See tests/RtsanControl.cpp for why the control exists at all.

EXIT CODES
    0  the planted call was reported
    1  it was not: the sanitizer is not watching, or its report went nowhere
    2  the control could not be run
"""

from __future__ import annotations

import subprocess
import sys

TELL = "Real" + "timeSanitizer"          # spelled apart: this file's own output must not trip the gate
KIND = "unsafe-library-call"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: rtsan_control.py <path to wfg_rtsan_control>")
        return 2

    try:
        done = subprocess.run([sys.argv[1]], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"the control could not be run: {exc}")
        return 2

    said = done.stdout + done.stderr

    if TELL in said and KIND in said and "malloc" in said:
        print("the sanitizer reported the planted allocation, as it must")
        return 0

    print("the sanitizer did NOT report the planted allocation: a green real-time job would mean nothing")
    print(f"exit code {done.returncode}; the control printed {len(said)} characters")
    return 1


if __name__ == "__main__":
    sys.exit(main())
