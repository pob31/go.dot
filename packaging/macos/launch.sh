#!/bin/sh
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
# What Go.dot.app does when it is opened: the macOS twin of go.dot.sh, run by
# the bundle's small launcher (launcher.c). POSIX sh, because that is what the
# launcher execs.
#
# THE EMPTY SHOW IS COPIED OUT OF THE BUNDLE, once, to
# ~/Library/Application Support/Go.dot/Untitled - the folder wfg already keeps
# as its own. Opened in place it would be saved in place, and a write inside a
# signed bundle breaks the signature Gatekeeper checks at the next launch.
# Not ~/Documents: that is a protected folder, and asking for it before the
# first window is a prompt nobody asked for.
#
# THE LOG IS A FILE, because Finder gives an app no terminal: one per launch in
# ~/Library/Logs/Go.dot, where Console.app shows it and a tester can attach it.
#
# NO --device, for the reason go.dot.sh gives: the show names its interface.

set -eu

resources="$(cd "$(dirname "$0")" && pwd)"
contents="$(dirname "$resources")"
support="$HOME/Library/Application Support/Go.dot"
logs="$HOME/Library/Logs/Go.dot"

mkdir -p "$support" "$logs"

if [ ! -d "$support/Untitled" ]; then
    cp -R "$resources/Untitled" "$support/Untitled"
fi

log="$logs/go.dot-$(date +%Y%m%d-%H%M%S).log"

# THE EMPTY SHOW OPENS ON ITS SHOW SETTINGS (--show-settings), as go.dot.sh's
# does: the first thing anybody starting from nothing needs is the interface.
#
# UNLESS A SHOW WAS DOUBLE-CLICKED (--yield-to-opened). Finder does not hand
# the file over on the command line: it starts the app and then says which
# file, so this script cannot know. wfg finishes the launch before choosing,
# and a show handed over takes the empty one's place, settings and all.
#
# --ui is resolved against the working directory, so run from Resources.
cd "$resources"
exec "$contents/MacOS/wfg" serve "$support/Untitled" --window --ui=console --show-settings --yield-to-opened >"$log" 2>&1
