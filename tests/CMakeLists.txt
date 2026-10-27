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
# tests/ — one test binary, and EVERY add_test() in the project.
#
# THE TEST FRAMEWORK is doctest 2.4.11, the copy Tracktion Engine already
# vendors at modules/3rd_party/doctest/. It costs us nothing: no fourth
# submodule, no FetchContent, no CMake of its own, and it is MIT-licensed. It
# is reachable as  #include <3rd_party/doctest/tracktion_doctest.hpp>  purely
# because wfg::deps puts ThirdParty/tracktion_engine/modules on the include
# path — there is no wiring to add here, and none should be added.
#
# Always the tracktion_doctest.hpp WRAPPER, never doctest.h directly: the
# wrapper brackets the include in choc's warning-suppression pragmas (which is
# what keeps doctest compiling under juce_recommended_warning_flags and under
# -Werror in the `strict` preset) and #undefs DOCTEST_CONFIG_IMPLEMENT on the
# way out, so the three TUs below can include it without fighting each other.
#
# -----------------------------------------------------------------------------
# WHY ctest, AND NOT WFS-DIY'S "BUILD THE BINARY AND RUN IT" STEP
#
# This is a deliberate divergence from the author's existing CI shape, flagged
# here so it can be overruled rather than inherited by accident. Two reasons:
#
#  1. The devplan's cross-cutting locale obligation (devplan:337 — "every
#     serialisation test runs under fr_FR as well as C") means the SAME binary
#     must be run twice with different arguments, on three platforms. As a
#     shell step that is six lines of per-platform shell in ci.yml; as two
#     add_test() calls it is two lines here, and `ctest --output-on-failure`
#     reports which of the two failed without anyone parsing a log.
#
#  2. CTest resolves a bare target name in COMMAND to $<TARGET_FILE:...> for
#     us. A hand-written build path works on the single-config Ninja jobs and
#     then fails on the multi-config Windows job, where the binary is under
#     Debug/ — a class of breakage that only ever shows up on one platform.
#
# All four tests are registered HERE, including the two that run the PRODUCT
# binary rather than the test binary, so the complete list of what CI executes
# can be read in one file. src/CMakeLists.txt deliberately contains no
# add_test(). enable_testing() is called once, by the root CMakeLists.txt.
# =============================================================================

# -----------------------------------------------------------------------------
# wfg_tests — the doctest runner.
#
# Links wfg::engine (not wfg::thirdparty directly) so that the tests compile
# against exactly the environment our own library compiles against. If those
# two ever diverge, these tests stop testing the thing we ship.
# -----------------------------------------------------------------------------
add_executable(wfg_tests
    TestMain.cpp
    TestSupport.h
    OutputTestSignalTests.cpp
    ImportRoutingTests.cpp
    OutputLayoutTests.cpp
    ToolchainTests.cpp
    LocaleTests.cpp
    OscValueTests.cpp
    OpenShowsTests.cpp
    FolderIconTests.cpp
    MachinePowerTests.cpp
    AssociateTests.cpp
    TemplateTests.cpp
    AutoNameTests.cpp
    CommandTests.cpp
    EventLogTests.cpp
    EngineTests.cpp
    DocumentTests.cpp
    SequenceTests.cpp
    BundleTests.cpp
    DocumentWriterTests.cpp
    ClockTests.cpp
    DeviceTests.cpp
    TreeTests.cpp
    PublishCostTests.cpp
    AudioTests.cpp
    AuxTests.cpp
    AuxRunTests.cpp
    CueMatrixTests.cpp
    CueEqTests.cpp
    LooperTests.cpp
    MountTests.cpp
    PresetTests.cpp
    DiscoveryTests.cpp
    MdnsTests.cpp
    NetworkCueTests.cpp
    AuthoringTests.cpp
    ProcessTests.cpp
    ScriptTests.cpp
    TimecodeTests.cpp
    LtcReader.h
    TimecodeRunTests.cpp
    TimeGroupTests.cpp
    JoinTests.cpp
    ChaseTests.cpp
    CodeMapTests.cpp
    CodeWatchTests.cpp
    ClockWatchTests.cpp
    SerialTests.cpp
    MqttTests.cpp
    WireTests.cpp
    TrafficTapTests.cpp
    VerifiedCueTests.cpp
    JsonTests.cpp
    CueListTests.cpp
    MediaCueTests.cpp
    RunTests.cpp
    GoTests.cpp
    TriggerTests.cpp
    BindingTests.cpp
    RangeTests.cpp
    LevelLaneTests.cpp
    SendLaneTests.cpp
    SectionTests.cpp
    MediaEditTests.cpp
    EditPlaybackTests.cpp
    MediaEditRenderTests.cpp
    CuePartTests.cpp
    BeatClockTests.cpp
    AlsReaderTests.cpp
    AlsWalkTests.cpp
    AlsImportTests.cpp
    BplistTests.cpp
    PlistWriter.h
    QlabFixture.h
    QlabReaderTests.cpp
    QlabWalkTests.cpp
    QlabImportTests.cpp
    LaneRecordTests.cpp
    SpaceMouseTests.cpp
    StreamDeckTests.cpp
    DeckBridgeTests.cpp
    RateClockTests.cpp
    AnalysisTests.cpp
    SolverTests.cpp
    SlotTests.cpp
    MidiTests.cpp
    SurfaceTests.cpp
    PluginSetTests.cpp
    MicTests.cpp
    VideoTests.cpp
    VideoHostTests.cpp
    VideoTextTests.cpp
    FontFileTests.cpp
    ShowFontsTests.cpp
    VideoGpuTests.cpp
    VideoNdiTests.cpp
    DeckLinkTests.cpp
    MovieTests.cpp
    MovieEditRenderTests.cpp
    StripTests.cpp
    TakeTests.cpp
    PluginCatalogueTests.cpp
    PluginScanTests.cpp
    ProxyTests.cpp
    EditorHelperTests.cpp
    FxRowsTests.cpp
    McuCodecTests.cpp
    DcaTests.cpp
    SamplerTests.cpp
    SurfaceBridgeTests.cpp
    SurfacePagesTests.cpp
    OscCodecTests.cpp
    SimpleWebToolchainTests.cpp
    SpatcoreToolchainTests.cpp
    UndoTests.cpp
    MachineScopeTests.cpp
    PairSettingsTests.cpp
    PeerLinkTests.cpp
    FollowTests.cpp
    SeatTests.cpp
    LeadershipTests.cpp
    SameShowTests.cpp
    SameSampleTests.cpp
    InputCaptureTests.cpp
    TimbreTests.cpp
    OscQueryTests.cpp
    ProblemsTests.cpp
    ClientTests.cpp)

# Where the hand-authored fixtures live. Passed as a definition rather than
# copied next to the binary: the fixtures are inputs to read, not artefacts to
# ship, and a copy step is one more thing that can be stale.
target_compile_definitions(wfg_tests PRIVATE
    WFG_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    # The repository root, for the one committed artefact that is not a fixture:
    # docs/schema/show.rng is published, so it lives where a reader of the docs
    # will look for it rather than where the tests keep their input.
    WFG_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# wfg::client_model and not wfg::client_ui: the model half is what a test can
# hold (ClientTests.cpp); the window is tested by opening it.
target_link_libraries(wfg_tests PRIVATE wfg::engine wfg::client_model wfg::warnings)

# -----------------------------------------------------------------------------
# A REAL LV2 PLUGIN, FOR THE TESTS ONLY (2026-09-26). A CI runner has no plugin
# of its own, so without this every hosting test there goes through the built-in
# `godot:test-gain`, which bypasses JUCE's formats altogether. This bundle is
# two plugins in plain C against the LV2 core header JUCE vendors: a stereo gain
# and a mono-to-stereo widener (tests/fixtures/lv2/godot_test_lv2.c says what
# each does). It is built into a bundle folder beside the tests, never installed
# and never on an LV2 path; the scan finds it through --path, and the unit case
# through WFG_TEST_LV2_BUNDLE. The manifest names the binary, whose suffix is
# the platform's, so it is configured rather than copied.
# -----------------------------------------------------------------------------
set(GODOT_TEST_LV2_FOLDER "${CMAKE_CURRENT_BINARY_DIR}/lv2")
set(GODOT_TEST_LV2_BUNDLE "${GODOT_TEST_LV2_FOLDER}/godot-test.lv2")
set(GODOT_TEST_LV2_BINARY "godot_test_lv2${CMAKE_SHARED_MODULE_SUFFIX}")

add_library(godot_test_lv2 MODULE fixtures/lv2/godot_test_lv2.c)
target_include_directories(godot_test_lv2 PRIVATE
    "${CMAKE_SOURCE_DIR}/ThirdParty/JUCE/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2")

# `$<1:...>` keeps a multi-config generator from adding its Debug/Release
# folder: the binary must sit inside the bundle, beside its manifest.
set_target_properties(godot_test_lv2 PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "$<1:${GODOT_TEST_LV2_BUNDLE}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${GODOT_TEST_LV2_BUNDLE}>")

configure_file(fixtures/lv2/manifest.ttl.in "${GODOT_TEST_LV2_BUNDLE}/manifest.ttl" @ONLY)
configure_file(fixtures/lv2/plugin.ttl "${GODOT_TEST_LV2_BUNDLE}/plugin.ttl" COPYONLY)

add_dependencies(wfg_tests godot_test_lv2)
target_compile_definitions(wfg_tests PRIVATE WFG_TEST_LV2_BUNDLE="${GODOT_TEST_LV2_BUNDLE}")

# Spout is Windows's (namespace draft §44, N.1): its loopback test is built there alone.
if(WIN32)
    target_sources(wfg_tests PRIVATE VideoSpoutTests.cpp)
endif()

# These native-window checks need a Windows desktop and exercise the actual
# settings panel with a live device; keep the ordinary unit runner headless.
if(WIN32)
    add_executable(wfg_audio_ui_tests TestMain.cpp ShowSettingsUiTests.cpp RunPaneUiTests.cpp MediaCopierUiTests.cpp
                                      UpdateCheckUiTests.cpp)
    target_link_libraries(wfg_audio_ui_tests PRIVATE wfg::engine wfg::client_ui wfg::warnings)
    # The repository root, for the one window test that reads the shipped
    # presets (the Network tab's chooser, namespace draft 57, DP.4): the same
    # definition wfg_tests carries, and the one the CI's whole build missed.
    target_compile_definitions(wfg_audio_ui_tests PRIVATE WFG_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    #
    # SIDE BY SIDE WITH THE REST, NOT ALONE (2026-10-13). These were RUN_SERIAL
    # from the day the binary was one panel on a live device: nothing else ran
    # while they did, which was free while ctest ran one entry at a time and is
    # a minute and a half of an idle machine once it runs twelve. What they
    # share with other tests is two things, and each is now a lock: the
    # desktop, where they make real windows (`desktop`, so the two locales and
    # ui.cue-errors take turns), and the machine's audio interface, which ONE
    # case opens live - its own entry, ui.live-device.<locale>, holding the
    # `audio-device` lock that unit.<locale>.devices and blackbox.device.C hold
    # (the unit shards' comment says why). Its name is excluded from the
    # whole-binary entries, so it still runs once a locale, and its entry
    # passes only on doctest's own count of one case run and passed: a rename
    # would otherwise leave it green over nothing. The other cases hand their
    # key presses and mouse events to the components directly, so nothing in
    # them needs the keyboard focus or the pointer another program could take.
    set(WFG_UI_LIVE_DEVICE "audio settings UI: opening and rescanning cannot stop the ASIO callback")
    add_test(NAME ui.show-settings.C COMMAND wfg_audio_ui_tests --wfg-locale=C "--test-case-exclude=${WFG_UI_LIVE_DEVICE}")
    add_test(NAME ui.show-settings.fr_FR COMMAND wfg_audio_ui_tests --wfg-locale=${WFG_LOCALE_FR} "--test-case-exclude=${WFG_UI_LIVE_DEVICE}")
    set_tests_properties(ui.show-settings.C ui.show-settings.fr_FR PROPERTIES
                         RESOURCE_LOCK desktop TIMEOUT 120 COST 70)
    add_test(NAME ui.live-device.C COMMAND wfg_audio_ui_tests --wfg-locale=C "--test-case=${WFG_UI_LIVE_DEVICE}")
    add_test(NAME ui.live-device.fr_FR COMMAND wfg_audio_ui_tests --wfg-locale=${WFG_LOCALE_FR} "--test-case=${WFG_UI_LIVE_DEVICE}")
    set_tests_properties(ui.live-device.C ui.live-device.fr_FR PROPERTIES
                         RESOURCE_LOCK "desktop;audio-device" TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "test cases: +1 [|] +1 passed [|] +0 failed")
    add_test(NAME ui.cue-errors.C COMMAND wfg_audio_ui_tests "--test-case=active cue errors:*" --wfg-locale=C)
    add_test(NAME ui.cue-errors.fr_FR COMMAND wfg_audio_ui_tests "--test-case=active cue errors:*" --wfg-locale=${WFG_LOCALE_FR})
    set_tests_properties(ui.cue-errors.C ui.cue-errors.fr_FR PROPERTIES RESOURCE_LOCK desktop TIMEOUT 30)
endif()

# -----------------------------------------------------------------------------
# The unit suite, run once per locale, in shards.
#
# The locale NAME is never spelled in a test source. The Windows UCRT does not
# accept the glibc spelling "fr_FR.UTF-8" and glibc does not accept the Windows
# spelling "fr-FR", so WFG_LOCALE_FR (cmake/WfgOptions.cmake) is the single
# place that difference exists, and it reaches the binary two ways: as this
# argument, and as a compile definition the test compares against.
#
# TestMain.cpp exits 2 WITHOUT running any test if setlocale() rejects the
# name. A skipped locale test that reports green is precisely the premiere-night
# failure PRD §3.20 exists to prevent, so "the locale was not installed" must
# be a red build and not a quiet pass. On the Linux runner that means
# `locale-gen fr_FR.UTF-8` is a hard prerequisite, and ci.yml runs it.
#
# --duration=true NAMES EVERY CASE AS IT ENDS, with its time. A case that fails
# names itself; one that CRASHES the process does not, and the macOS runner's
# unit suite died twice with a bus error (runs 38227264401 at 493 s in C,
# 38299523050 at 361 s in fr_FR) with nothing to say which case was running.
# The last name printed is the case before the one that died. ctest shows the
# output only when a suite fails, so a green run's log is unchanged. Every
# shard below keeps it.
#
# SHARDS, AND WHY (2026-10-13). The author: "The tests are now over an hour
# long slowing down the development to a grind". The suite in one process was
# ten minutes a locale on the Windows box, twelve on the Windows runner, nine on
# macOS - half of every run, and one core of the machine busy while the others
# waited for it. So the binary is run as several entries a locale, unit.C.<name>
# and unit.fr_FR.<name>, each given a slice of the cases by doctest's own
# filters, and ctest runs them side by side with everything else (the test
# presets' `jobs`, CMakePresets.json). The same cases, the same assertions, the
# same two locales: only the process each runs in has changed.
#
# A SHARD IS <name>=<files>, the files comma-separated, matched against the
# path each case was compiled from as `*tests?<file>` - anchored on the folder,
# because `*RunTests.cpp` would also catch TimecodeRunTests.cpp. One file may
# be cut by its cases' order, `<file>@<first>-<last>` (the last left open for
# the rest of the file): doctest numbers the cases that pass the other filters,
# and a file's cases run in the order of their lines. AudioTests.cpp is cut so,
# into four, because it was 257 of the 616 seconds alone - and one of its cases,
# the 64-track graph whose every node needs an identity, is a minute on its own,
# which is the floor of how short a shard can be. EVERYTHING NOT NAMED runs in
# unit.<locale>.rest: a new test file runs there from its first build, and is
# moved into a shard when its time says it should be.
#
# BALANCED BY WHAT EACH FILE TOOK, measured with `wfg_tests --duration=true`
# under C on the Windows box in Debug (616 s, 2585 cases) and checked against
# the Linux runner's figures (277 s): each shard is near a minute here, from
# 53 to 83 s, which is about the longest single case. To balance them again,
# run the suite that way, add up each file's lines, and move files between the
# lists; adding cases to a file only makes its shard longer, never wrong.
#
# NOTHING CAN FALL BETWEEN THEM: unit.shards (after the entries) asks the binary
# which cases each shard's own arguments select, and fails if a case is in no
# shard or in two, or if a file a shard names holds nothing it runs - a rename,
# a typo, or a cut whose numbers no longer meet. It is a ctest entry rather than
# a configure check because the binary is the only thing that knows its cases.
#
# devices holds the one file that opens the machine's real audio interfaces:
# its two locales, blackbox.device.C and the window suites take turns on the
# `audio-device` lock rather than open one card from two processes at once,
# which a device in exclusive mode refuses and the case would read as a fault.
# Every shard is given a COST so a fresh build tree - every CI run - starts the
# shards early, after only the few drivers that are longer still (the end of
# this file): started last, they would be the tail everything else waits
# behind.
# -----------------------------------------------------------------------------
set(WFG_UNIT_SHARDS
    "audio-1=AudioTests.cpp@1-20"
    "audio-2=AudioTests.cpp@21-27"
    "audio-3=AudioTests.cpp@28-78"
    "audio-4=AudioTests.cpp@79-"
    "devices=DeviceTests.cpp"
    "presets=PresetTests.cpp,DiscoveryTests.cpp,MdnsTests.cpp,AnalysisTests.cpp,SeatTests.cpp,OscQueryTests.cpp,TimbreTests.cpp"
    "proxy=ProxyTests.cpp,GoTests.cpp,SurfaceBridgeTests.cpp,BundleTests.cpp,SerialTests.cpp"
    "video-host=VideoHostTests.cpp,AuthoringTests.cpp,MountTests.cpp,MediaEditRenderTests.cpp,PeerLinkTests.cpp"
    "editor-helper=EditorHelperTests.cpp,ClientTests.cpp,VerifiedCueTests.cpp,TimecodeTests.cpp,MidiTests.cpp")

# What every shard is run with, and the check with it: the GPU cases have their
# own entry below.
set(WFG_UNIT_EVERY "--test-case-exclude=video gpu:*")

set(WFG_UNIT_SHARD_NAMES "")
set(WFG_UNIT_NAMED_FILES "")

foreach(shard IN LISTS WFG_UNIT_SHARDS)
    if(NOT shard MATCHES "^([a-z0-9-]+)=(.+)$")
        message(FATAL_ERROR "tests/CMakeLists.txt: a unit shard is <name>=<files>, not \"${shard}\"")
    endif()

    set(shard_name "${CMAKE_MATCH_1}")
    set(shard_files "${CMAKE_MATCH_2}")
    set(shard_range "")

    if(shard_files MATCHES "^([^@,]+)@([0-9]+)-([0-9]*)$")
        set(shard_files "${CMAKE_MATCH_1}")
        set(shard_range "--first=${CMAKE_MATCH_2}")

        if(NOT "${CMAKE_MATCH_3}" STREQUAL "")
            list(APPEND shard_range "--last=${CMAKE_MATCH_3}")
        endif()
    endif()

    string(REPLACE "," ";" shard_files "${shard_files}")
    set(shard_patterns "")

    foreach(file IN LISTS shard_files)
        list(APPEND shard_patterns "*tests?${file}")
    endforeach()

    list(APPEND WFG_UNIT_NAMED_FILES ${shard_patterns})
    list(JOIN shard_patterns "," shard_filter)
    set(WFG_UNIT_SHARD_ARGS_${shard_name} "--source-file=${shard_filter}" ${shard_range})
    list(APPEND WFG_UNIT_SHARD_NAMES ${shard_name})
endforeach()

list(REMOVE_DUPLICATES WFG_UNIT_NAMED_FILES)
list(JOIN WFG_UNIT_NAMED_FILES "," WFG_UNIT_REST_FILTER)
set(WFG_UNIT_SHARD_ARGS_rest "--source-file-exclude=${WFG_UNIT_REST_FILTER}")
list(APPEND WFG_UNIT_SHARD_NAMES rest)

set(WFG_UNIT_SHARD_CHECK "")

foreach(shard_name IN LISTS WFG_UNIT_SHARD_NAMES)
    foreach(locale IN ITEMS C fr_FR)
        if(locale STREQUAL "C")
            set(spelling C)
        else()
            set(spelling ${WFG_LOCALE_FR})
        endif()

        add_test(NAME unit.${locale}.${shard_name}
                 COMMAND wfg_tests --wfg-locale=${spelling} ${WFG_UNIT_SHARD_ARGS_${shard_name}}
                         ${WFG_UNIT_EVERY} --duration=true)
        set_tests_properties(unit.${locale}.${shard_name} PROPERTIES LABELS unit COST 60)
    endforeach()

    list(JOIN WFG_UNIT_SHARD_ARGS_${shard_name} "|" shard_joined)
    list(APPEND WFG_UNIT_SHARD_CHECK "--shard=${shard_name}|${shard_joined}")
endforeach()

set_tests_properties(unit.C.devices unit.fr_FR.devices PROPERTIES RESOURCE_LOCK audio-device)

add_test(NAME unit.shards
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/check-unit-shards.py"
                 $<TARGET_FILE:wfg_tests> "--common=${WFG_UNIT_EVERY}" ${WFG_UNIT_SHARD_CHECK})
set_tests_properties(unit.shards PROPERTIES LABELS unit TIMEOUT 300)

# THE VIDEO RENDERER ON THE GRAPHICS DEVICE (namespace draft §44), in a process
# of its own: the system's software rasteriser where there is no card - WARP,
# llvmpipe - starts from a clean slate rather than after two thousand cases,
# and a fault there is named as the device's, not as the whole suite's.
add_test(NAME video.gpu.C COMMAND wfg_tests --wfg-locale=C "--test-case=video gpu:*")

# -----------------------------------------------------------------------------
# THE REAL-TIME SANITIZER'S CONTROL (2026-09-30), built only where it means
# something: the rtsan preset, whose flags carry -fsanitize=realtime. It walks
# into PRD §4.2 on purpose through WFG_AUDIO_THREAD, and rtsan_control.py
# passes only if the sanitizer reported it - so the rtsan job's green says the
# sanitizer looked rather than that nobody could hear it (RtsanControl.cpp
# tells how the job went weeks unable to go red). The planted report stays
# inside the wrapper: the job's gate fails on the sanitizer's name anywhere in
# the tests' output, and this one is the only report that is meant.
# -----------------------------------------------------------------------------
if(CMAKE_CXX_FLAGS MATCHES "fsanitize=realtime")
    add_executable(wfg_rtsan_control RtsanControl.cpp)
    target_include_directories(wfg_rtsan_control PRIVATE "${CMAKE_SOURCE_DIR}/src")
    add_test(NAME rtsan.control
             COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/rtsan_control.py"
                     "$<TARGET_FILE:wfg_rtsan_control>")
endif()

# -----------------------------------------------------------------------------
# The product binary boots.
#
# This is the whole of what Phase 0 claims: the thing we ship starts, links
# Tracktion Engine's compiled code for real, stands up a JUCE message thread on
# a headless runner, and exits 0. Compiling and linking do not prove any of it —
# a static library whose symbols nobody references links perfectly and runs
# nothing.
# -----------------------------------------------------------------------------
add_test(NAME wfg.version  COMMAND wfg --version)
add_test(NAME wfg.selftest COMMAND wfg selftest)

# -----------------------------------------------------------------------------
# The CLI's own serialisation surfaces, run once per locale.
#
# The unit suite above already runs twice, but these exercise the PRODUCT
# binary's verbs rather than the library, and `wfg replay` is the one that
# decides whether Phase 1 met its "the event log replays the session
# bit-for-bit" criterion. A verb that writes or reads anything a human or a
# diff will see takes --wfg-locale for exactly this reason.
#
# `wfg commands` is registered too, and it is not a formality: PRD section 4.11
# says every gesture-reachable action exists as a named command, and a list
# nobody ever runs is a promise nobody ever checks.
# -----------------------------------------------------------------------------
# `wfg midi` is registered for the reason `wfg devices` is: it exits 0 with
# nothing to say on a machine with no MIDI interface, which is every CI runner,
# and that is what makes the verb testable at all. A port list is a fact about
# the machine; an empty one is a fact too.
add_test(NAME wfg.midi COMMAND wfg midi)

# `wfg plugins` reads the machine's known list from Go.dot's own known.xml
# (2026-09-26) - here a committed one, through --engine-folder, so the run
# never touches the developer's real list. The AU in it must read as the show's
# word, AU, and not JUCE's AudioUnit.
add_test(NAME wfg.plugins.known
         COMMAND wfg plugins "--engine-folder=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/engine")
set_tests_properties(wfg.plugins.known PROPERTIES
                     PASS_REGULAR_EXPRESSION "Fixture Delay [|] AU [|] Go[.]dot tests")

# A real scan, out of process as always, of the in-tree LV2 bundle (built with
# the tests, above) through --path, into an engine folder of the build's own:
# both plugins of the bundle must come back as LV2, the second one proving the
# bundle's path was not scanned twice into one entry.
add_test(NAME wfg.plugins.scan-lv2
         COMMAND wfg plugins --scan=lv2 "--path=${GODOT_TEST_LV2_FOLDER}"
                 "--engine-folder=${CMAKE_CURRENT_BINARY_DIR}/lv2-scan-engine")
set_tests_properties(wfg.plugins.scan-lv2 PROPERTIES
                     TIMEOUT 120
                     PASS_REGULAR_EXPRESSION "Go[.]dot test LV2 gain [|] LV2 [|] Go[.]dot tests.*Go[.]dot test LV2 widen [|] LV2 [|] Go[.]dot tests")

if(APPLE)
    # And a real AU scan on the macOS runner (2026-09-26): Apple's AUBandpass,
    # which every Mac has, scanned alone by its identifier - an AU has no folder,
    # the system lists its components.
    add_test(NAME wfg.plugins.scan-au
             COMMAND wfg plugins --scan=au "--retry=AudioUnit:Effects/aufx,bpas,appl"
                     "--engine-folder=${CMAKE_CURRENT_BINARY_DIR}/au-scan-engine")
    set_tests_properties(wfg.plugins.scan-au PROPERTIES
                         TIMEOUT 120
                         PASS_REGULAR_EXPRESSION "AUBandpass [|] AU [|] Apple")
endif()

add_test(NAME wfg.commands.C     COMMAND wfg commands --wfg-locale=C)
add_test(NAME wfg.commands.fr_FR COMMAND wfg commands --wfg-locale=${WFG_LOCALE_FR})

# The replay fixture: a hand-written, hand-checked log that the engine must
# reproduce record for record. It is committed rather than generated, and that
# is the point — a golden produced by the code under test only proves the code
# agrees with itself. Its three exit codes are distinct on purpose: 0 matched,
# 1 the engine diverged, 2 the log could not be read at all.
add_test(NAME wfg.replay.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/skeleton.wfglog"
                 --wfg-locale=C)
add_test(NAME wfg.replay.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/skeleton.wfglog"
                 --wfg-locale=${WFG_LOCALE_FR})

# The second replay fixture: the audio side reporting the shape of the graph it
# built. What it proves is that a transition observed on a machine with an audio
# interface reproduces on one without — the whole reason the report is a logged
# command rather than a variable somebody set.
add_test(NAME wfg.replay.audio.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/edit-built.wfglog"
                 --wfg-locale=C)
add_test(NAME wfg.replay.audio.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/edit-built.wfglog"
                 --wfg-locale=${WFG_LOCALE_FR})

# The third replay fixture, and the phase's own guarantee: a cue fired, played
# and finished. It needs the bundle because GO reads standby out of the show; it
# needs no media, because a replay has no audio side to arm and never looks for
# the file. That is the whole point — a performance recorded on a machine with an
# interface reproduces, record for record, on one without.
add_test(NAME wfg.replay.go.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/first-go.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/first-sound
                 --wfg-locale=C)
add_test(NAME wfg.replay.go.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/first-go.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/first-sound
                 --wfg-locale=${WFG_LOCALE_FR})

# The fourth replay fixture, and the one that says what a log of a FADE is. A
# fade produces fifty values a second and not one of them is written down
# (section 3.15: transitions are events, continuous readouts are not), so what
# has to reproduce is the GO that started it and the tick its run finished on -
# everything between the two is recomputed from this log and the bundle beside
# it. Which is also why it needs no audio side at all.
#
# It earned its place before it was committed: it failed on a takeover reporting
# from inside a command handler, which a replay re-runs, so the record arrived
# twice. Nothing else in the suite would have found that.
add_test(NAME wfg.replay.fade.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fade-stop.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fade-stop
                 --wfg-locale=C)
add_test(NAME wfg.replay.fade.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fade-stop.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fade-stop
                 --wfg-locale=${WFG_LOCALE_FR})

# A fade somebody DREW (PR 5.16a, namespace section 14.6): one read from the
# file, one drawn during the session by a single node.set carrying the whole
# list, and two drawings refused - `bad-value` for numbers that are not a curve,
# `type-mismatch` for a word where a number goes. The levels a drawn fade passes
# through are the unit suite's to check, for fade-stop's reason; what this pins
# is the write door, which before 5.16a refused every list. Run against the old
# door it fails on the first node.set, which was tried.
add_test(NAME wfg.replay.fade-curve.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fade-curve.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fade-curve
                 --wfg-locale=C)
add_test(NAME wfg.replay.fade-curve.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fade-curve.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fade-curve
                 --wfg-locale=${WFG_LOCALE_FR})

# The fifth replay fixture, and the first that is not about Go.dot's own state
# at all: every record in it is about a lighting desk that is not here. A replay
# re-applies all of them against a mount table built from the bundle, reaches
# the same tree, produces the same records - and puts NOTHING on any wire.
#
# That last part is the point rather than a limitation. `wfg replay` is what
# somebody runs at three in the morning to find out why a cue misfired, on a
# laptop that may still be plugged into the show network. A replay that also
# sent would move the rig while they were reading about it.
add_test(NAME wfg.replay.osc.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/network-cue.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/network
                 --wfg-locale=C)
add_test(NAME wfg.replay.osc.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/network-cue.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/network
                 --wfg-locale=${WFG_LOCALE_FR})

# The sixth replay fixture: a closed loop, reproduced with nothing on the other
# end of it. A verified cue asks a device what a value is over HTTP, on a thread
# of its own, and none of that can be replayed - but the ANSWER can, because
# section 3.15 makes a read-back an event. The record is re-injected and the cue
# reaches the same verdict on the same tick with no network in the room.
#
# It is the case that would have made a verified cue the one thing in the engine
# that could not be replayed, if the probe had handed its answer straight to the
# Runner instead of submitting it.
add_test(NAME wfg.replay.verified.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/verified-chain.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/network
                 --wfg-locale=C)
add_test(NAME wfg.replay.verified.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/verified-chain.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/network
                 --wfg-locale=${WFG_LOCALE_FR})

# The seventh replay fixture, and the one that says a DEADLINE reproduces. A
# wait is the easiest thing in an engine to get right live and wrong on replay:
# the obvious way to write one is a counter in a tick hook, and `wfg replay`
# runs no hooks at all - so a cue with a two-second pre-wait would sit in
# `waiting` for the whole replayed session while the live one fired it
# perfectly.
#
# Which is why a wait coming due is a COMMAND rather than something a hook
# quietly does, and why this fixture is the proof: every wait in it elapses
# because the log says it did, on the tick the log says, with no clock of its
# own. It also carries the case that had no owner at all - a run killed while it
# was still waiting.
add_test(NAME wfg.replay.waits.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/waits.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/waits
                 --wfg-locale=C)
add_test(NAME wfg.replay.waits.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/waits.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/waits
                 --wfg-locale=${WFG_LOCALE_FR})

# The eighth, and the one that says a SCHEDULER reproduces. Every decision a
# manual group takes is the Runner's, taken in a tick hook - and `wfg replay`
# runs no hooks. So each of them leaves as a command, and this fixture is what
# that buys: the pointer descends into two nested groups, a header runs before
# the members, three presses walk it out again, and a footer blocks the group's
# end, all of it reproduced from the records with no hook in the room.
#
# What it does NOT do is catch the double-spawn that made the nested case worth
# writing down: that came out of a hook, and a replay runs none. The unit cases
# in GoTests are what fail if it comes back. This one says the other half - that
# the records a correct descent writes are a session that reproduces - and it
# was checked against a live run of the same show, record for record, because a
# replay cannot tell whether the engine would have written this log at all.
add_test(NAME wfg.replay.descent.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/manual-descent.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/descent
                 --wfg-locale=C)
add_test(NAME wfg.replay.descent.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/manual-descent.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/descent
                 --wfg-locale=${WFG_LOCALE_FR})

# The ninth, and the one that says an AUTOMATIC chain reproduces. A sequence
# group advances itself: a member reports done, the scheduler notices, the next
# one starts - and that noticing happens in a tick hook, which a replay does not
# run. An engine that advanced its chain from inside one would replay a session
# where the first member played and nothing followed it.
#
# Its first job is to fail if a HANDLER reports. A replay re-injects every record
# and re-runs every handler, so a handler that submitted a report of its own
# would produce it twice and this log would grow. That is the rule the whole
# scheduler is shaped around, and this is where it is checked.
#
# The chain's arithmetic is in the tick numbers rather than in a comment
# somewhere: ended at 3, next member spawned at 4, launched at 5.
add_test(NAME wfg.replay.chain.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/auto-chain.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/chain
                 --wfg-locale=C)
add_test(NAME wfg.replay.chain.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/auto-chain.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/chain
                 --wfg-locale=${WFG_LOCALE_FR})

# The tenth, and the one that says a RANDOM decision reproduces. A shuffled
# group draws an order with a random number generator, at a moment nobody typed,
# and a decision nobody wrote down is a session that cannot come back.
#
# So it is written down: `run.round` carries the seed and the members in the
# order they will play, and this replay reads them back. The generator is never
# consulted here at all - which is the assertion, and it is the same one the
# generated identifiers have always had, applied to an order.
#
# It also carries the two things an operator does to a bed that is running: a
# member pruned out of the round in progress without the show being edited, and
# `afterIteration`, which leaves an infinite loop at a boundary rather than
# cutting it.
add_test(NAME wfg.replay.rounds.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/rounds.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/rounds
                 --wfg-locale=C)
add_test(NAME wfg.replay.rounds.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/rounds.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/rounds
                 --wfg-locale=${WFG_LOCALE_FR})

# The eleventh, and the one that says an INPUT THAT IS NOT THERE reproduces. A
# trigger fires a cue from a lighting desk's datagram, a MIDI note, or a time of
# day - and a machine replaying a log has no socket, no MIDI interface, and it
# is a different hour on a different day.
#
# Which is exactly why the matching happens outside the model and what reaches
# the engine is a command. Three origins, one record shape, and the standby does
# not move once anywhere in the file - which is the property the whole feature
# rests on (PRD 3.5, 3.7).
add_test(NAME wfg.replay.triggers.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/triggers.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/triggers
                 --wfg-locale=C)
add_test(NAME wfg.replay.triggers.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/triggers.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/triggers
                 --wfg-locale=${WFG_LOCALE_FR})

# The twelfth, and the one where the model knows the least about what is
# actually happening. A ranged media cue's passes are counted from a sample
# counter that a replay does not have, and its boundaries are placed on a graph
# a replay does not build - so the ONLY thing that can carry which range a cue
# was in is a record, and this fixture is what says one is written.
#
# It also carries the two ends of §3.24: a range that loops for ever, left with
# an `advance` at the end of the pass it was on rather than where the operator's
# hand came down, and a playlist that runs to its end and finishes there.
add_test(NAME wfg.replay.ambience.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/ambience.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/ambience
                 --wfg-locale=C)
add_test(NAME wfg.replay.ambience.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/ambience.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/ambience
                 --wfg-locale=${WFG_LOCALE_FR})

# The thirteenth, and the one whose subject is deliberately NOT in it. A fade
# aimed at a group is a trim over its members (PRD 3.6), and the arithmetic of
# that is a level moving fifty times a second - which 3.15 keeps out of the log
# and a replay recomputes. So what this asserts is the session around it: a
# group fired, its members spawned and run, a fade fired over the top of them
# while they ran, and the fade outliving the group it was trimming.
add_test(NAME wfg.replay.dca-trim.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/dca-trim.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/dca
                 --wfg-locale=C)
add_test(NAME wfg.replay.dca-trim.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/dca-trim.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/dca
                 --wfg-locale=${WFG_LOCALE_FR})
add_test(NAME wfg.replay.sampler.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/sampler.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/sampler
                 --wfg-locale=C)
add_test(NAME wfg.replay.sampler.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/sampler.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/sampler
                 --wfg-locale=${WFG_LOCALE_FR})
# PHASE 9a'S EQ AS RECORDS: a band written to a media cue while it sounds,
# eq.reset, and an undo - decisions about the show, logged as node.set and
# replayed through the document door under both locales (§17.9).
add_test(NAME wfg.replay.eq.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/eq.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/eq
                 --wfg-locale=C)
add_test(NAME wfg.replay.eq.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/eq.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/eq
                 --wfg-locale=${WFG_LOCALE_FR})
# A MEDIA CUE'S LEVEL LANE AS RECORDS (namespace draft §20): a lane redrawn in
# one node.set, three drawings refused with the reason each deserves, a lane
# cleared and put back by undo, and a lane refused on a memo - replayed through
# the document door under both locales.
add_test(NAME wfg.replay.lane.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/lane.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/lane
                 --wfg-locale=C)
add_test(NAME wfg.replay.lane.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/lane.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/lane
                 --wfg-locale=${WFG_LOCALE_FR})
# A SEND'S LANE AS RECORDS (namespace draft §28): a lane redrawn on a send in one
# node.set, two drawings refused, a lane cleared and put back by undo - replayed
# through the document door under both locales.
add_test(NAME wfg.replay.send-lane.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/send-lane.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/send-lane
                 --wfg-locale=C)
add_test(NAME wfg.replay.send-lane.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/send-lane.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/send-lane
                 --wfg-locale=${WFG_LOCALE_FR})
# A MEDIA CUE'S SPEED AS RECORDS (namespace draft §22): a speed edited, three
# refused with the reason each deserves, a mode changed and taken back by undo, a
# fade's switches set, and the fade fired with nothing to move - its run ending
# once - replayed through the document door under both locales.
add_test(NAME wfg.replay.rate.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/rate.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/rate
                 --wfg-locale=C)
add_test(NAME wfg.replay.rate.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/rate.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/rate
                 --wfg-locale=${WFG_LOCALE_FR})
# A LANE RECORDED FROM A FADER AS RECORDS (namespace draft §20.9): the pick,
# every refusal the lane commands owe, and a pass - replayed under both locales.
add_test(NAME wfg.replay.lane-record.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/lane-record.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/lane-record
                 --wfg-locale=C)
add_test(NAME wfg.replay.lane-record.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/lane-record.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/lane-record
                 --wfg-locale=${WFG_LOCALE_FR})
# AN OSC CUE'S CURVES PLAYED, AND A PASS RECORDED FROM A DEVICE'S PUSHES
# (namespace draft 45, O.12): what the device said is in the log, what the
# curves sent is not - replayed under both locales, nothing on any wire.
add_test(NAME wfg.replay.osc-curves.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/osc-curves.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/osc-curves
                 --wfg-locale=C)
add_test(NAME wfg.replay.osc-curves.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/osc-curves.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/osc-curves
                 --wfg-locale=${WFG_LOCALE_FR})
# A LOCKED SHOW'S EQ AND SENDS RIDDEN LIVE (2026-09-25): rides refused by the
# document and taken by the live layer in front of it, a send made live with
# its drawn identifier, then unlocked, kept as one transaction and undone -
# reproduced through the same doors serve installs, under both locales.
add_test(NAME wfg.replay.live.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/live.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/live
                 --wfg-locale=C)
add_test(NAME wfg.replay.live.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/live.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/live
                 --wfg-locale=${WFG_LOCALE_FR})
# PHASE 9a'S PLUGIN SET AS RECORDS: an entry declared with its four words and
# its drawn identifier, renamed, deleted and the delete undone - the set made
# on demand under Audio, reproduced exactly under both locales (§17.9).
add_test(NAME wfg.replay.plugins.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/plugins.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fx
                 --wfg-locale=C)
add_test(NAME wfg.replay.plugins.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/plugins.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fx
                 --wfg-locale=${WFG_LOCALE_FR})

# The persistent section through a double Esc and a jump (2026-09-26, namespace
# draft 18.8): asserted at a step, silenced but not suspended by a double Esc and
# back at the next GO, and left sounding by a jump whose own cue is relaunched.
# A mic cue's life (stage 9b.5, namespace draft 18.5): GO on its rack channel, Esc
# ringing out, fired again, its plugin's child killed and the failure in words, and a
# double Esc - recorded from a hosted session with an input WAV, replayed with no child.
# The plugin's parameter write left the show unsaved long enough to autosave, so the
# replay is given somewhere to save to, never the fixture itself.
add_test(NAME wfg.replay.mic.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/mic.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/mic
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/mic-C
                 --wfg-locale=C)
add_test(NAME wfg.replay.mic.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/mic.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/mic
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/mic-fr_FR
                 --wfg-locale=${WFG_LOCALE_FR})

# A take's life (stages 9c.3 and 9c.6, namespace draft 19.6 and 19.8): recorded and
# looped by transport cues, its points ridden through the tree's door, a layer laid
# and undone, kept as a file and as a cue, held by Esc, looped again by a later mic cue
# whose GO loops what it finds, cleared, and a double Esc - recorded from a hosted
# session with an input WAV, replayed with no recorder and no disk: the take's close
# and the file's name are the audio side's take.closed and take.kept, re-injected. Keep
# as cue edits the show, which autosaves, so the replay is given somewhere to save to.
add_test(NAME wfg.replay.take.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/take.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/take
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/take-C
                 --wfg-locale=C)
add_test(NAME wfg.replay.take.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/take.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/take
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/take-fr_FR
                 --wfg-locale=${WFG_LOCALE_FR})

add_test(NAME wfg.replay.persistent.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/persistent.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/persistent
                 --wfg-locale=C)
add_test(NAME wfg.replay.persistent.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/persistent.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/persistent
                 --wfg-locale=${WFG_LOCALE_FR})

# DOH! TAKING BACK A GO PRESSED TOO SOON (PRD 3.32, namespace draft 24.15): a heard
# ramp paused and carried on by the next GO, a scene paused part-way and re-seated,
# a console that takes back given its value back, a lighting desk left to its
# operator named and sent nothing again, a cue the GO stopped made again, the report
# as the engine's own record, a second press forgetting and a press past the window
# refused - recorded by blackbox/go_doh.py from a hosted session and replayed here
# with no audio side, no device and no wire, under both locales.
add_test(NAME wfg.replay.doh.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/doh.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/doh
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/doh-C
                 --wfg-locale=C)
add_test(NAME wfg.replay.doh.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/doh.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/doh
                 --out=${CMAKE_CURRENT_BINARY_DIR}/replayed/doh-fr_FR
                 --wfg-locale=${WFG_LOCALE_FR})

# A cue's insert made, turned, switched off and a turn undone (Phase 9a, PR 9a.8):
# the FX door's records replay on a machine with no child and no plugin.
add_test(NAME wfg.replay.fx.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fx.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fx
                 --wfg-locale=C)
add_test(NAME wfg.replay.fx.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/fx.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/fx
                 --wfg-locale=${WFG_LOCALE_FR})
add_test(NAME wfg.replay.group-fade.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/group-fade.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/group-fade
                 --wfg-locale=C)
add_test(NAME wfg.replay.group-fade.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/group-fade.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/group-fade
                 --wfg-locale=${WFG_LOCALE_FR})

# The fourteenth, and the one where the thing the cues DO cannot happen. A MIDI
# cue's whole visible effect is bytes leaving a cable, and JUCE cannot even make
# a virtual port on Windows - no CI runner has a MIDI interface at all. So the
# sender is a seam the cue layer knows nothing about, a replay installs none,
# and this says the runs and the log reproduce with nothing going anywhere.
add_test(NAME wfg.replay.midi.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/midi.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/midi
                 --wfg-locale=C)
add_test(NAME wfg.replay.midi.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/midi.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/midi
                 --wfg-locale=${WFG_LOCALE_FR})

# The fifteenth, and the only one whose whole subject is a command that undoes
# another. PR 5.4 named this file and PR 5.4 did not write it; an audit of what
# Phase 5 still owed found the gap, and section 14.9 had been claiming in the
# present tense that it was here.
#
# `undo` and `redo` answer with the domain they moved and the NAME of the
# transaction they moved, and that pair is the whole of the guarantee. A replay
# rebuilds its own undo stack from the same inputs - the transaction hook is
# installed by `replay` as well as by `serve`, and the coalescing window keys on
# the address, the origin and the tick, all three of which are in the log - so a
# stack that drifted by one step answers with a different word, or runs out a
# gesture early, and the record does not match. The unit suite asserts the
# stack; only a log can assert what the stack SAYS.
#
# It was checked the way the others were, by making it fail: the window widened
# from twenty-five ticks to two hundred and fifty joins two renames and a drag
# into fewer transactions, and record 15 comes back `nothing-to-undo`.
add_test(NAME wfg.replay.undo.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/undo.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal
                 --wfg-locale=C)
add_test(NAME wfg.replay.undo.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/undo.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal
                 --wfg-locale=${WFG_LOCALE_FR})

# The first session recorded from the compiled client (M1, 2026-09-17): the
# author's own first look at the window over the phase4 show, GO pressed from
# it, the window closed and the engine stopped by the close button's dialogue.
# What it proves is narrow and worth having: a record whose origin is `window`
# replays with no window in the room, because a click and a datagram reach the
# engine through the same door and leave the same record (namespace draft
# section 14.16, rule 1). The window itself is tested by opening it.
add_test(NAME wfg.replay.window.C
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/window-go.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/phase4
                 --wfg-locale=C)
add_test(NAME wfg.replay.window.fr_FR
         COMMAND wfg replay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/logs/window-go.wfglog"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/phase4
                 --wfg-locale=${WFG_LOCALE_FR})

# `wfg canon` over the hand-authored fixture. It writes to stdout, so ctest is
# checking the exit code and the unit suite checks the bytes; what this adds is
# that the PRODUCT BINARY can read a show document under either locale, which a
# library test cannot prove.
add_test(NAME wfg.canon.C
         COMMAND wfg canon "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/documents/messy-input.xml"
                 --wfg-locale=C)
add_test(NAME wfg.canon.fr_FR
         COMMAND wfg canon "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/documents/messy-input.xml"
                 --wfg-locale=${WFG_LOCALE_FR})

# The generated schema header against the parameter table it comes from. Two
# things that must agree need something that checks they do; this is the same
# gate as check-claude-md.py, for the same reason.
add_test(NAME schema.generated
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/generate-schema.py" --check)

# -----------------------------------------------------------------------------
# The bundle: its grammar, and two opinions about whether a document obeys it.
# -----------------------------------------------------------------------------

# The committed RELAX NG against what the parameter table generates now. Same
# gate as schema.generated and check-claude-md.py, one level further out: that
# one keeps the generated HEADER honest, this one keeps the published GRAMMAR
# honest, and the grammar is the half that people outside this repository read.
#
# Registered under both locales because it is a serialisation surface. A range
# in the CSV reaches the grammar as a minInclusive facet written through the
# number formatter, and a formatter that consulted the locale would put a comma
# in it under fr_FR — which is exactly the class of bug the locale rule exists
# to catch, and it would otherwise reach a published file.
add_test(NAME wfg.schema.C
         COMMAND wfg schema --check=${CMAKE_SOURCE_DIR}/docs/schema/show.rng
                 --wfg-locale=C)
add_test(NAME wfg.schema.fr_FR
         COMMAND wfg schema --check=${CMAKE_SOURCE_DIR}/docs/schema/show.rng
                 --wfg-locale=${WFG_LOCALE_FR})

# The device presets shipped beside the binary (namespace draft §57, AFN): the
# shipped verb reads the source tree's folder and exits 1 on the first file that
# does not load, is not named after itself or names no source. The unit suite
# reads the same folder through the same table; this is the binary doing it.
add_test(NAME wfg.presets.C
         COMMAND wfg presets --check --folder=${CMAKE_SOURCE_DIR}/presets/devices --wfg-locale=C)
add_test(NAME wfg.presets.fr_FR
         COMMAND wfg presets --check --folder=${CMAKE_SOURCE_DIR}/presets/devices --wfg-locale=${WFG_LOCALE_FR})

# The product binary opening a real bundle folder: manifest, show.xml, state.xml.
# Exit 0 means it loaded AND had nothing to report, so a fixture that acquires a
# stale standby fails here rather than being quietly tolerated.
add_test(NAME wfg.validate.C
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
                 --wfg-locale=C)
add_test(NAME wfg.validate.fr_FR
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
                 --wfg-locale=${WFG_LOCALE_FR})

# And a bundle that declares Phase 4's slots: two processor inputs on a mount,
# two rack channels, and cues that feed and insert into them. It is the only
# fixture carrying any of those elements, so it is what proves the grammar, the
# containment table and the validate rules agree about a show that uses them -
# and, through `schema.fixtures` below, what puts them in front of lxml.
add_test(NAME wfg.validate.slots.C
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/slots"
                 --wfg-locale=C)
add_test(NAME wfg.validate.slots.fr_FR
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/slots"
                 --wfg-locale=${WFG_LOCALE_FR})

# And the devices bundle: one device that describes itself and one that does
# not, plus a cue aimed under no device at all. It is the only fixture carrying
# an opaque mount - a <Mount> with no namespace file - which before 2026-09-22
# was refused outright, so it is what proves the grammar, the containment table
# and the load-time checks agree that a device is allowed not to describe
# itself. Through `schema.fixtures` below it also goes in front of lxml.
add_test(NAME wfg.validate.devices.C
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/devices"
                 --wfg-locale=C)
add_test(NAME wfg.validate.devices.fr_FR
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/devices"
                 --wfg-locale=${WFG_LOCALE_FR})

# And a bundle saved LOCKED: `<Show locked="true"/>` in its state.xml, which is
# the first entry any fixture has carried for the Show root and the first time
# the container branch of state.xml has been put in front of lxml or of this
# verb. It is also the check on the edit lock's own rule, from the outside: the
# lock is restored through the same door every other write takes, so a lock
# that refused state writes would make a locked bundle refuse to load its own
# lock - and this would exit 1 saying so.
add_test(NAME wfg.validate.locked.C
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/locked"
                 --wfg-locale=C)
add_test(NAME wfg.validate.locked.fr_FR
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/locked"
                 --wfg-locale=${WFG_LOCALE_FR})

# The parameter tree, printed by the product binary. Registered under both
# locales for the same reason as `wfg schema`: this is a serialisation surface,
# every number in it goes through the project's formatter, and a formatter that
# consulted the locale would put a comma into a published OSCQuery reply. The
# unit suite checks the bytes against a hand-written golden; what this adds is
# that the BINARY can do it under either locale, which a library test cannot
# show.
add_test(NAME wfg.tree.C
         COMMAND wfg tree "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
                 --wfg-locale=C)
add_test(NAME wfg.tree.fr_FR
         COMMAND wfg tree "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
                 --wfg-locale=${WFG_LOCALE_FR})

# THE OUTSIDE OPINION, and the reason the grammar is generated at all. Every
# other check in this file is the engine agreeing with itself: our reader parsed
# it, our schema approved it. This runs the published grammar through lxml,
# which shares no code with us, over every fixture in the tree. When the two
# disagree we get to find out which one is wrong.
#
# No SKIP_RETURN_CODE, and validate-show.py exits 2 rather than 0 when lxml is
# missing. The root CMakeLists.txt refuses to configure without it for the same
# reason: a check that reports success when it did not run is worse than no
# check, because it also removes the pressure to fix it.
add_test(NAME schema.fixtures
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/validate-show.py")

# -----------------------------------------------------------------------------
# The shipped binary links no OpenSSL.
#
# juce_simpleweb declares libssl and libcrypto as link dependencies on all three
# platforms, and Go.dot compiles it with SIMPLEWEB_SECURE_SUPPORTED=0 and calls
# no TLS code at all. WfgThirdParty.cmake section 2b clears the module's
# INTERFACE_LINK_LIBRARIES to keep them off the link line.
#
# This asks the BINARY rather than trusting that property to have stayed edited.
# It is not a hypothetical regression: the upstream recipe we transcribed filters
# the list by the names `libssl libcrypto z`, which are the macOS and Windows
# spellings — the module spells them `ssl,crypto` on Linux, so that filter leaves
# a Linux build linking an OpenSSL it never calls. A gate that reads the property
# would have agreed with the bug; one that reads the ELF cannot.
#
# $<TARGET_FILE:wfg> rather than a constructed path: the multi-config Windows
# generator puts the binary under Debug/ and the single-config Ninja jobs do not.
add_test(NAME deps.no-openssl
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/check-no-openssl.py"
                 $<TARGET_FILE:wfg>)

# -----------------------------------------------------------------------------
# The compiled client reads one door and names nothing behind it.
#
# Namespace draft section 14.16 gives the desktop client three rules, and the
# second - it reads only ParameterTree::snapshot() - is the one an afternoon
# breaks without noticing: the document is on the same heap and a reach past
# the door compiles. This reads src/wfg/client and says so: no childrenOf(),
# nothing the tick thread owns named, exactly one snapshot() call site, and no
# JUCE at all in the model half. Once, not per locale - it reads source.
add_test(NAME client.boundary
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/check-client-boundary.py")

# -----------------------------------------------------------------------------
# No comment delimiter hidden inside a comment.
#
# Go.dot's comments explain OSC addresses, and address PATTERNS use `*`. Two
# spellings are booby-trapped inside a C block comment and this project has hit
# both: a star followed by a slash ENDS the comment early, and a slash followed
# by a star inside one is -Wcomment, an error under the strict preset.
#
# The first cost three debugging sessions in Phase 1; the second cost a CI round
# trip, because -Wcomment is GCC-only and MSVC says nothing. This gate is fifteen
# seconds and runs everywhere, which is the whole argument for it.
add_test(NAME source.comments
         COMMAND ${WFG_PYTHON} "${CMAKE_SOURCE_DIR}/scripts/check-comments.py")

# -----------------------------------------------------------------------------
# THE PHASE 1 DONE-WHEN CLAUSE, driven from outside the process.
#
# The devplan's criterion word for word: an external OSCQuery client can load a
# document, read and write nodes, move standby, and the event log replays the
# session bit-for-bit. tests/blackbox/phase1_session.py is that sentence as a
# program — it launches the SHIPPED BINARY on a throwaway copy of a bundle,
# talks to it over HTTP, WebSocket and UDP, saves, and then makes the engine
# reproduce its own log.
#
# It found two bugs nothing else could, and both were invisible from inside:
# `serve` published its first tree AFTER opening the socket, so a fast client
# got a 404 from a server that had already printed its port; and nothing marked
# the parameter tree stale, so every write landed in the model, was logged as
# applied, was saved to disk, and was invisible to every client for ever.
#
# WFG_BINARY is how the driver finds what to run. $<TARGET_FILE:wfg> is the only
# spelling that works on both the single-config Ninja jobs and the multi-config
# Windows one, where the binary is under Debug/.
#
# Registered under both locales like every other serialisation surface: the
# session writes numbers into show.xml and reads them back over HTTP, and a
# formatter that consulted the locale would put a comma in one of them.
add_test(NAME blackbox.phase1.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase1_session.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase1.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

add_test(NAME blackbox.phase1.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase1_session.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase1.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

add_test(NAME blackbox.import-playback.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_playback.py" --wfg-locale=C)
add_test(NAME blackbox.import-playback.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_playback.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.import-playback.C blackbox.import-playback.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 60)

# A Performance plays its Show's sound: a cue in Hamlet/Paris naming storm.wav
# finds Hamlet/media/storm.wav, through the one resolver (audio/MediaInfo.h).
add_test(NAME blackbox.performance-media.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/performance_media.py" --wfg-locale=C)
add_test(NAME blackbox.performance-media.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/performance_media.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.performance-media.C blackbox.performance-media.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 60)

# The analysis cache swept at launch and when asked (namespace draft section 52).
add_test(NAME blackbox.cache-sweep.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/cache_sweep.py" --wfg-locale=C)
add_test(NAME blackbox.cache-sweep.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/cache_sweep.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.cache-sweep.C blackbox.cache-sweep.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# A sound edited: sections, a move carrying the lane, the render, Freeze and
# Unfreeze (namespace draft §55). Hosted, so the GO on the bounce reaches a launch.
add_test(NAME blackbox.media-edit.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/media_edit.py" --wfg-locale=C)
add_test(NAME blackbox.media-edit.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/media_edit.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.media-edit.C blackbox.media-edit.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 180)

# A movie edited the same way (namespace draft §55.5): its shots reordered with a dissolve, its sound
# cut in step, the pair frozen and unfrozen, the render's frames read back against the source's.
add_test(NAME blackbox.movie-edit.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/movie_edit.py" --wfg-locale=C)
add_test(NAME blackbox.movie-edit.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/movie_edit.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.movie-edit.C blackbox.movie-edit.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# The output list, and the patch that follows it until a cue has played. Hosted,
# because the latch is at a real launch: without --hosted the run sits at
# `armed` and never reaches it.
add_test(NAME blackbox.output-layout.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/output_layout.py" --wfg-locale=C)
add_test(NAME blackbox.output-layout.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/output_layout.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.output-layout.C blackbox.output-layout.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# The devices a show talks to: an opaque one loading, its rows retyped from a
# client and reaching the socket, a cue putting real bytes on a real UDP port,
# `tx` off leaving the show running and the wire quiet, and strict senders
# dropping and counting a datagram from somebody the show does not declare.
#
# It owns the far end - a socket it binds itself - so "it was sent" is the
# datagram rather than a number the engine keeps about itself. Not hosted:
# nothing here needs audio, and a network cue holds no voice.
add_test(NAME blackbox.devices.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/devices.py"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/devices
                 --wfg-locale=C)
add_test(NAME blackbox.devices.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/devices.py"
                 --bundle=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/devices
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.devices.C blackbox.devices.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# -----------------------------------------------------------------------------
# A PROCESSOR WRITING CUES INTO THE SHOW (namespace draft §56): the driver is
# the processor - it declares itself, captures cues, hears GO send them back,
# is answered and refused - and `wfg replay` rebuilds the show from the log.
add_test(NAME blackbox.authoring.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/authoring.py" --wfg-locale=C)
add_test(NAME blackbox.authoring.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/authoring.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.authoring.C blackbox.authoring.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# -----------------------------------------------------------------------------
# A SPATIAL PROCESSOR'S SNAPSHOT OF SEVENTY INPUTS (namespace draft §88): 7,500
# pairs captured in chunks sent back to back, landed whole with the tick kept
# up, and fired - every message heard, in order, bundled and spread at the
# device's twenty datagrams a tick, the cue playing until the last has left.
add_test(NAME blackbox.big-cue.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/big_cue.py" --wfg-locale=C)
add_test(NAME blackbox.big-cue.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/big_cue.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.big-cue.C blackbox.big-cue.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# -----------------------------------------------------------------------------
# A DEVICE FOUND ON THE NETWORK, SAVED AS A PRESET (namespace draft §76, UD.10):
# the shipped binary against mock_target.py describing itself and answering
# multicast DNS put to it directly - found, made, saved into a folder of the
# driver's own, made again from the preset - and `wfg replay` rebuilds the
# show from the log. No part of the network's 5353 is touched.
add_test(NAME blackbox.discovery.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/discovery.py" --wfg-locale=C)
add_test(NAME blackbox.discovery.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/discovery.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.discovery.C blackbox.discovery.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# -----------------------------------------------------------------------------
# A DEVICE OVER A CONNECTION (namespace draft §57, AFJ; DP.6): the shipped
# binary against mock_target.py listening for connections - the link opened on
# its own thread, a cue arriving down the stream cut by length, the console
# closing the connection and the link coming back after its pause, the console
# gone and the row saying so, the same device moved to a SLIP console.
add_test(NAME blackbox.wires.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/wires.py" --wfg-locale=C)
add_test(NAME blackbox.wires.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/wires.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.wires.C blackbox.wires.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 400)

# -----------------------------------------------------------------------------
# A MACHINE THAT HOLDS (namespace draft §58, RD.1): `serve --hold` against a
# device over UDP and one over TCP - nothing reaches either, the connection is
# never opened, a verified cue at a device that never answers ends at once, and
# every cue ends done, unwarned - then the same without the hold, which reaches
# both and times the verified cue out. A copy of the devices fixture each time.
add_test(NAME blackbox.hold.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/hold.py" --wfg-locale=C)
add_test(NAME blackbox.hold.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/hold.py" --wfg-locale=${WFG_LOCALE_FR})
#
# UNBUFFERED, AND FOUR HUNDRED SECONDS: on its first CI run (38123547592) macOS
# timed both locales out at 120 s against 8.6 s here, and printed nothing - a
# piped Python buffers its report and loses it to the kill - so the next run says
# where it stood; the limit is the wires driver's, which macOS also outran.
set_tests_properties(blackbox.hold.C blackbox.hold.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE MACHINE'S PAIRING (namespace draft §58, RD.4): `serve --role=backup
# --peer=... --link-timeout=2.5` publishes the three rows and writes nothing;
# `pair.setRole main` over OSC writes pairing.xml beside the engine folder - a
# temporary one, never the developer's - keeping the file's own values for what
# the command line overrode; two more serves on that folder read it back, 1.5 s
# exactly under either locale; and a value typed wrong stops the start. Three
# serves one after another, unbuffered and with hold's limit, for hold's reason.
add_test(NAME blackbox.pairing.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pairing.py" --wfg-locale=C)
add_test(NAME blackbox.pairing.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pairing.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pairing.C blackbox.pairing.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE LINK BETWEEN A MAIN AND ITS BACKUP (namespace draft §58, RD.5): two serves
# on one machine, each on a copy of one fixture and a temporary engine folder -
# `--role=main`, then `--role=backup --peer=127.0.0.1:<the main's port>`. Both
# linked within 3 s; a cue.fire, a standby.next and a go sent to the main raise
# the backup's pair/received by exactly three; the round trip under a tenth of a
# second; the main killed, the backup unlinked within the timeout and a half
# second; a new main on the same port found again. Unbuffered, and hold's limit,
# for hold's reason.
add_test(NAME blackbox.pair-link.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_link.py" --wfg-locale=C)
add_test(NAME blackbox.pair-link.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_link.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pair-link.C blackbox.pair-link.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# A BACKUP FOLLOWING ITS MAIN (namespace draft §58, RD.6): the two serves above
# on copies of the devices fixture, with mock_target.py over UDP aimed at by
# both. The backup reads `following`, the main `leading`; the backup holds and
# its delay is one or two ticks; a GO at the main plays on both and the mock
# hears it once; a GO sent to the backup's WebSocket is passed on, plays on both
# and is heard once; a refusal of what was passed on reaches the backup's
# lastError as the main's; a GO sent to the backup over UDP is ignored and
# neither machine fires. Unbuffered, and hold's limit, for hold's reason.
add_test(NAME blackbox.pair-follow.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_follow.py" --wfg-locale=C)
add_test(NAME blackbox.pair-follow.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_follow.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pair-follow.C blackbox.pair-follow.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE LINK LOST, TAKING OVER, BOTH SENDING (namespace draft §58, RD.7): a main
# and its backup on copies of the devices fixture, mock_target.py over UDP
# aimed at by both. GOs at the main heard once; the main killed, the backup
# leads and sends within the timeout, and GOs at its WebSocket are heard once;
# the main started again on its port follows the backup (AFR) and the device
# still hears each GO once. Then a TCP relay between a second backup's dialler
# and its main, paused past the timeout: both lead, one GO at each heard once;
# resumed, both read bothSending and say so, a GO at either is sent by both and
# heard twice (AFQ); `pair.keep` at the main and the next GO is heard once.
add_test(NAME blackbox.redundancy-failover.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/redundancy_failover.py" --wfg-locale=C)
add_test(NAME blackbox.redundancy-failover.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/redundancy_failover.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.redundancy-failover.C blackbox.redundancy-failover.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# A BACKUP STARTED LATE, SEATED AND IN STEP (namespace draft §58, RD.8): two
# hosted serves on copies of the phase 2 fixture, its show rewritten by the
# driver and its sound generated there. A main plays a long bed and a timeline
# scene; a backup started late follows, is seated at the main's position - the
# bed and the scene under the main's run names, the scene's memo waiting, the
# standby the main's - and reads `pair/inStep` true, its bed's playhead within
# two ticks of the main's once the delay is allowed for (four on CI);
# `pair.realign` on the backup seats it again and it stays in step, on the main
# it is refused `not-following`; the main killed, the backup leads, and the main
# started again follows it and is seated where the backup is, in step.
add_test(NAME blackbox.pair-seat.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_seat.py" --wfg-locale=C)
add_test(NAME blackbox.pair-seat.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_seat.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pair-seat.C blackbox.pair-seat.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE SAME SHOW, CHECKED (namespace draft §58, RD.10; AFU, AFY): a main and a
# backup on copies of one show, each with its own interface in its show.xml,
# say their shows match; the backup stopped, a level edited at the main, the
# backup started again says on link-up that its show differs and names the cue,
# and its alarm says so; `pair.adoptShow` refused on the main and taken on the
# backup - the main's level, the shows matching, the backup's interface row its
# own in the show and on disk, its old files in a set-aside folder; the backup
# started again with its sound removed names it as missing here. Two serves on
# the dummy clock, unbuffered, and hold's limit, for hold's reason.
add_test(NAME blackbox.pair-show.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_show.py" --wfg-locale=C)
add_test(NAME blackbox.pair-show.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_show.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pair-show.C blackbox.pair-show.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE SAME SAMPLE (namespace draft §58, RD.9; AFS, AGI, AGQ; M61): a main and a
# backup on copies of one show, hosted and each writing its render; the backup
# follows and says its launches are on the main's samples, the drift between
# the two sound clocks near nought, the map's error, each side's output
# latency; a GO at the main, and the click's onset in each render put on the
# steady clock through that engine's engine/sampleZeroAt - within a millisecond
# of each other (a block on CI); M61 printed: the distance, and Δ. Two serves,
# unbuffered, and hold's limit, for hold's reason.
add_test(NAME blackbox.pair-sample.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_sample.py" --wfg-locale=C)
add_test(NAME blackbox.pair-sample.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pair_sample.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pair-sample.C blackbox.pair-sample.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# -----------------------------------------------------------------------------
# THE PHASE 2 DONE-WHEN CLAUSE, driven from outside the process.
#
# The devplan's criterion word for word: `Rien à faire` -> load show -> GO ->
# sound -> fade -> GO -> next cue, driven entirely over OSC, and the replay log
# reproduces it. tests/blackbox/first_sound.py is that sentence as a program.
#
# Everything below the command is real - the shipped binary, a generated
# Tracktion Edit, a playback graph, the output plugin and its routing matrix, a
# UDP socket, an OSCQuery server, and a WAV written from what came out. On the
# other end of the network cue is mock_target.py, a device written from the
# specification in another language, so that when the two agree the agreement
# means something.
#
# THE SOUND IS ASSERTED AS ARITHMETIC. The media file is generated by the driver
# with every sample the same known constant, so a routing coefficient of 0.5
# means the right output carries exactly half, a fade to -20 dB means exactly a
# tenth, and silence means zeros. A driver that asserted "roughly quiet" would
# pass against a bug that made every cue 3 dB down.
#
# It found run.kill stopping nothing: the command marked a run `stopping` and
# nothing ever told the audio side, so a killed cue read as stopping and played
# on until its file ran out. Every model-level check passed while four seconds
# of audio nobody wanted went to the outputs, and only an assertion on SAMPLES
# could see it.
#
# Longer timeout than the Phase 1 driver because this one runs in real time: the
# hosted driver paces itself off a clock rather than free-running, which is what
# makes the render a recording of a session rather than of a render.
add_test(NAME blackbox.first-sound.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/first_sound.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.first-sound.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)

add_test(NAME blackbox.first-sound.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/first_sound.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.first-sound.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)

# The engine serving its own client, and serving nothing else. `--ui=<dir>`
# answers /ui from disk on the same port the tree is on - same origin, so the
# page needs no CORS, and a tablet reaches it by the address it already has to
# know to reach the engine at all.
#
# That is a file server inside a process which binds an interface and answers
# anybody who can reach it, so most of what this asserts is the REFUSALS: three
# ways of climbing out of the directory, a file that is not there, and - the one
# that would be easiest to lose - the OSCQuery tree still answering on the same
# port, because a route put in front of the namespace is exactly the kind of
# change that can shadow it.
# `serve --device=` end to end, which nothing covered: the device layer has unit
# tests that open a card and the serve wiring around it had none. It used to
# print its whole banner - both ports, the client URL, the granted settings -
# and then die of an access violation before the first tick, because the clock
# was chosen as a pair when there are three of them.
#
# SKIPS ITSELF on a machine with no audio device, which is every CI runner, and
# that is honest rather than convenient: what it checks cannot be checked
# without one. It runs where the configuration it covers is actually used.
#
# THE AUDIO-DEVICE LOCK (2026-10-13): it opens the machine's own interface, as
# unit.<locale>.devices and the window suites do, and an interface held by one
# process is one another cannot open - or opens and never ticks: on 2026-09-18
# this failed whenever a --hosted window held "Primary Sound Driver". So they
# take turns, and the rest of the suite runs beside them.
add_test(NAME blackbox.device.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/device_serve.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.device.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300
                     RESOURCE_LOCK audio-device)

# PHASE 3'S DONE-WHEN CLAUSE, on the shipped binary, over a socket, and read
# back off the WAV that came out. The devplan states it in one sentence - "a
# complex background auto-sequence with a looping ambience runs while manual
# foreground cues fire on top; an advance cue exits the loop cleanly; the replay
# log reproduces all of it" - and every clause of it is a check in there.
#
# The longest driver in the suite by some way, because it drives a whole scene:
# a ranged bed that loops for ever, a nested timeline group, a footer, a
# foreground cue landing on top, a trigger that moves nothing, and an advance.
# Three hundred seconds is generous on a loaded runner and still a tenth of what
# a hang would cost.
add_test(NAME blackbox.phase3.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase3_groups.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase3.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase3.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase3_groups.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase3.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# PHASE 4'S DONE-WHEN, and the devplan states it in one sentence: "load-to-time
# into the middle of a scene lands the right cues at the right offsets with the
# right slots claimed; reordering cues produces the right overlap warnings; a
# claim on a busy slot waits and says so; all headless."
#
# Every clause is a check in there, and three of them found engine faults the
# unit suite could not: a scene whose header is entirely derived read `partial`
# for ever, a plain media cue at the pointer held its voice and its slots after
# the pointer moved on, and a preset network cue standing second in a scene
# stopped the scene when its own row came round. A driver that drives a WHOLE
# show is what finds those - each is a seam between two things that are
# individually correct.
#
# The longest in the suite: a prepared scene, a revocation, a claim that waits
# for a bed, an overlap that appears when a cue moves and goes when it moves
# back, a jump into the middle of a timeline group read off a ramp in the
# render, a persistent section put back after a hand on the desk, and a replay
# of all of it. Four hundred seconds is generous on a loaded runner.
add_test(NAME blackbox.phase4.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase4_prepare.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase4.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 400)
add_test(NAME blackbox.phase4.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase4_prepare.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase4.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 400)

# PHASE 5'S DOCUMENT HALF, started by PR 5.3 with the edit lock and grown by the
# pull requests after it. What it asks is what only a shipped binary can answer:
# that a bundle saved locked opens locked, that the ENGINE refuses an edit from
# any client with `locked` in lastError - a client that hides its buttons has
# locked exactly one client - and that GO, a write to a mounted device and the
# lock's own release all get through. Then the session's log replays, refusals
# included. No audio side, so it is short; three hundred seconds is the suite's
# usual allowance for a loaded runner.
add_test(NAME blackbox.phase5.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase5_document.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase5.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase5.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase5_document.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase5.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# H6: CRASH-SAFE SAVING, KILLED HALFWAY THROUGH A WRITE (namespace draft §23.4,
# and §23.5 for the two engine fixes it failed first on). Phase 5's kill waits
# for the autosave to land before it pulls the plug; this one never waits. A
# stream of renames, saves and autosaves keeps the writer thread busy, and the
# engine is killed at twenty seeded random points while the writes are in
# flight. After each kill the files must be exactly one of the states written -
# show.xml byte for byte the calibrated template with one marker in it,
# recovery/ absent or complete and never older than a show its own session
# saved, and the offered folder the one the engine's rule picks - with every
# temp beside them ignored. Then a plain restart must publish show.xml's state
# and take document.recover, and a --recover restart must start rather than
# exit 2. Killed and never stopped: on POSIX a stop is a clean exit that drains
# the writer. REGISTERED WITH THE ENGINE FIXES IT FAILED FIRST ON (H6b): before
# them every run on the Windows box was red - an autosave older than the save
# left on offer in about one kill in five, on every platform, and on Windows a
# file under no name in about one kill in twenty-five. A red here is the
# engine's, never a flake to re-run. Ports are ephemeral and the scratch folder
# is unique, so no RUN_SERIAL. About twenty seconds on the Windows box in Debug;
# three hundred for a loaded runner - and under the real-time sanitizer, where
# every run is a loaded runner, 298 s for the one that passed and 300.04 for the
# one that did not (2026-10-10, run 38091447705), so the limit is twice that:
# the time is the sanitizer's, and the check above is what a red still means.
add_test(NAME blackbox.crash-write.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/crash_during_write.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.crash-write.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)
add_test(NAME blackbox.crash-write.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/crash_during_write.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.crash-write.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)

# PRD §3.30'S CHECK, BEFORE ANYTHING IS DRAWN (PR 5.7, namespace draft §14.12):
# a 1 kHz sine saturated at the ramp's 1 kHz hue, white noise grey, a sweep
# walking the ramp - three WAVs the driver writes with stdlib `wave`, analysed
# by `wfg analyse`, and decoded by a `.tpy` reader of the driver's own, so it
# cannot share a mistake with the engine's. A PAIR, because WAV is a format
# every build reads (§14.13 left the single shape for a format some build
# lacks) and because the verb prints numbers a comma would break. Then one
# `wfg serve`, on the dummy clock, for the analyser thread the verb shares its
# code with: every named file coloured unasked, and a file a mid-session edit
# introduces coloured without a reopen.
#
# AND WHAT A SESSION SAYS WITH IT (PR 5.8, §14.5): a second `wfg serve`,
# --hosted because a playhead needs a track, of a copy routed to a bus and
# parked on the SWEEP. The cues must name their files' SHA-256 as hashlib
# computes it, the sine's renamed copy the same one. GO must play the sweep,
# and at every playhead the driver catches, its run must publish the very frame
# the driver's reader finds under that playhead - with a point under fr_FR -
# and the ramp's colour at the sweep's frequency then. The sweep and not the
# sine, because a steady sine is one frame repeated, and a playhead rounded, a
# frame out, or read off the level above would all have passed; the driver
# reads on until it holds a reading only its own frame could have produced.
# /media/<hash>/timbre must hand back the cache's own bytes and its header as
# JSON, formatVersion included, marked no-cache - the URL names the audio and
# not the analysis, and a moved ramp stop rebuilds the pyramid under the same
# name - and refuse what it does not hold with no-store, since that URL is a
# 200 the day the file is imported. A real graph with no device under it, run
# in real time, hence the first-sound driver's allowance.
add_test(NAME blackbox.timbre.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timbre_cache.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.timbre.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)
add_test(NAME blackbox.timbre.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timbre_cache.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.timbre.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 600)

# The wiring the unit suite cannot reach: a real datagram, on a real socket, on
# a thread that may not read the document, reaching the right cue - and the
# operator's pointer not moving when it does.
add_test(NAME blackbox.triggers.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/triggers.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.triggers.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.triggers.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/triggers.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.triggers.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# BINDINGS, HEARD (namespace draft §74, BD.12): a datagram on Go.dot's port and a
# controller played into a declared port's input each moving a cue's level as
# its binding, and the level heard in the render; a control learned by moving it.
add_test(NAME blackbox.bindings.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/bindings.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.bindings.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.bindings.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/bindings.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.bindings.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A STREAM DECK SURFACE, SERVED (namespace draft §95, SD.8): made by command with
# no strips, its keys laid out over the show's cart and published - as an MK.2's
# while no deck is found, which is every runner - its status in words, the
# show's keys arriving when switched on, a strip refused on it.
add_test(NAME blackbox.streamdeck.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/streamdeck.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.streamdeck.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)
add_test(NAME blackbox.streamdeck.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/streamdeck.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.streamdeck.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 120)

# PHASE 6'S PADS, HEARD: a sampler bank armed by GO and played by hand over
# the network, with a real Tracktion graph under it and the render read back as
# arithmetic - a press at velocity 64 is a tenth of the constant, a hold clip is
# the constant until the hand lets go, and the session replays. The fixture's
# copy is given a bus and routes; the committed bundle stays the one
# sampler.wfglog was recorded from.
add_test(NAME blackbox.phase6-sampler.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase6_sampler.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase6-sampler.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase6-sampler.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase6_sampler.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase6-sampler.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A show that declares a Mackie surface, served where there is no MIDI - every
# CI runner, and a laptop on the train: the surface says which port it cannot
# reach and why, the virtual panel and the network go on working, a DCA written
# over OSC trims the cue marked with it, and the session replays (§16.8).
add_test(NAME blackbox.phase6-surfaces.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase6_surfaces.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase6-surfaces.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase6-surfaces.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase6_surfaces.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase6-surfaces.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# PHASE 9a'S EQ, HEARD: a two-tone file through a media cue whose EQ cuts one
# of the tones by twenty decibels, served with a real Tracktion graph and
# rendered; a Goertzel filter per tone reads the cut, and a node.set to the cue
# while it sounds is heard in the tail. Then the session replays (§17.9).
add_test(NAME blackbox.phase9a-eq.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9a_eq.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase9a-eq.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9a-eq.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9a_eq.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9a-eq.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A MEDIA CUE'S LEVEL LANE, HEARD (namespace draft §20.6): a tone through a
# cue whose lane ramps, holds and steps, then a looping slice hearing the same
# stretch of its lane on every pass and a new lane written while it loops,
# served with a real Tracktion graph and rendered; M45 is printed. Then the
# session replays.
add_test(NAME blackbox.lane-level.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_level.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.lane-level.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.lane-level.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_level.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.lane-level.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A SEND'S LANE, HEARD (namespace draft §28.4): the level lane driver's ramp,
# step and looping slice drawn on a send into the only mix there is, so the
# render hears the send; a new lane written on the loop's send while it sounds;
# M48 printed, with a step judged within one coefficient glide. Then the session
# replays.
add_test(NAME blackbox.lane-send.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_send.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.lane-send.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.lane-send.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_send.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.lane-send.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A QLAB WORKSPACE IMPORTED BY THE SHIPPED BINARY (namespace draft §46.3,
# QL.6): a QLab 5 workspace written by the harness with plistlib, in a folder
# whose name is not ASCII, beside a WAV it finds and copies; the bundle accepted
# by `wfg validate` and validate-show.py's RELAX NG, the same bytes twice, a
# folder holding a show refused untouched, a QLab 3 workspace refused in words,
# and the report saying what did not come over.
add_test(NAME blackbox.import-qlab.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_qlab.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.import-qlab.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.import-qlab.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_qlab.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.import-qlab.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# AN ABLETON LIVE SET IMPORTED BY THE SHIPPED BINARY (namespace draft §29.3):
# `wfg import-als` on the session fixture, the bundle it writes accepted by
# `wfg validate` and by validate-show.py's RELAX NG, the same bytes twice, a
# folder holding a show refused untouched, and the report saying what did not
# come over.
add_test(NAME blackbox.import-als.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_als.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.import-als.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.import-als.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/import_als.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.import-als.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A MEDIA CUE'S SPEED, HEARD (namespace draft §22): four cues at twice and half
# their speed in varispeed and timestretch - M14, each one's length and pitch
# read out of the render - then a cue whose speed is edited while it sounds and
# a fade cue that takes it to nought, a tape stop, exactly silent and still
# playing; served with a real Tracktion graph, and the session replayed. What
# follows the tick waits a margin past it; nothing here times a tick.
add_test(NAME blackbox.rate-speed.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/rate_speed.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.rate-speed.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.rate-speed.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/rate_speed.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.rate-speed.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A LANE RECORDED FROM A FADER, HEARD AND WRITTEN (namespace draft §20.9): a
# virtual panel's strip taken, a pass recorded - touched, moved, let go,
# stopped - over a real Tracktion graph; the ride heard, the lane written in
# one pass and taken away in one undo, and the session replayed. Timing judged
# off CI only.
add_test(NAME blackbox.lane-record.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_record.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.lane-record.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.lane-record.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/lane_record.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.lane-record.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# AN OSC CUE'S CURVES END TO END (namespace draft 45, O.12): three messages on
# three curves into mock_target.py --listen as bundles, every value against the
# drawing (M51), then a pass recorded from what the device pushes down its
# LISTEN socket (M52), and the session replayed. No hosted audio.
add_test(NAME blackbox.osc-curves.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/osc_curves.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.osc-curves.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.osc-curves.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/osc_curves.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.osc-curves.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A PROCESS CUE'S PATCH END TO END (namespace draft 51, PC.12): Pure Data inside
# serve, Go.dot's ready-made patches beside it, a device's reports doubled and
# sent back to mock_target.py (M57, the time a report takes to come back), a
# go.edge firing a cue under the patch's own origin, Esc ending it, and the
# session replayed. No hosted audio.
add_test(NAME blackbox.process.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/process.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.process.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.process.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/process.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.process.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A SCRIPT CUE END TO END (namespace draft 66, SC.8): process.py's twin for a
# script cue's JavaScript - a device's report doubled and sent back to
# mock_target.py, Bell fired by godot.fire under the script's own origin, Esc
# ending it, and the session replayed. No hosted audio.
add_test(NAME blackbox.script.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/script.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.script.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.script.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/script.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.script.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# MQTT END TO END (namespace draft 67, MQ.8): serve with its own broker on, a
# device of the show on it with no socket, and a script cue hearing it - driven
# by MQTT packets written in Python from the specification: a publish doubled
# and back at a client (M72), Bell fired past a line, fifty clients publishing
# at once heard whole (M73), Esc, and the session replayed.
add_test(NAME blackbox.mqtt.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/mqtt.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.mqtt.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.mqtt.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/mqtt.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.mqtt.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# THE REMOTE'S STREAM (namespace draft §68, RM.1): a WebSocket client asks for
# a bundle a tick, reads the snapshot after it, and holds what the engine holds -
# every tick once and in order, a datagram's write and its own in the stream, a
# new cue announced before the bundle that carries it.
add_test(NAME blackbox.remote.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/remote.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.remote.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.remote.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/remote.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.remote.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# TIMECODE GENERATED END TO END (namespace draft 59, TC.9): serve --hosted
# --render with --midi-render=TC=<file>, the show fired by GO, and what came out
# read by a third implementation of the standards in Python - a biphase decoder
# for the LTC, a quarter-frame reader for the MTC. The song and its code on one
# sample, frames from 01:00:00:00, the takeover to 02:00:00:00 with no quiet,
# Esc on a frame boundary, a full frame then quarters a quarter apart stopping
# at the length; again held (no MTC, the LTC still drawn); again at 29.97
# drop-frame and 44.1 kHz. Three serves one after another: unbuffered, and
# hold's limit, for hold's reason.
add_test(NAME blackbox.timecode.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timecode.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.timecode.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)
add_test(NAME blackbox.timecode.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timecode.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.timecode.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# TIME GROUPS AND CHASE END TO END (namespace draft 60, TG.11): serve --hosted
# --render hearing LTC on an input (--input-wav, written by the driver's own LTC
# writer) and MTC on a port (--midi-play-loop, its own MTC writer), a time group
# on each, every member's sound itself LTC so the render, decoded, says where
# each stood on every sample. Members on the code's sample, a free runner never
# moved, each chaser by its method through +50 ppm, +0.1 %, a stop, a return, a
# locate back and the loop point, code.heard sparse from ltc:/mtc: origins; and
# a second serve held, firing the same. Two serves one after another: unbuffered,
# and hold's limit, for hold's reason.
add_test(NAME blackbox.timegroup.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timegroup.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.timegroup.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)
add_test(NAME blackbox.timegroup.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/timegroup.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.timegroup.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>;PYTHONUNBUFFERED=1" TIMEOUT 400)

# DOH! END TO END (namespace draft 24.15): the GO pressed too soon and taken back,
# over a real Tracktion graph and two mock devices on real sockets - a console that
# takes back and a lighting desk left to its operator. What each device received,
# message by message, what the render heard (the Doh fade, the carry-on over the
# de-click, the bed made again fading in, a forgotten resume from its top), the
# report, a press past the window refused, and the session replayed. A reading the
# tick places is judged off CI only, and voided where the tick ran behind its audio
# by more than the reading missed.
add_test(NAME blackbox.doh.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/go_doh.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.doh.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.doh.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/go_doh.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.doh.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# Pause / Play end to end (PRD §3.34): a hosted session held and let go over
# OSC - the ramp read back where a show that never paused would read it, a wait
# that counts show time and not the pause, a GO refused paused, Esc while paused
# leaving nothing of the paused sound - and the session replayed.
add_test(NAME blackbox.pause.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pause.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.pause.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.pause.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/pause.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.pause.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# A cue's insert, heard (Phase 9a, PR 9a.8): the test-gain child on a voice through
# the sandbox - the render at a half, a value written through the p<n> door moving it,
# the kill switch taking the child down and the voice silent, never dry, through the
# relaunch it kills again (namespace draft 18.13), the entry reading failed in words,
# and the session replaying with no child at all.
add_test(NAME blackbox.phase9a-fx.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9a_fx.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.phase9a-fx.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9a-fx.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9a_fx.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9a-fx.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# Finishing plugin hosting, stage 3 (the author's decision of 2026-09-26: a Scan
# button in the app): `plugin.scan lv2 <folder>` runs the command line's own scan
# as a child of a serve with an engine folder of its own, /godot/plugin/scan/...
# says where it is, the known list read again offers the in-tree test bundle's
# two plugins as LV2, a locked show refuses the scan in the log, and the session
# replays with no scan launched.
add_test(NAME blackbox.phase9b-plugins.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_plugins.py"
                 --wfg-locale=C "--lv2-folder=${GODOT_TEST_LV2_FOLDER}")
set_tests_properties(blackbox.phase9b-plugins.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9b-plugins.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_plugins.py"
                 --wfg-locale=${WFG_LOCALE_FR} "--lv2-folder=${GODOT_TEST_LV2_FOLDER}")
set_tests_properties(blackbox.phase9b-plugins.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# Stage 8 (the author's decision of 2026-09-26: a plugin can make a mono cue
# stereo), heard: a mono cue through the widening test child on a stereo direct
# out comes out as two different sides; switched out, the mono cue on both at
# full as ever; on a mono bus, the two sides summed at a half each; and the
# session replays with no child.
add_test(NAME blackbox.phase9b-width.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_width.py" --wfg-locale=C)
set_tests_properties(blackbox.phase9b-width.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9b-width.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_width.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9b-width.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# The live rack, stage 9b.2 (namespace draft 18.10): named inputs made over OSC
# on a hosted interface fed from a WAV, one meter reading a tone and one
# silence with nothing listening, one input past the interface saying so, and
# the session replayed.
add_test(NAME blackbox.phase9b-inputs.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_inputs.py" --wfg-locale=C)
set_tests_properties(blackbox.phase9b-inputs.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9b-inputs.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_inputs.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9b-inputs.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# The live rack, stage 9b.5 (namespace draft 18.5 and 18.10): the `mic` fixture on
# a hosted interface fed a steady input, its mic cue heard through Vox 1's own
# test-gain child after rising over its fade-in, Esc ringing out and freeing the
# channel, the child killed mid-cue and the voice silent through its relaunch, never
# dry (namespace draft 18.13), with the failure in words, a double Esc ending it at
# once, and the session replayed with no child.
add_test(NAME blackbox.phase9b-mic.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_mic.py" --wfg-locale=C)
set_tests_properties(blackbox.phase9b-mic.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9b-mic.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9b_mic.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9b-mic.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# Live sampling, stage 9c.3 (namespace draft 19.3 and 19.6): the `take` fixture on a
# hosted interface fed a steady input - a mic cue whose take waits, Rec and Loop it
# from transport cues, silent while it records, the take looping at a half through
# the plugin before the recorder, the points ridden, a layer laid a pass at a time and
# undone, the take held by Esc and looped again by a later mic cue with its input
# heard through, cleared, a double Esc, and the session replayed with no audio.
add_test(NAME blackbox.phase9c-take.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9c_take.py" --wfg-locale=C)
set_tests_properties(blackbox.phase9c-take.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)
add_test(NAME blackbox.phase9c-take.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/phase9c_take.py" --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.phase9c-take.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# The client the engine serves at /ui: the page, its files as bytes, and since
# PR 5.10 its modules - the graph walked from app.js the way a browser loads it,
# every file answering as JavaScript and none left that nothing imports - and
# its gesture table, every command named in gestures/commands.json checked
# against `wfg commands` for its name and the arguments each gesture sends.
add_test(NAME blackbox.client.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/client_page.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.client.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

add_test(NAME blackbox.client.fr_FR
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/client_page.py"
                 --wfg-locale=${WFG_LOCALE_FR})
set_tests_properties(blackbox.client.fr_FR PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

# -----------------------------------------------------------------------------
# The console's pure modules under `node --test` (PR 5.18, namespace section
# 14.3): the OSC encoder against the bytes tests/OscCodecTests.cpp writes by
# hand, the keyed reconciler counted on the least DOM it touches, and the read
# half's model. tests/console/*.test.mjs, importing the page's own files from
# clients/console exactly as the browser does - nothing is copied or built.
#
# OPTIONAL, AND SAID SO. Node.js is the one dependency this adds and nothing
# else needs it: a machine without one configures, builds and tests as before,
# and this prints why the entry is missing rather than leaving it to be
# noticed. The version matters too. The page's files are ES modules ending in
# .js with no package.json to say so - the browser knows from the page's
# <script type="module">, and the folder stays the page and nothing else - and
# Node reads such a file as a module only from 22.7, or from 20.19 on the 20
# line. An older one would fail every import, which is a machine's problem and
# not the page's, so it is skipped with the reason as well.
#
# ONE ENTRY, NOT ONE PER LOCALE. Nothing here formats a number for a person:
# JavaScript's number-to-text is not locale-aware, and the bytes on the wire
# never were.
find_program(WFG_NODE node)

if(WFG_NODE)
    execute_process(COMMAND "${WFG_NODE}" --version
                    OUTPUT_VARIABLE WFG_NODE_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)

    set(WFG_NODE_MAJOR 0)
    set(WFG_NODE_MINOR 0)

    if(WFG_NODE_VERSION MATCHES "^v([0-9]+)\\.([0-9]+)")
        set(WFG_NODE_MAJOR ${CMAKE_MATCH_1})
        set(WFG_NODE_MINOR ${CMAKE_MATCH_2})
    endif()

    if(WFG_NODE_MAJOR GREATER 22
       OR (WFG_NODE_MAJOR EQUAL 22 AND WFG_NODE_MINOR GREATER_EQUAL 7)
       OR (WFG_NODE_MAJOR EQUAL 20 AND WFG_NODE_MINOR GREATER_EQUAL 19))
        file(GLOB WFG_CONSOLE_TESTS CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/console/*.test.mjs")

        add_test(NAME console.unit COMMAND "${WFG_NODE}" --test ${WFG_CONSOLE_TESTS})
        set_tests_properties(console.unit PROPERTIES TIMEOUT 120)

        message(STATUS "console.unit: node ${WFG_NODE_VERSION} at ${WFG_NODE}")
    else()
        message(STATUS "console.unit: skipped - node ${WFG_NODE_VERSION} at ${WFG_NODE} is older than "
                       "22.7 (or 20.19), and reads the page's .js files as CommonJS")
    endif()
else()
    message(STATUS "console.unit: skipped - no node on this machine; the console's pure modules "
                   "are tested where Node.js 22.7 or later is installed")
endif()

# -----------------------------------------------------------------------------
# THE LONGEST DRIVERS START FIRST (2026-10-13), once ctest runs several entries
# at once. ctest starts the costliest first, and on a build tree that has never
# run - every CI run - the only costs it knows are these: a COST of what each
# took on the slowest runner in the last green CI run (38287091845), so they are
# under way while the short ones fill the gaps, rather than starting last and
# being the tail everything waits behind. macOS is where they are long - the
# wires driver 222 s there against 17 s on Windows, the hold 151 s against 8 s.
# The unit shards' COST is set where they are made. A COST set here stays as
# set; an entry with none is costed by ctest's own measurement of its last run
# (Testing/Temporary/CTestCostData.txt), which a CI build tree never has.
# -----------------------------------------------------------------------------
set_tests_properties(blackbox.wires.C blackbox.wires.fr_FR PROPERTIES COST 225)
set_tests_properties(blackbox.hold.C blackbox.hold.fr_FR PROPERTIES COST 150)
set_tests_properties(blackbox.crash-write.C blackbox.crash-write.fr_FR PROPERTIES COST 110)
# The doh and first-sound drivers, long on macOS too, are costed with the
# `realtime` chain below.

# THE AUDIO-DEVICE CHAIN STARTS FIRST OF ALL. The entries that take turns on the
# `audio-device` lock - unit.<locale>.devices, ui.live-device.<locale> and
# blackbox.device.C - are a chain no number of jobs can overlap, and on a box
# with interfaces it is the longest thing in the suite: at twelve jobs on the
# Windows box (2026-10-13) it was 318 s of a 341 s run when it started 24 s in,
# behind drivers costed above it. Costed above everything, it starts at once
# and the suite runs beside it. How long it is is the interfaces' doing - the
# devices file alone took 83 s there in the morning and 209 s two hours later,
# on the same build, every enumeration of the machine's interfaces three
# times slower - so on that box the local run takes as long as the chain. On a
# runner, with no interface, it is a few seconds and its place costs nothing.
set_tests_properties(unit.C.devices unit.fr_FR.devices blackbox.device.C PROPERTIES COST 300)
if(WIN32)
    set_tests_properties(ui.live-device.C ui.live-device.fr_FR PROPERTIES COST 300)
endif()

# -----------------------------------------------------------------------------
# THE DRIVERS THAT JUDGE AN ENGINE'S TIME TAKE TURNS (2026-10-13), on the
# `realtime` lock - on CI; see the lanes below for a workstation - while the
# unit shards and the other drivers run beside them.
#
# WHY. The first CI run with four entries at once (38350416745) failed eleven of
# them on the Windows runner, every one a driver of this kind, and the log says
# what they had in common: up to four of them were running at the same time,
# and with a main and its backup that was seven or eight engines on four
# processors. An engine's audio and tick threads run at the audio priority, so
# an engine that falls behind catches up block after block on a whole processor;
# with more engines than processors behind at once, none kept time. Then the
# render written beside a hosted engine dropped whole seconds when its writer
# could not keep up - the first sound written a second before the GO it
# followed, a plugin's half-level read in a window that was really after Esc, a
# stretched cue a second short (M14) - and what a driver waits for in its own
# wall time came and went before it looked: a footer's run, a claim's wait, a
# tape that ran out before its fade, a backup that took more than six seconds to
# follow. Against the unit shards, which run at the ordinary priority, one
# engine keeps its time - so on a runner these run one at a time, and only these.
#
# WHICH, and why each: a driver is here when what it checks is the engine
# keeping time - in a render read at the engine's frames, in a plugin's child
# answering by its deadline, in two engines in step, in a state it must see
# before the engine moves on - or when its numbers are the point (M14, M45,
# M48, M51, M52, M57, the timecode's frames).
#   first-sound, phase3, phase4, phase6-sampler, phase9a-eq, doh   the render
#       read at the frames the engine placed a GO, a fade, a press, an advance
#   lane-level, lane-send, lane-record, rate-speed                  the same,
#       and M45, M48, M14 printed from it
#   timecode                                                         LTC and MTC
#       read frame by frame against the samples they were drawn on
#   timegroup                                                        members
#       started on the code's sample, chasers read frame by frame against it
#   phase9a-fx, phase9b-width, phase9b-mic, phase9c-take             a plugin's
#       child answering each block by its deadline, heard in the render
#   pair-link, pair-follow, pair-seat, redundancy-failover          two engines:
#       linked, in step, taking over within the link's timeout
#   osc-curves, process                                              M51, M52
#       and M57, the times a value and a report take (judged off CI)
# And not here, though hosted: media-edit, movie-edit, output-layout,
# import-playback, performance-media, phase6-surfaces, phase9b-inputs,
# phase9b-plugins and timbre read what was played, never when.
#
# ONE AT A TIME ON CI; ONE PER FOUR PROCESSORS ON A MACHINE OF ONE'S OWN. On a
# runner the one lock is the whole of it: four processors keep one engine in
# time beside three shards. A workstation keeps more, and one at a time made the
# chain the floor of the whole run - 700 s of a 700 s run at twelve jobs on the
# author's sixteen-core box, against 341 to 659 s before any lock. So where
# GITHUB_ACTIONS is not set at configure time the drivers are dealt, in turn,
# across one lane per four logical processors - `realtime`, `realtime-1`, ... -
# each lane its own lock: four such drivers at most at once on that box, where
# before nothing held them back, and the whole run green there in 296 and 302 s.
# WFG_REALTIME_LANES overrides the count (1 is CI's arrangement anywhere).
#
# COSTED ABOVE EVERYTHING BUT THE AUDIO-DEVICE CHAIN, so that each starts the
# moment the one before it in its lane ends: one lane is the longest thing on a
# runner - twelve minutes on Windows and twenty-five on macOS, added up from the
# last CI runs that ran one entry at a time - and an entry costed below a
# waiting shard would leave it idle until another slot came free.
# -----------------------------------------------------------------------------
if(DEFINED ENV{GITHUB_ACTIONS})
    set(wfg_lanes_default 1)
else()
    cmake_host_system_information(RESULT wfg_processors QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR wfg_lanes_default "${wfg_processors} / 4")

    if(wfg_lanes_default LESS 1)
        set(wfg_lanes_default 1)
    endif()
endif()

set(WFG_REALTIME_LANES "${wfg_lanes_default}" CACHE STRING
    "How many black-box drivers that judge an engine's time may run at once (tests/CMakeLists.txt): 1 on CI")

if(NOT WFG_REALTIME_LANES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "WFG_REALTIME_LANES is a count of one or more, not \"${WFG_REALTIME_LANES}\"")
endif()

message(STATUS "Go.dot: the drivers that judge an engine's time run ${WFG_REALTIME_LANES} at a time")

set(WFG_REALTIME_DRIVERS
    first-sound phase3 phase4 phase6-sampler phase9a-eq doh
    lane-level lane-send lane-record rate-speed timecode timegroup
    phase9a-fx phase9b-width phase9b-mic phase9c-take
    pair-link pair-follow pair-seat redundancy-failover
    osc-curves process)

set(wfg_lane 0)

foreach(driver IN LISTS WFG_REALTIME_DRIVERS)
    foreach(locale IN ITEMS C fr_FR)
        if(NOT TEST blackbox.${driver}.${locale})
            message(FATAL_ERROR "tests/CMakeLists.txt: the realtime lock names blackbox.${driver}.${locale}, which is no test")
        endif()

        if(wfg_lane EQUAL 0)
            set(wfg_lock realtime)
        else()
            set(wfg_lock realtime-${wfg_lane})
        endif()

        set_tests_properties(blackbox.${driver}.${locale} PROPERTIES RESOURCE_LOCK ${wfg_lock} COST 280)

        math(EXPR wfg_lane "(${wfg_lane} + 1) % ${WFG_REALTIME_LANES}")
    endforeach()
endforeach()

# THE RELEASE TIMING LANE (2026-10-27): the drivers whose checks judge how
# closely the engine keeps time - the realtime lock's, and the four that judge
# a round trip or a takeover without holding it - labelled `timed`, in C only
# (a locale moves no clock). The farm's test presets farm-linux-timed and
# farm-macos-timed run them against a Release build, one at a time.
set(WFG_TIMED_DRIVERS ${WFG_REALTIME_DRIVERS} mqtt script pair-show pair-sample)

foreach(driver IN LISTS WFG_TIMED_DRIVERS)
    if(NOT TEST blackbox.${driver}.C)
        message(FATAL_ERROR "tests/CMakeLists.txt: the timed label names blackbox.${driver}.C, which is no test")
    endif()

    set_property(TEST blackbox.${driver}.C APPEND PROPERTY LABELS timed)
endforeach()

# THE TIME GROUP'S DRIVER HOLDS EVERY LANE (2026-10-14, namespace draft §60.7,
# TG.12). It plays the MTC it judges from inside the engine it judges
# (`--midi-play-loop`), so a starved machine makes its own sender late and the
# quarter frames arrive milliseconds off their moments - which its check that
# MTC's `code.heard` is sparse then reads as the engine's wander. The first full
# run at twelve jobs dealt its C and fr_FR runs to two lanes at once, beside two
# other timing drivers and the unit shards, and that check went red in both (26
# and 20 records a pass, against 8 alone); the two run one after the other, they
# were green. CI, one lane, already runs it alone among the timing drivers and
# voids that check besides. Here it waits for every lane, so no other driver
# that judges time runs beside it.
set(wfg_every_lane realtime)
math(EXPR wfg_last_lane "${WFG_REALTIME_LANES} - 1")

if(wfg_last_lane GREATER 0)
    foreach(wfg_n RANGE 1 ${wfg_last_lane})
        list(APPEND wfg_every_lane realtime-${wfg_n})
    endforeach()
endif()

set_tests_properties(blackbox.timegroup.C blackbox.timegroup.fr_FR PROPERTIES RESOURCE_LOCK "${wfg_every_lane}")
