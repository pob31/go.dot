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

# WfgGuards.cmake — fail early, and fail with instructions.
#
# Every check here exists because someone (or CI) already hit the failure it
# prevents, and the NATIVE error message for that failure named the wrong file,
# the wrong directory, or nothing useful at all. A guard whose message is no
# better than the error it replaces is not worth the configure time; each of
# these earns its place by replacing a specific, measured, misleading message.
#
# Included from the root CMakeLists BEFORE WfgOptions and WfgThirdParty, so it
# runs before we touch a single ThirdParty file. It defines no targets and no
# variables the rest of the build reads — it either passes silently or stops.
#
# Ordered cheapest-and-most-fundamental first, not by the contract's guard
# numbers; each guard is labelled with its number so the build contract stays
# cross-referenceable.

# ---------------------------------------------------------------------------
# Guard 4 — in-source build
# ---------------------------------------------------------------------------
# First because it is the "you are standing in the wrong place" error, and
# because everything below would otherwise scatter its droppings into a tree
# that also contains two large submodules. Undoing an in-source configure by
# hand across a JUCE checkout is genuinely unpleasant: CMakeFiles/ directories
# appear next to module sources, `git status` fills with hundreds of untracked
# entries, and `git clean -fdx` — the obvious fix — will happily delete the
# submodule working trees along with them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
"Go.dot: in-source builds are not supported.

CMake was pointed at the repo root as its BUILD directory, which would write
CMakeCache.txt, CMakeFiles/ and generated makefiles directly into the source
tree — including alongside ThirdParty/JUCE and ThirdParty/tracktion_engine.

First, delete what this run already created at the repo root:

    rm -rf CMakeCache.txt CMakeFiles/

then configure into a build directory instead:

    cmake --preset dev

or, without presets:

    cmake -S . -B build/dev -DCMAKE_BUILD_TYPE=Debug

Do NOT reach for `git clean -fdx` to tidy up: it removes the submodule working
trees too, and you will be re-cloning JUCE and Tracktion Engine afterwards.")
endif()

# ---------------------------------------------------------------------------
# Guard 1 — ThirdParty submodules are checked out
# ---------------------------------------------------------------------------
# Without this, a clone made without --recurse-submodules gets ~200 lines into
# the configure and then dies inside our own cmake/WfgThirdParty.cmake with
#     CMake Error: Unknown CMake command "juce_add_modules".
# That message names TE's modules/CMakeLists.txt and mentions neither JUCE nor
# submodules, so the reader's first guess is that our CMake is broken. It is
# not: JUCE simply was never added, because ThirdParty/JUCE is an empty
# directory.
#
# We probe for the two files we actually add_subdirectory(), not for the
# directories: `git clone` without submodules leaves the mount points present
# but EMPTY, so `if(EXISTS <dir>)` is true and proves nothing.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/JUCE/CMakeLists.txt"
   OR NOT EXISTS "${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules/CMakeLists.txt")
    message(FATAL_ERROR
"Go.dot: the ThirdParty submodules are not checked out.

Run, from the repo root:

    git submodule update --init ThirdParty/JUCE ThirdParty/tracktion_engine

or just:  ./scripts/bootstrap.sh   (bootstrap.ps1 on Windows)

Do NOT add --recursive. tracktion_engine's own .gitmodules points modules/juce at
git@github.com:juce-framework/JUCE.git over SSH, which fails on every CI runner and
every keyless clone with 'Permission denied (publickey)' three levels down. Go.dot
pins JUCE itself; TE's vendored copy is never built and may stay empty forever.

Do NOT use --depth 1 either: our JUCE pin (19edd538, JUCE 8.0.13+19) is not the tip of
develop, and a shallow submodule fetch fails with
'fatal: reference is not a tree: 19edd538429c93d277bf95b55aaa7e3eb545f951'.

A clone without --recurse-submodules leaves the gitlinks empty and the juce:: and
tracktion:: targets are never defined, which is why this message exists instead of an
'Unknown CMake command juce_add_modules' error 200 lines later.")
endif()

# ---------------------------------------------------------------------------
# Guard 2 — somebody recursed into Tracktion Engine's SSH-pinned JUCE
# ---------------------------------------------------------------------------
# WARNING, deliberately NOT fatal. On a machine with an SSH key loaded,
# `git submodule update --init --recursive` SUCCEEDS and leaves a perfectly
# working tree; failing that person's build would be punishing them for a
# command that, for them, worked. What it costs is a second full JUCE checkout
# on disk (~500 MB) that this build never enters — we add
# ThirdParty/tracktion_engine/modules, never TE's root, so modules/juce is not
# reachable from any add_subdirectory() we issue.
#
# The reason to say anything at all is that the SAME command is a hard failure
# for the next person: no key, no clone, 'Permission denied (publickey)'.
# Catching it on the machine where it silently worked is the only place the
# warning reaches the person who introduced it.
#
# This WILL trip the Linux CI job's clean-configure gate (which fails on any
# 'CMake Warning' originating outside ThirdParty/). That is the intended nudge,
# not an accident — do not "fix" it by downgrading this to message(STATUS).
file(GLOB _wfg_te_juce "${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules/juce/*")
if(_wfg_te_juce)
    message(WARNING
"Go.dot: ThirdParty/tracktion_engine/modules/juce/ is populated. Something ran
'git submodule update --recursive' (or checkout with submodules: recursive). It is
harmless here — this build NEVER enters that directory, and we add only
tracktion_engine/modules — but it means a second JUCE checkout is on disk, and the
same command WILL fail on any machine or CI runner without an SSH key. Remove it with:
    rm -rf ThirdParty/tracktion_engine/modules/juce/*
See scripts/bootstrap.sh for the correct, non-recursive init.")
endif()
unset(_wfg_te_juce)

# ---------------------------------------------------------------------------
# Guard 3 — compiler floor
# ---------------------------------------------------------------------------
# The floor is TRACKTION ENGINE's, not ours. TE 3.5.0 is a C++20 codebase that
# uses concepts and constrained templates in headers we include transitively
# from tracktion_engine.h; TE's own CI builds with gcc-11 and Xcode 15.3, so
# those are the versions its C++20 usage is actually exercised against.
#
# The symptom this replaces is the reason it is worth four lines: an older
# compiler does not say "I do not support C++20". It reaches
# tracktion_SafeScopedListener.h, hits `concept` / `requires`, and emits several
# hundred lines of template instantiation noise whose first named file is a
# standard library header. Nothing in that wall mentions the compiler version,
# and the natural conclusion is that the Tracktion submodule is broken.
#
# Bumping a floor here is cheap; diagnosing it from the wall of noise is not.
set(_wfg_cxx_floor "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_wfg_cxx_floor 11)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # AppleClang versions do not track upstream Clang: AppleClang 15 ships with
    # Xcode 15, which is the oldest Xcode TE 3.5.0's CI covers.
    set(_wfg_cxx_floor 15)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_wfg_cxx_floor 14)
endif()

if(_wfg_cxx_floor AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS _wfg_cxx_floor)
    message(FATAL_ERROR
        "Go.dot: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} is too old.\n"
        "Minimum is ${CMAKE_CXX_COMPILER_ID} ${_wfg_cxx_floor}.\n"
        "\n"
        "This floor comes from Tracktion Engine, not from Go.dot's own code: TE 3.5.0 "
        "requires C++20 (concepts and constrained templates appear in headers we "
        "include), and TE's own CI validates against gcc-11 and Xcode 15.3.\n"
        "\n"
        "Without this check the build reaches "
        "ThirdParty/tracktion_engine/modules/tracktion_core/utilities/"
        "tracktion_SafeScopedListener.h and produces several hundred lines of template "
        "noise that names no useful file and never mentions the compiler version.\n"
        "\n"
        "Select a newer compiler, for example:\n"
        "    sudo apt-get install g++-11\n"
        "    cmake -S . -B build/dev -DCMAKE_CXX_COMPILER=g++-11 -DCMAKE_C_COMPILER=gcc-11\n"
        "\n"
        "The compiler is baked into an existing build tree, so delete it before "
        "reconfiguring: rm -rf build/")
endif()
unset(_wfg_cxx_floor)

# MSVC is checked separately: MSVC_VERSION, not CMAKE_CXX_COMPILER_VERSION, is the
# number people recognise and the one Microsoft's own documentation uses.
# 1930 = Visual Studio 2022 17.0 / MSVC 14.30, the first toolset with the C++20
# conformance TE needs. The author's box is MSVC 14.51 and 14.44; CI pins the
# windows-2025 image. This is a floor, not a target.
#
# Guarded on CMAKE_CXX_COMPILER_ID rather than the MSVC variable, because MSVC is
# also true for clang-cl — which reports its own upstream Clang version and is
# already covered by the "Clang" branch above.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MSVC_VERSION LESS 1930)
    message(FATAL_ERROR
        "Go.dot: MSVC toolset ${MSVC_VERSION} is too old; minimum is 1930 "
        "(Visual Studio 2022 17.0 / MSVC 14.30).\n"
        "\n"
        "This floor comes from Tracktion Engine's C++20 requirement, not from Go.dot. "
        "Older toolsets fail deep inside TE's constrained templates with errors that "
        "name neither C++20 nor the compiler version.\n"
        "\n"
        "Install a current 'Desktop development with C++' workload from the Visual "
        "Studio Installer, then configure from an x64 Native Tools prompt:\n"
        "    cmake --preset vs")
endif()
