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

# WfgThirdParty.cmake — the only file in Go.dot that knows JUCE and Tracktion
# Engine exist.
#
# Targets defined here, and the whole interface the rest of the build may use:
#
#   wfg::deps        INTERFACE  headers, compile definitions, cxx_std_20, config
#                               flags, per-platform compile options. Everything a
#                               translation unit needs in order to #include a JUCE
#                               or TE header — and NOTHING that compiles code.
#   wfg::warnings    INTERFACE  the warning policy. Linked to OUR targets only,
#                               never to vendor sources. This separation is the
#                               entire reason WFG_WARNINGS_AS_ERRORS can exist.
#   wfg::thirdparty  STATIC     the ONE place the JUCE and TE module sources
#                               compile. Links wfg::deps PUBLIC, so anything that
#                               links wfg::thirdparty gets the headers too.
#
# Nothing outside this file may name a juce:: or tracktion:: target. src/, tests/
# and spikes/ link wfg::thirdparty (or wfg::engine, which re-exports it) and get
# the whole environment; that is deliberate, and DO-NOT #5 in the build contract.
#
# There are no functions or macros here. Author-A exposes targets and variables
# only: a target you forget to link fails loudly at your first #include, whereas a
# helper function you forget to call gives you a translation unit compiled with
# DIFFERENT definitions from every other one — which is a silent ODR violation and
# the exact class of bug that costs a weekend.

# ---------------------------------------------------------------------------
# 1. JUCE — added first, and by us, not through Tracktion Engine
# ---------------------------------------------------------------------------

# JUCE_MODULES_ONLY=ON: JUCE returns straight after add_subdirectory(modules)
# (ThirdParty/JUCE/CMakeLists.txt:66), which skips the nested configure-AND-build of
# the juceaide helper tool that otherwise runs inside OUR configure step. Measured on
# this box: 4.0 s instead of ~20 s, on every configure, on three platforms, on five
# CI jobs, on a repo that is private and therefore billed. We can afford this only
# because we create no juce_add_* targets — JUCE_MODULES_ONLY removes
# juce_add_console_app / juce_add_gui_app along with juceaide. juce_add_modules and
# juce::juce_recommended_{warning,config}_flags SURVIVE, because JUCEModuleSupport.cmake
# (JUCE/CMakeLists.txt:50, which in turn includes JUCEHelperTargets.cmake at
# JUCEModuleSupport.cmake:56) is included BEFORE that early return.
#
# JUCE's own comment beside this option reads "This option is not recommended - use at
# your own risk!". That warning is aimed at projects that expect juce_add_* to work;
# for our shape it is measured-safe and re-verified by every CI run. If Phase 5 wants
# juce_add_gui_app, flip this ON->OFF and nothing else in this file changes.
set(JUCE_MODULES_ONLY ON CACHE BOOL
    "Configure JUCE's modules only; we create no juce_add_* targets" FORCE)

# JUCE FIRST. Reverse these two add_subdirectory calls and you get, at
# ThirdParty/tracktion_engine/modules/CMakeLists.txt:24:
#     CMake Error: Unknown CMake command "juce_add_modules".
# and nothing in that message mentions JUCE.
#
# CMAKE_SOURCE_DIR is the repo root: Go.dot is always the top-level project (it is an
# application, never a subdirectory of someone else's build).
add_subdirectory("${CMAKE_SOURCE_DIR}/ThirdParty/JUCE" juce)

# ---------------------------------------------------------------------------
# 2. Tracktion Engine — the modules/ SUBDIRECTORY ONLY
# ---------------------------------------------------------------------------

# set(JUCE_VERSION ...): JUCE's own project(JUCE VERSION 8.0.6) sets that variable in
# JUCE's DIRECTORY SCOPE only, so it is not visible here. Without this line, TE's
# modules/CMakeLists.txt:25
#     INSTALL_PATH "include/JUCE-${JUCE_VERSION}/modules"
# expands to a malformed "include/JUCE-/modules" and registers three install(DIRECTORY)
# rules pointing at it. Harmless until somebody runs `cmake --install` — which is
# exactly when it will be least welcome — and one line to make sane now.
# Sourced from WFG_PIN_JUCE so the version we pin is stated in one place.
set(JUCE_VERSION "${WFG_PIN_JUCE}")

# We add tracktion_engine's modules/ SUBDIRECTORY ONLY, never TE's root CMakeLists.
# TE's root does add_subdirectory(modules/juce) + enable_testing() + add_subdirectory
# (examples), which would (a) pull a SECOND JUCE through TE's own .gitmodules, whose
# URL is git@github.com:juce-framework/JUCE.git — SSH, which fails on every CI runner
# and every keyless clone; (b) call enable_testing() in our tree; (c) add DemoRunner,
# Benchmarks, TestRunner and EngineInPluginDemo to `all`. modules/CMakeLists.txt is 29
# lines containing a single juce_add_modules() call, reads nothing from TE's root, and
# needs no TE-vendored JUCE — verified with modules/juce/ completely empty.
#
# scripts/check-pins.py enforces this mechanically (check (d)): it fails CI if any
# add_subdirectory of tracktion_engine in our tree is not followed by "/modules".
add_subdirectory("${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules" tracktion_modules)

# ---------------------------------------------------------------------------
# 3. wfg::deps — the compile environment, and nothing that compiles
# ---------------------------------------------------------------------------
add_library(wfg_deps INTERFACE)
add_library(wfg::deps ALIAS wfg_deps)

# TE requires C++20 (concepts, std::span, std::ranges) but its module declarations
# carry no minimumCppStandard, so JUCE falls back to cxx_std_11 on the tracktion
# targets. The root CMakeLists sets CMAKE_CXX_STANDARD 20 for our own targets; this
# line is what puts the requirement on the TARGET rather than on a global default, so
# it survives anything that resets the global.
# If we do not set it, nobody does.
target_compile_features(wfg_deps INTERFACE cxx_std_20)

# These two are literally the only include directories the module targets export:
# JUCEModuleSupport.cmake:570 attaches each module's PARENT directory, which is why a
# single path covers every module in the tree.
#
# SYSTEM is load-bearing, not cosmetic. It compiles JUCE and TE headers with -isystem,
# which keeps their warnings out of the -Wconversion / -Wsign-conversion baseline that
# juce::juce_recommended_warning_flags puts on OUR code. Without it, WFG_WARNINGS_AS_ERRORS
# could never be turned on at all.
#
# It is also what makes tests/ resolve
#     #include <3rd_party/doctest/tracktion_doctest.hpp>
# with no extra wiring, no submodule and no FetchContent: doctest is vendored inside
# TE's modules directory and is reachable the moment that directory is on the path.
target_include_directories(wfg_deps SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/ThirdParty/JUCE/modules"
    "${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules")

target_compile_definitions(wfg_deps INTERFACE
    # --- Replicated from JUCE's own module INTERFACE, because we link the modules
    #     PRIVATE to wfg_thirdparty and their usage requirements therefore do NOT
    #     reach consumers. JUCE applies these in _juce_add_standard_defs()
    #     (JUCEModuleSupport.cmake:104-110), which it calls on juce_core (:519) and
    #     from the juce_add_* helpers we deliberately do not use.
    #     JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED is MANDATORY: without it
    #     juce_TargetPlatform.h:53-68 is a hard
    #     #error "No global header file was included!"
    #     on the FIRST JUCE header any of our own translation units includes.
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    $<$<CONFIG:Debug>:DEBUG=1>
    $<$<CONFIG:Debug>:_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:_NDEBUG=1>
    $<$<PLATFORM_ID:Linux>:LINUX=1>

    # --- JUCE_MODULE_AVAILABLE_* : one per module reachable from our headers,
    #     including the three that arrive only transitively via juce_audio_processors.
    #     JUCE sets these INTERFACE per module at JUCEModuleSupport.cmake:572; same
    #     PRIVATE-linkage reason as above means we must restate them here.
    JUCE_MODULE_AVAILABLE_juce_core=1
    JUCE_MODULE_AVAILABLE_juce_events=1
    JUCE_MODULE_AVAILABLE_juce_data_structures=1
    JUCE_MODULE_AVAILABLE_juce_audio_basics=1
    JUCE_MODULE_AVAILABLE_juce_audio_formats=1
    JUCE_MODULE_AVAILABLE_juce_audio_devices=1
    JUCE_MODULE_AVAILABLE_juce_audio_processors=1
    JUCE_MODULE_AVAILABLE_juce_audio_utils=1
    JUCE_MODULE_AVAILABLE_juce_dsp=1
    JUCE_MODULE_AVAILABLE_juce_osc=1
    JUCE_MODULE_AVAILABLE_juce_graphics=1
    JUCE_MODULE_AVAILABLE_juce_gui_basics=1
    JUCE_MODULE_AVAILABLE_juce_gui_extra=1
    JUCE_MODULE_AVAILABLE_tracktion_core=1
    JUCE_MODULE_AVAILABLE_tracktion_engine=1
    JUCE_MODULE_AVAILABLE_tracktion_graph=1

    # --- Our module configuration. Transcribed from
    #     ThirdParty/tracktion_engine/examples/TestRunner/CMakeLists.txt:73-85,
    #     with what we DECLINE recorded in the block below.
    JUCE_USE_CURL=0                 # drops libcurl4-openssl-dev from the Linux apt line
    JUCE_WEB_BROWSER=0              # drops libwebkit2gtk-4.1-dev; revisit at Phase 11 (PRD 3.23)
    JUCE_STRICT_REFCOUNTEDPOINTER=1 # hygiene; TE's own reference sets it, zero TE references
    JUCE_MODAL_LOOPS_PERMITTED=0    # all 20 TE uses are #if-guarded; a modal loop in a show engine is a hang
    JUCE_JACK=0                     # already the default; explicit because it is what keeps libjack-jackd2-dev off the apt line
    JUCE_PLUGINHOST_LADSPA=0        # already the default; explicit because it is what keeps ladspa-sdk off the apt line

    # --- Ours. NEVER call a macro VERSION or __TEXT: tracktion_engine_playback.cpp:124-153
    #     #undefs and redefines VERSION mid-TU, and tracktion_engine.h:72 bare-#undefs __TEXT.
    #     A macro of either name would be silently erased partway through the build with
    #     no diagnostic, and the failure would surface as a mysteriously empty string.
    WFG_VERSION="${PROJECT_VERSION}"
    WFG_PRODUCT_NAME="Go.dot"
    WFG_PIN_JUCE="${WFG_PIN_JUCE}"
    WFG_PIN_TE="${WFG_PIN_TE}"
    WFG_LOCALE_FR="${WFG_LOCALE_FR}"
    $<$<BOOL:${WFG_RT_CHECKS}>:WFG_RT_CHECKS=1>)

# Config flags, NOT warning flags. On MSVC juce_recommended_config_flags gives /MP —
# a large parallel-build win across the 31 vendor translation units — plus /EHsc and
# the per-config /Od /Zi or /Ox (JUCEHelperTargets.cmake:117-133). The warning flags
# live in wfg::warnings and stay off vendor code; see the next section.
#
# Note for anyone adding a compiler cache to the Windows CI job: the /Zi this injects
# in debug configs is precisely what stops sccache working with MSVC, which needs /Z7.
target_link_libraries(wfg_deps INTERFACE juce::juce_recommended_config_flags)

if(MSVC)
    target_compile_options(wfg_deps INTERFACE /bigobj /utf-8)
    # /bigobj: JUCE attaches it INTERFACE to only juce_gui_basics|juce_audio_processors|
    # juce_core|juce_graphics (JUCEModuleSupport.cmake:649-651), so OUR TE-heavy TUs are
    # uncovered and hit "fatal error C1128: number of sections exceeded object file
    # format limit". The fix belongs here, not in JUCE.
    # /utf-8: our sources are UTF-8 and contain accented French text; without it MSVC
    # reads them as the system codepage and mangles every non-ASCII string literal.
elseif(APPLE)
    # libc++ hardening: bounds and precondition checks in the standard library, at
    # full strength in Debug and in the cheap "fast" mode otherwise.
    #
    # The spelling matters and has changed. TE's TestRunner:107-108 uses
    # _LIBCPP_ENABLE_ASSERTIONS / _LIBCPP_ENABLE_HARDENED_MODE; BOTH were removed
    # from libc++, and the SDK now refuses them outright rather than ignoring them:
    #
    #   hardening.h:25: error: "_LIBCPP_ENABLE_ASSERTIONS has been removed,
    #                           please use _LIBCPP_HARDENING_MODE=<mode> instead"
    #
    # which fails EVERY .mm translation unit - i.e. all of JUCE on Apple. Copying
    # TE's line verbatim is what put the first macOS CI run in the red, and it is
    # a good example of why the three-platform job exists: the same line builds
    # fine on Windows and Linux, where these macros mean nothing at all.
    target_compile_definitions(wfg_deps INTERFACE
        $<IF:$<CONFIG:Debug>,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG,_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
endif()

# DECLINED from TE's examples/TestRunner/CMakeLists.txt, so nobody "fixes" it later:
#  * -latomic (l.115-121) — JUCE runs try_compile probes in JUCECheckAtomic.cmake:33-125
#    and links it through juce::juce_atomic_wrapper, which juce_core links
#    unconditionally (JUCEModuleSupport.cmake:521). Copying TE's line hard-fails on
#    arches where libatomic is absent, to fix a problem JUCE already solved.
#  * -m64 as a LINK option keyed off CMAKE_HOST_SYSTEM_PROCESSOR (l.119) — reads the
#    HOST processor to decide a TARGET flag, so it breaks cross-compiles and
#    Apple-silicon-to-x86 builds.
#  * CACHE INTERNAL on CMAKE_OSX_DEPLOYMENT_TARGET (l.28) — force-writes the cache and
#    makes the deployment target un-overridable from a preset or the command line.
#  * -fno-aligned-allocation (l.105-109) — only needed below macOS 10.14; we target 11.0.
#  * the static MSVC runtime (l.22-24) — see the root CMakeLists; we are a plugin host.
#  * JUCE_MODAL_LOOPS_PERMITTED=1 (l.79) — see above; we set 0 deliberately.
#  * TRACKTION_UNIT_TESTS=1 (l.82) — compiles TE's ENTIRE test corpus into the binary.
#  * every TRACKTION_ENABLE_TIMESTRETCH_* — TE degrades cleanly with all four at 0
#    (TimeStretcher::Mode::defaultMode resolves to `disabled`, every accessor is
#    null-guarded). Varispeed already works: libsamplerate is compiled unconditionally
#    into tracktion_engine_playback.cpp. RubberBand is a LICENCE decision (PRD 3.25
#    "licence permitting") plus a fourth submodule that hard-#errors on a clean clone.
#  * TRACKTION_LOG_DEVICES — a product decision, not a build-system default.
#  * juce_generate_juce_header — configure-time FATAL_ERROR on a plain add_library
#    (JUCEUtils.cmake:551-556: "does not have a generated sources directory"). Our
#    sources include module headers directly, which is the supported JUCE 8 style
#    (JUCE docs/CMake API.md:721-723).
#  * addModuleSourceTarget() / JUCE_ENABLE_MODULE_SOURCE_GROUPS — IDE cosmetics that
#    glob into ../../tests/ and into modules/juce/, the vendored JUCE we leave empty.

# ---------------------------------------------------------------------------
# 4. wfg::warnings — our code only
# ---------------------------------------------------------------------------
# Kept as a target SEPARATE from wfg::deps for one reason, and it is the reason
# WFG_WARNINGS_AS_ERRORS is possible at all: compile flags propagated INTERFACE cannot
# be stripped back off individual sources once those sources are in the same target.
# If the warning flags rode along with the headers, every JUCE and TE translation unit
# would carry -Werror too, and a single new warning in a vendor header under a future
# compiler would redden the entire build with nothing we could do about it short of
# forking.
#
# Linked to: wfg_engine, wfg, wfg_tests, and every spike.
# Linked to wfg_thirdparty: NEVER. See DO-NOT #20.
add_library(wfg_warnings INTERFACE)
add_library(wfg::warnings ALIAS wfg_warnings)

target_link_libraries(wfg_warnings INTERFACE juce::juce_recommended_warning_flags)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # TE's own recommendation, from examples/TestRunner/CMakeLists.txt:111-113 (where
    # it is applied by appending to CMAKE_CXX_FLAGS globally — we scope it to a target
    # instead). GCC warns on the #pragma clang / #pragma warning directives that JUCE
    # and TE sprinkle through headers we include.
    target_compile_options(wfg_warnings INTERFACE -Wno-unknown-pragmas)
endif()

if(WFG_WARNINGS_AS_ERRORS)
    target_compile_options(wfg_warnings INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# 5. wfg::thirdparty — the one place vendor code compiles
# ---------------------------------------------------------------------------
# add_library(... STATIC) with NO source files of its own. That is not an oversight
# and it is not a placeholder awaiting a .cpp: it is VERIFIED to configure, generate,
# compile and link. The JUCE and TE module targets are INTERFACE libraries whose
# INTERFACE_SOURCES carry the module .cpp files (JUCEModuleSupport.cmake:97-100), so
# linking them supplies all 31 translation units. Do not add a token source file to
# make this "look right" — measured: wfg_thirdparty builds 31 TUs, wfg_engine builds
# exactly 1, and the modules compile exactly once for the whole project.
add_library(wfg_thirdparty STATIC)
add_library(wfg::thirdparty ALIAS wfg_thirdparty)

# The engine will eventually be linked into things that are themselves shared objects
# (Phase 9's plugin scanner runs out of process; a future LV2/VST3 build of any part
# of this would too). PIC costs nothing on a static archive and is a link-time error
# to retrofit.
set_target_properties(wfg_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(wfg_thirdparty
    PUBLIC  wfg::deps
    PRIVATE
        # Every one of these is PRIVATE without exception. PUBLIC would put each module's
        # .cpp files into INTERFACE_SOURCES and recompile them in every consumer —
        # "silent ODR violations in the worst case" (JUCE docs/CMake API.md:776-780).
        # Consumers get the headers from wfg::deps and the system link deps
        # (alsa, X11, CoreAudio frameworks, juce_atomic_wrapper) via $<LINK_ONLY:>.
        #
        # Measured from our includes, 2026-09:
        juce::juce_core                 # String, File, SystemStats, ConsoleApplication
        juce::juce_events               # ScopedJuceInitialiser_GUI / MessageManager
        juce::juce_data_structures      # ValueTree — TE's model layer is built on it
        juce::juce_audio_basics         # AudioBuffer, MidiBuffer
        juce::juce_audio_formats        # required by tracktion_graph
        juce::juce_audio_devices        # required by tracktion_engine
        juce::juce_audio_processors     # required by juce_audio_utils; PRD 3.18 hosting
        juce::juce_audio_utils          # required by tracktion_engine
        juce::juce_dsp                  # required by tracktion_engine
        juce::juce_osc                  # required by tracktion_engine; PRD 3.17 OSCQuery
        # juce_graphics, juce_gui_basics and juce_gui_extra arrive TRANSITIVELY via
        # juce_audio_processors -> juce_gui_extra -> juce_gui_basics -> juce_graphics.
        # That chain is unbreakable in JUCE 8 and is why the Linux apt line carries
        # X11, freetype and fontconfig even though Phase 0 ships no UI. They are NOT
        # listed here because the validated configuration does not list them.
        #
        tracktion::tracktion_core       # header-only in a non-unit-test build (verified:
                                        # tracktion_core.cpp contains only .test.cpp
                                        # includes), but linked for parity with TE's own
                                        # reference and because it becomes load-bearing
                                        # the moment TRACKTION_UNIT_TESTS=1 is ever set.
        tracktion::tracktion_engine
        tracktion::tracktion_graph)
