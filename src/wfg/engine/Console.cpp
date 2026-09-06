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
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/midi/MidiInputs.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/RelaxNg.h>
#include <wfg/engine/tree/MountProbe.h>
#include <wfg/engine/tree/OscQueryJson.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/rt/RtCheck.h>
#include <wfg/engine/audio/HostPlayer.h>
#include <wfg/engine/audio/DeviceLayer.h>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/clock/DummyAudioClock.h>
#include <wfg/engine/clock/TickThread.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/oscquery/EngineNamespace.h>
#include <wfg/engine/oscquery/OscQueryServer.h>
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
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
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
        /*  THE WHOLE SET, REGISTERED AGAINST AN EMPTY SHOW.

            A bare Engine knows `noop` and nothing else, and this verb printed
            exactly that until Phase 1's close-out - which made PRD 4.11's
            promise the one thing nobody could check, in the verb that exists to
            check it. ctest runs `wfg commands` under both locales for that
            reason, and it was passing on a list of one.

            Registering against an empty document is honest rather than a
            workaround: the registry is construction-time (CommandRegistry has
            no unregister), and WHICH commands exist does not depend on what is
            in the show. Only whether they would succeed does, and this verb
            does not run them.

            document.save is registered against a folder that is deliberately
            not written to. It appears in the list, which is the point, and
            nothing here invokes it. */
        wfg::Engine engine;

        wfg::doc::ShowDocument document;
        wfg::tree::TouchTable touches;
        wfg::tree::MountTable mounts;
        wfg::cue::RunTable runs;

        /*  Runs draw from their own registry rather than the document's.
            A run is not an object in the show - it is what the machine is
            doing - and the identifier it draws is written into the log as
            the argument the caller left out, so a replay re-supplies it
            rather than having to draw the same number again. */
        auto runIds = wfg::doc::IdRegistry::withSystemEntropy();

        wfg::cue::Focus focus;
        /*  The Runner with no Player until something gives it one. That is a
            complete configuration, not a degraded one: `wfg replay` and
            `wfg tree` have no audio side at all and must still create runs,
            advance standby and produce the same log. */
        wfg::cue::Runner runner { document, runs, runIds, focus };
        wfg::audio::AudioState audioState;

        const auto nowhere = juce::File::getCurrentWorkingDirectory();

        wfg::doc::registerDocumentCommands (engine.commands(), document);
        wfg::cue::registerCueCommands (engine.commands(), document, focus);
        wfg::cue::registerRunCommands (engine.commands(), runs);
        wfg::cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
        wfg::tree::registerTreeCommands (engine.commands(), touches);
        wfg::tree::registerMountCommands (engine.commands(), document, mounts, nowhere);
        wfg::doc::registerBundleCommands (engine.commands(), document, nowhere);
        wfg::audio::registerAudioCommands (engine.commands(), audioState);

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

                //  "as many more of these as you have", spelled the way a shell
                //  usage line spells it.
                if (p.variadic)
                    signature += "...";

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
    /*  `wfg devices` - what this machine can play through, and what to type.

        EXITS 0 WITH NOTHING TO SAY on a machine with no sound card, which is
        every CI runner, and that is why the verb is testable at all. A device
        list is a fact about the machine; an empty one is a fact too, and making
        it an error would mean the only assertion CI could make about this verb
        is that it fails.

        It is SLOW, deliberately. A device type will list a name without knowing
        anything about its channels or its rates - only the device object knows,
        and creating one means touching the driver. Printing a name and no
        numbers would be printing the half that is easy, and the numbers are the
        half somebody actually needs before they can write --buffer=.
    */
    /*  `wfg midi` - what this machine can listen to and talk to.

        EXITS 0 WITH NOTHING TO SAY on a machine with no MIDI interface, which
        is every CI runner and is why the verb is testable at all. A port list
        is a fact about the machine; an empty one is a fact too, and making it
        an error would mean the only build that could run this test is one with
        hardware plugged into it.

        The names are the ones `--midi-in=` and `--midi-out=` take, so this is
        also the answer to "what do I type". */
    void listMidi()
    {
        const auto inputs = wfg::midi::availableInputs();
        const auto outputs = wfg::midi::availableOutputs();

        if (inputs.empty() && outputs.empty())
        {
            std::cout << "wfg midi: this machine has no MIDI ports" << std::endl;
            return;
        }

        std::cout << "inputs" << (inputs.empty() ? "   (none)" : "") << std::endl;

        for (const auto& name : inputs)
            std::cout << "    " << name << std::endl;

        std::cout << "outputs" << (outputs.empty() ? "   (none)" : "") << std::endl;

        for (const auto& name : outputs)
            std::cout << "    " << name << std::endl;

        std::cout << std::flush;
    }

    void listDevices()
    {
        const auto devices = wfg::audio::availableDevices();

        if (devices.empty())
        {
            std::cout << "wfg devices: this machine has no audio devices" << std::endl;
            return;
        }

        std::string currentType;

        for (const auto& device : devices)
        {
            if (device.type != currentType)
            {
                currentType = device.type;
                std::cout << currentType
                          << (device.isDefaultType ? "   (default)" : "") << std::endl;
            }

            std::cout << "    " << device.name << std::endl;
            std::cout << "        " << device.outputChannels << " out, "
                      << device.inputChannels << " in" << std::endl;

            /*  Both lists are what the DEVICE claims, not what it will grant.
                A driver may decline a rate it advertises, and the only way to
                find out is to open it - which `serve` does, and then reports
                what it actually got. */
            if (! device.sampleRates.empty())
            {
                std::cout << "        rates";

                for (const auto rate : device.sampleRates)
                    std::cout << " " << static_cast<long long> (rate);

                std::cout << std::endl;
            }

            if (! device.bufferSizes.empty())
            {
                std::cout << "        buffers";

                for (const auto size : device.bufferSizes)
                    std::cout << " " << size;

                std::cout << std::endl;
            }
        }
    }

    //==============================================================================
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

        /*  THE ENGINE NEEDS THE SAME COMMANDS THE SESSION HAD, or every record
            replays as `unknown-command` and the comparison is meaningless.

            A bare Engine knows `noop` and nothing else. Replaying a real
            session against one produced a tidy, confident report that six
            applied records had become rejections - which is exactly what it
            would look like if the engine had genuinely stopped being
            deterministic, and it was instead the replay tool holding no show.
            The black-box driver is what surfaced it.

            So `--bundle=` is how a session log is replayed: load the show, wire
            the same command set `serve` wires, and re-execute against that. The
            single-argument form still works and is still worth having - the
            committed skeleton fixture is a log of `noop`s, and it tests the
            log format rather than the document.

            The bundle is opened READ-ONLY as far as the disk is concerned:
            document.save is registered against a folder the caller names with
            --out, or refused. A replay that silently overwrote the show it was
            handed would destroy the evidence it was asked to check. */
        wfg::Engine engine;

        wfg::doc::ShowDocument document;
        wfg::tree::TouchTable touches;
        wfg::tree::MountTable mounts;
        wfg::cue::RunTable runs;

        /*  Runs draw from their own registry rather than the document's.
            A run is not an object in the show - it is what the machine is
            doing - and the identifier it draws is written into the log as
            the argument the caller left out, so a replay re-supplies it
            rather than having to draw the same number again. */
        auto runIds = wfg::doc::IdRegistry::withSystemEntropy();

        wfg::cue::Focus focus;
        /*  The Runner with no Player until something gives it one. That is a
            complete configuration, not a degraded one: `wfg replay` and
            `wfg tree` have no audio side at all and must still create runs,
            advance standby and produce the same log. */
        wfg::cue::Runner runner { document, runs, runIds, focus };
        wfg::audio::AudioState audioState;

        /*  REGISTERED WHETHER OR NOT A BUNDLE WAS GIVEN, unlike everything
            below. `audio.editBuilt` needs no document - it is the machine
            reporting the shape of a graph - and a log carrying one must replay
            as applied on a machine with no sound card and no show, which is
            exactly the guarantee the event exists to provide. */
        wfg::audio::registerAudioCommands (engine.commands(), audioState);

        /*  The run lifecycle, unconditionally and for the same reason. Only
            `audio.arm` reads the document, and it answers unknown-id against an
            empty one - which is the right answer. Everything else is the
            machine reporting what happened to a run, and a log of a performance
            has to replay on a laptop with no show open. */
        wfg::cue::registerRunCommands (engine.commands(), runs);
        wfg::cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

        /*  The mounts, and deliberately no sender. A network cue replayed
            reaches the same tree it reached live and puts nothing on any wire -
            see the note in the bundle branch below. */
        runner.setMounts (&mounts, nullptr);

        const auto bundlePath = args.containsOption ("--bundle")
                                  ? args.getValueForOption ("--bundle")
                                  : juce::String();

        if (bundlePath.isNotEmpty())
        {
            const juce::File bundle {
                juce::File::getCurrentWorkingDirectory().getChildFile (bundlePath) };

            if (! bundle.isDirectory())
            {
                std::cerr << "wfg replay: not a bundle folder: "
                          << bundle.getFullPathName() << std::endl;
                return 2;
            }

            const auto opened = wfg::doc::Bundle::open (bundle, document);

            if (! opened.ok)
            {
                std::cerr << "wfg replay: cannot load " << bundle.getFullPathName()
                          << std::endl;

                for (const auto& problem : opened.problems)
                    std::cerr << "    " << problem << std::endl;

                return 2;
            }

            /*  A WRITE TO A MOUNTED NODE, REPLAYED, AND ON NO ACCOUNT SENT.

                The write has to happen: a session where somebody moved a
                console fader must reach the same tree it reached live, and
                without this the record replays as `bad-address` and a perfectly
                good log looks like a divergence.

                The datagram must NOT happen, and that is the more important
                half. `wfg replay` is what somebody runs at three in the morning
                to find out why a cue misfired, on a laptop that may well be on
                the show network. A replay that also sent would move the rig. So
                there is no sender here, and the absence is the feature. */
            wfg::doc::registerDocumentCommands (
                engine.commands(), document,
                [&mounts] (const std::string& address, const wfg::osc::Value& value)
                {
                    const auto written = mounts.write (address, value);

                    return written.ok
                             ? wfg::Outcome::ok ({ wfg::osc::Value::string (address),
                                                   written.value })
                             : wfg::Outcome::rejected (written.reason);
                });

            wfg::cue::registerCueCommands (engine.commands(), document, focus);
            wfg::tree::registerTreeCommands (engine.commands(), touches);
            wfg::tree::registerMountCommands (engine.commands(), document, mounts, bundle);

            /*  Saving goes to --out, never to the bundle that was handed in.
                Absent, document.save is not registered at all and replays as a
                rejection - which is loud, and better than a replay that wrote
                over the show it was checking. */
            const auto outPath = args.containsOption ("--out")
                                   ? args.getValueForOption ("--out")
                                   : juce::String();

            if (outPath.isNotEmpty())
            {
                const juce::File out {
                    juce::File::getCurrentWorkingDirectory().getChildFile (outPath) };

                out.createDirectory();
                wfg::doc::registerBundleCommands (engine.commands(), document, out);
            }

            for (const auto& problem :
                   wfg::tree::loadAllMountsFromBundle (document, mounts, bundle))
                std::cerr << "    " << problem << std::endl;
        }

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
        wfg::cue::RunTable runs;

        /*  Runs draw from their own registry rather than the document's.
            A run is not an object in the show - it is what the machine is
            doing - and the identifier it draws is written into the log as
            the argument the caller left out, so a replay re-supplies it
            rather than having to draw the same number again. */
        auto runIds = wfg::doc::IdRegistry::withSystemEntropy();

        wfg::cue::Focus focus;
        /*  The Runner with no Player until something gives it one. That is a
            complete configuration, not a degraded one: `wfg replay` and
            `wfg tree` have no audio side at all and must still create runs,
            advance standby and produce the same log. */
        wfg::cue::Runner runner { document, runs, runIds, focus };
        wfg::audio::AudioState audioState;

        wfg::doc::registerDocumentCommands (engine.commands(), document);
        wfg::cue::registerCueCommands (engine.commands(), document, focus);
        wfg::cue::registerRunCommands (engine.commands(), runs);
        wfg::cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
        wfg::tree::registerTreeCommands (engine.commands(), touches);
        wfg::tree::registerMountCommands (engine.commands(), document, mounts, target);
        wfg::audio::registerAudioCommands (engine.commands(), audioState);

        /*  The mounts are loaded before the first publish, so what this prints
            includes somebody else's namespace at its own prefix. A mount that
            fails is reported and costs only that target: an unreadable
            description of one processor must not stop a person reading the
            rest of their show. */
        if (target.isDirectory())
            for (const auto& problem :
                   wfg::tree::loadAllMountsFromBundle (document, mounts, target))
                std::cerr << "    " << problem << std::endl;

        wfg::tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

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

        /*  THE CUES THAT ASK FOR SOMETHING THEIR TARGET CANNOT GIVE, checked
            here as well as at mount-load time, because this is the verb
            somebody runs on a laptop with nothing plugged in - and that is the
            machine they are sitting at when they have time to fix it. It reads
            the document and needs no device, no socket and no mount table. */
        for (auto& problem : wfg::tree::checkNetworkCues (document))
            result.problems.push_back (std::move (problem));

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

    //==========================================================================
    /*  Ctrl-C, and the one flag a signal handler may touch.

        `volatile sig_atomic_t` is not superstition here: it is the only type
        the C standard permits a handler to write and the rest of the program to
        read. Everything else - a std::atomic, a mutex, a call into JUCE - is
        undefined behaviour inside a signal handler, however well it appears to
        work. The handler sets this; the message loop notices. */
    volatile std::sig_atomic_t interrupted = 0;

    extern "C" void onInterrupt (int) { interrupted = 1; }

#if JUCE_MAC
    /*  DECLARED HERE because JUCE does not declare it in a header.
        juce_ApplicationBase.cpp:195 has it as a bare `extern` inside its own
        translation unit, and JUCEApplicationBase::main calls it (:243) before
        running the loop. Without it, runDispatchLoop - which is [NSApp run]
        (juce_MessageManager_mac.mm:323-351) - runs against an NSApplication
        that was never created.

        A console binary that never shows a window still needs it, because the
        run loop itself is NSApplication's. */
    namespace juce { void initialiseNSApplication(); }
#endif

    /*  The JUCE dispatch loop on the MAIN thread, until interrupted.

        Phase 2 needs a message thread - plugin scanning and device callbacks
        are message-thread work - so it is stood up now rather than retrofitted
        around a loop of our own later. Nothing in Phase 1 posts to it; it waits.

        RUN, NOT runUntil. runDispatchLoopUntil is inside
        `#if JUCE_MODAL_LOOPS_PERMITTED` (juce_MessageManager.h:99-106) and this
        build sets that to 0 on purpose - a modal loop in a show engine is a
        hang. runDispatchLoop itself is not gated, so that is what runs.

        WHICH MAKES THE WATCHDOG NECESSARY, and it is not ceremony. A signal
        handler may do exactly one thing portably: write a
        `volatile sig_atomic_t`. It may not call stopDispatchLoop, allocate, or
        touch a mutex, however reliably that appears to work. So the handler
        sets the flag, an ordinary thread notices it and calls stopDispatchLoop
        - which is thread-safe, and posts the quit message the loop is waiting
        for. */
    void runMessageLoopUntilInterrupted()
    {
       #if JUCE_MAC
        juce::initialiseNSApplication();
       #endif

        std::signal (SIGINT, onInterrupt);
        std::signal (SIGTERM, onInterrupt);

        auto* manager = juce::MessageManager::getInstance();

        std::atomic<bool> watching { true };

        std::thread watchdog { [manager, &watching]
                               {
                                   while (watching.load (std::memory_order_relaxed))
                                   {
                                       if (interrupted != 0)
                                       {
                                           manager->stopDispatchLoop();
                                           return;
                                       }

                                       std::this_thread::sleep_for (
                                           std::chrono::milliseconds { 50 });
                                   }
                               } };

        manager->runDispatchLoop();

        watching.store (false, std::memory_order_relaxed);
        watchdog.join();
    }

    /*  What shape the show's audio is, read off the document rather than
        guessed at.

        The track count is Show/Audio/@tracks - the polyphony ceiling, required
        and with no default anywhere, because a new show must say. The rig's
        width is the furthest channel any bus reaches, because a bus is where
        the author declared that a channel exists: PRD §3.9b says a width is
        stated and never inferred, and this is that statement being read back.

        A show with tracks and no buses is refused rather than given a silent
        stereo rig. It is a show that says "play audio" and does not say where,
        and the honest answer to that is a message, not a guess. */
    /*  Where the engine keeps what is its own rather than the show's:
        Tracktion's settings, and the silent placeholder WAV that every resident
        clip sits on until a cue is armed onto it. Per user, shared between
        runs, and outside every bundle. */
    juce::File engineCacheFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("Go.dot")
                 .getChildFile ("engine");
    }

    struct AudioShape
    {
        int tracks = 0;
        int outputs = 0;
        std::string problem;
    };

    AudioShape audioShapeOf (const wfg::doc::ShowDocument& document)
    {
        AudioShape shape;

        const auto audio = document.root().getChildWithName ("Audio");

        if (! audio.isValid())
        {
            shape.problem = "the show has no <Audio> element";
            return shape;
        }

        shape.tracks = static_cast<int> (audio["tracks"]);

        for (const auto bus : audio)
        {
            const auto first = static_cast<int> (bus["firstChannel"]);
            const auto width = static_cast<int> (bus["width"]);

            shape.outputs = std::max (shape.outputs, first + width);
        }

        if (shape.tracks > 0 && shape.outputs <= 0)
        {
            shape.problem = "the show has audio tracks and no buses, so there is"
                            " nowhere for them to go";
            return shape;
        }

        /*  A show with no audio still needs an interface to exist, because the
            hosted device is what advances the clock. Two channels, carrying
            silence, is what a show with nothing to play sounds like. */
        if (shape.outputs <= 0)
            shape.outputs = 2;

        return shape;
    }

    /*  `wfg serve <bundle> --sample-rate=N --buffer=N [--hosted [--render=<wav>]]`
        `                     [--http-port=N] [--osc-port=N] [--log=<file>]`

        The whole of Phase 1, running: a document loaded, a tree published every
        tick, an OSCQuery server answering, a UDP socket taking OSC, and an
        event log recording every one of it.

        THE CLOCK PARAMETERS HAVE NO DEFAULTS, deliberately, exactly as the
        spikes had none. A sample rate Go.dot picked for itself is a sample rate
        nobody chose, and Phase 2 replaces this flag with the rate the device
        actually reports - at which point a default here would be a number
        silently disagreeing with the hardware.

        PORT 0 IS THE INTERESTING CASE and the reason a fix went upstream: both
        ports accept 0, bind an ephemeral one, and PRINT THE NUMBER THEY GOT.
        That is what lets the black-box harness run several instances at once,
        and what lets a second Go.dot start on a machine already running one.
        The two lines are written to stdout in a fixed shape because a driver
        parses them:

            wfg: http 51234
            wfg: osc 51235

        `--hosted` PUTS THE AUDIO GRAPH UNDER THE CLOCK. Without it the blocks
        come from Phase 1's dummy clock, which advances a counter and makes no
        sound. With it they come from a real Tracktion playback graph, generated
        from this show's own <Audio> element, paced on the same schedule -
        `--render=<wav>` then writes what came out, which is how a machine with
        no audio interface can be shown that a cue made a sound. The width of
        the imaginary rig is not a flag: it is the furthest channel the show's
        buses reach, because that is where the author said a channel exists.

        THE MAIN THREAD RUNS THE JUCE DISPATCH LOOP and nothing else. Phase 2
        needs it there - plugin scanning and device callbacks are message-thread
        work - and standing it up now means Phase 2 does not restructure this
        verb. The model belongs to the tick thread; this one waits.
    */
    int runServe (const juce::ArgumentList& args)
    {
        const auto path = args.arguments.size() > 1 ? args.arguments[1].text : juce::String();

        if (path.isEmpty())
        {
            std::cerr << "wfg serve: give me a bundle folder" << std::endl;
            return 2;
        }

        if (! args.containsOption ("--sample-rate") || ! args.containsOption ("--buffer"))
        {
            std::cerr << "wfg serve: --sample-rate=N and --buffer=N are both required.\n"
                         "    They have no defaults on purpose: a rate Go.dot chose for\n"
                         "    itself is a rate nobody chose. Phase 2 reads it from the device."
                      << std::endl;
            return 2;
        }

        const auto sampleRate = args.getValueForOption ("--sample-rate").getIntValue();
        const auto blockSize = args.getValueForOption ("--buffer").getIntValue();
        const auto hosted = args.containsOption ("--hosted");

        /*  `--midi-in=<device>`, repeatable, because a rig has a surface and a
            foot switch and they are two devices.

            THE PORTS ARE A FACT ABOUT THIS MACHINE and never a thing the
            document says - the same rule the audio device follows (§4.9: the
            controller arrives knowing nothing). A show that named its
            interfaces would be a show that only ran in one building.

            Read out of the raw arguments rather than through
            `getValueForOption`, which answers with the first of a repeated
            option and would silently open one device of the two. */
        std::vector<std::string> midiInputNames;

        for (const auto& argument : args.arguments)
            if (argument.text.startsWith ("--midi-in="))
                midiInputNames.push_back (argument.text.fromFirstOccurrenceOf ("=", false, false)
                                            .toStdString());

        /*  `--ui=<directory>` serves a client from `/ui` on the OSCQuery port.

            A DIRECTORY RATHER THAN A COPY IN THE BINARY, while the layout is
            being designed: the author edits the page and presses refresh. A
            page compiled in would mean a rebuild per adjustment, which is the
            wrong loop for the one part of this project that is decided by
            looking at it.

            It is checked here rather than at the first request, because a
            mistyped path should be a sentence on the terminal the moment
            somebody starts the engine and not a 404 twenty minutes later. */
        juce::File clientDirectory;

        if (args.containsOption ("--ui"))
        {
            const auto given = args.getValueForOption ("--ui");
            clientDirectory = juce::File::getCurrentWorkingDirectory()
                                .getChildFile (given);

            if (! clientDirectory.isDirectory())
            {
                std::cerr << "wfg serve: --ui wants a directory, and "
                          << clientDirectory.getFullPathName() << " is not one" << std::endl;
                return 2;
            }

            if (! clientDirectory.getChildFile ("index.html").existsAsFile())
            {
                std::cerr << "wfg serve: no index.html in "
                          << clientDirectory.getFullPathName() << std::endl;
                return 2;
            }
        }

        const juce::File target { juce::File::getCurrentWorkingDirectory().getChildFile (path) };

        if (! target.isDirectory())
        {
            std::cerr << "wfg serve: not a bundle folder: "
                      << target.getFullPathName() << std::endl;
            return 2;
        }

        //  --- the document -----------------------------------------------
        wfg::doc::ShowDocument document;
        const auto result = wfg::doc::Bundle::open (target, document);

        if (! result.ok)
        {
            std::cerr << "wfg serve: " << target.getFileName().toStdString()
                      << " could not be loaded:" << std::endl;

            for (const auto& problem : result.problems)
                std::cerr << "    " << problem << std::endl;

            return 2;
        }

        //  --- the engine and everything it needs --------------------------
        wfg::Engine engine;
        wfg::tree::TouchTable touches;
        wfg::tree::MountTable mounts;
        wfg::cue::RunTable runs;

        /*  Runs draw from their own registry rather than the document's.
            A run is not an object in the show - it is what the machine is
            doing - and the identifier it draws is written into the log as
            the argument the caller left out, so a replay re-supplies it
            rather than having to draw the same number again. */
        auto runIds = wfg::doc::IdRegistry::withSystemEntropy();

        wfg::cue::Focus focus;
        /*  The Runner with no Player until something gives it one. That is a
            complete configuration, not a degraded one: `wfg replay` and
            `wfg tree` have no audio side at all and must still create runs,
            advance standby and produce the same log. */
        wfg::cue::Runner runner { document, runs, runIds, focus };
        wfg::audio::AudioState audioState;

        /*  THE OUTBOUND SIDE OF A MOUNT, which is what stops it being a stub.

            It is declared here, before the socket exists, because the command
            handler below has to capture it and `wfg serve` cannot open its port
            until the OSCQuery namespace is built - and the namespace needs the
            engine, which needs the commands. setSocket closes that loop further
            down; until it is called this sender queues, coalesces and reports
            exactly as it will afterwards, and sends nothing. */
        wfg::tree::MountSender sender;

        /*  AND THE THREAD THAT ASKS. A verified cue reads a value back off the
            target's own OSCQuery server, which is an HTTP exchange with a
            deadline on it - seconds, when a device has gone away. The tick
            thread cannot wait for that, so it does not: the question goes to
            this thread and the answer comes back as a `mount.readback` command,
            applied and logged like everything else the machine learns. */
        wfg::tree::MountProbe probe { engine };

        wfg::doc::registerDocumentCommands (
            engine.commands(), document,
            [&mounts, &sender] (const std::string& address, const wfg::osc::Value& value)
            {
                /*  A WRITE TO SOMEBODY ELSE'S NODE, arriving through the same
                    command as a write to one of ours (PRD 4.11) - one named
                    action, one log record, one refusal vocabulary, whether the
                    address turns out to be a cue's level or a console's fader.

                    Two things happen and they are separate on purpose: the
                    value lands in the mount table, which is what a client reads
                    back and what a replay reproduces; and it is queued for the
                    end of this tick, which is what the other box hears. */
                const auto written = mounts.write (address, value);

                if (! written.ok)
                    return wfg::Outcome::rejected (written.reason);

                if (const auto* declaration = mounts.declarationOf (written.mountId))
                    sender.queue (written.mountId,
                                  { declaration->host, declaration->port },
                                  address, written.value);

                /*  Logged AS APPLIED, so the record carries the value that
                    actually landed rather than the one that was offered - an
                    integer 1 written to a float node is `f:1` in the log, and a
                    replay puts the same bytes on the wire. */
                return wfg::Outcome::ok ({ wfg::osc::Value::string (address), written.value });
            });

        wfg::cue::registerCueCommands (engine.commands(), document, focus);
        wfg::cue::registerRunCommands (engine.commands(), runs);
        wfg::cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
        wfg::tree::registerTreeCommands (engine.commands(), touches);
        wfg::tree::registerMountCommands (engine.commands(), document, mounts, target);
        wfg::doc::registerBundleCommands (engine.commands(), document, target);
        wfg::audio::registerAudioCommands (engine.commands(), audioState);

        runner.setMounts (&mounts, &sender, &probe);

        for (const auto& problem : wfg::tree::loadAllMountsFromBundle (document, mounts, target))
            std::cerr << "    " << problem << std::endl;

        wfg::tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

        wfg::tree::EngineState state;
        state.version = WFG_VERSION;
        state.documentPath = target.getFullPathName().toStdString();
        state.documentName = target.getFileNameWithoutExtension().toStdString();

        /*  THE CLOCK IS CHECKED AND THE TREE PUBLISHED BEFORE ANY SOCKET OPENS,
            and the order is load-bearing rather than tidy.

            Published after the server had started, there was a window - short,
            but a black-box driver hit it on its first request - in which
            `GET /` returned 404 from a process that had already printed its
            port. An empty tree is indistinguishable from a wrong address, so a
            client's honest reaction is to conclude the node does not exist.

            The rate is validated here for the same reason: refusing 44101 Hz
            before anything binds means the failure is a message on stderr and
            an exit code, rather than a server that answers for a while and then
            stops. */
        const auto schedule = wfg::TickClock::create (sampleRate);

        if (! schedule.has_value())
        {
            std::cerr << "wfg serve: " << sampleRate
                      << " Hz does not divide into 50 ticks a second, so a tick"
                         " would not sit on an exact sample."
                      << std::endl;
            return 2;
        }

        state.sampleRate = sampleRate;
        state.blockSize = blockSize;
        state.samplesPerTick = schedule->samplesPerTick();

        auto previous = parameters.publish (0, state);

        //  --- the log ------------------------------------------------------
        const auto logPath = args.containsOption ("--log")
                               ? args.getValueForOption ("--log").toStdString()
                               : std::string();

        if (! logPath.empty())
        {
            if (! engine.log().open (logPath, wfg::doc::Bundle::logHeaderLines (target)))
            {
                std::cerr << "wfg serve: cannot write the log at " << logPath << std::endl;
                return 2;
            }
        }
        else
        {
            engine.log().openInMemory (wfg::doc::Bundle::logHeaderLines (target));
        }

        //  --- the transports ------------------------------------------------
        const auto requestedOsc = args.containsOption ("--osc-port")
                                    ? args.getValueForOption ("--osc-port").getIntValue()
                                    : 8010;

        /*  Constructed, then wired, then started, in that order. The namespace
            is built between the endpoint's construction and its start, which is
            what lets the handler hold it and the namespace hold the endpoint
            without either waiting on the other. */
        wfg::osc::UdpEndpoint udp;

        wfg::oscquery::EngineNamespace nameSpace { engine, parameters, touches, udp };

        /*  The triggers, rebuilt on the tick thread whenever the document moves
            and held here so the clock read below has one too. */
        std::shared_ptr<const wfg::cue::TriggerIndex> triggerIndex;

        /*  THE MIDI INPUTS, opened before the clock starts so that a mistyped
            device name is a sentence on the terminal somebody is still looking
            at rather than a trigger that never fires.

            FATAL, like `--ui` pointed at nothing. A cue that can be fired from
            a foot switch and silently cannot is the failure this whole feature
            exists to avoid, and "the device is not there" is almost always a
            name spelled differently - so the message lists what this machine
            does have, and the remedy is to read it. */
        wfg::midi::MidiInputs midiIn;
        midiIn.sendTo (engine);

        for (const auto& name : midiInputNames)
            midiIn.open (name);

        if (! midiIn.problems().empty())
        {
            for (const auto& problem : midiIn.problems())
                std::cerr << "wfg serve: " << problem << std::endl;

            return 2;
        }

        if (midiIn.count() > 0)
            std::cout << "wfg: listening on " << midiIn.count()
                      << " MIDI input(s)" << std::endl;

        /*  The loop closed. From here a mounted write reaches a socket; before
            it, the same write reached the tree and the log and stopped. */
        sender.setSocket (udp);
        parameters.setSender (&sender);
        probe.start();

        if (! udp.start (requestedOsc,
                         [&engine, &nameSpace] (wfg::osc::Datagram datagram)
                         {
                             const auto decoded = wfg::osc::decode (datagram.bytes.data(),
                                                                    datagram.bytes.size());

                             /*  A packet that never became a command is an `X`
                                 record carrying who sent it, which guard refused
                                 it and the bytes themselves. Dropping it silently
                                 would leave an operator with a surface that does
                                 nothing and no way to find out why. */
                             if (! decoded.ok)
                             {
                                 wfg::Drop drop;
                                 drop.origin = datagram.origin();
                                 drop.reason = decoded.reason;
                                 drop.payload = std::move (datagram.bytes);
                                 engine.submit (std::move (drop));
                                 return;
                             }

                             /*  Not applied here. This is a socket thread; the
                                 tick thread owns the model, and this hands it an
                                 event like every other producer - through the
                                 same seam the WebSocket writes go through, so
                                 one address means one thing on both transports. */
                             nameSpace.write (datagram.origin(), decoded.packet);
                         }))
        {
            std::cerr << "wfg serve: cannot bind the OSC port " << requestedOsc << std::endl;
            return 2;
        }

        const auto requestedHttp = args.containsOption ("--http-port")
                                     ? args.getValueForOption ("--http-port").getIntValue()
                                     : 5010;

        wfg::oscquery::OscQueryServer server;

        if (clientDirectory != juce::File())
            server.serveClientFrom (clientDirectory);

        if (! server.start (requestedHttp, nameSpace))
        {
            std::cerr << "wfg serve: cannot bind the HTTP port " << requestedHttp << std::endl;
            return 2;
        }

        /*  The two lines a driver parses. Flushed, because a harness reading
            them is blocked until they arrive and a buffered stdout would hang
            it until the process exits. */
        std::cout << "wfg: http " << server.boundPort() << std::endl;
        std::cout << "wfg: osc " << udp.boundPort() << std::endl;

        //  --- the clock ------------------------------------------------------
        /*  TWO BLOCK SOURCES, ONE COUNTER, AND NOTHING ABOVE THEM KNOWS WHICH.

            Phase 1's dummy clock advances a sample counter on a paced thread
            and produces no audio. `--hosted` advances the SAME counter by
            running a real block through a real playback graph, on the same
            schedule, and can write what comes out to a WAV - which is the only
            way a runner with no audio interface can be shown that a cue made a
            sound. TickThread takes a SampleClock either way and cannot tell
            them apart, which is what makes the hosted mode a rehearsal of the
            device mode rather than a simulation of it. */
        std::unique_ptr<wfg::DummyAudioClock> dummy;
        std::unique_ptr<wfg::audio::HostedAudioDriver> driver;
        std::unique_ptr<wfg::audio::DeviceAudioDriver> deviceDriver;
        std::unique_ptr<wfg::audio::HostPlayer> player;
        const wfg::SampleClock* blockSource = nullptr;

        const auto deviceName = args.containsOption ("--device")
                                  ? args.getValueForOption ("--device").toStdString()
                                  : std::string {};

        const auto onDevice = args.containsOption ("--device");

        if (onDevice && hosted)
        {
            std::cerr << "wfg serve: --device and --hosted are two different block"
                         " sources; give one" << std::endl;
            return 2;
        }

        if (onDevice)
        {
            /*  A SHOW OFF A SOUND CARD, which is the same program as the two
                lines below it with a different thing deciding when a block
                happens. TickThread takes a SampleClock and cannot tell which. */
            const auto shape = audioShapeOf (document);

            if (! shape.problem.empty())
            {
                std::cerr << "wfg serve: " << shape.problem << std::endl;
                return 2;
            }

            deviceDriver = std::make_unique<wfg::audio::DeviceAudioDriver> (
                             engineCacheFolder().getFullPathName().toStdString());

            wfg::audio::DeviceAudioDriver::Request request;
            request.deviceName = deviceName;
            request.deviceType = args.containsOption ("--device-type")
                                   ? args.getValueForOption ("--device-type").toStdString()
                                   : std::string {};
            request.blockSize = blockSize;
            request.edit.tracks = shape.tracks;

            if (! deviceDriver->open (request))
            {
                std::cerr << "wfg serve --device: " << deviceDriver->lastError() << std::endl;
                std::cerr << "    `wfg devices` lists what this machine has." << std::endl;
                return 2;
            }

            if (const auto duplicates = deviceDriver->host().inspectNodeIds();
                ! duplicates.ok())
            {
                std::cerr << "wfg serve --device: the playback graph has "
                          << duplicates.duplicates << " duplicated node identities out of "
                          << duplicates.nodes << std::endl;
                return 2;
            }

            const auto ids = deviceDriver->host().inspectNodeIds();
            const auto granted = deviceDriver->settings();

            /*  A RATE THAT IS NOT THE ONE ASKED FOR STOPS THE SHOW HERE.

                The tick schedule was built above from `--sample-rate`, and
                everything downstream is arithmetic on it: samples per tick, the
                launch-tick rule, the fade's fifty values a second. A card that
                opened at 44100 while the schedule says 48000 would put every
                one of those 8.8% out - every cue late, every fade the wrong
                length - and nothing would look wrong.

                REFUSING IS THE SAFE READING OF PRD §6.2, whose mismatch policy
                (refuse, warn, or resample) is the author's to settle and is
                deliberately still open. Refusing is the one of the three that
                cannot be wrong quietly, and the message says the number to
                pass, so the remedy is one flag rather than an investigation. */
            if (granted.sampleRate != sampleRate)
            {
                std::cerr << "wfg serve --device: \"" << deviceDriver->deviceName()
                          << "\" opened at " << granted.sampleRate
                          << " Hz, not the " << sampleRate << " Hz this was asked for."
                          << std::endl
                          << "    The rate is the device's to choose (PRD 6.2), and every"
                             " tick is computed from it," << std::endl
                          << "    so re-run with --sample-rate=" << granted.sampleRate
                          << " or set the device to " << sampleRate << " Hz." << std::endl;
                return 2;
            }

            /*  REPORTED WITH THE DEVICE'S NAME, and the numbers it GRANTED
                rather than the ones that were asked for. A driver is entitled
                to open at a rate and a block size of its own choosing - this
                machine answers a request for 256 frames with 480 - and PRD §6.2
                is that the rate is observed, never set. Everything downstream
                is computed from what came back. */
            engine.submit ("engine", "audio.editBuilt",
                           { wfg::osc::Value::string (deviceDriver->deviceName()),
                             wfg::osc::Value::int32 (shape.tracks),
                             wfg::osc::Value::int32 (granted.outputChannels),
                             wfg::osc::Value::int32 (ids.nodes) });

            std::cout << "wfg: audio device \"" << deviceDriver->deviceName() << "\" "
                      << granted.sampleRate << " Hz " << granted.blockSize << " frames "
                      << granted.outputChannels << " outputs " << ids.nodes << " nodes"
                      << std::endl;

            player = std::make_unique<wfg::audio::HostPlayer> (deviceDriver->host(), engine);
            runner.setPlayer (player.get());
            runner.setMediaFolder (target.getChildFile ("media")
                                     .getFullPathName().toStdString());

            blockSource = &deviceDriver->host().clock();
        }
        else if (hosted)
        {
            const auto shape = audioShapeOf (document);

            if (! shape.problem.empty())
            {
                std::cerr << "wfg serve --hosted: " << shape.problem << std::endl;
                return 2;
            }

            /*  THE PER-USER CACHE, NEVER THE BUNDLE. Tracktion keeps its own
                settings here and Go.dot writes the silent placeholder every
                resident clip starts on. Neither is anything the author
                decided, so neither belongs in the show folder: a bundle is
                copied between machines, hashed, and put under version control,
                and a directory that appeared inside it the first time somebody
                pressed play would travel with it. */
            driver = std::make_unique<wfg::audio::HostedAudioDriver> (
                         engineCacheFolder().getFullPathName().toStdString());

            wfg::audio::HostedAudioDriver::Settings hostSettings;
            hostSettings.sampleRate = sampleRate;
            hostSettings.blockSize = blockSize;
            hostSettings.outputChannels = shape.outputs;
            hostSettings.renderFile = args.containsOption ("--render")
                                        ? args.getValueForOption ("--render").toStdString()
                                        : std::string();

            if (! driver->open (hostSettings))
            {
                std::cerr << "wfg serve --hosted: " << driver->lastError() << std::endl;
                return 2;
            }

            /*  THE EDIT IS BUILT HERE AND NEVER AGAIN. PRD §3.25: the graph is
                fixed at show load, and this is show load. It happens before a
                single block goes through, because building it while the pump
                ran would be a structural edit racing the graph that reads it. */
            wfg::audio::EditSpec spec;
            spec.tracks = shape.tracks;

            if (! driver->host().buildEdit (spec))
            {
                std::cerr << "wfg serve --hosted: " << driver->host().lastError() << std::endl;
                return 2;
            }

            /*  Asked once, at load, about the graph that will play. A duplicate
                is a defect rather than a warning - two nodes sharing an id
                adopt one another's state across a rebuild - so it stops the
                show here instead of during it. */
            if (const auto duplicates = driver->host().inspectNodeIds(); ! duplicates.ok())
            {
                std::cerr << "wfg serve --hosted: the playback graph has "
                          << duplicates.duplicates << " duplicated node identities out of "
                          << duplicates.nodes << std::endl;
                return 2;
            }

            /*  THE GRAPH EXISTS, AND THAT IS AN EVENT, not a variable being
                set. State transitions enter the model as logged commands
                applied on the tick they were observed (PRD §3.15, §4.11); a
                replay of this session re-injects this one at the same tick,
                with no engine and no sound card, and produces the same
                `/godot/audio` a client saw live. Setting the fields here
                instead would make the tree depend on when a message thread
                happened to run, and leave nothing in the log to reproduce. */
            const auto ids = driver->host().inspectNodeIds();

            engine.submit ("engine", "audio.editBuilt",
                           { wfg::osc::Value::string ("hosted"),
                             wfg::osc::Value::int32 (shape.tracks),
                             wfg::osc::Value::int32 (shape.outputs),
                             wfg::osc::Value::int32 (ids.nodes) });

            std::cout << "wfg: audio hosted " << shape.tracks << " tracks "
                      << shape.outputs << " outputs " << ids.nodes << " nodes" << std::endl;

            /*  THE CUE LAYER MEETS THE AUDIO SIDE, here and nowhere else.
                Everything above this line is a graph; everything below it is a
                show. The Runner holds a Player and has never heard of
                Tracktion. */
            player = std::make_unique<wfg::audio::HostPlayer> (driver->host(), engine);
            runner.setPlayer (player.get());
            runner.setSamplesPerTick (schedule->samplesPerTick());
            runner.setMediaFolder (target.getChildFile ("media").getFullPathName().toStdString());

            state.launchLatencyTicks = runner.latencyTicks();

            std::cout << "wfg: audio launch latency " << state.launchLatencyTicks
                      << " ticks" << std::endl;

            blockSource = &driver->clock();
        }
        else
        {
            dummy = std::make_unique<wfg::DummyAudioClock> (sampleRate, blockSize);
            blockSource = &dummy->clock();
        }

        wfg::TickThread ticks { engine, *blockSource, *schedule };

        /*  Publish then flush, on the tick thread, once per tick, in that
            order. The snapshot has to be the finished answer to the tick that
            just ran; a push carrying a value from a tick still in progress is a
            push of something nobody decided. */
        /*  BEFORE the tick's commands are drained, so a cue that started or
            ended is applied on the tick it was observed rather than the one
            after. See TickThread::setBeforeTick. */
        /*  THE WALL CLOCK, read here and nowhere inside the engine.

            `Engine.h` states it as an invariant: "nothing in here reads a wall
            clock, which is what makes a replay reproducible". A clock trigger
            needs one, so it is read OUTSIDE - in the serve wiring, on the tick
            thread, once per tick - and a crossing becomes a `trigger.fire`
            record like any other input. A replay re-injects that record and
            consults nothing: the same session reproduces on a machine where it
            is a different time of day, in a different year.

            "Which second it was last time" is machine state and lives here,
            never in the document (§4.10). It starts at -1, which is what makes
            the FIRST tick cross nothing: a show opened at 19:30:00 must not
            fire the 19:30:00 cue because it happened to be started then. */
        int previousSecond = -1;

        ticks.setBeforeTick ([&] (std::int64_t tickIndex)
                             {
                                 runner.beforeTick (engine, tickIndex);

                                 const auto now = juce::Time::getCurrentTime();
                                 const auto second = now.getHours() * 3600
                                                       + now.getMinutes() * 60
                                                       + now.getSeconds();

                                 if (previousSecond >= 0 && second != previousSecond)
                                     if (const auto index = triggerIndex)
                                         for (const auto& id : wfg::cue::clockCrossings (
                                                                 *index, previousSecond, second))
                                             engine.submit ({ "clock", "trigger.fire",
                                                              { wfg::osc::Value::string (id) } });

                                 previousSecond = second;
                             });

        ticks.setAfterTick ([&] (const wfg::Engine::TickResult& outcome)
                            {
                                /*  THE SHOW'S OWN TRAFFIC FIRST, ahead of the
                                    diagnostics and the client push below it.

                                    Everything this tick wrote to a mounted node
                                    leaves here, together, which is what makes
                                    the twelve messages of one GO one gesture
                                    rather than a dribble (PRD 3.4). It goes
                                    first because what follows is a full tree
                                    rebuild and a diff, and a console should not
                                    wait behind a client's screen refresh. */
                                sender.flush();

                                /*  The runtime half of the state, refreshed
                                    before the publish that carries it. Left
                                    out, `/godot/engine/tick` reads 0 for the
                                    life of the process - which is what the
                                    first hand-run of this verb did, and it
                                    looks exactly like a stopped clock. */
                                state.tick = outcome.tick;

                                /*  The audio side's own model, as the tick
                                    thread left it. Both are read here, on that
                                    thread, and go into the same snapshot. */
                                state.audioDevice = audioState.device;
                                state.audioOutputs = audioState.outputs;
                                state.audioStatus = audioState.status;

                                state.lateness = ticks.lateness();
                                state.latenessMax = ticks.latenessMax();

                                /*  THE REFUSAL ITSELF, and not only the count
                                    of them.

                                    OSC has no reply channel: a client that
                                    writes a word to a number gets no answer at
                                    all, and `/godot/engine/lastError` is the
                                    whole of what it can read back instead. The
                                    count was published here and the TEXT was
                                    not - so every refusal in a running engine
                                    arrived as an empty string, and a client
                                    doing the right thing with it showed
                                    nothing. The console client is what found
                                    it, on its first edit.

                                    READ ONLY WHEN THE COUNT HAS MOVED, which
                                    is a mutex and a string copy avoided fifty
                                    times a second for the whole of a show that
                                    is going well. */
                                const auto errorCount = engine.errorCount();

                                if (errorCount != state.errorCount)
                                    state.lastError = engine.lastError();

                                state.errorCount = errorCount;

                                /*  Published every tick, from the tick thread,
                                    like the lateness beside it. The audio
                                    thread only ever increments them. */
                                state.rtViolations = wfg::rt::violations();
                                state.rtForeignAllocations = wfg::rt::foreignAllocations();

                                /*  ANYTHING APPLIED MEANS THE DOCUMENT MAY HAVE
                                    MOVED, so the projection is rebuilt.

                                    Without this the whole serve loop is a
                                    read-only window: ParameterTree caches its
                                    document part and reuses it until told
                                    otherwise, so a write landed in the model,
                                    was logged as applied, was saved to disk -
                                    and was invisible to every client, for ever,
                                    with no error anywhere. The black-box driver
                                    is what found it; nothing in the unit suite
                                    could, because each of those tests publishes
                                    once.

                                    COARSER THAN THE PLAN'S LISTENER, which
                                    would rebuild only the affected subtree.
                                    This rebuilds on any applied command,
                                    including ones that touch no document node
                                    at all - a node.touch costs a full rebuild.
                                    It is correct and it is cheap where it
                                    matters: a tick with nothing in its queue
                                    does no work, and that is almost every tick.
                                    The listener is the Phase 2 refinement, when
                                    a show is large enough for the difference to
                                    be measurable. */
                                if (outcome.applied > 0)
                                    parameters.markStale();

                                /*  AND THE TRIGGERS, republished with the tree
                                    and for the same reason: the matching
                                    happens on whichever thread the input
                                    arrived on - a socket thread for a datagram,
                                    this one for the clock - and none of them
                                    may read a document. So the tick thread
                                    reads it once, here, and publishes something
                                    immutable that anybody can hold. */
                                if (outcome.applied > 0 || triggerIndex == nullptr)
                                {
                                    triggerIndex =
                                        wfg::cue::TriggerIndex::build (document);
                                    nameSpace.publishTriggers (triggerIndex);
                                    midiIn.publishTriggers (triggerIndex);
                                }

                                auto current = parameters.publish (outcome.tick, state);

                                if (previous != nullptr && current != nullptr)
                                    server.publishChanges (wfg::tree::diff (*previous, *current),
                                                           *current, outcome.soleOrigin);

                                previous = std::move (current);
                            });

        if (driver != nullptr)
        {
            if (! driver->start())
            {
                std::cerr << "wfg serve --hosted: " << driver->lastError() << std::endl;
                return 2;
            }
        }
        else
        {
            dummy->start();
        }

        ticks.start();

        /*  The main thread from here on is JUCE's, and only JUCE's. Phase 2
            needs a message thread for plugin scanning and device callbacks, so
            it is stood up now rather than retrofitted around a loop of our own. */
        runMessageLoopUntilInterrupted();

        ticks.stop();

        if (driver != nullptr)
            driver->stop();
        else
            dummy->stop();

        server.stop();
        probe.stop();
        udp.stop();
        engine.log().close();

        return 0;
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

    app.addCommand ({ "devices",
                      "devices",
                      "Lists the audio devices this machine can play through",
                      {},
                      [] (const juce::ArgumentList&) { listDevices(); } });

    app.addCommand ({ "midi",
                      "midi",
                      "Lists the MIDI ports this machine has, by the names --midi-in takes",
                      {},
                      [] (const juce::ArgumentList&) { listMidi(); } });

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

    app.addCommand ({ "serve",
                      "serve <bundle> --sample-rate=N --buffer=N [--hosted [--render=<wav>]]"
                      " [--ui=<dir>] [--midi-in=<device>]"
                      " [--http-port=N] [--osc-port=N] [--log=<file>]",
                      "Serves a bundle over OSCQuery and OSC until interrupted",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runServe (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    app.addCommand ({ "replay",
                      "replay <log> [--bundle=<dir>] [--out=<dir>]",
                      "Replays an event log into a fresh engine and checks it reproduces it exactly",
                      {},
                      [] (const juce::ArgumentList& args)
                      {
                          if (const auto code = runReplay (args); code != 0)
                              juce::ConsoleApplication::fail ({}, code);
                      } });

    return app.findAndRunCommand (argc, argv);
}
