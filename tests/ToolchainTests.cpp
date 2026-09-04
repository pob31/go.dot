/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
    PHASE 0 SCAFFOLD — four assertions, each about something a green compile
    does NOT prove.

    Every one of these would still pass trivially if it were checking that the
    build compiled, which is why none of them does. They check that the pinned
    versions are the ones actually LINKED, that the compile definitions from
    wfg::deps actually ARRIVED at this target, and that the vendor code is
    reachable at run time rather than merely present on a link line.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

#include <string>

//==============================================================================
TEST_CASE ("toolchain: the linked JUCE is the pinned JUCE")
{
    /*  A RUNTIME pin assertion, and that is the whole value of it.

        scripts/check-pins.py already checks the gitlink SHA, but a git-level
        gate cannot see a stale ccache entry or a .lib left over from before a
        submodule bump. This one can: SystemStats::getJUCEVersion() is compiled
        into juce_core's object code, so if the string here disagrees with
        WFG_PIN_JUCE, what is linked is not what is checked out.

        WFG_PIN_JUCE arrives from cmake/WfgOptions.cmake through wfg::deps.
    */
    const std::string reported = juce::SystemStats::getJUCEVersion().toStdString();

    INFO ("linked JUCE reports: " << reported << ", pin is " << WFG_PIN_JUCE);
    CHECK (reported.find (WFG_PIN_JUCE) != std::string::npos);
}

//==============================================================================
TEST_CASE ("toolchain: Tracktion Engine's compiled code is linked")
{
    /*  Engine::getVersion() is an out-of-line static (tracktion_Engine.cpp:120),
        so this call cannot be satisfied by headers alone — it forces the linker
        to pull that translation unit out of wfg_thirdparty. It constructs
        nothing: an Engine builds fifteen subsystems and writes PropertyStorage
        into the user's application-data directory, which has no business
        running in a unit test on a CI runner.

        DO NOT ASSERT THE NUMBER. At the develop (3.5.0) tag this returns
        "Tracktion Engine v3.1.0" — measured on 2026-09-04, and it disagrees
        with TE's own VERSION.md. Asserting "3.2.0" here would fail today;
        asserting "3.1.0" would silently pass through a future bump that fixed
        the string. The prefix is the part that means something, and
        WFG_PIN_TE stays a documented pin only, checked at the git level by
        scripts/check-pins.py.
    */
    const std::string reported = tracktion::engine::Engine::getVersion().toStdString();

    INFO ("linked Tracktion Engine reports: " << reported);
    CHECK_FALSE (reported.empty());
    CHECK (reported.rfind ("Tracktion Engine", 0) == 0);
}

//==============================================================================
TEST_CASE ("toolchain: the wfg::deps compile definitions reached this target")
{
    /*  A compile-time assertion inside a test case on purpose: if wfg::deps
        did not reach wfg_tests, this file does not build, and the failure names
        the flag that went missing instead of producing a link error or — far
        worse — a second, differently-configured copy of the JUCE headers.

        These four are the module-configuration decisions with consequences
        outside the compiler:
          JUCE_USE_CURL=0                 keeps libcurl4-openssl-dev off the apt line
          JUCE_WEB_BROWSER=0              keeps libwebkit2gtk-4.1-dev off it
          JUCE_MODAL_LOOPS_PERMITTED=0    a modal loop in a show engine is a hang
          JUCE_STRICT_REFCOUNTEDPOINTER=1 hygiene, and TE's own reference sets it
    */
    static_assert (JUCE_USE_CURL == 0,
                   "JUCE_USE_CURL must be 0 - see cmake/WfgThirdParty.cmake and the Linux apt line");
    static_assert (JUCE_WEB_BROWSER == 0,
                   "JUCE_WEB_BROWSER must be 0 - see cmake/WfgThirdParty.cmake and the Linux apt line");
    static_assert (JUCE_MODAL_LOOPS_PERMITTED == 0,
                   "JUCE_MODAL_LOOPS_PERMITTED must be 0 - a modal loop in a show engine is a hang");
    static_assert (JUCE_STRICT_REFCOUNTEDPOINTER == 1,
                   "JUCE_STRICT_REFCOUNTEDPOINTER must be 1");

    // Our own definitions travel the same path, so proving one of them arrives
    // as a usable string literal proves the mechanism, not just the flags.
    CHECK (std::string (WFG_PRODUCT_NAME) == "Go.dot");
    CHECK_FALSE (std::string (WFG_VERSION).empty());
}

//==============================================================================
TEST_CASE ("toolchain: the WFG_RT_CHECKS switch is threaded through")
{
    /*  devplan:336 makes RT-safety instrumentation a permanent build
        configuration from Phase 2 onwards. Nothing reads WFG_RT_CHECKS yet, so
        this case exists purely to keep the switch from rotting between now and
        then: it asserts that when the option is ON the define actually reaches
        a compiled target.

        KNOWN LIMIT, recorded rather than papered over. This translation unit
        cannot distinguish "the option was turned OFF deliberately" from "the
        option is ON and the define failed to arrive" — both look like
        #ifndef WFG_RT_CHECKS from in here, because there is no second define
        carrying the option's state. The #else branch therefore reports rather
        than fails, and the guarantee we actually have is that every CI preset
        leaves the option at its ON default. Closing the gap properly means
        adding a WFG_RT_CHECKS_REQUESTED define in cmake/WfgOptions.cmake, which
        is AUTHOR-A's file and outside this pass's contract.
    */
   #ifdef WFG_RT_CHECKS
    CHECK (WFG_RT_CHECKS == 1);
   #else
    MESSAGE ("WFG_RT_CHECKS is not defined - expected only in a build configured with -DWFG_RT_CHECKS=OFF");
   #endif
}
