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
    PHASE 0 SCAFFOLD — replaced wholesale in Phase 1.

    This is not an engine. It is the one translation unit in src/ that names a
    JUCE type and a Tracktion type, so that a mis-wired ThirdParty tree fails
    here, at compile and link time, instead of producing a green build in which
    nothing ever proved the vendor code was reachable.

    Concretely, three things are proved that a bare `cmake --build` does not:

      * the JUCE macro environment from wfg::deps arrives (without
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED these includes are a hard #error
        at juce_TargetPlatform.h:53-68, not a link failure);
      * Tracktion Engine's compiled code is actually linked — see the note on
        Engine::getVersion() below;
      * Tracktion's header-only core types compile in OUR translation unit.
*/

#include <wfg/engine/Boot.h>

/*
    juce_core and juce_events are what this file uses directly and are named
    directly, even though <tracktion_engine/tracktion_engine.h> would drag both
    in: an explicit include survives a Tracktion header reshuffle, an implicit
    one does not.

    tracktion_engine.h itself pulls juce_audio_basics, juce_audio_utils,
    juce_dsp, juce_osc and juce_gui_extra, and — through tracktion_graph —
    tracktion_core, which is where TimePosition and TimeDuration live.

    TWO NAMING TRAPS, recorded here because this is the file where someone
    would first reach for either name:
      * tracktion_engine.h:72 does a bare  #undef __TEXT  . Anything in this
        tree that defines or relies on __TEXT after this point is undefined.
      * tracktion_engine_playback.cpp:124-153 #undefs and redefines VERSION
        mid-translation-unit. Never call one of our macros VERSION; ours are
        WFG_-prefixed for exactly this reason.
*/
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <iostream>

namespace
{
    //==============================================================================
    /*  `wfg --version` — three lines, exit 0.

        Line 3 is the Tracktion Engine LINK PROOF and the reason this command
        exists at all. Engine::getVersion() is an out-of-line static defined at
        tracktion_Engine.cpp:120, so calling it forces the linker to pull that
        translation unit out of wfg_thirdparty. It constructs nothing: an
        Engine instance builds fifteen subsystems and writes PropertyStorage
        into the user's application-data directory, which is engine behaviour
        and has no place in a toolchain proof.

        DO NOT assert on the number this returns. At the v3.2.0 tag it still
        reports "Tracktion Engine v3.1.0" — measured, not inferred. The tests
        assert the prefix only; see tests/ToolchainTests.cpp.
    */
    void printVersion()
    {
        std::cout << WFG_PRODUCT_NAME " (wfg) " WFG_VERSION "\n"
                  << "juce: "      << juce::SystemStats::getJUCEVersion().toStdString()          << "\n"
                  << "tracktion: " << tracktion::engine::Engine::getVersion().toStdString()      << std::endl;
    }

    //==============================================================================
    /*  `wfg selftest` — exit 0, and the LAST line of stdout is exactly
        "selftest: ok". ctest matches on the exit code; the fixed last line is
        there so a human running the binary by hand gets the same answer.
    */
    void runSelftest()
    {
        // Proves the JUCE message thread stands up in a headless process. This
        // opens no X display on Linux: ScopedJuceInitialiser_GUI only reaches
        // MessageManager::getInstance() (juce_MessageManager.cpp:507-527), which
        // is why the Linux CI job installs no xvfb. If a display error ever does
        // appear in ctest, the fix is `xvfb-run -a` on the ctest step, and this
        // comment should get the date it stopped being true.
        const juce::ScopedJuceInitialiser_GUI juceInitialiser;

        // Proves tracktion_core's header-only time types compile and are usable
        // in our own translation unit. These are the types Phase 1's cue model
        // and Phase 2's tick clock are built on, so a failure here is worth
        // catching before either exists.
        //
        // The numbers are arbitrary arithmetic. They are NOT a sample rate, a
        // buffer size or a track count: those three are open author decisions
        // (devplan:49-50) and appear nowhere in this build system.
        const auto start  = tracktion::TimePosition::fromSeconds (2.5);
        const auto length = tracktion::TimeDuration::fromSeconds (0.75);
        const auto end    = start + length;

        std::cout << "product: "   << WFG_PRODUCT_NAME " " WFG_VERSION                       << "\n"
                  << "juce: "      << juce::SystemStats::getJUCEVersion().toStdString()      << "\n"
                  << "tracktion: " << tracktion::engine::Engine::getVersion().toStdString()  << "\n"
                  << "time: " << start.inSeconds() << " s + " << length.inSeconds()
                  << " s = " << end.inSeconds() << " s\n"
                  << "message thread: up" << "\n";

        std::cout << "selftest: ok" << std::endl;
    }
}

//==============================================================================
int wfg::runConsole (int argc, char** argv)
{
    juce::ConsoleApplication app;

    /*  EXACTLY TWO COMMANDS, and no help or default command. That is a scope
        decision, not an omission: a default command makes ConsoleApplication
        return 0 for ANY unrecognised argument (juce_ConsoleApplication.cpp:330-343
        falls through to commandIfNoOthersRecognised), and a Phase 0 binary that
        exits 0 on nonsense is worse than one with no help text.

        As it stands, `wfg` with no arguments — or with an argument we do not
        know — prints "Unrecognised arguments" to stderr and returns 1, via
        ConsoleApplication::fail()'s default return code. Phase 1 gives this a
        real command surface; until then the honest answer is a non-zero exit.

        Both handlers return void. A handler that completes without calling
        ConsoleApplication::fail() IS the success path: findAndRunCommand wraps
        the call in invokeCatchingFailures (juce_ConsoleApplication.cpp:346-357)
        and returns 0. There is no second place to spell the exit code.
    */
    app.addCommand ({ "--version|-v",
                      "--version",
                      "Prints the Go.dot version and the linked JUCE and Tracktion Engine versions",
                      {},
                      [] (const juce::ArgumentList&) { printVersion(); } });

    app.addCommand ({ "selftest",
                      "selftest",
                      "Boots the JUCE message thread, touches the Tracktion time types, exits 0",
                      {},
                      [] (const juce::ArgumentList&) { runSelftest(); } });

    return app.findAndRunCommand (argc, argv);
}
