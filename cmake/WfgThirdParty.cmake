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
#   wfg::thirdparty  STATIC     the ONE place the JUCE, TE and juce_simpleweb
#                               module sources compile. Links wfg::deps PUBLIC, so
#                               anything that links wfg::thirdparty gets the
#                               headers too.
#
# Nothing outside this file may name a juce::, tracktion:: or juce_simpleweb
# target. src/, tests/ and spikes/ link wfg::thirdparty (or wfg::engine, which
# re-exports it) and get the whole environment; that is deliberate, and DO-NOT #5
# in the build contract.
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

# set(JUCE_VERSION ...): JUCE's own project(JUCE VERSION 8.0.13) sets that variable in
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
#
# Go.dot's own changes to Tracktion go on FIRST (decision DQ, namespace draft §22.3):
# a patch series the build applies to the submodule's working tree, so everything
# below reads and compiles the patched sources. check-pins.py check (g) says whether
# the series fits the pin before any toolchain runs.
include(WfgTracktionPatches)
add_subdirectory("${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules" tracktion_modules)

# ---------------------------------------------------------------------------
# 2b. juce_simpleweb — HTTP and WebSocket on one port, with the TLS ripped out
# ---------------------------------------------------------------------------
# Ben Kuper's JUCE module (GPL-3), pinned at pob31/juce_simpleweb. It is what
# carries OSCQuery: the spec puts HTTP and WebSocket on the SAME port, which is
# the one thing juce::StreamingSocket cannot be talked into doing.
#
# TRANSCRIBED, NOT INCLUDED. spatcore ships this recipe as a function,
# spatcore_add_juce_simpleweb() in cmake/SpatcoreConsumer.cmake, and including
# it would be the obvious move. Two reasons not to. First, the no-functions rule
# at the top of this file: a helper you forget to call gives you a TU compiled
# with different definitions from every other one. Second, and concretely, that
# function is WRONG ON LINUX - see the link-libraries note below.
juce_add_module("${CMAKE_SOURCE_DIR}/ThirdParty/juce_simpleweb")

# CLEARED OUTRIGHT, not filtered, and this is the divergence worth reading.
#
# The module declares its OpenSSL dependency three times, once per platform, and
# NOT in the same spelling (juce_simpleweb.h:25-27):
#
#     linuxLibs:    ssl,crypto            <- bare
#     OSXLibs:      libssl,libcrypto,z    <- lib-prefixed
#     windowsLibs:  libssl,libcrypto      <- lib-prefixed
#
# JUCE turns whichever line matches the platform into INTERFACE link libraries
# (JUCEModuleSupport.cmake:610-643). spatcore's function then removes the items
# `libssl libcrypto z` - which matches macOS and Windows exactly, and misses
# Linux entirely, because there the items are `ssl` and `crypto`. The result is
# `-lssl -lcrypto` on the link line of a build that compiles no TLS at all: it
# needs an OpenSSL SDK present to link, and links a library it never calls.
#
# A REMOVE_ITEM list that has to be kept in step with three vendor spellings is a
# gate that fails open. Clearing the property says what we mean - this module
# contributes no link libraries - and it stays true if a fourth spelling appears.
# It is safe in full: juce_simpleweb declares no `dependencies:` and no
# `searchpaths:`, so those OpenSSL names are the ONLY thing in the property.
# `deps.no-openssl` (tests/CMakeLists.txt) asserts the outcome on the shipped
# binary rather than trusting this comment.
set_target_properties(juce_simpleweb PROPERTIES INTERFACE_LINK_LIBRARIES "")

# The module's .cpp files #include <JuceHeader.h>, which only the Projucer
# generates. This directory holds a two-line stub and is put on the interface of
# THIS TARGET ONLY, so `#include <JuceHeader.h>` anywhere in src/ or tests/ stays
# the error it should be.
target_include_directories(juce_simpleweb INTERFACE
    "${CMAKE_SOURCE_DIR}/cmake/JuceHeaderStub")

# ---------------------------------------------------------------------------
# 2c. spatcore — headers only, and no add_subdirectory
# ---------------------------------------------------------------------------
# spatcore (GPL-3, pob31/spatcore) is the author's shared control plane. It is
# consumed at SOURCE level: `#include <spatcore/io/DeviceHost.h>` and
# `#include <spatcore/rt/RtThreadPriority.h>`, and nothing more. There is
# deliberately no add_subdirectory().
#
# Its own CMakeLists builds targets that call juce_add_modules() again, which
# would compile JUCE a SECOND time in this build tree and break the one-compile
# rule this whole file exists to hold (section 5). The headers used here need
# no library of their own: RtThreadPriority.h is JUCE-free, and DeviceHost.h
# needs only juce_audio_devices, which wfg_thirdparty already compiles.
#
# So spatcore needs exactly one thing from the build, and it is the ThirdParty
# include root added in section 3 below. spatcore builds against JUCE 9 and
# this tree against JUCE 8.0.13, so compile a header here before depending on
# it. Go.dot's own osc/ codec is not a version workaround: spatcore's parser
# compiles on JUCE 8 (OSCArgument(true) promotes to the int32 overload in both
# versions) but drops bundle time tags - see docs/godot-reuse-map-0.1.md.

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
# ThirdParty itself is the third entry, and it is what makes
#     #include <juce_simpleweb/juce_simpleweb.h>
#     #include <spatcore/rt/RtThreadPriority.h>
# resolve. juce_add_module attaches the module's PARENT directory to the MODULE
# target, but that target is linked PRIVATE below, so its interface reaches
# nothing of ours; and spatcore has no target at all. SYSTEM for the same reason
# as the other two: it keeps somebody else's warnings out of the -Werror
# baseline that wfg::warnings puts on our code.
target_include_directories(wfg_deps SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/ThirdParty/JUCE/modules"
    "${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine/modules"
    "${CMAKE_SOURCE_DIR}/ThirdParty")

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
    # Audio ASIO (not the networking Asio library). Keep the definition shared
    # by JUCE, the engine and the UI so device enumeration agrees everywhere.
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>

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
    JUCE_MODULE_AVAILABLE_juce_cryptography=1
    JUCE_MODULE_AVAILABLE_juce_simpleweb=1
    JUCE_MODULE_AVAILABLE_juce_graphics=1
    JUCE_MODULE_AVAILABLE_juce_gui_basics=1
    JUCE_MODULE_AVAILABLE_juce_gui_extra=1
    JUCE_MODULE_AVAILABLE_juce_opengl=1
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
    # JACK on Linux (namespace draft decision I, 2026-09-05, carried out 2026-09-30):
    # the way to every channel of a multichannel interface on a desktop where
    # PipeWire holds it and ALSA offers its stereo pair. Only the header is
    # needed to build (libjack-jackd2-dev); JUCE dlopens libjack.so.0 when Go.dot
    # starts, so a machine without JACK still runs it, with no JACK choice.
    JUCE_JACK=$<IF:$<PLATFORM_ID:Linux>,1,0>
    # 2026-10-08 (namespace draft §44, XZ, the author's pick): the video renderer
    # draws through sokol_gfx on each system's own graphics - Direct3D 11, Metal,
    # OpenGL through EGL. HERE, on the interface every translation unit sees: the
    # shaders' generated header (video.glsl.h) keeps only the backend named, and
    # the one translation unit that builds sokol (render/SokolGfx.cpp) must agree.
    $<$<PLATFORM_ID:Windows>:SOKOL_D3D11>
    $<$<PLATFORM_ID:Darwin>:SOKOL_METAL>
    $<$<PLATFORM_ID:Linux>:SOKOL_GLCORE>
    JUCE_PLUGINHOST_LADSPA=0        # already the default; explicit because it is what keeps ladspa-sdk off the apt line
    # 2026-09-28 (namespace draft §22, decision DZ): Signalsmith Stretch, the time-
    # stretcher behind a media cue's `timestretch` mode. MIT-licensed, header-only and
    # vendored inside Tracktion (3rd_party/signalsmith-stretch), so it costs no
    # submodule and no package. HERE, on the interface every translation unit sees,
    # and not on Tracktion's module alone: the define decides what
    # TimeStretcher::defaultMode IS, and a translation unit of ours reading a different
    # answer from Tracktion's own would be an ODR violation nothing reports. Patch 0001
    # is what keeps a varispeed cue off it (Go.dot's engine behaviour says so), since
    # every auto-tempo clip would otherwise be handed the stretcher the moment it exists.
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1
    # Phase 9a (2026-09-23, decision AF): VST3 hosting compiled in, on every
    # platform, in the one place WfgOptions.cmake reserved for it. What hosts
    # a plugin is the child process (wfg plugin-host) and the scan child;
    # the engine's own process never instantiates one.
    JUCE_PLUGINHOST_VST3=1
    # 2026-09-26 (the author's decision): LV2 on every platform. JUCE vendors
    # the LV2 SDK (lilv, serd, sord, sratom, zix), so this costs no system
    # package on any platform - THIRD_PARTY_NOTICES.md carries their licences.
    # Each child registers only the format of the plugin it hosts
    # (plugin/PluginLoad.cpp), since JUCE's LV2 format reads every bundle on
    # the default folders the moment it is made.
    JUCE_PLUGINHOST_LV2=1
    # And AU on macOS, the author's decision of the same day. JUCE's own
    # helpers would link AudioUnit and CoreAudioKit for a plugin host; this
    # build uses its modules only, so the APPLE block below links them.
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>

    # --- juce_simpleweb, TLS off.
    #     Upstream defaults SECURE support ON, which compiles asio's OpenSSL
    #     paths and then fails at link with unresolved SSL symbols in a build
    #     that ships no OpenSSL. It is set HERE, on wfg::deps, rather than on the
    #     module target as spatcore sets it, and one line of reasoning covers
    #     both places it has to reach: wfg_thirdparty links wfg::deps PUBLIC, so
    #     the module .cpp files compiled into it get this define, and so does
    #     every TU of ours that includes a juce_simpleweb header. spatcore needs
    #     the define on the module because it links the module PUBLIC and the
    #     .cpps are therefore recompiled in each consumer; we link it PRIVATE, so
    #     they compile exactly once, here.
    #     SimpleWebToolchainTests.cpp static_asserts that it arrived.
    SIMPLEWEB_SECURE_SUPPORTED=0

    # --- asio, built as C++20.
    #     std::result_of was deprecated in C++17 and REMOVED in C++20. This asio
    #     (2020-era, pinned through juce_simpleweb) still reaches for it, and its
    #     own switch away from it is gated on the compiler rather than on the
    #     language version - config.hpp:934-943 is, in full:
    #
    #         #if !defined(ASIO_HAS_STD_INVOKE_RESULT)
    #         # if !defined(ASIO_DISABLE_STD_INVOKE_RESULT)
    #         #  if defined(ASIO_MSVC)
    #         #   if (_MSC_VER >= 1911 && _MSVC_LANG >= 201703)
    #         #    define ASIO_HAS_STD_INVOKE_RESULT 1
    #
    #     MSVC only. On clang and GCC it is never defined, so detail/type_traits.h
    #     falls through to `using std::result_of;` and every asio header that
    #     touches it fails to compile.
    #
    #     That asymmetry is exactly why this is set here and not left to chance:
    #     the Windows build of PR 1.D was green and the macOS build was not, with
    #     dozens of "no template named 'result_of'" from inside asio. Linux
    #     happens to survive because libstdc++ still ships the removed template
    #     as a deprecated extension - which is luck, not support, and would end
    #     at any libstdc++ release.
    #
    #     Defining it makes asio use std::invoke_result on all three, which is
    #     what the macro is for. TE requires C++20 (cxx_std_20 above), so
    #     compiling this module at C++17 instead is not an option, and mixing
    #     standards across TUs of one target is an ODR hazard rather than a fix.
    ASIO_HAS_STD_INVOKE_RESULT=1

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

# std::thread, std::mutex and std::condition_variable, which the tick clock uses.
#
# Named rather than left to arrive by accident. JUCE's juce_core does declare
# pthread among its Linux libraries, so this would probably link without it
# today - but "probably, through somebody else's dependency" is not a thing to
# rest a thread on. Threads::Threads is the portable spelling: -pthread on
# Linux, and nothing at all on Windows and macOS, where the standard library
# needs no help.
find_package(Threads REQUIRED)
target_link_libraries(wfg_deps INTERFACE Threads::Threads)

if(MSVC)
    target_compile_options(wfg_deps INTERFACE /bigobj /utf-8)
    # /bigobj: JUCE attaches it INTERFACE to only juce_gui_basics|juce_audio_processors|
    # juce_core|juce_graphics (JUCEModuleSupport.cmake:649-651), so OUR TE-heavy TUs are
    # uncovered and hit "fatal error C1128: number of sections exceeded object file
    # format limit". The fix belongs here, not in JUCE.
    # /utf-8: our sources are UTF-8 and contain accented French text; without it MSVC
    # reads them as the system codepage and mangles every non-ASCII string literal.
endif()

# --- libatomic ---------------------------------------------------------------------
# Tracktion has at least one atomic that is far too large to be lock-free:
#
#     struct AudioClipPlayhead::State { std::optional<TimePosition> position;
#                                       uint32_t lastUpdateMs; };
#     std::atomic<State> state;                        (tracktion_AudioClipBase.h:49-55)
#
# About 24 bytes, so GCC cannot do it in hardware and emits calls to __atomic_store /
# __atomic_load, which live in libatomic. MSVC provides those inline and Apple's libc++
# carries them, which is why this was a Linux-only link failure, and only after the move
# to TE develop where that type was introduced:
#
#     undefined reference to `__atomic_store'
#       std::atomic<tracktion::engine::AudioClipPlayhead::State>::store(...)
#
# TWO WRONG ANSWERS WERE TRIED BEFORE THIS ONE, AND BOTH ARE INSTRUCTIVE:
#
#  1. Declining TE's own `-latomic` line, on the reasoning that JUCE's
#     JUCECheckAtomic.cmake probe and juce::juce_atomic_wrapper had already handled it.
#     They had not - JUCE's probe tests JUCE's atomics, not Tracktion's.
#
#  2. find_library(atomic). It reports NOT FOUND on a perfectly ordinary Ubuntu runner,
#     because GCC ships libatomic.so inside its own private directory
#     (/usr/lib/gcc/x86_64-linux-gnu/13/) which CMake does not search - while `-latomic`
#     via the compiler driver works fine. Asking the filesystem was the wrong question.
#
# So: ask the TOOLCHAIN, with a probe that reproduces the actual failure shape rather
# than a token atomic. If a large atomic links unaided, add nothing; if it needs
# -latomic, add exactly that; if neither works, fail at configure with the reason
# instead of at link with a mangled symbol.
if(NOT MSVC)
    include(CheckCXXSourceCompiles)

    set(WFG_BIG_ATOMIC_PROBE "
        #include <atomic>
        #include <optional>
        struct Big { std::optional<double> position; unsigned lastUpdateMs; };
        int main()
        {
            std::atomic<Big> a;
            a.store (Big{});
            return a.load().lastUpdateMs == 0u ? 0 : 1;
        }")

    check_cxx_source_compiles("${WFG_BIG_ATOMIC_PROBE}" WFG_BIG_ATOMIC_LINKS_UNAIDED)

    if(NOT WFG_BIG_ATOMIC_LINKS_UNAIDED)
        set(CMAKE_REQUIRED_LIBRARIES atomic)
        check_cxx_source_compiles("${WFG_BIG_ATOMIC_PROBE}" WFG_BIG_ATOMIC_NEEDS_LIBATOMIC)
        unset(CMAKE_REQUIRED_LIBRARIES)

        if(WFG_BIG_ATOMIC_NEEDS_LIBATOMIC)
            target_link_libraries(wfg_deps INTERFACE atomic)
            message(STATUS "wfg: large std::atomic needs libatomic - linking it")
        else()
            message(FATAL_ERROR
                "wfg: a large std::atomic links neither unaided nor with -latomic on this "
                "toolchain.\n"
                "Tracktion needs one (AudioClipPlayhead::State, ~24 bytes), so the build "
                "would fail at link with an undefined reference to __atomic_store.\n"
                "Install the toolchain's libatomic, or report which platform this is.")
        endif()
    else()
        message(STATUS "wfg: large std::atomic links unaided - libatomic not required")
    endif()
endif()

if(APPLE)
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

    # AU hosting (2026-09-26): the frameworks JUCE's juce_add_* helpers would
    # have linked for a plugin host, which a modules-only build must name
    # itself - AudioUnit for the component API, CoreAudioKit for an AU's own
    # window in the editing helper.
    target_link_libraries(wfg_deps INTERFACE "-framework AudioUnit" "-framework CoreAudioKit")

    # The video renderer (namespace draft §44): Metal for the device, QuartzCore
    # for the layer each projector's window shows it through.
    target_link_libraries(wfg_deps INTERFACE "-framework Metal" "-framework QuartzCore")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The video renderer (namespace draft §44) on OpenGL through EGL: EGL for the
    # context - one, drawing every projector's window and, with no window, a
    # picture in memory - and libGL for the calls sokol makes.
    find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
    target_link_libraries(wfg_deps INTERFACE OpenGL::EGL OpenGL::OpenGL)
endif()

# DECLINED from TE's examples/TestRunner/CMakeLists.txt, so nobody "fixes" it later:
#  * -latomic AS TE WRITES IT (l.115-121) — see the libatomic probe above. TE's line
#    is right about the NEED and wrong about being unconditional; we detect it with a
#    try_compile instead. Two of our own earlier answers were also wrong and are
#    recorded up there, because both looked correct.
#  * -m64 as a LINK option keyed off CMAKE_HOST_SYSTEM_PROCESSOR (l.119) — reads the
#    HOST processor to decide a TARGET flag, so it breaks cross-compiles and
#    Apple-silicon-to-x86 builds.
#  * CACHE INTERNAL on CMAKE_OSX_DEPLOYMENT_TARGET (l.28) — force-writes the cache and
#    makes the deployment target un-overridable from a preset or the command line.
#  * -fno-aligned-allocation (l.105-109) — only needed below macOS 10.14; we target 11.0.
#  * the static MSVC runtime (l.22-24) — see the root CMakeLists; we are a plugin host.
#  * JUCE_MODAL_LOOPS_PERMITTED=1 (l.79) — see above; we set 0 deliberately.
#  * TRACKTION_UNIT_TESTS=1 (l.82) — compiles TE's ENTIRE test corpus into the binary.
#  * TRACKTION_ENABLE_TIMESTRETCH_ELASTIQUE, _RUBBERBAND and _SOUNDTOUCH — Signalsmith,
#    switched on above (2026-09-28), is the one stretcher a media cue's speed needs: the
#    only one of Tracktion's four that reaches the speed's twenty and freezes at nought.
#    Elastique is a commercial SDK that is not here. RubberBand is a LICENCE decision
#    (PRD 3.25 "licence permitting") plus a fourth submodule that hard-#errors on a clean
#    clone. SoundTouch (LGPL) is sized for 0.25 to 4 and asserts at twenty
#    (tracktion_TimeStretch.cpp:503-538, 1364-1368). Before that day all four were
#    declined, and Tracktion degraded cleanly: defaultMode resolved to `disabled` and
#    every accessor is null-guarded.
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
# Compile the shared widget under the vendor warning policy, without changing
# spatcore's API or pulling a second set of JUCE module sources into the build.
target_sources(wfg_thirdparty PRIVATE
    "${CMAKE_SOURCE_DIR}/ThirdParty/spatcore/ui/patch/PatchMatrixComponent.cpp")
# sokol_gfx's implementation, once, under the vendor warning policy (namespace
# draft §44). Objective-C++ on Apple, where its Metal half is written in it.
if(APPLE)
    target_sources(wfg_thirdparty PRIVATE "${CMAKE_SOURCE_DIR}/src/wfg/engine/video/render/SokolGfx.mm")
else()
    target_sources(wfg_thirdparty PRIVATE "${CMAKE_SOURCE_DIR}/src/wfg/engine/video/render/SokolGfx.cpp")
endif()
# Spout's SpoutDX (BSD-2), vendored in ThirdParty/spout - eight files of a
# 655 MB repository - for an output sent to other programs on Windows
# (namespace draft §44, YA).
if(WIN32)
    foreach(_spout SpoutDX SpoutCopy SpoutDirectX SpoutFrameCount SpoutSenderNames SpoutSharedMemory SpoutUtils)
        target_sources(wfg_thirdparty PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/spout/${_spout}.cpp")
    endforeach()
    unset(_spout)
endif()

# Syphon (BSD), the Mac's Spout: its Metal server and client and what they need,
# never its OpenGL half - a library of its own, because Syphon is Objective-C
# written for ARC with a prefix header of its own, which nothing else here is.
# The submodule sits at ThirdParty/Syphon so its headers' own <Syphon/...>
# imports find each other on the include path everything already has.
if(APPLE)
    set(_syphon "${CMAKE_SOURCE_DIR}/ThirdParty/Syphon")
    add_library(wfg_syphon STATIC)
    foreach(_file SyphonServerBase SyphonServerConnectionManager
                  SyphonMetalClient SyphonClientBase SyphonClientConnectionManager SyphonServerDirectory
                  SyphonImageBase SyphonPrivate SyphonMessaging SyphonMessageQueue SyphonMessageSender
                  SyphonMessageReceiver SyphonCFMessageSender SyphonCFMessageReceiver)
        target_sources(wfg_syphon PRIVATE "${_syphon}/${_file}.m")
    endforeach()
    target_sources(wfg_syphon PRIVATE "${_syphon}/SyphonDispatch.c"
        # Go.dot's server on Syphon's base class: it only blits, and so never
        # needs the shaders SyphonMetalServer loads from Syphon.framework's
        # bundle, which a static library has not got.
        "${CMAKE_SOURCE_DIR}/src/wfg/engine/video/render/GoDotSyphonServer.m")
    target_include_directories(wfg_syphon SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty")
    target_compile_definitions(wfg_syphon PRIVATE SYPHON_CORE_SHARE)
    target_compile_options(wfg_syphon PRIVATE
        $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
        "SHELL:-include ${_syphon}/Syphon_Prefix.pch"
        -w)
    set_target_properties(wfg_syphon PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(wfg_syphon PUBLIC "-framework Cocoa" "-framework IOSurface" "-framework Metal")
    target_link_libraries(wfg_deps INTERFACE wfg_syphon)
    unset(_file)
    unset(_syphon)
endif()

# hidapi (BSD-3, libusb/hidapi at hidapi-0.15.0), for the SpaceMouse the engine
# reads itself (namespace draft 45, O.11, ZE): the one platform file and nothing
# of hidapi's own CMake - spatcore_add_hidapi's settings, transcribed (static, no
# hidtest, hidraw on Linux). Windows loads hid.dll and cfgmgr32.dll at run time,
# so links nothing; macOS needs IOKit and CoreFoundation; Linux libudev, which
# scripts/install-linux-deps.sh installs. Its headers are reached as
# <hidapi/hidapi/hidapi.h> through ThirdParty, never ThirdParty/hidapi itself, whose
# VERSION file would answer `#include <version>` on a case-blind disk.
set(_hidapi "${CMAKE_SOURCE_DIR}/ThirdParty/hidapi")
add_library(wfg_hidapi STATIC)
if(WIN32)
    target_sources(wfg_hidapi PRIVATE "${_hidapi}/windows/hid.c" "${_hidapi}/windows/hidapi_descriptor_reconstruct.c")
elseif(APPLE)
    target_sources(wfg_hidapi PRIVATE "${_hidapi}/mac/hid.c")
    target_link_libraries(wfg_hidapi PUBLIC "-framework IOKit" "-framework CoreFoundation")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WFG_UDEV REQUIRED IMPORTED_TARGET libudev)
    target_sources(wfg_hidapi PRIVATE "${_hidapi}/linux/hid.c")
    target_link_libraries(wfg_hidapi PUBLIC PkgConfig::WFG_UDEV)
endif()
target_include_directories(wfg_hidapi SYSTEM PRIVATE "${_hidapi}/hidapi")
target_compile_definitions(wfg_hidapi PUBLIC HID_API_NO_EXPORT_DEFINE)
target_compile_options(wfg_hidapi PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(wfg_hidapi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wfg_deps INTERFACE wfg_hidapi)
unset(_hidapi)

# Pure Data (BSD-3, pure-data/pure-data at 0.56-5) and libpd's wrapper (BSD-3,
# libpd/libpd at 0.16.1), which runs a process cue's patch (namespace draft §51).
# Pd's sources come from ThirdParty/pure-data, never from libpd's own nested
# pure-data, which stays empty as TE's JUCE does: scripts/check-pins.py (h) holds
# the two gitlinks equal. Nothing of libpd's own CMake: its source list transcribed,
# its settings with three changes -
#   * one Pd instance per patch, each bound to its own thread (PDINSTANCE,
#     PDTHREADS; namespace draft §51, ACT), and libpd's extras left out;
#   * LIBPD_NO_NUMERIC: libpd_init would otherwise set the WHOLE process's number
#     format to C - each patch's thread sets its own instead (ACQ);
#   * no compiled externals (ACP): Pd's loader tries a binary first, with
#     LoadLibrary or dlopen; the build compiles a copy of s_loader.c whose first
#     loader refuses, so only .pd patches are ever found;
#   * a patch cannot end Go.dot (ACP): "pd quit" calls exit() and "pd exit" sets
#     a quit flag every instance's scheduler stops on, so one message box would
#     end the show or freeze every patch's clocks; the copy of s_inter.c refuses
#     both, in Pd's console's words;
#   * nothing printed behind the show's back: Pd 0.56-5's [expr] still prints a
#     debugging line to stdout whenever it makes a long expression, which the
#     copy of x_vexp.c does not.
# Each copy is made at configure time - line endings made LF first, so a checkout
# with CRLF matches too - and the submodule is left as pinned. If a text the build
# replaces ever moves, configure stops here rather than building a Pd without
# the change.
# On MSVC Pd's <pthread.h> is Go.dot's own (src/wfg/engine/process/pthread-win32),
# seen by Pd's sources alone.
set(_pd "${CMAKE_SOURCE_DIR}/ThirdParty/pure-data/src")
set(_libpd "${CMAKE_SOURCE_DIR}/ThirdParty/libpd/libpd_wrapper")
add_library(wfg_pd STATIC)
foreach(_file d_arithmetic d_array d_ctl d_dac d_delay d_fft d_fft_fftsg d_filter d_global
              d_math d_misc d_osc d_resample d_soundfile d_soundfile_aiff d_soundfile_caf
              d_soundfile_next d_soundfile_wave d_ugen g_all_guis g_array g_bang g_canvas
              g_clone g_editor g_editor_extras g_graph g_guiconnect g_io g_mycanvas g_numbox
              g_radio g_readwrite g_rtext g_scalar g_slider g_template g_text g_toggle
              g_traversal g_undo g_vumeter m_atom m_binbuf m_class m_conf m_glob m_memory
              m_obj m_pd m_sched s_audio s_audio_dummy s_inter_gui s_main s_net
              s_path s_print s_utf8 x_acoustics x_arithmetic x_array x_connective x_file
              x_gui x_interface x_list x_midi x_misc x_net x_scalar x_text x_time
              x_vexp_fun x_vexp_if)
    target_sources(wfg_pd PRIVATE "${_pd}/${_file}.c")
endforeach()

# One of Pd's files, read and changed in memory: `_wfg_pd_amend(<file> <from> <to>)`
# replaces <from> by <to> in the copy being built for <file>, or stops configure.
function(_wfg_pd_amend file from to)
    if(NOT DEFINED _wfg_pd_${file})
        file(READ "${CMAKE_SOURCE_DIR}/ThirdParty/pure-data/src/${file}" _text)
        string(REPLACE "\r\n" "\n" _text "${_text}")
    else()
        set(_text "${_wfg_pd_${file}}")
    endif()
    string(FIND "${_text}" "${from}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR
            "ThirdParty/pure-data/src/${file} no longer holds\n    ${from}\n"
            "which Go.dot's build changes (namespace draft §51, ACP). Rework "
            "cmake/WfgThirdParty.cmake for the new Pd before moving the pin.")
    endif()
    string(REPLACE "${from}" "${to}" _text "${_text}")
    set(_wfg_pd_${file} "${_text}" PARENT_SCOPE)
endfunction()

_wfg_pd_amend(s_loader.c
    "static loader_queue_t loaders = {sys_do_load_lib, NULL};"
    "static int wfg_refuse_compiled_externals(t_canvas *canvas, const char *objectname, const char *path)\n{ (void)canvas; (void)objectname; (void)path; return 0; }\nstatic loader_queue_t loaders = {wfg_refuse_compiled_externals, NULL};")
_wfg_pd_amend(s_inter.c
    "void glob_exit(void *dummy, t_floatarg status)\n{\n    sys_exit(status);\n}"
    "void glob_exit(void *dummy, t_floatarg status)\n{\n    (void)dummy; (void)status;\n    pd_error(0, \"Go.dot: a patch cannot end Go.dot - [pd exit( is refused\");\n}")
_wfg_pd_amend(s_inter.c
    "void glob_quit(void *dummy, t_floatarg status)\n{\n    exit(status);\n}"
    "void glob_quit(void *dummy, t_floatarg status)\n{\n    (void)dummy; (void)status;\n    pd_error(0, \"Go.dot: a patch cannot end Go.dot - [pd quit( is refused\");\n}")
_wfg_pd_amend(x_vexp.c
    "    printf (\"realloc called with %zu bytes\\n\", size);\n"
    "")
foreach(_file s_loader.c s_inter.c x_vexp.c)
    file(WRITE "${CMAKE_BINARY_DIR}/wfg_pd/${_file}.in" "${_wfg_pd_${_file}}")
    configure_file("${CMAKE_BINARY_DIR}/wfg_pd/${_file}.in" "${CMAKE_BINARY_DIR}/wfg_pd/${_file}" COPYONLY)
    target_sources(wfg_pd PRIVATE "${CMAKE_BINARY_DIR}/wfg_pd/${_file}")
    unset(_wfg_pd_${_file})
endforeach()
target_sources(wfg_pd PRIVATE
    "${_libpd}/s_libpdmidi.c"
    "${_libpd}/x_libpdreceive.c"
    "${_libpd}/z_hooks.c"
    "${_libpd}/z_libpd.c")
target_include_directories(wfg_pd SYSTEM PUBLIC "${_libpd}" "${_pd}")
target_compile_definitions(wfg_pd
    PUBLIC PD=1 USEAPI_DUMMY=1 PDINSTANCE=1 PDTHREADS=1 LIBPD_NO_NUMERIC=1
    PRIVATE PD_INTERNAL=1)
set_property(TARGET wfg_pd PROPERTY C_STANDARD 11)
include(CheckIncludeFile)
foreach(_h alloca.h endian.h machine/endian.h unistd.h)
    string(MAKE_C_IDENTIFIER "WFG_PD_HAVE_${_h}" _var)
    check_include_file("${_h}" ${_var})
    if(${_var})
        string(TOUPPER "${_h}" _def)
        string(MAKE_C_IDENTIFIER "HAVE_${_def}" _def)
        target_compile_definitions(wfg_pd PUBLIC ${_def}=1)
    endif()
endforeach()
if(MSVC)
    # Pd's own MSVC settings from libpd's CMake: a 64-bit t_int, the C11 atomics
    # its scheduler uses, and Windows' names for what POSIX calls otherwise.
    target_compile_definitions(wfg_pd
        PUBLIC "PD_LONGINTTYPE=long long" EXTERN=extern HAVE_STRUCT_TIMESPEC=1
        PRIVATE _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_sources(wfg_pd PRIVATE "${CMAKE_SOURCE_DIR}/src/wfg/engine/process/pthread-win32/pthread_win32.c")
    target_include_directories(wfg_pd PRIVATE "${CMAKE_SOURCE_DIR}/src/wfg/engine/process/pthread-win32")
    target_compile_options(wfg_pd PRIVATE /experimental:c11atomics /w)
    target_link_libraries(wfg_pd PUBLIC Ws2_32)
else()
    target_compile_options(wfg_pd PRIVATE -w)
    find_package(Threads REQUIRED)
    target_link_libraries(wfg_pd PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(APPLE)
        target_compile_definitions(wfg_pd PUBLIC _DARWIN_C_SOURCE)
    else()
        target_link_libraries(wfg_pd PUBLIC m)
    endif()
endif()
set_target_properties(wfg_pd PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wfg_deps INTERFACE wfg_pd)
unset(_file)
unset(_h)
unset(_var)
unset(_def)
unset(_libpd)
unset(_pd)

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
        juce::juce_opengl               # Phase 8a (namespace draft 35, UX): the video
                                        # renderer's windows, drawn in a child process
                                        # (`wfg video-render`); the engine itself draws
                                        # nothing with it.
        juce::juce_cryptography         # SHA256, for the event log's bundle hash. The log
                                        # header records a hash over show.xml, state.xml and
                                        # namespaces/*, so a replay can refuse a log that was
                                        # recorded against a different show rather than
                                        # reproducing it against the wrong one and calling
                                        # the difference a divergence. PR 1.D needs this
                                        # module anyway: juce_simpleweb includes it
                                        # unconditionally.
        # juce_graphics, juce_gui_basics and juce_gui_extra arrive TRANSITIVELY via
        # juce_audio_processors -> juce_gui_extra -> juce_gui_basics -> juce_graphics.
        # That chain is unbreakable in JUCE 8 and is why the Linux apt line carries
        # X11, freetype and fontconfig even though Phase 0 ships no UI. They are NOT
        # listed here because the validated configuration does not list them.
        #
        juce_simpleweb                  # HTTP + WebSocket on one port, for OSCQuery.
                                        # Not namespaced juce:: - it is a module added
                                        # by us in section 2b, not one of JUCE's own.
                                        # PRIVATE like everything else here, which is
                                        # what makes its four .cpp files compile once.

        tracktion::tracktion_core       # header-only in a non-unit-test build (verified:
                                        # tracktion_core.cpp contains only .test.cpp
                                        # includes), but linked for parity with TE's own
                                        # reference and because it becomes load-bearing
                                        # the moment TRACKTION_UNIT_TESTS=1 is ever set.
        tracktion::tracktion_engine
        tracktion::tracktion_graph)
