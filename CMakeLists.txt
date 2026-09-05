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

# Go.dot — cross-platform show control for theatre, dance and live music.
# Product requirements: docs/godot-prd-draft-0.8.md
# Phase plan:           docs/godot-devplan-draft-0.1.md
#
# This file ORCHESTRATES. It defines no source target of its own: the target
# graph lives in cmake/WfgThirdParty.cmake (the vendor-facing targets) and in
# src/, tests/ and spikes/ (ours). Read it as a running order, not as a build.
#
#   wfg::deps        INTERFACE   headers + defines + cxx_std_20 + config flags
#   wfg::warnings    INTERFACE   warning policy — our code only
#   wfg::thirdparty  STATIC      the one place JUCE + TE module sources compile
#   wfg::engine      STATIC      our engine library                     (src/)
#   wfg              EXECUTABLE  the product binary                     (src/)
#   wfg_tests        EXECUTABLE  doctest runner                       (tests/)
#   spike*           EXECUTABLE  throwaway validation programs, opt-in (spikes/)
#
# THE ORDER BELOW IS LOAD-BEARING. Two steps in particular have to happen where
# they are and not where they read more naturally — the macOS deployment target
# above project(), and MSVC's runtime library before the ThirdParty include.
# Both are commented at the point of use. Moving them produces no error, just a
# silently different build, which is the worst kind.
#
# Quick start:
#     cmake --preset dev && cmake --build --preset dev-debug
# Full instructions, including the submodule trap, are in README.md.

cmake_minimum_required(VERSION 3.22)

# Why 3.22 and not higher: JUCE 8.0.13's own root CMakeLists declares
# cmake_minimum_required(VERSION 3.22), so that is the floor we inherit whatever
# we write here, and it is also the version in Ubuntu 22.04's apt. Raising it to
# 3.25 would buy CMAKE_MSVC_DEBUG_INFORMATION_FORMAT (see the Windows CI job's
# note about sccache needing /Z7) at the cost of every contributor on a stock LTS
# box. Not worth it in Phase 0.
#
# Why not LOWER: CMake 4.x hard-refuses any cmake_minimum_required below 3.5. Any
# dependency added later that still declares 3.0 will fail outright on a modern
# CMake, and the fix is CMAKE_POLICY_VERSION_MINIMUM on that dependency, not a
# change here.

# --- macOS deployment target -------------------------------------------------
# ABOVE project(), and that is the whole point. project() -> enable_language()
# CREATES the CMAKE_OSX_DEPLOYMENT_TARGET cache entry itself, and once a cache
# entry exists, a plain set(... CACHE ...) is a silent no-op. Written below
# project(), this line would appear to work, produce no warning, and be ignored:
# the binary would silently target whatever SDK the build machine happens to
# have, which on a CI runner changes without notice.
#
# 13.3, and the number is load-bearing rather than conservative. It is the
# version in which Apple's libc++ made std::to_chars available for floating-point
# types, and Go.dot writes every number in every file it produces — the show
# document, the event log, the OSCQuery replies — with std::to_chars, because
# nothing else round-trips.
#
# That was measured, not assumed. Over 19 993 random finite doubles, JUCE's own
# writer lost 9 214 of them (46%): it stops at fifteen significant digits, so
# almost half of all doubles read back as a different number. A document format
# that cannot survive a save and a load cannot support "the event log replays the
# session bit-for-bit", which is Phase 1's whole acceptance criterion. See
# src/wfg/engine/osc/OscValue.cpp for the full table and for why the READER is a
# classic-locale stream rather than std::from_chars, which libc++ still lacks.
#
# What it costs: macOS 11 and 12 machines. The author has confirmed that floor is
# acceptable. macOS 13 shipped in October 2022 and 13.3 in March 2023.
#
# Being >= 10.14 also makes TE's -fno-aligned-allocation unnecessary for us (see
# WfgThirdParty.cmake's DECLINED list). A normal cache STRING, deliberately NOT
# the CACHE INTERNAL that TE's TestRunner:28 uses — INTERNAL would make it
# un-overridable from a preset or the command line, which is precisely what
# someone testing an older target needs.
#
# CMAKE_OSX_ARCHITECTURES is deliberately left unset: each machine builds for
# itself, and universal binaries are a packaging decision for a future release
# workflow, not a Phase-0 build-system default.
#
# The condition is doubled on purpose. Measured on this box with CMake 4.3.1:
# pre-project(), CMAKE_HOST_WIN32 and CMAKE_HOST_SYSTEM_NAME are both already
# populated, so the mechanism works — but CMAKE_HOST_APPLE could only be
# confirmed absent on a non-Apple host, never confirmed present on an Apple one.
# The OR costs one line and removes the possibility of a SILENT wrong deployment
# target on macOS, which is a failure mode nothing downstream would catch.
if(CMAKE_HOST_APPLE OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# --- Project -----------------------------------------------------------------
# The project is named `wfg`, not `godot`: PRD 7 fixes `wfg` as the binary and
# package name, and `GODOT_`-prefixed identifiers collide with the Godot game
# engine in every search path a contributor will ever use. The repo is `go.dot`,
# the docs are `godot-*.md`, the product is "Go.dot", and the build system says
# `wfg`. That mismatch is deliberate and is explained once in README.md.
#
# C is in LANGUAGES because JUCE compiles C sources of its own (vendored
# libraries under juce_audio_formats and juce_graphics, among others). Omitting
# it fails at generate time, not configure time, with a message about a missing
# CMAKE_C_COMPILE_OBJECT rule.
project(wfg
    VERSION 0.1.0
    DESCRIPTION "Go.dot — cross-platform show control"
    LANGUAGES C CXX)

# --- Objective-C / Objective-C++ ---------------------------------------------
# On Apple, JUCE's module sources are .mm, not .cpp: _juce_module_sources()
# (JUCEModuleSupport.cmake:216-240) swaps each .cpp for the .mm beside it, and
# those sources are attached INTERFACE to whichever target links the module —
# for us, wfg_thirdparty.
#
# CMake loads a language's compile rules only into the directory scope that
# enables it, and below. Enabling OBJCXX here, at the top level, covers cmake/,
# src/, tests/ and spikes/ in one stroke. Do it lower down (in
# WfgThirdParty.cmake, say) and generation fails with the famously unhelpful
#     Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT
# which names no file, no target and no language anybody recognises.
#
# This costs nothing on Linux and Windows because it is guarded, and no separate
# FATAL_ERROR guard is needed the way spatcore has one: spatcore is a library
# that cannot reach into its consumer's top-level scope, whereas Go.dot IS the
# top-level scope. The failure it guards against is not reachable from here.
#
# (The Xcode generator infers languages per file and needs none of this; enabling
# them anyway is harmless and keeps Ninja and Xcode configuring identically.)
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# --- Guards ------------------------------------------------------------------
# Before anything else touches ThirdParty. Checks the submodules are present,
# warns if somebody recursed into TE's SSH-pinned JUCE, enforces the compiler
# floor that Tracktion Engine's C++20 usage imposes, and refuses in-source
# builds. Every message is copy-pasteable; see cmake/WfgGuards.cmake for why
# each one earns its configure time.
include(WfgGuards)

# Every option() and cache STRING in one file, each with the reason it exists and
# who would turn it off. Included before WfgThirdParty because WFG_RT_CHECKS,
# WFG_LOCALE_FR, WFG_PIN_JUCE and WFG_PIN_TE all become compile definitions on
# wfg::deps, and WFG_WARNINGS_AS_ERRORS decides what goes on wfg::warnings.
include(WfgOptions)

# --- Language and runtime settings -------------------------------------------
# C++20 is Tracktion Engine's floor, not a preference: TE uses concepts,
# std::span and std::ranges in headers we include. EXTENSIONS OFF so we compile
# as -std=c++20 rather than -std=gnu++20 and a GNU-only extension cannot creep in
# on Linux and then fail on MSVC.
#
# wfg::deps also carries target_compile_features(... cxx_std_20) — belt and
# braces, and the target-scoped requirement is the one that survives anybody
# resetting the global default.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC runtime: DYNAMIC (/MD, /MDd), and set BEFORE include(WfgThirdParty) so it
# is in effect when the JUCE and TE module targets are created — a target picks
# up MSVC_RUNTIME_LIBRARY from this variable at creation time, so setting it
# afterwards would leave the 31 vendor TUs on the default while our own code
# moved, and mismatched CRTs across a static-library boundary is a link error at
# best and heap corruption at worst.
#
# This is a DELIBERATE divergence from TE's examples/TestRunner/CMakeLists.txt:22-24,
# which chooses the static runtime. Go.dot is a plugin HOST (PRD 3.18): /MT would
# give the host a private CRT heap, and the first time a third-party plugin DLL
# allocates on its heap and we free on ours — across the VST3/LV2 boundary, which
# happens constantly with juce::String and shared buffers — the process dies
# somewhere unrelated to the actual bug. TestRunner hosts nothing and can afford
# a choice we cannot.
#
# Relies on CMP0091 being NEW, which it is by default at cmake_minimum_required 3.22.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# --- Third-party ------------------------------------------------------------
# Adds JUCE, then Tracktion Engine's modules/ subdirectory ONLY (never TE's
# root — see that file for the SSH-submodule and examples/ traps), and defines
# wfg::deps, wfg::warnings and wfg::thirdparty. This is the only file in the
# repo that names a juce:: or tracktion:: target.
include(WfgThirdParty)

# --- Our code ----------------------------------------------------------------
# enable_testing(), NOT include(CTest). include(CTest) additionally defines a
# BUILD_TESTING option (a second, competing switch alongside WFG_BUILD_TESTS) and
# pulls in the whole CDash/Nightly submission machinery — a DartConfiguration.tcl
# in the build tree and a set of dashboard targets we will never use. We want the
# `test` target and add_test(); that is all enable_testing() does.
#
# Inside the if() so that -DWFG_BUILD_TESTS=OFF genuinely produces no test
# harness rather than an empty one that reports success.
if(WFG_BUILD_TESTS)
    enable_testing()

    # Python is a HARD requirement once tests are on, not a nicety. It runs the
    # gate that keeps the generated schema header honest against the parameter
    # table, and a gate that silently does not run is worse than no gate — the
    # same reasoning as the locale rule, where a skipped test reports green.
    #
    # It is NOT required to BUILD: the generated header is committed, so a clone
    # with no Python compiles and ships. Only checking it needs the interpreter.
    # LOCATION, not CMake's default of VERSION, and this is a correctness
    # choice rather than a preference.
    #
    # The default picks the HIGHEST version it can find anywhere - PATH, the
    # Windows registry, the macOS frameworks. Somebody who types
    # `pip install lxml` installs into the interpreter their SHELL resolves, so
    # the two disagree the moment a machine has more than one Python, and the
    # symptom is a configure failure saying a module is missing that the person
    # can see is installed.
    #
    # Measured, on the Windows CI runner: `python` is 3.12 and CMake chose a
    # 3.14.7 out of the tool cache, so the lxml the workflow had just installed
    # was invisible to the probe below. LOCATION searches PATH first and takes
    # the first match, which is the same interpreter the shell would run.
    set(Python3_FIND_STRATEGY LOCATION)

    # STRATEGY alone was not enough, measured on the same runner: CMake still
    # chose the 3.14.7 while `python` was 3.12. Python3_FIND_REGISTRY defaults
    # to FIRST on Windows and Python3_FIND_FRAMEWORK to FIRST on macOS, and both
    # are consulted BEFORE the strategy gets to look at PATH. All three knobs are
    # needed to say the one thing meant here: use the interpreter the shell
    # would run.
    set(Python3_FIND_REGISTRY LAST)     # Windows
    set(Python3_FIND_FRAMEWORK LAST)    # macOS

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(WFG_PYTHON "${Python3_EXECUTABLE}")
    message(STATUS "Go.dot: Python for the test gates: ${WFG_PYTHON}")

    # lxml, likewise hard. It runs the OUTSIDE opinion: the committed RELAX NG
    # grammar against every fixture, through a validator that shares no code
    # with the engine. Every other check on a show document is the engine
    # agreeing with itself, so this is the only one that can catch a mistake our
    # reader and our schema both make.
    #
    # Probed here rather than left to fail at test time, because the failure
    # mode we are avoiding is the quiet one. scripts/validate-show.py exits 2
    # instead of 0 when the import fails, and no test in this project carries
    # SKIP_RETURN_CODE — the locale rule set that precedent and the reasoning
    # carries over: a check that reports green without running is worse than a
    # check that is not there, because nobody goes looking for it.
    execute_process(COMMAND "${WFG_PYTHON}" -c "import lxml.etree"
                    RESULT_VARIABLE WFG_LXML_RESULT
                    OUTPUT_QUIET ERROR_QUIET)

    if(NOT WFG_LXML_RESULT EQUAL 0)
        message(FATAL_ERROR
                "lxml is required to build the tests, and it is not importable by
"
                "  ${WFG_PYTHON}
"
                "It validates every show fixture against docs/schema/show.rng.
"
                "Install it:
"
                "  pip install lxml          (or: apt install python3-lxml)
"
                "or configure with -DWFG_BUILD_TESTS=OFF to build without the suite.")
    endif()
endif()

add_subdirectory(src)

# tests/ owns every add_test() call in the repo, including the two that run the
# `wfg` binary itself (wfg.version and wfg.selftest). Keeping all test
# registration in one file is worth the small oddity of tests/CMakeLists.txt
# naming a target defined in src/ — CMake resolves target names in COMMAND at
# generate time, so the ordering here is sufficient and no export is needed.
if(WFG_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# LAST, ALWAYS. Spikes are throwaway (devplan:19 — "they live in spikes/, never
# migrate into src/"), they link wfg::thirdparty but never wfg::engine, and no
# src/ or tests/ target may depend on one. Adding this directory last is the
# structural expression of that: nothing configured above can reference a target
# that does not exist yet, so a spike CANNOT become load-bearing by accident.
if(WFG_BUILD_SPIKES)
    add_subdirectory(spikes)
endif()
