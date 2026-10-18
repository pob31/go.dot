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
# tests/fuzz/ — fuzz targets for the modules that read bytes they did not
# write. Off by default (WFG_BUILD_FUZZ); see docs/fuzzing.md.
#
# Each target compiles ITS OWN SOURCES directly rather than linking wfg::engine:
# the modules fuzzed here are standard-library only (Bplist, OscCodec,
# OscAddress), and a target that needed JUCE and Tracktion to build would be
# a target nobody runs. A module that gains a JUCE dependency leaves this list.
#
# Two ways to build each, chosen by WFG_FUZZ_ENGINE:
#   driver     (default) FuzzDriver.cpp: replays files and runs a built-in
#              deterministic mutator, on any compiler.
#   libfuzzer  -fsanitize=fuzzer, for coverage-guided runs. Clang only.
# Both add ASan + UBSan on GCC and Clang. MSVC gets neither, so the option is
# refused there rather than quietly building targets that find nothing.
# =============================================================================

# Standalone:  cmake -S tests/fuzz -B build/fuzz && cmake --build build/fuzz
# needs no submodules and no JUCE, which is what lets CI run it in seconds.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(wfg_fuzz CXX)
    enable_testing()
endif()

if(MSVC)
    message(FATAL_ERROR "WFG_BUILD_FUZZ needs GCC or Clang (sanitizers); MSVC is not supported.")
endif()

set(WFG_FUZZ_ENGINE "driver" CACHE STRING "driver (any compiler) or libfuzzer (Clang)")
set_property(CACHE WFG_FUZZ_ENGINE PROPERTY STRINGS driver libfuzzer)

set(WFG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src")

function(wfg_fuzz_target name)
    cmake_parse_arguments(F "" "MAIN" "SOURCES" ${ARGN})

    if(WFG_FUZZ_ENGINE STREQUAL "libfuzzer")
        add_executable(${name} ${F_MAIN} ${F_SOURCES})
        target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        add_executable(${name} ${F_MAIN} FuzzDriver.cpp ${F_SOURCES})
        target_compile_options(${name} PRIVATE -fsanitize=address,undefined)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()

    target_compile_features(${name} PRIVATE cxx_std_20)
    target_compile_options(${name} PRIVATE -fno-sanitize-recover=undefined -g -O1)
    target_include_directories(${name} PRIVATE "${WFG_SRC}")
endfunction()

wfg_fuzz_target(fuzz_bplist MAIN FuzzBplist.cpp SOURCES "${WFG_SRC}/wfg/engine/import/Bplist.cpp")
wfg_fuzz_target(fuzz_osc    MAIN FuzzOsc.cpp    SOURCES "${WFG_SRC}/wfg/engine/osc/OscCodec.cpp"
                                    "${WFG_SRC}/wfg/engine/osc/OscAddress.cpp"
                                    "${WFG_SRC}/wfg/engine/osc/OscValue.cpp")

# A short mutation pass per target as ctest cases, so the driver build is
# also a smoke test: the valid seeds must pass, and 20000 mutations must not
# trap. Only in the driver build; a libFuzzer binary runs for as long as it is told.
if(WFG_FUZZ_ENGINE STREQUAL "driver")
    foreach(t fuzz_bplist fuzz_osc)
        add_test(NAME fuzz.${t} COMMAND ${t} --mutate 20000 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()
