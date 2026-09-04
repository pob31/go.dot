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

#include <wfg/engine/Console.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/log/Replay.h>

/*  juce_core and juce_events are named directly even though tracktion_engine.h
    would drag both in: an explicit include survives a Tracktion header
    reshuffle, an implicit one does not.

    TWO NAMING TRAPS, recorded where someone would first reach for either name:
      * tracktion_engine.h:72 does a bare  #undef __TEXT .
      * tracktion_engine_playback.cpp:124-153 #undefs and redefines VERSION
        mid-translation-unit. Never call one of our macros VERSION; ours are
        WFG_-prefixed for exactly this reason.
*/
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <clocale>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
    //==============================================================================
    /*  --wfg-locale=<name> is stripped before JUCE's argument parser sees it,
        so every verb accepts it and no verb has to declare it.

        In-process setlocale rather than an environment variable: it is the only
        reading of the locale rule that behaves identically on glibc, the macOS
        libc and the Windows UCRT, and it is the same reading tests/TestMain.cpp
        makes. A locale the platform rejects is a HARD failure, exit 2 - a
        serialisation test that quietly runs under "C" while claiming to run
        under fr_FR is exactly the premiere-night bug the rule exists to prevent.

        The guard is only as strong as the platform underneath it, and the
        Windows UCRT is the lenient one: measured, it ACCEPTS "xx_XX" (parsing it
        as language xx, country XX) and rejects only names it cannot parse at
        all, such as "definitely-not-a-locale". glibc rejects both. That is why
        no test invents a locale name: WFG_LOCALE_FR carries the one spelling
        each platform really has, and it is the only name ever passed here.
    */
    constexpr const char* localeFlag = "--wfg-locale=";

    int applyLocaleAndStrip (int& argc, char** argv)
    {
        const auto flagLength = std::strlen (localeFlag);
        const char* requested = nullptr;
        int out = 0;

        for (int i = 0; i < argc; ++i)
        {
            if (i > 0 && std::strncmp (argv[i], localeFlag, flagLength) == 0)
                requested = argv[i] + flagLength;
            else
                argv[out++] = argv[i];
        }

        argc = out;

        if (requested != nullptr && std::setlocale (LC_ALL, requested) == nullptr)
        {
            std::cerr << "wfg: locale \"" << requested
                      << "\" is not available on this system" << std::endl;
            return 2;
        }

        return 0;
    }

    //==============================================================================
    /*  `wfg --version` - three lines, exit 0.

        Line 3 is the Tracktion Engine LINK PROOF and half the reason this
        command exists. Engine::getVersion() is an out-of-line static, so calling
        it forces the linker to pull that translation unit out of
        wfg_thirdparty. It constructs nothing: a tracktion Engine instance builds
        fifteen subsystems and writes PropertyStorage into the user's
        application-data directory, which has no place in a version string.

        DO NOT assert on the number it returns. At the develop (3.5.0) tag it
        still reports "Tracktion Engine v3.1.0" - measured, not inferred. The
        tests assert the prefix only; see tests/ToolchainTests.cpp.
    */
    void printVersion()
    {
        std::cout << WFG_PRODUCT_NAME " (wfg) " WFG_VERSION "\n"
                  << "juce: "      << juce::SystemStats::getJUCEVersion().toStdString()     << "\n"
                  << "tracktion: " << tracktion::engine::Engine::getVersion().toStdString() << std::endl;
    }

    //==============================================================================
    /*  `wfg selftest` - exit 0, and the LAST line of stdout is exactly
        "selftest: ok". ctest matches on the exit code; the fixed last line is
        there so a human running the binary by hand gets the same answer.
    */
    void runSelftest()
    {
        // Proves the JUCE message thread stands up in a headless process. This
        // opens no X display on Linux: ScopedJuceInitialiser_GUI only reaches
        // MessageManager::getInstance(), which is why the Linux CI job installs
        // no xvfb.
        const juce::ScopedJuceInitialiser_GUI juceInitialiser;

        // Proves tracktion_core's header-only time types compile and are usable
        // in our own translation unit. These are the types the tick clock and
        // Phase 2's transport are built on.
        //
        // The numbers are arbitrary arithmetic. They are NOT a sample rate, a
        // buffer size or a track count: those three are open author decisions
        // and appear nowhere in this build system.
        const auto start  = tracktion::TimePosition::fromSeconds (2.5);
        const auto length = tracktion::TimeDuration::fromSeconds (0.75);
        const auto end    = start + length;

        // And proves the engine itself boots, takes an event, applies it on a
        // tick and records it - the whole Phase 1 skeleton in four lines.
        wfg::Engine engine;
        engine.log().openInMemory ({});
        engine.submit (wfg::origin::cli, "noop");
        const auto tickResult = engine.processTick (0);

        std::cout << "product: "   << WFG_PRODUCT_NAME " " WFG_VERSION                     << "\n"
                  << "juce: "      << juce::SystemStats::getJUCEVersion().toStdString()    << "\n"
                  << "tracktion: " << tracktion::engine::Engine::getVersion().toStdString()<< "\n"
                  << "time: " << start.inSeconds() << " s + " << length.inSeconds()
                  << " s = " << end.inSeconds() << " s\n"
                  << "message thread: up" << "\n"
                  << "commands: " << engine.commands().size() << "\n"
                  << "tick 0: applied " << tickResult.applied
                  << ", rejected " << tickResult.rejected
                  << ", dropped " << tickResult.dropped << "\n";

        std::cout << "selftest: ok" << std::endl;
    }

    //==============================================================================
    /*  `wfg commands` - the named command set, one per line, in registration
        order. This is the same list the OSCQuery namespace publishes under
        /godot/cmd, printed for a human: PRD 4.11 says every action is a named
        command, and a list nobody can read is a promise nobody can check.
    */
    void listCommands()
    {
        const wfg::Engine engine;

        for (const auto& command : engine.commands().all())
        {
            std::string signature;

            for (const auto& p : command.params)
            {
                signature += ' ';
                signature += p.optional ? '[' : '<';
                signature += p.name;
                signature += ':';
                signature += p.typeTag;
                signature += p.optional ? ']' : '>';
            }

            std::cout << command.name << signature << "\n    " << command.description << "\n";
        }

        std::cout << std::flush;
    }

    //==============================================================================
    /*  `wfg replay <log>` - read a log, feed it back into a fresh engine, and
        require the engine to write the same log again.

        Exit 0 when it matches, 1 when it diverges (with the first divergences
        named), 2 when the log cannot be read at all. The distinction matters:
        "the log is unreadable" and "the engine is not deterministic" are
        different failures with different remedies.
    */
    int runReplay (const juce::ArgumentList& args)
    {
        const auto path = args.arguments.size() > 1 ? args.arguments[1].text : juce::String();

        if (path.isEmpty())
        {
            std::cerr << "wfg replay: give me a log file to replay" << std::endl;
            return 2;
        }

        const auto logFile = wfg::LogFile::read (path.toStdString());

        if (! logFile)
        {
            std::cerr << "wfg replay: cannot read " << path.toStdString() << std::endl;
            return 2;
        }

        if (! logFile->errors.empty())
        {
            std::cerr << "wfg replay: " << path.toStdString() << " has unreadable lines:" << std::endl;

            for (const auto& e : logFile->errors)
                std::cerr << "    " << e << std::endl;

            return 2;
        }

        wfg::Engine engine;
        const auto result = wfg::replay (engine, *logFile);

        if (! result.ok)
        {
            std::cerr << "wfg replay: FAILED - the replay did not reproduce the log" << std::endl;

            for (const auto& m : result.mismatches)
                std::cerr << "    " << m << std::endl;

            return 1;
        }

        std::cout << "replay: ok - " << result.recordsReplayed
                  << " record(s) reproduced exactly" << std::endl;
        return 0;
    }
}

//==============================================================================
int wfg::runConsole (int argc, char** argv)
{
    if (const auto localeFailure = applyLocaleAndStrip (argc, argv); localeFailure != 0)
        return localeFailure;

    juce::ConsoleApplication app;

    /*  No default command and no help command, and that is a scope decision
        rather than an omission: a default command makes ConsoleApplication
        return 0 for ANY unrecognised argument, and a binary that exits 0 on
        nonsense is worse than one with no help text. `wfg` with no arguments,
        or with an argument we do not know, prints "Unrecognised arguments" to
        stderr and returns 1.

        Handlers that return void take the success path: findAndRunCommand wraps
        the call in invokeCatchingFailures and returns 0 unless fail() was
        called. The ones that need a real exit code call fail() with it. */
    app.addCommand ({ "--version|-v",
                      "--version",
                      "Prints the Go.dot version and the linked JUCE and Tracktion Engine versions",
                      {},
                      [] (const juce::ArgumentList&) { printVersion(); } });

    app.addCommand ({ "selftest",
                      "selftest",
                      "Boots the message thread and the engine, applies one command, exits 0",
                      {},
                      [] (const juce::ArgumentList&) { runSelftest(); } });

    app.addCommand ({ "commands",
                      "commands",
                      "Lists every named command the engine exposes",
                      {},
                      [] (const juce::ArgumentList&) { listCommands(); } });

    app.addCommand ({ "replay",
                      "replay <log>",
                      "Replays an event log into a fresh engine and checks it reproduces it exactly",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runReplay (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    return app.findAndRunCommand (argc, argv);
}
