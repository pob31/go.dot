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
    wfg/engine/audio/AudioSettings.cpp
    wfg/engine/audio/AudioSettings.h
    wfg/engine/audio/OutputTestSignal.h
    wfg/engine/Engine.cpp
    wfg/engine/Engine.h
    wfg/engine/Console.cpp
    wfg/engine/Console.h
    wfg/engine/cue/CueCommands.cpp
    wfg/engine/cue/CueCommands.h
    wfg/engine/cue/CueList.cpp
    wfg/engine/cue/FadeJob.h
    wfg/engine/cue/GroupJob.h
    wfg/engine/cue/FadeJob.cpp
    wfg/engine/cue/OscJob.h
    wfg/engine/cue/OscJob.cpp
    wfg/engine/cue/Run.h
    wfg/engine/cue/Run.cpp
    wfg/engine/cue/ListState.h
    wfg/engine/cue/ListState.cpp
    wfg/engine/cue/ShowWalk.h
    wfg/engine/cue/Solver.h
    wfg/engine/cue/Solver.cpp
    wfg/engine/cue/SlotAnalysis.h
    wfg/engine/cue/SlotAnalysis.cpp
    wfg/engine/cue/SamplerLayout.h
    wfg/engine/cue/SamplerLayout.cpp
    wfg/engine/cue/RunCommands.h
    wfg/engine/cue/RunCommands.cpp
    wfg/engine/cue/Runner.h
    wfg/engine/cue/Runner.cpp
    wfg/engine/cue/CueList.h
    wfg/engine/cue/TriggerIndex.h
    wfg/engine/cue/TriggerIndex.cpp
    wfg/engine/cue/DcaTable.h
    wfg/engine/cue/LiveRows.h
    wfg/engine/cue/LiveRows.cpp
    wfg/engine/cue/LiveEdits.h
    wfg/engine/cue/LiveEdits.cpp
    wfg/engine/cue/FxValues.h
    wfg/engine/cue/FxValues.cpp
    wfg/engine/cue/FxRows.h
    wfg/engine/cue/FxRows.cpp
    wfg/engine/midi/MidiInputs.h
    wfg/engine/midi/MidiInputs.cpp
    wfg/engine/midi/MidiSink.h
    wfg/engine/midi/MidiMessages.h
    wfg/engine/midi/MidiMessages.cpp
    wfg/engine/midi/MidiSender.h
    wfg/engine/midi/PortTable.h
    wfg/engine/midi/MidiSender.cpp
    wfg/engine/midi/PortBinder.h
    wfg/engine/midi/PortBinder.cpp
    wfg/engine/surface/McuCodec.h
    wfg/engine/surface/McuCodec.cpp
    wfg/engine/surface/FaderCurve.h
    wfg/engine/surface/SurfaceProfile.h
    wfg/engine/surface/SurfaceProfile.cpp
    wfg/engine/surface/SurfacePages.h
    wfg/engine/surface/SurfacePages.cpp
    wfg/engine/surface/SurfaceBridge.h
    wfg/engine/surface/SurfaceBridge.cpp
    wfg/engine/surface/SurfaceTable.h
    wfg/engine/surface/SurfaceCommands.h
    wfg/engine/surface/SurfaceCommands.cpp
    wfg/engine/plugin/PluginTable.h
    wfg/engine/plugin/Catalogue.h
    wfg/engine/plugin/Catalogue.cpp
    wfg/engine/plugin/PluginScan.h
    wfg/engine/plugin/PluginScan.cpp
    wfg/engine/plugin/KnownList.h
    wfg/engine/plugin/ScanTable.h
    wfg/engine/plugin/ScanJob.h
    wfg/engine/plugin/ScanJob.cpp
    wfg/engine/plugin/ProcessUtil.h
    wfg/engine/plugin/ChildLaunch.h
    wfg/engine/plugin/ChildLaunch.cpp
    wfg/engine/plugin/SharedRegion.h
    wfg/engine/plugin/ProxyLane.h
    wfg/engine/plugin/ProxyLane.cpp
    wfg/engine/plugin/ProxyHost.h
    wfg/engine/plugin/ProxyHost.cpp
    wfg/engine/plugin/TestGainState.h
    wfg/engine/plugin/PluginLoad.h
    wfg/engine/plugin/PluginLoad.cpp
    wfg/engine/plugin/PluginHostChild.h
    wfg/engine/plugin/PluginHostChild.cpp
    wfg/engine/plugin/EditorRegion.h
    wfg/engine/plugin/EditorHost.h
    wfg/engine/plugin/EditorHost.cpp
    wfg/engine/plugin/PluginEditorChild.h
    wfg/engine/plugin/PluginEditorChild.cpp
    wfg/engine/plugin/PluginCommands.h
    wfg/engine/plugin/PluginCommands.cpp
    wfg/engine/audio/AudioCommands.h
    wfg/engine/audio/AudioCommands.cpp
    wfg/engine/audio/AudioHost.h
    wfg/engine/audio/AudioHost.cpp
    wfg/engine/audio/CueMatrix.h
    wfg/engine/audio/CueMatrix.cpp
    wfg/engine/audio/EqSettings.h
    wfg/engine/audio/EqColours.h
    wfg/engine/audio/EqMath.h
    wfg/engine/audio/CueEq.h
    wfg/engine/audio/CueEq.cpp
    wfg/engine/audio/EqPlugin.h
    wfg/engine/audio/EqPlugin.cpp
    wfg/engine/audio/ProxyPlugin.h
    wfg/engine/audio/ProxyPlugin.cpp
    wfg/engine/audio/DeviceLayer.h
    wfg/engine/audio/DeviceLayer.cpp
    wfg/engine/audio/MediaInfo.h
    wfg/engine/audio/MediaInfo.cpp
    wfg/engine/audio/Timbre.h
    wfg/engine/audio/Timbre.cpp
    wfg/engine/audio/Peaks.h
    wfg/engine/audio/Peaks.cpp
    wfg/engine/audio/MediaAnalyser.h
    wfg/engine/audio/MediaAnalyser.cpp
    wfg/engine/audio/CueOutputPlugin.h
    wfg/engine/audio/CueOutputPlugin.cpp
    wfg/engine/audio/HostPlayer.h
    wfg/engine/audio/HostPlayer.cpp
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
    wfg/engine/document/DocumentSession.h
    wfg/engine/document/DocumentWriter.cpp
    wfg/engine/document/DocumentWriter.h
    wfg/engine/document/EphemeralState.cpp
    wfg/engine/document/EphemeralState.h
    wfg/engine/document/FadePoints.cpp
    wfg/engine/document/FadePoints.h
    wfg/engine/document/Ids.cpp
    wfg/engine/document/Ids.h
    wfg/engine/document/OutputLayout.cpp
    wfg/engine/document/OutputLayout.h
    wfg/engine/document/RelaxNg.cpp
    wfg/engine/document/RelaxNg.h
    wfg/engine/document/Schema.cpp
    wfg/engine/document/Schema.h
    wfg/engine/document/SchemaTable.generated.h
    wfg/engine/document/SchemaTypes.h
    wfg/engine/document/Sequence.cpp
    wfg/engine/document/Sequence.h
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
    wfg/engine/osc/SenderGate.h
    wfg/engine/osc/UdpEndpoint.cpp
    wfg/engine/osc/UdpEndpoint.h
    wfg/engine/oscquery/EngineNamespace.cpp
    wfg/engine/oscquery/EngineNamespace.h
    wfg/engine/oscquery/OscQueryClient.h
    wfg/engine/oscquery/OscQueryClient.cpp
    wfg/engine/oscquery/OscQueryServer.cpp
    wfg/engine/oscquery/OscQueryServer.h
    wfg/engine/oscquery/Subscriptions.cpp
    wfg/engine/oscquery/Subscriptions.h
    wfg/engine/oscquery/TimbreRoute.cpp
    wfg/engine/oscquery/TimbreRoute.h
    wfg/engine/tree/Mount.cpp
    wfg/engine/tree/MountProbe.h
    wfg/engine/tree/MountProbe.cpp
    wfg/engine/tree/MountSender.h
    wfg/engine/tree/MountSender.cpp
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
# The compiled client, in two halves (namespace draft §14.16).
#
# wfg_client_model names no JUCE type at all: what a label shows, the theme's
# tokens and the Event each gesture submits, all std-only so that wfg_tests can
# assert them with no window. wfg_client_ui is the JUCE half - the window, the
# look, the components - built by every CI job so the strict preset reads it,
# executed by none of them, because the only test of a window is a person.
#
# Neither is linked by wfg_engine. The engine is handed the window as a factory
# from main() (Console.h), so the dependency arrow points one way: ui -> model
# -> engine. The boundary gate (scripts/check-client-boundary.py, ctest
# client.boundary) reads the client's source and holds the split.
# -----------------------------------------------------------------------------
add_library(wfg_client_model STATIC
    wfg/client/model/Gestures.h
    wfg/client/model/Gestures.cpp
    wfg/client/model/Media.h
    wfg/client/model/Media.cpp
    wfg/client/model/NewCue.h
    wfg/client/model/NewCue.cpp
    wfg/client/model/Foot.h
    wfg/client/model/Foot.cpp
    wfg/client/model/OutputList.h
    wfg/client/model/OutputList.cpp
    wfg/client/model/Ranges.h
    wfg/client/model/Ranges.cpp
    wfg/client/model/Curve.h
    wfg/client/model/Curve.cpp
    wfg/client/model/DirectOuts.h
    wfg/client/model/MidiPorts.cpp
    wfg/client/model/MidiPorts.h
    wfg/client/model/Surfaces.cpp
    wfg/client/model/Surfaces.h
    wfg/client/model/Devices.cpp
    wfg/client/model/Devices.h
    wfg/client/model/DirectOuts.cpp
    wfg/client/model/Fader.h
    wfg/client/model/Fader.cpp
    wfg/client/model/Sends.h
    wfg/client/model/Sends.cpp
    wfg/client/model/Eq.h
    wfg/client/model/Eq.cpp
    wfg/client/model/Fx.h
    wfg/client/model/Fx.cpp
    wfg/client/model/FxEditor.h
    wfg/client/model/FxEditor.cpp
    wfg/client/model/Timeline.h
    wfg/client/model/Timeline.cpp
    wfg/client/model/View.h
    wfg/client/model/Panic.h
    wfg/client/model/Reorder.h
    wfg/client/model/Reorder.cpp
    wfg/client/model/Selection.h
    wfg/client/model/Selection.cpp
    wfg/client/model/Inspector.h
    wfg/client/model/Inspector.cpp
    wfg/client/model/LoadToTime.h
    wfg/client/model/LoadToTime.cpp
    wfg/client/model/UndoHistory.h
    wfg/client/model/UndoHistory.cpp
    wfg/client/model/RunModel.h
    wfg/client/model/RunModel.cpp
    wfg/client/model/Scrub.h
    wfg/client/model/Scrub.cpp
    wfg/client/model/ShowModel.h
    wfg/client/model/ShowModel.cpp
    wfg/client/model/Text.h
    wfg/client/model/Text.cpp
    wfg/client/model/Theme.h
    wfg/client/model/Theme.cpp
    wfg/client/model/Transport.h
    wfg/client/model/Transport.cpp
    wfg/client/model/Waveform.h
    wfg/client/model/Waveform.cpp)
add_library(wfg::client_model ALIAS wfg_client_model)

target_link_libraries(wfg_client_model
    PUBLIC  wfg::engine
    PRIVATE wfg::warnings)

add_library(wfg_client_ui STATIC
    wfg/client/ui/ShowSettingsWindow.h
    wfg/client/ui/ShowSettingsWindow.cpp
    wfg/client/ui/SurfacePanelComponent.h
    wfg/client/ui/SurfacePanelComponent.cpp
    wfg/client/Client.h
    wfg/client/ui/Client.cpp
    wfg/client/ui/FootPanelComponent.cpp
    wfg/client/ui/FootPanelComponent.h
    wfg/client/ui/WaveformEditorComponent.cpp
    wfg/client/ui/WaveformEditorComponent.h
    wfg/client/ui/Look.h
    wfg/client/ui/Look.cpp
    wfg/client/ui/CueListComponent.h
    wfg/client/ui/CueListComponent.cpp
    wfg/client/ui/MainWindow.h
    wfg/client/ui/MainWindow.cpp
    wfg/client/ui/InspectorComponent.h
    wfg/client/ui/InspectorComponent.cpp
    wfg/client/ui/HistoryPanelComponent.h
    wfg/client/ui/HistoryPanelComponent.cpp
    wfg/client/ui/UndoPanelComponent.h
    wfg/client/ui/UndoPanelComponent.cpp
    wfg/client/ui/NewCueBarComponent.h
    wfg/client/ui/NewCueBarComponent.cpp
    wfg/client/ui/LiveBarComponent.h
    wfg/client/ui/LiveBarComponent.cpp
    wfg/client/ui/RangeTableComponent.h
    wfg/client/ui/RangeTableComponent.cpp
    wfg/client/ui/RunPaneComponent.h
    wfg/client/ui/RunPaneComponent.cpp
    wfg/client/ui/CurveEditorComponent.h
    wfg/client/ui/CurveEditorComponent.cpp
    wfg/client/ui/SendMixerComponent.h
    wfg/client/ui/SendMixerComponent.cpp
    wfg/client/ui/EqPanelComponent.h
    wfg/client/ui/EqPanelComponent.cpp
    wfg/client/ui/FxPanelComponent.h
    wfg/client/ui/FxPanelComponent.cpp
    wfg/client/ui/PluginEditors.h
    wfg/client/ui/PluginEditors.cpp
    wfg/client/ui/TimelineComponent.h
    wfg/client/ui/TimelineComponent.cpp
    wfg/client/ui/Shell.h
    wfg/client/ui/Shell.cpp
    wfg/client/ui/TransportComponent.h
    wfg/client/ui/TransportComponent.cpp)
add_library(wfg::client_ui ALIAS wfg_client_ui)

target_link_libraries(wfg_client_ui
    PUBLIC  wfg::client_model
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

# wfg::engine carries wfg::thirdparty PUBLIC; wfg::client_ui is the window
# main() hands the console. wfg::warnings is named again because it is PRIVATE
# on every library and so does not propagate; that is deliberate, not an
# oversight.
target_link_libraries(wfg PRIVATE wfg::engine wfg::client_ui wfg::warnings)

# NOTE: there is no add_test() in this file. Every add_test() in the project
# lives in tests/CMakeLists.txt, including the ones that run THIS binary, so
# that the full list of what CI actually executes can be read in one place.
