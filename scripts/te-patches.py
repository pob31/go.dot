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
"""Go.dot's patches to Tracktion Engine, by hand: scripts/patches.py tracktion_engine.

Usage:   python3 scripts/te-patches.py status | apply [--3way] | revert | refresh
                                       | new <name.patch> <file> [<file>...]

The same commands as `scripts/patches.py tracktion_engine ...`, kept under the
name the Tracktion patches were made with (patches/tracktion_engine/README.md,
namespace draft §22.3), since 2026-10-14 when JUCE's patches joined them.
"""

import runpy
import sys
from pathlib import Path

sys.argv[1:1] = ["tracktion_engine"]
runpy.run_path(str(Path(__file__).with_name("patches.py")), run_name="__main__")
