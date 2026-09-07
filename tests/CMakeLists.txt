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
    ToolchainTests.cpp
    LocaleTests.cpp
    OscValueTests.cpp
    CommandTests.cpp
    EventLogTests.cpp
    EngineTests.cpp
    DocumentTests.cpp
    BundleTests.cpp
    ClockTests.cpp
    DeviceTests.cpp
    TreeTests.cpp
    AudioTests.cpp
    CueMatrixTests.cpp
    MountTests.cpp
    NetworkCueTests.cpp
    VerifiedCueTests.cpp
    JsonTests.cpp
    CueListTests.cpp
    MediaCueTests.cpp
    RunTests.cpp
    GoTests.cpp
    TriggerTests.cpp
    RangeTests.cpp
    MidiTests.cpp
    OscCodecTests.cpp
    SimpleWebToolchainTests.cpp
    SpatcoreToolchainTests.cpp
    OscQueryTests.cpp)

# Where the hand-authored fixtures live. Passed as a definition rather than
# copied next to the binary: the fixtures are inputs to read, not artefacts to
# ship, and a copy step is one more thing that can be stale.
target_compile_definitions(wfg_tests PRIVATE
    WFG_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    # The repository root, for the one committed artefact that is not a fixture:
    # docs/schema/show.rng is published, so it lives where a reader of the docs
    # will look for it rather than where the tests keep their input.
    WFG_REPO_ROOT="${CMAKE_SOURCE_DIR}")

target_link_libraries(wfg_tests PRIVATE wfg::engine wfg::warnings)

# -----------------------------------------------------------------------------
# The unit suite, run once per locale.
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
# -----------------------------------------------------------------------------
add_test(NAME unit.C     COMMAND wfg_tests --wfg-locale=C)
add_test(NAME unit.fr_FR COMMAND wfg_tests --wfg-locale=${WFG_LOCALE_FR})

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

# The product binary opening a real bundle folder: manifest, show.xml, state.xml.
# Exit 0 means it loaded AND had nothing to report, so a fixture that acquires a
# stale standby fails here rather than being quietly tolerated.
add_test(NAME wfg.validate.C
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
                 --wfg-locale=C)
add_test(NAME wfg.validate.fr_FR
         COMMAND wfg validate "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bundles/minimal"
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
add_test(NAME blackbox.device.C
         COMMAND ${WFG_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/device_serve.py"
                 --wfg-locale=C)
set_tests_properties(blackbox.device.C PROPERTIES
                     ENVIRONMENT "WFG_BINARY=$<TARGET_FILE:wfg>" TIMEOUT 300)

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
