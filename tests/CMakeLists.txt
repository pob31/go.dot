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
    TreeTests.cpp
    MountTests.cpp
    JsonTests.cpp
    CueListTests.cpp
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
