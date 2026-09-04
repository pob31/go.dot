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
# ---------------------------------------------------------------------------
# Produces: an initialised ThirdParty/JUCE and ThirdParty/tracktion_engine, and
#           a local git setting that makes a recursive submodule update a no-op
#           instead of an SSH failure.
# Usage:    ./scripts/bootstrap.sh          (from anywhere; it finds the root)
# Build requirements: git. Nothing else — this runs before any toolchain does.
#
# Idempotent. Safe to re-run any time you re-init a submodule, and safe to run
# after a clone that forgot --recurse-submodules.
#
# ===========================================================================
# READ THIS BEFORE YOU "IMPROVE" THE GIT COMMAND BELOW
# ===========================================================================
# It is deliberately NOT `git submodule update --init --recursive`, which is
# what WFS-DIY's tools/setup.sh does and what every instinct reaches for.
#
# tracktion_engine's own .gitmodules pins a nested JUCE at modules/juce with the
# URL git@github.com:juce-framework/JUCE.git — SSH. Recursing into it fails with
#
#     git@github.com: Permission denied (publickey).
#     fatal: Could not read from remote repository.
#
# on every CI runner and on any clone by someone without an SSH key registered
# with GitHub, three levels down, in a message that names neither Go.dot nor
# Tracktion Engine. Go.dot pins JUCE itself, at ThirdParty/JUCE, at the exact
# SHA TE v3.2.0 records in that gitlink — so TE's vendored copy is redundant, is
# never entered by our CMake (we add tracktion_engine/modules only), and may
# stay empty forever.
#
# It is also NOT `--depth 1`. Our JUCE pin (19edd538, JUCE 8.0.6+19) is not the
# tip of develop, and a shallow submodule fetch fails with
#     fatal: reference is not a tree: 19edd538429c93d277bf95b55aaa7e3eb545f951
# ===========================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

echo "==> Initialising submodules (first run pulls ~1 GB; be patient)"
echo "    NOT --recursive, NOT --depth 1 — see the header of this script."
git submodule update --init ThirdParty/JUCE ThirdParty/tracktion_engine

# --------------------------------------------------------------------------
# Neutralise TE's nested SSH submodule.
#
# This is local, untracked config inside the tracktion_engine submodule's own
# repository — it changes no file the author of TE ships, and it is invisible to
# `git status`. With submodule.modules/juce.update = none, a contributor who
# types the reflex command
#
#     git submodule update --init --recursive
#
# gets "Skipping submodule 'modules/juce'" instead of an SSH failure, and TE's
# vendored JUCE stays empty — which is what cmake/WfgGuards.cmake's second guard
# and check (c) of scripts/check-pins.py both want.
#
# We set `update = none` rather than rewriting the URL to HTTPS on purpose:
# rewriting it would make the recursion SUCCEED and put a second, complete,
# ~1 GB JUCE checkout on disk that nothing ever builds.
# --------------------------------------------------------------------------
if [ -e ThirdParty/tracktion_engine/.git ]; then
    echo "==> Disarming tracktion_engine's nested SSH JUCE submodule (local config only)"
    git -C ThirdParty/tracktion_engine config submodule.modules/juce.update none
fi

echo
echo "==> Checking submodule pins"
if command -v python3 >/dev/null 2>&1; then
    python3 scripts/check-pins.py
elif command -v python >/dev/null 2>&1; then
    python scripts/check-pins.py
else
    echo "    (skipped: no python3 on PATH — CI runs this gate anyway)"
fi

cat <<'EOF'

Bootstrap complete.

Configure and build:

    cmake --preset dev                  # Ninja Multi-Config, build/dev/
    cmake --build --preset dev-debug
    cmake --build --preset dev-release

Run the suite (Debug). There is deliberately no `dev` test preset - the ci-*
test presets run in the ci-* build trees, so point ctest at the tree you built:

    ctest --test-dir build/dev -C Debug --output-on-failure

On Linux, install the build dependencies first:

    bash scripts/install-linux-deps.sh

On Windows, either open the folder in Visual Studio (which uses the `vs`
preset), or use `scripts/bootstrap.ps1` and an x64 Native Tools prompt — the
`dev` preset's Ninja generator cannot find cl.exe from a plain shell.
EOF
