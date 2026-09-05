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
# Phase 1 subsystems, in the order they depend on each other:
#
#   osc/      the value type of the whole control plane (OSC 1.1), the one
#             number formatter every text surface writes through, the codec that
#             puts both on a wire, and the UDP socket that says who sent what
#   document/ the show document: the schema generated from the parameter table,
#             stable identifiers, canonical XML, and the commands that are the
#             only way any of it changes; then the bundle it lives in - what
#             someone decided, what the engine was doing, and the RELAX NG
#             grammar that lets somebody else's validator have an opinion
#   json/     JSON, read. Ours rather than JUCE's, and measured: JUCE's parser
#             overflows an int64 on a long integer literal and returns a
#             different number (see JsonValue.h). Mounts read other people's
#             range bounds through this
#   command/  named commands, their signatures, and the single ordered queue
#             every mutation arrives on
#   log/      the tick-indexed flight recorder (PRD §3.15) and deterministic
#             replay — built before there is anything to record, deliberately
#   cue/      the cue list's pointer: where GO will act, which list it acts on,
#             and the commands that move it. It owns no data - the standby is an
#             attribute of the show; this is the arithmetic of moving it
#   clock/    the sample counter every other clock derives from (PRD §3.4), the
#             50 Hz tick derived from it, and the thread that turns one into the
#             other - plus a paced stand-in for the audio device Phase 2 brings
#   tree/     the /godot namespace: a projection of the document and the
#             engine's own counters, published once per tick as an immutable
#             snapshot the server threads read without ever waiting on the model
#   Engine    the facade that ties them together; Console the command line
#
# Header-only files are listed as sources on purpose: without them an IDE's
# project tree shows a library with three files in it, and the headers are
# where most of the reasoning lives.
# -----------------------------------------------------------------------------
add_library(wfg_engine STATIC
    wfg/engine/Engine.cpp
    wfg/engine/Engine.h
    wfg/engine/Console.cpp
    wfg/engine/Console.h
    wfg/engine/cue/CueCommands.cpp
    wfg/engine/cue/CueCommands.h
    wfg/engine/cue/CueList.cpp
    wfg/engine/cue/CueList.h
    wfg/engine/audio/AudioCommands.h
    wfg/engine/audio/AudioCommands.cpp
    wfg/engine/audio/AudioHost.h
    wfg/engine/audio/AudioHost.cpp
    wfg/engine/audio/CueMatrix.h
    wfg/engine/audio/CueMatrix.cpp
    wfg/engine/audio/CueOutputPlugin.h
    wfg/engine/audio/CueOutputPlugin.cpp
    wfg/engine/audio/HostedAudioDriver.h
    wfg/engine/audio/HostedAudioDriver.cpp
    wfg/engine/rt/RtCheck.h
    wfg/engine/rt/RtCheck.cpp
    wfg/engine/rt/RtCounters.h
    wfg/engine/rt/RtAllocatorHook.cpp
    wfg/engine/clock/AudioClockSource.h
    wfg/engine/clock/DummyAudioClock.cpp
    wfg/engine/clock/DummyAudioClock.h
    wfg/engine/clock/SampleClock.h
    wfg/engine/clock/SampleTime.cpp
    wfg/engine/clock/SampleTime.h
    wfg/engine/clock/TickClock.cpp
    wfg/engine/clock/TickClock.h
    wfg/engine/clock/TickThread.cpp
    wfg/engine/clock/TickThread.h
    wfg/engine/document/Bundle.cpp
    wfg/engine/document/Bundle.h
    wfg/engine/document/CanonicalXml.cpp
    wfg/engine/document/CanonicalXml.h
    wfg/engine/document/DocumentCommands.cpp
    wfg/engine/document/DocumentCommands.h
    wfg/engine/document/EphemeralState.cpp
    wfg/engine/document/EphemeralState.h
    wfg/engine/document/Ids.cpp
    wfg/engine/document/Ids.h
    wfg/engine/document/RelaxNg.cpp
    wfg/engine/document/RelaxNg.h
    wfg/engine/document/Schema.cpp
    wfg/engine/document/Schema.h
    wfg/engine/document/SchemaTable.generated.h
    wfg/engine/document/SchemaTypes.h
    wfg/engine/document/ShowDocument.cpp
    wfg/engine/document/ShowDocument.h
    wfg/engine/command/Command.h
    wfg/engine/command/CommandRegistry.cpp
    wfg/engine/command/CommandRegistry.h
    wfg/engine/command/Event.h
    wfg/engine/command/EventQueue.cpp
    wfg/engine/command/EventQueue.h
    wfg/engine/json/JsonValue.cpp
    wfg/engine/json/JsonValue.h
    wfg/engine/log/EventLog.cpp
    wfg/engine/log/EventLog.h
    wfg/engine/log/Replay.cpp
    wfg/engine/log/Replay.h
    wfg/engine/osc/OscAddress.cpp
    wfg/engine/osc/OscAddress.h
    wfg/engine/osc/OscCodec.cpp
    wfg/engine/osc/OscCodec.h
    wfg/engine/osc/OscValue.cpp
    wfg/engine/osc/OscValue.h
    wfg/engine/osc/UdpEndpoint.cpp
    wfg/engine/osc/UdpEndpoint.h
    wfg/engine/oscquery/EngineNamespace.cpp
    wfg/engine/oscquery/EngineNamespace.h
    wfg/engine/oscquery/OscQueryServer.cpp
    wfg/engine/oscquery/OscQueryServer.h
    wfg/engine/oscquery/Subscriptions.cpp
    wfg/engine/oscquery/Subscriptions.h
    wfg/engine/tree/Mount.cpp
    wfg/engine/tree/Mount.h
    wfg/engine/tree/Node.h
    wfg/engine/tree/OscQueryJson.cpp
    wfg/engine/tree/OscQueryJson.h
    wfg/engine/tree/ParameterTree.cpp
    wfg/engine/tree/ParameterTree.h
    wfg/engine/tree/Touches.cpp
    wfg/engine/tree/Touches.h
    wfg/engine/tree/TreeCommands.cpp
    wfg/engine/tree/TreeCommands.h
    wfg/engine/tree/TreeSnapshot.cpp
    wfg/engine/tree/TreeSnapshot.h)
add_library(wfg::engine ALIAS wfg_engine)

# PUBLIC, and pointed at src/ rather than at src/wfg/ — consumers therefore
# write  #include <wfg/engine/Engine.h>  , a rooted path that cannot be
# ambiguous. Do NOT add src/wfg/ or src/wfg/engine/ as an include directory as a
# convenience: a bare #include <Engine.h> would then resolve against a search
# path that also carries two whole vendor module trees, and the first vendor
# header that ever takes the same name wins silently.
target_include_directories(wfg_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# wfg::thirdparty is PUBLIC on purpose. The engine's own public headers name no
# JUCE type — Engine.h and Console.h are vendor-free, and that is a rule, not an
# accident (see the comment at the top of Engine.h) — but wfg_tests links
# wfg::engine now and the Phase 5 UI and the Phase 9 plugin host will too, and
# every one of them must compile against the SAME JUCE macro environment. A
# PRIVATE link here would leave those consumers to assemble their own — and a
# JUCE/Tracktion header seen under two different sets of JUCE_* defines is an
# ODR violation that links cleanly and misbehaves at run time.
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
# lives in tests/CMakeLists.txt, including the ones that run THIS binary, so
# that the full list of what CI actually executes can be read in one place.
