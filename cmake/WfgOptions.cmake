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

# WfgOptions.cmake — every option() and every cache STRING Go.dot has, in one
# place, so "what can I turn off, and what does it cost me?" is one file to read
# rather than a grep across the tree.
#
# The rule for this file: an entry may only exist here if something in the build
# READS it. An option nothing consumes is not a feature, it is rot — it survives
# refactors because nobody dares delete it and it silently stops meaning what its
# help string says. Phase 9 will add JUCE_PLUGINHOST_* in exactly one place
# (cmake/WfgThirdParty.cmake); adding the switch now, with no hosting code behind
# it, would buy nothing and start that rot early.

# Two of the author's Phase-0 "Needs from the author" (devplan:49-50) are still OPEN:
# the default fixed track count, and the target sample rates / buffer sizes.
# Neither appears anywhere in this build system — not as an option(), not as a cache
# entry, not in a preset, not as a constexpr in a placeholder header — because a
# default here is an ANSWER to his question. Where spike04 needs numbers they live
# inside spikes/spike04_graph_stability/main.cpp as argv, which is exactly what
# "throwaway, never migrates into src/" makes safe. Do not helpfully add them.

# ---------------------------------------------------------------------------
# Build-scope switches
# ---------------------------------------------------------------------------

# Turned OFF by a packager who wants only the `wfg` binary and has no interest in
# building or running the suite. Note what this does NOT save: the JUCE and
# Tracktion module compile — 31 translation units and the overwhelming majority of
# the wall-clock build — happens once in wfg_thirdparty, which `wfg` links too.
# Expect this to save seconds, not minutes. Anyone turning it off hoping for a fast
# build is turning off the wrong thing.
option(WFG_BUILD_TESTS
    "Build wfg_tests and register the ctest suite (does NOT save the JUCE/TE compile)" ON)

# Spikes are throwaway validation programs (devplan:19 — "they live in spikes/,
# never migrate into src/"). Default OFF so throwaway code can never break the
# product build: a spike that stops compiling after a TE bump must redden its own
# clearly-labelled CI job, not everyone's `cmake --build`.
# ON in exactly two places: the `spikes` preset and the `spikes` CI job.
option(WFG_BUILD_SPIKES
    "Build the PRD 6.1 Tracktion Engine validation spikes (throwaway code)" OFF)

# ON only in the `strict` preset (and in `strict-ci`, which is that preset plus the
# two ccache launchers; CI runs the -ci variant as a second Linux build over a warm
# ccache, while `strict` itself stays free of ccache so a contributor on any of the
# three platforms can run it without installing one). Default OFF because juce::juce_recommended_warning_flags is genuinely
# strict (-Wconversion, -Wsign-conversion, -Wshadow, -Wfloat-equal, -Wswitch-enum),
# and a JUCE or TE bump that reddens one header must not redden every contributor's
# build the morning after the bump lands. CI catches it; contributors keep working.
# Applies to wfg::warnings — OUR code — and never to wfg_thirdparty.
option(WFG_WARNINGS_AS_ERRORS
    "Treat warnings as errors in Go.dot's own code (not in JUCE/TE sources)" OFF)

# devplan:336 makes RT-safety instrumentation a permanent build configuration from
# Phase 2 onward ("RT-safety instrumentation stays on in tests"). Nothing reads
# WFG_RT_CHECKS at runtime yet — there is no audio callback to instrument until
# Phase 2 — so the only thing it does today is prove the wiring: the definition is
# threaded through wfg::deps and tests/ToolchainTests.cpp asserts it actually
# arrives. Getting that plumbing wrong is much cheaper to find now than under a
# real-time assertion two phases from here.
option(WFG_RT_CHECKS
    "Compile Go.dot's real-time safety instrumentation (WFG_RT_CHECKS=1)" ON)

# ---------------------------------------------------------------------------
# Locale — the cross-cutting obligation from devplan:337
# ---------------------------------------------------------------------------
# "Locale test: every serialisation test runs under fr_FR as well as C."
#
# The locale NAME must be a variable and never a literal in a test, because the
# three platforms do not spell it the same way. The Windows UCRT does not accept
# the glibc form: setlocale(LC_ALL, "fr_FR.UTF-8") returns nullptr there, while
# "fr-FR" (the BCP-47 form) works and was measured working on this box —
# printf("%.1f", 0.5) yields "0,5". glibc and macOS want "fr_FR.UTF-8" and reject
# "fr-FR". Hard-coding either spelling makes the test silently skip on two
# platforms out of three, and a locale test that skips is worse than no locale
# test: it reports green.
#
# tests/TestMain.cpp treats an unavailable locale as a FATAL exit(2), never a skip.
if(WIN32)
    set(_wfg_locale_fr_default "fr-FR")
else()
    set(_wfg_locale_fr_default "fr_FR.UTF-8")
endif()

set(WFG_LOCALE_FR "${_wfg_locale_fr_default}" CACHE STRING
    "Name of the French locale to run the test suite under, in this platform's spelling")
unset(_wfg_locale_fr_default)

# ---------------------------------------------------------------------------
# Dependency pins — documentation that the binary can check itself
# ---------------------------------------------------------------------------

# Handed to the binary as the WFG_PIN_JUCE string definition, and asserted at
# RUNTIME by tests/ToolchainTests.cpp against juce::SystemStats::getJUCEVersion().
# That runtime check catches something scripts/check-pins.py structurally cannot:
# check-pins.py compares gitlinks, so it is happy the instant the submodule moves,
# while a stale ccache entry or an un-rebuilt wfg_thirdparty.lib can still be
# linking the PREVIOUS JUCE. Only asking the linked binary what version it is will
# catch that.
set(WFG_PIN_JUCE "8.0.6" CACHE STRING
    "JUCE version pinned by ThirdParty/JUCE; asserted at runtime against SystemStats::getJUCEVersion()")

# Documentation only — there is no runtime counterpart to the check above, and
# there must not be one. MEASURED THIS PASS: at the v3.2.0 tag,
# tracktion::engine::Engine::getVersion() returns "Tracktion Engine v3.1.0".
# TE's own version string is stale in the release it ships with. Any test that
# asserts this number is asserting a bug in someone else's repo and will fail the
# moment they fix it. ToolchainTests.cpp checks the "Tracktion Engine" PREFIX and
# non-emptiness only.
set(WFG_PIN_TE "3.2.0" CACHE STRING
    "Tracktion Engine version pinned by ThirdParty/tracktion_engine (documentation only — see comment)")
