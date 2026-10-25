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
# Each target compiles ITS OWN SOURCES directly rather than linking wfg::engine.
# Bplist and OscCodec are standard-library only; the QLab and Ableton readers
# and walkers take juce_core, built below as one translation unit from the JUCE
# submodule (and skipped without it). A target that needed all of JUCE and
# Tracktion to build would be a target nobody runs.
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
    cmake_parse_arguments(F "JUCE" "MAIN" "SOURCES" ${ARGN})

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

    if(F_JUCE)
        target_link_libraries(${name} PRIVATE wfg_fuzz_juce_core)
    endif()
endfunction()

# --- juce_core, for the readers and walkers that take a juce::File or XmlElement -
# One translation unit, built on its own WITHOUT the sanitizers: what is under
# test is Go.dot's reading of the bytes, and JUCE's own allocator-heavy XML and
# string code would only add noise and minutes. Linux, and macOS through
# juce_core.mm and the frameworks the module names (Cocoa Foundation IOKit
# Security); the CI job is Linux. Skipped with a message, not an error, when
# the JUCE submodule is not checked out.
set(WFG_JUCE_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty/JUCE/modules")
set(WFG_FUZZ_HAVE_JUCE OFF)
if(NOT EXISTS "${WFG_JUCE_MODULES}/juce_core/juce_core.cpp")
    message(STATUS "fuzz: ThirdParty/JUCE is not checked out - fuzz_qlab and fuzz_als are skipped")
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT APPLE)
    message(STATUS "fuzz: fuzz_qlab and fuzz_als are built on Linux and macOS only")
else()
    set(WFG_FUZZ_HAVE_JUCE ON)
    set(WFG_FUZZ_JUCE_CORE "${WFG_JUCE_MODULES}/juce_core/juce_core.cpp")
    if(APPLE)
        enable_language(OBJCXX)
        set(WFG_FUZZ_JUCE_CORE "${WFG_JUCE_MODULES}/juce_core/juce_core.mm")
    endif()
    add_library(wfg_fuzz_juce_core STATIC "${WFG_FUZZ_JUCE_CORE}"
                                          "${WFG_JUCE_MODULES}/juce_core/juce_core_CompilationTime.cpp")
    target_compile_features(wfg_fuzz_juce_core PRIVATE cxx_std_20)
    target_compile_definitions(wfg_fuzz_juce_core PUBLIC
        NDEBUG=1 JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
        JUCE_MODULE_AVAILABLE_juce_core=1 JUCE_USE_CURL=0 JUCE_LOAD_CURL_SYMBOLS_LAZILY=0)
    target_include_directories(wfg_fuzz_juce_core SYSTEM PUBLIC "${WFG_JUCE_MODULES}")
    target_compile_options(wfg_fuzz_juce_core PRIVATE -O1 -w)
    target_link_libraries(wfg_fuzz_juce_core PUBLIC ${CMAKE_DL_LIBS} pthread)
    if(APPLE)
        target_link_libraries(wfg_fuzz_juce_core PUBLIC "-framework Cocoa" "-framework Foundation"
                                                        "-framework IOKit" "-framework Security")
    endif()
endif()

wfg_fuzz_target(fuzz_bplist MAIN FuzzBplist.cpp SOURCES "${WFG_SRC}/wfg/engine/import/Bplist.cpp")
wfg_fuzz_target(fuzz_osc    MAIN FuzzOsc.cpp    SOURCES "${WFG_SRC}/wfg/engine/osc/OscCodec.cpp"
                                    "${WFG_SRC}/wfg/engine/osc/OscAddress.cpp"
                                    "${WFG_SRC}/wfg/engine/osc/OscValue.cpp")

if(WFG_FUZZ_HAVE_JUCE)
    set(WFG_IMPORT "${WFG_SRC}/wfg/engine/import")
    wfg_fuzz_target(fuzz_qlab MAIN FuzzQlab.cpp JUCE
        SOURCES "${WFG_IMPORT}/Bplist.cpp" "${WFG_IMPORT}/QlabReader.cpp" "${WFG_IMPORT}/QlabWalk.cpp"
                "${WFG_SRC}/wfg/engine/cue/KeyChord.cpp" "${WFG_SRC}/wfg/engine/cue/SendPlacement.cpp"
                "${WFG_SRC}/wfg/engine/osc/OscValue.cpp" FuzzImportShim.cpp)
    wfg_fuzz_target(fuzz_als MAIN FuzzAls.cpp JUCE
        SOURCES "${WFG_IMPORT}/AlsReader.cpp" "${WFG_IMPORT}/AlsWalk.cpp"
                "${WFG_SRC}/wfg/engine/osc/OscValue.cpp" FuzzImportShim.cpp)
    target_compile_definitions(fuzz_als PRIVATE WFG_FUZZ_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")
endif()

# A short mutation pass per target as ctest cases, so the driver build is
# also a smoke test: the valid seeds must pass, and 20000 mutations must not
# trap. Only in the driver build; a libFuzzer binary runs for as long as it is told.
if(WFG_FUZZ_ENGINE STREQUAL "driver")
    set(WFG_FUZZ_TARGETS fuzz_bplist fuzz_osc)
    if(WFG_FUZZ_HAVE_JUCE)
        list(APPEND WFG_FUZZ_TARGETS fuzz_qlab fuzz_als)
    endif()
    foreach(t ${WFG_FUZZ_TARGETS})
        add_test(NAME fuzz.${t} COMMAND ${t} --mutate 20000 1 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()
