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

# =============================================================================
# src/ — the product library and the product binary. Two targets, nothing else:
#
#   wfg_engine  (alias wfg::engine)  STATIC      our code
#   wfg                              EXECUTABLE  the product binary
#
# Everything this directory needs from the build system arrives through the
# three targets cmake/WfgThirdParty.cmake defines — wfg::deps (headers, the
# JUCE/Tracktion macro environment, cxx_std_20, the platform options),
# wfg::thirdparty (the ONE place the 31 JUCE/TE vendor TUs compile) and
# wfg::warnings (our warning policy, and -Werror under the `strict` preset).
#
# This file therefore sets no CMAKE_* variable of its own, calls no project()
# or enable_testing(), and names no juce:: or tracktion:: module target. If you
# find yourself reaching for one of those, the wiring belongs in
# cmake/WfgThirdParty.cmake and not here: a module linked directly from this
# directory would put that module's .cpp files into a SECOND target's compile
# and give us two copies of JUCE with two different macro environments.
# =============================================================================

# -----------------------------------------------------------------------------
# wfg_engine — the engine library.
#
# PHASE 0 SCAFFOLD. Boot.{h,cpp} is a toolchain proof and not an engine: it
# exists so that a broken JUCE or Tracktion wiring fails at COMPILE time inside
# OUR code rather than passing a hollow build in which nothing ever named a
# vendor type. Phase 1 replaces the contents of these two files wholesale with
# the document/tree layer; the TARGET stays exactly as it is here.
# -----------------------------------------------------------------------------
add_library(wfg_engine STATIC
    wfg/engine/Boot.cpp
    wfg/engine/Boot.h)
add_library(wfg::engine ALIAS wfg_engine)

# PUBLIC, and pointed at src/ rather than at src/wfg/ — consumers therefore
# write  #include <wfg/engine/Boot.h>  , a rooted path that cannot be ambiguous.
# Do NOT add src/wfg/ or src/wfg/engine/ as an include directory as a
# convenience: a bare #include <Boot.h> would then resolve against a search path
# that also carries two whole vendor module trees, and the first vendor header
# that ever takes the same name wins silently.
target_include_directories(wfg_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# wfg::thirdparty is PUBLIC on purpose. Boot.h is JUCE-free today, but
# wfg_tests links wfg::engine now and the Phase 5 UI and the Phase 9 plugin
# host will too, and every one of them must compile against the SAME JUCE macro
# environment. A PRIVATE link here would leave those consumers to assemble
# their own — and a JUCE/Tracktion header seen under two different sets of
# JUCE_* defines is an ODR violation that links cleanly and misbehaves at run
# time. One PUBLIC edge here is what makes that impossible.
#
# wfg::warnings is PRIVATE, always. It carries -Werror under the `strict`
# preset; propagating that would let a JUCE or Tracktion bump redden a
# downstream target whose author never opted in to our warning policy.
target_link_libraries(wfg_engine
    PUBLIC  wfg::thirdparty
    PRIVATE wfg::warnings)

# -----------------------------------------------------------------------------
# wfg — the product binary.
#
# Plain add_executable, deliberately NOT juce_add_console_app. That helper needs
# JUCE_MODULES_ONLY=OFF, which puts the juceaide nested configure-and-build back
# into every one of our configures (measured: ~20 s instead of 4.0 s); it moves
# the output to wfg_artefacts/$<CONFIG>/, which is not where PRD §7 says the
# binary lives; and it warns on every target because COMPANY_NAME
# "Pierre-Olivier Boulant" yields a BUNDLE_ID containing spaces. The one define
# it adds that we would otherwise lack, JUCE_STANDALONE_APPLICATION, appears
# zero times across every module we link — verified with grep over modules/,
# excluding juce_audio_plugin_client, which we do not use.
#
# The CMake target name IS the binary name (wfg / wfg.exe), which is what PRD §7
# requires. Do not set OUTPUT_NAME: it would decouple the two and the ctest
# COMMAND entries in tests/CMakeLists.txt resolve through the TARGET name.
# -----------------------------------------------------------------------------
add_executable(wfg wfg/app/Main.cpp)

# wfg::engine is enough — it carries wfg::thirdparty PUBLIC. wfg::warnings is
# named again because it is PRIVATE on wfg_engine and so does not propagate;
# that is deliberate, not an oversight.
target_link_libraries(wfg PRIVATE wfg::engine wfg::warnings)

# NOTE: there is no add_test() in this file. Every add_test() in the project
# lives in tests/CMakeLists.txt, including the two that run THIS binary
# (wfg.version and wfg.selftest), so that the full list of what CI actually
# executes can be read in one place. See tests/CMakeLists.txt.
