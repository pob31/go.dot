#!/usr/bin/env bash
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
#
# The macOS launcher: Finder runs a .command in Terminal on a double-click, so
# the engine's log stays on screen, which is what a tester's report wants.
# Same behaviour as go.dot.sh - no argument opens the empty show beside it.
#
#     ./Go.dot.command                    the empty show
#     ./Go.dot.command ~/shows/Tuesday    a show of your own

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

show="$here/Untitled"
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    show="$(cd "$1" && pwd)"
    shift
fi

# --ui is resolved against the working directory, so run from beside the binary.
cd "$here"
exec ./wfg serve "$show" --device --window --ui=console "$@"
