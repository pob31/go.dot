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
# Opens a show in the window, on the default audio device, and serves the web
# client beside it. With no argument it opens the empty show shipped next to
# this script. Any further arguments go to `wfg serve` as they are.
#
#     ./go.dot.sh                         the empty show
#     ./go.dot.sh ~/shows/Tuesday         a show of your own
#     ./go.dot.sh ~/shows/Tuesday --recover

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
