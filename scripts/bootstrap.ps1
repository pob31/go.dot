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

<#
    Produces: an initialised ThirdParty\JUCE and ThirdParty\tracktion_engine,
              and a local git setting that makes a recursive submodule update a
              no-op instead of an SSH failure.
    Usage:    from the repo root, any PowerShell:
                  scripts\bootstrap.ps1
    Build requirements: git on PATH. Nothing else — this runs before any
              toolchain does.

    The Windows twin of scripts/bootstrap.sh. Idempotent; safe to re-run.

    ========================================================================
    READ THIS BEFORE YOU "IMPROVE" THE GIT COMMAND BELOW
    ========================================================================
    It is deliberately NOT `git submodule update --init --recursive`, which is
    what WFS-DIY's tools/setup.sh does and what every instinct reaches for.

    tracktion_engine's own .gitmodules pins a nested JUCE at modules/juce with
    the URL git@github.com:juce-framework/JUCE.git — SSH. Recursing into it
    fails with "Permission denied (publickey)" on every CI runner and on any
    clone by someone without an SSH key registered with GitHub, three levels
    down, in a message that names neither Go.dot nor Tracktion Engine.

    Go.dot pins JUCE itself, at ThirdParty\JUCE, at the exact SHA TE v3.2.0
    records in that gitlink. TE's vendored copy is redundant, is never entered
    by our CMake (we add tracktion_engine/modules only), and may stay empty
    forever.

    It is also NOT `--depth 1`: our JUCE pin (19edd538, JUCE 8.0.6+19) is not
    the tip of develop, and a shallow submodule fetch fails with
        fatal: reference is not a tree: 19edd538429c93d277bf95b55aaa7e3eb545f951
    ========================================================================
#>

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..").Path
Push-Location $Root

try {
    Write-Host "==> Initialising submodules (first run pulls ~1 GB; be patient)"
    Write-Host "    NOT --recursive, NOT --depth 1 - see the header of this script."
    git submodule update --init ThirdParty/JUCE ThirdParty/tracktion_engine ThirdParty/spatcore
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed with exit code $LASTEXITCODE" }

    # juce_simpleweb, WITH a scoped --recursive, because it has a nested asio it
    # cannot compile without. This is the one path in the tree where recursion is
    # both safe and required: benkuper/asio is an HTTPS URL, unlike TE's vendored
    # JUCE, which is the SSH one the line above is careful to keep out of.
    #
    # Scoped by NAME rather than by a blanket --recursive: the blanket form would
    # also descend into tracktion_engine/modules/juce and fail on any machine
    # without a GitHub SSH key. check-pins.py asserts both halves - (c) that TE's
    # copy stayed empty, (f) that this one did not.
    git submodule update --init --recursive ThirdParty/juce_simpleweb
    if ($LASTEXITCODE -ne 0) { throw "git submodule update (juce_simpleweb) failed with exit code $LASTEXITCODE" }

    # ----------------------------------------------------------------------
    # Neutralise TE's nested SSH submodule.
    #
    # Local, untracked config inside the tracktion_engine submodule's own
    # repository: it changes no tracked file and is invisible to `git status`.
    # With submodule.modules/juce.update = none, a contributor who types the
    # reflex command
    #
    #     git submodule update --init --recursive
    #
    # gets "Skipping submodule 'modules/juce'" instead of an SSH failure, and
    # TE's vendored JUCE stays empty - which is what cmake/WfgGuards.cmake's
    # second guard and check (c) of scripts/check-pins.py both want.
    #
    # `update = none` rather than rewriting the URL to HTTPS, on purpose:
    # rewriting it would make the recursion SUCCEED and put a second, complete,
    # ~1 GB JUCE checkout on disk that nothing ever builds.
    # ----------------------------------------------------------------------
    if (Test-Path (Join-Path $Root "ThirdParty\tracktion_engine\.git")) {
        Write-Host "==> Disarming tracktion_engine's nested SSH JUCE submodule (local config only)"
        git -C ThirdParty/tracktion_engine config submodule.modules/juce.update none
    }

    Write-Host ""
    Write-Host "==> Checking submodule pins"
    $py = Get-Command python3 -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
    if ($py) {
        & $py.Source "scripts/check-pins.py"
        if ($LASTEXITCODE -ne 0) { throw "check-pins.py reported a problem (exit $LASTEXITCODE)" }
    } else {
        Write-Host "    (skipped: no python on PATH - CI runs this gate anyway)"
    }

    Write-Host ""
    Write-Host "Bootstrap complete."
    Write-Host ""
    # Single-quoted, deliberately: in a double-quoted PowerShell string the
    # backtick is the escape character, so "the `vs` preset" silently renders as
    # "the <vertical tab>s` preset". Anything containing a backtick goes in
    # single quotes here.
    Write-Host 'In Visual Studio: File > Open > Folder on the repo root. VS reads'
    Write-Host 'CMakePresets.json and offers the `vs` preset, which names no generator'
    Write-Host 'so VS picks its own. Nothing else to set up.'
    Write-Host ""
    Write-Host 'From a command line, use an *x64 Native Tools Command Prompt for VS*'
    Write-Host '(or a shell where vcvars64.bat has been run). The `dev` preset''s Ninja'
    Write-Host 'generator cannot find cl.exe from a plain PowerShell window - that is the'
    Write-Host "'CMAKE_CXX_COMPILER not set' error, not a broken preset."
    Write-Host ""
    Write-Host "    cmake --preset dev"
    Write-Host "    cmake --build --preset dev-debug"
    Write-Host "    ctest --test-dir build/dev -C Debug --output-on-failure"
    Write-Host ""
    Write-Host "(There is no ``dev`` test preset on purpose: the ci-* test presets run in"
    Write-Host " the ci-* build trees. Point ctest at the tree you built.)"
    Write-Host ""
    Write-Host "Or, without a Native Tools prompt:"
    Write-Host ""
    Write-Host "    cmake --preset vs"
    Write-Host "    cmake --build --preset vs-debug"
}
finally {
    Pop-Location
}
