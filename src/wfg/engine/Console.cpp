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
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/RelaxNg.h>
#include <wfg/engine/tree/OscQueryJson.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
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

#include <algorithm>
#include <clocale>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

    //==============================================================================
    /*  `wfg canon <file>` - read a show document, write it back canonically.

        The point is that canonical form is REACHABLE by hand. Someone editing a
        show file in a text editor, or generating one from a spreadsheet, can run
        this and get the spelling the engine would have produced - so a later
        diff shows what they changed rather than how they typed it.

        It also refuses. An unknown attribute, a number out of range, a duplicate
        identifier: each is reported with the element it is in, and nothing is
        written. A file that half-canonicalises would be worse than one that did
        not, because the parts that failed would look like the parts that worked.

        --in-place rewrites the file; without it the result goes to stdout, which
        is what makes `wfg canon a.xml | diff - a.xml` a one-line check.
    */
    int runCanon (const juce::ArgumentList& args)
    {
        const auto path = args.arguments.size() > 1 ? args.arguments[1].text : juce::String();

        if (path.isEmpty())
        {
            std::cerr << "wfg canon: give me a show document to canonicalise" << std::endl;
            return 2;
        }

        const juce::File file { juce::File::getCurrentWorkingDirectory().getChildFile (path) };

        if (! file.existsAsFile())
        {
            std::cerr << "wfg canon: cannot read " << file.getFullPathName().toStdString() << std::endl;
            return 2;
        }

        wfg::doc::ReadResult result;
        const auto canonical = wfg::doc::CanonicalXml::canonicalise (
            file.loadFileAsString().toStdString(), result);

        if (! result.ok)
        {
            std::cerr << "wfg canon: " << file.getFileName().toStdString()
                      << " is not a valid show document:" << std::endl;

            for (const auto& problem : result.problems)
                std::cerr << "    " << problem << std::endl;

            return 1;
        }

        if (args.containsOption ("--in-place"))
        {
            /*  Written as raw bytes, not with juce::File::replaceWithText: that
                would translate newlines on Windows and the canonical form says
                "
" everywhere. A show file that differs between platforms is
                not canonical. */
            juce::FileOutputStream stream { file };

            if (! stream.openedOk())
            {
                std::cerr << "wfg canon: cannot write " << file.getFullPathName().toStdString() << std::endl;
                return 2;
            }

            stream.setPosition (0);
            stream.truncate();
            stream.write (canonical.data(), canonical.size());
            return 0;
        }

        std::cout << canonical << std::flush;
        return 0;
    }

    //==============================================================================
    /*  Where two files that must match stop matching.

        "They differ" is not a useful thing to tell someone at the end of a CI
        log. The line number and the two spellings of that line usually make the
        cause obvious - a description edited in the CSV, a row moved, a range
        widened - without anybody having to run a diff locally to find out. */
    void reportFirstDifference (const std::string& committed, const std::string& generated)
    {
        const auto split = [] (const std::string& text)
        {
            std::vector<std::string> out;
            std::string current;

            for (const char c : text)
            {
                if (c == '\n')
                {
                    out.push_back (current);
                    current.clear();
                }
                else
                {
                    current += c;
                }
            }

            if (! current.empty())
                out.push_back (current);

            return out;
        };

        const auto left = split (committed);
        const auto right = split (generated);

        for (std::size_t i = 0; i < std::max (left.size(), right.size()); ++i)
        {
            const auto a = i < left.size()  ? left[i]  : std::string ("(end of file)");
            const auto b = i < right.size() ? right[i] : std::string ("(end of file)");

            if (a != b)
            {
                std::cerr << "    first difference at line " << (i + 1) << ':' << std::endl
                          << "      committed: " << a << std::endl
                          << "      generated: " << b << std::endl;
                return;
            }
        }
    }

    /*  `wfg schema [--out=<file>] [--check=<file>]`

        Bare, it writes the bundle's RELAX NG grammar to stdout, for reading.
        With --out it writes that grammar to a file, which is how
        docs/schema/show.rng is produced. With --check it compares a committed
        file against what the parameter table generates NOW and fails if the two
        have drifted apart.

        The gate is the reason the verb exists. A grammar generated at build
        time would always agree with the code and would prove nothing; a
        committed one is a promise to everybody outside this repository about
        what a show file may contain, and this is what keeps that promise
        checkable.
    */
    int runSchema (const juce::ArgumentList& args)
    {
        const auto generated = wfg::doc::RelaxNg::generate();

        /*  --out rather than a shell redirect, and the reason is Windows.
            stdout is a TEXT stream there, so `wfg schema > show.rng` turns every
            LF into CRLF and produces a file that --check then rejects on the
            machine that just wrote it. Writing the bytes ourselves is the same
            fix `canon --in-place` makes, for the same reason. */
        if (args.containsOption ("--out"))
        {
            const auto outPath = args.getValueForOption ("--out");

            if (outPath.isEmpty())
            {
                std::cerr << "wfg schema: --out=<file> needs a file to write" << std::endl;
                return 2;
            }

            const juce::File out { juce::File::getCurrentWorkingDirectory().getChildFile (outPath) };

            if (const auto created = out.getParentDirectory().createDirectory(); created.failed())
            {
                std::cerr << "wfg schema: cannot create "
                          << out.getParentDirectory().getFullPathName().toStdString() << std::endl;
                return 2;
            }

            juce::FileOutputStream stream { out };

            if (! stream.openedOk())
            {
                std::cerr << "wfg schema: cannot write " << out.getFullPathName().toStdString()
                          << std::endl;
                return 2;
            }

            stream.setPosition (0);
            stream.truncate();
            stream.write (generated.data(), generated.size());
            return 0;
        }

        if (! args.containsOption ("--check"))
        {
            std::cout << generated << std::flush;
            return 0;
        }

        const auto path = args.getValueForOption ("--check");

        if (path.isEmpty())
        {
            std::cerr << "wfg schema: --check=<file> needs the file to compare against" << std::endl;
            return 2;
        }

        const juce::File file { juce::File::getCurrentWorkingDirectory().getChildFile (path) };

        /*  Raw bytes, not loadFileAsString: this is a byte comparison, and a
            reader that normalised line endings would report a match between a
            CRLF file and an LF one. The whole point is that they are the same
            bytes on all three platforms. */
        juce::MemoryBlock raw;

        if (! file.existsAsFile() || ! file.loadFileAsData (raw))
        {
            std::cerr << "wfg schema: cannot read " << file.getFullPathName().toStdString()
                      << std::endl;
            return 2;
        }

        const std::string committed { static_cast<const char*> (raw.getData()), raw.getSize() };

        if (committed == generated)
            return 0;

        std::cerr << "wfg schema: " << file.getFileName().toStdString()
                  << " is not what the parameter table generates." << std::endl;

        reportFirstDifference (committed, generated);

        std::cerr << "    the table is the source; regenerate with:  wfg schema --out="
                  << file.getFileName().toStdString() << std::endl;

        return 1;
    }

    /*  `wfg tree <bundle-or-file> [--address=<addr>]`

        The parameter tree as OSCQuery JSON, without a server in the way.

        The same reasoning as `wfg canon` and `wfg schema`: the thing the engine
        will put on a socket in PR 1.9 is worth being able to look at now, from
        a shell, with no client to install and no port to guess. It is also what
        makes the namespace reviewable - a diff of this output between two
        commits says exactly what a client would see change.

        --address narrows it to a subtree, which is the difference between
        reading one cue and reading four thousand lines.
    */
    int runTree (const juce::ArgumentList& args)
    {
        const auto path = args.arguments.size() > 1 ? args.arguments[1].text : juce::String();

        if (path.isEmpty())
        {
            std::cerr << "wfg tree: give me a bundle folder or a show document" << std::endl;
            return 2;
        }

        const juce::File target { juce::File::getCurrentWorkingDirectory().getChildFile (path) };

        wfg::doc::ShowDocument document;
        wfg::doc::ReadResult result;

        if (target.isDirectory())
            result = wfg::doc::Bundle::open (target, document);
        else if (target.existsAsFile())
            result = wfg::doc::CanonicalXml::read (target.loadFileAsString().toStdString(), document);
        else
        {
            std::cerr << "wfg tree: cannot read " << target.getFullPathName().toStdString()
                      << std::endl;
            return 2;
        }

        if (! result.ok)
        {
            std::cerr << "wfg tree: " << target.getFileName().toStdString()
                      << " could not be loaded:" << std::endl;

            for (const auto& problem : result.problems)
                std::cerr << "    " << problem << std::endl;

            return 1;
        }

        /*  Wired exactly as `serve` will wire it, so what this prints is what a
            client would get rather than an approximation of it. */
        wfg::Engine engine;
        wfg::tree::TouchTable touches;
        wfg::tree::MountTable mounts;

        wfg::doc::registerDocumentCommands (engine.commands(), document);
        wfg::tree::registerTreeCommands (engine.commands(), touches);
        wfg::tree::registerMountCommands (engine.commands(), document, mounts, target);

        /*  The mounts are loaded before the first publish, so what this prints
            includes somebody else's namespace at its own prefix. A mount that
            fails is reported and costs only that target: an unreadable
            description of one processor must not stop a person reading the
            rest of their show. */
        if (target.isDirectory())
            for (const auto& problem :
                   wfg::tree::loadAllMountsFromBundle (document, mounts, target))
                std::cerr << "    " << problem << std::endl;

        wfg::tree::ParameterTree parameters { document, engine.commands(), mounts };

        wfg::tree::EngineState state;
        state.version = WFG_VERSION;
        state.documentPath = target.getFullPathName().toStdString();
        state.documentName = target.getFileNameWithoutExtension().toStdString();

        const auto snapshot = parameters.publish (0, state);

        const auto address = args.containsOption ("--address")
                               ? args.getValueForOption ("--address").toStdString()
                               : std::string ("/");

        const auto json = wfg::tree::OscQueryJson::describe (*snapshot, address);

        if (json.empty())
        {
            std::cerr << "wfg tree: no node at " << address << std::endl;
            return 1;
        }

        std::cout << json << std::flush;
        return 0;
    }

    /*  `wfg validate <bundle-or-file>`

        Takes a bundle folder or a bare show document, because both are things
        someone has on disk and wants an opinion about.

        Three exit codes, distinct on purpose and matching `wfg replay`:
        0 nothing to say, 1 it loaded but something is wrong (or it did not
        load), 2 there was nothing there to read. A script that treats "invalid
        show" and "wrong path" the same way will eventually report a green build
        for a directory that does not exist.

        This is the INSIDE opinion, and it is only half the check: it runs the
        engine's own schema against a document the engine's own reader parsed,
        so it cannot catch a mistake both halves share. scripts/validate-show.py
        runs the generated grammar through lxml for the other half.
    */
    int runValidate (const juce::ArgumentList& args)
    {
        const auto path = args.arguments.size() > 1 ? args.arguments[1].text : juce::String();

        if (path.isEmpty())
        {
            std::cerr << "wfg validate: give me a bundle folder or a show document" << std::endl;
            return 2;
        }

        const juce::File target { juce::File::getCurrentWorkingDirectory().getChildFile (path) };

        wfg::doc::ShowDocument document;
        wfg::doc::ReadResult result;
        std::string what;

        if (target.isDirectory())
        {
            what = "bundle " + target.getFileName().toStdString();
            result = wfg::doc::Bundle::open (target, document);
        }
        else if (target.existsAsFile())
        {
            what = target.getFileName().toStdString();
            result = wfg::doc::CanonicalXml::read (target.loadFileAsString().toStdString(),
                                                  document);
        }
        else
        {
            std::cerr << "wfg validate: cannot read "
                      << target.getFullPathName().toStdString() << std::endl;
            return 2;
        }

        for (const auto& problem : result.problems)
            std::cerr << "    " << problem << std::endl;

        if (! result.ok)
        {
            std::cerr << "wfg validate: " << what << " could not be loaded" << std::endl;
            return 1;
        }

        if (! result.problems.empty())
        {
            std::cerr << "wfg validate: " << what
                      << " loaded, but the problems above need attention" << std::endl;
            return 1;
        }

        std::cout << what << " is valid" << std::endl;
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

    app.addCommand ({ "canon",
                      "canon <file> [--in-place]",
                      "Rewrites a show document in canonical form, or reports why it cannot",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runCanon (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    app.addCommand ({ "validate",
                      "validate <bundle-or-file>",
                      "Checks a bundle or a show document against the schema and reports every problem",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runValidate (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    app.addCommand ({ "tree",
                      "tree <bundle-or-file> [--address=<addr>]",
                      "Prints the parameter tree as OSCQuery JSON, with no server in the way",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runTree (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    app.addCommand ({ "schema",
                      "schema [--out=<file>] [--check=<file>]",
                      "Writes the bundle's RELAX NG grammar, or checks a committed copy against it",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runSchema (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

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
