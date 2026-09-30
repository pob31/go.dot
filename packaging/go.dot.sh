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
# Opens a show in the window, on the audio interface the show names, and serves
# the web client beside it. With no argument it opens the empty show shipped
# next to this script. Any further arguments go to `wfg serve` as they are.
#
#     ./go.dot.sh                         the empty show
#     ./go.dot.sh ~/shows/Tuesday         a show of your own
#     ./go.dot.sh ~/shows/Tuesday/Tuesday.wfg   the same, by its .wfg
#     ./go.dot.sh ~/shows/Tuesday --recover
#
# `wfg associate` makes a double-clicked .wfg run this script with it, for
# this user (README.txt, and src/wfg/engine/app/Associate.h).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

show="$here/Untitled"
first=(--show-settings)
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    # A folder, or a file in one - the .wfg - which wfg takes for its folder.
    if [ -d "$1" ]; then
        show="$(cd "$1" && pwd)"
    else
        show="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    fi
    shift
    first=()
fi

# NO --device, deliberately. A bare --device means "the system default" and
# overrides the interface a show has saved for itself, and the window's New and
# Open pass every flag on to the show they start - so it would pin every show to
# the default. Each show says whether to open an interface and which one; the
# empty one beside this script says "the system default".
#
# THE EMPTY SHOW OPENS ON ITS SHOW SETTINGS (--show-settings): the first thing
# anybody starting from nothing needs is the interface to play through. A show
# of your own opens as it was saved.
#
# THROUGH PIPEWIRE'S JACK WHEN IT IS INSTALLED (pw-jack, from pipewire-jack).
# A desktop running PipeWire holds the audio interface, and plain ALSA then
# offers only its stereo pair; as a JACK client Go.dot sees every channel, and
# the desktop keeps its sound. pw-jack points wfg at PipeWire's libjack, which
# is what makes "JACK" appear in Show settings - from a double-click too, since
# `wfg associate`'s desktop entry runs this script. Without pw-jack, plain wfg.
#
# --ui is resolved against the working directory, so run from beside the binary.
cd "$here"
if command -v pw-jack >/dev/null 2>&1; then
    exec pw-jack ./wfg serve "$show" --window --ui=console ${first[@]+"${first[@]}"} "$@"
fi
exec ./wfg serve "$show" --window --ui=console ${first[@]+"${first[@]}"} "$@"
