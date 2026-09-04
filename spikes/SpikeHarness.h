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

#pragma once

/*
    SPIKE HARNESS — shared bring-up for the seven PRD §6.1 spikes.
    THROWAWAY CODE (devplan:19). Dies with the rest of spikes/.

    WHY A SHARED HEADER, GIVEN "SEPARATE THROWAWAY PROGRAMS"
    -------------------------------------------------------
    devplan:40 says the spikes are "separate throwaway programs", and
    spikes/CMakeLists.txt gives each exactly one main.cpp and no shared target.
    Both still hold: this is a header, so it never appears in add_executable(),
    the EXISTS .../main.cpp guard is untouched, and the wfg::thirdparty-only
    link line — the thing that makes "never migrates into src/" structural — is
    unchanged. Seven executables, seven main()s, seven verdicts.

    What a shared header buys that duplication does not: the deliverable of
    Phase 0 is seven REPORTS whose numbers get compared with each other. Seven
    independent engine bring-ups make those numbers incomparable by
    construction, because a difference between two reports could be a difference
    between two Engine constructions. And the two hazards handled below (the
    unbounded map wait, the app-data deletion) are bring-up bugs: duplicated,
    each would have to be found seven times.

    THE RULE: if only one spike would use it, it belongs in that spike's
    main.cpp, not here. Nothing here may answer devplan:49-50 — no track count,
    no sample rate, no buffer size, not even as a fallback.

    ON THE INCLUDES BELOW
    ---------------------
    Two of the three are marked @internal by Tracktion and neither is reachable
    from <tracktion_engine/tracktion_engine.h>. They can move on a TE bump. That
    is a known and accepted cost, and the spikes CI job exists precisely to tell
    us the day it happens.

    tracktion_graph's own tracktion_TestUtilities.h is deliberately NOT included,
    although it has exactly the tone and transient generators we want. It is not
    consumable from outside the module: from line 185 on it uses createNodeMap,
    getNodes, postordering and ThreadPoolStrategy, none of which the graph
    umbrella header exports, so including it is ~20 compile errors in TE's own
    source. The generators below are the twenty lines it would have saved us,
    and not depending on it is the better trade anyway - one fewer internal
    header to break on a bump.
*/

#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_engine/testing/tracktion_EnginePlayer.h>
#include <tracktion_engine/utilities/tracktion_TestUtilities.h>
#include <tracktion_engine/playback/graph/tracktion_EditNodeBuilder.h>
#include <tracktion_graph/tracktion_graph.h>

#if JUCE_WINDOWS
 #include <crtdbg.h>
 #include <stdlib.h>
#endif

#include <atomic>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace spike
{
    //==============================================================================
    /*  Exit codes. Every spike uses these and no others, so a CI log line is
        readable without opening the source.

          0  the invariant held
          1  the invariant was VIOLATED - a real finding, not a crash
          2  usage error (a required argument missing or unparseable)
          3  harness error - the spike could not get far enough to measure
             anything (file never mapped, device would not open). Distinct from
             1 on purpose: a 3 says nothing at all about the engine.
    */
    enum ExitCode { ok = 0, violated = 1, usageError = 2, harnessError = 3 };

    //==============================================================================
    /*  Route CRT assertion failures to stderr instead of a modal dialog.

        On Windows a failed assert() in a Debug build pops a message box with
        Abort/Retry/Ignore. On a developer's machine that is merely annoying. In
        CI it is far worse than a failure: the job does not go red, it HANGS,
        holding a runner until the timeout expires - the same failure shape as
        TE's unbounded file-mapping wait, and just as expensive on a private
        repo. Nothing here changes whether an assert fires; it changes whether
        anyone finds out why.

        Call this as the FIRST line of main(), before any engine construction.
    */
    inline void makeAssertsNonInteractive()
    {
       #if JUCE_WINDOWS && defined (_DEBUG)
        _set_error_mode (_OUT_TO_STDERR);

        for (int report : { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN })
        {
            _CrtSetReportMode (report, _CRTDBG_MODE_FILE);
            _CrtSetReportFile (report, _CRTDBG_FILE_STDERR);
        }
       #endif
    }

    //==============================================================================
    /*  Deliberately hand-rolled and about fifteen lines: a spike must not grow
        an argument-parsing dependency, and juce::ArgumentList lives on the far
        side of a boundary the spikes are not allowed to cross.

        Moved verbatim from spike04_graph_stability/main.cpp, which is where the
        convention was set.
    */
    inline std::optional<long> valueFor (int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg { argv[i] };

            if (arg.size() > flag.size() && arg.compare (0, flag.size(), flag) == 0)
            {
                const auto* first = arg.data() + flag.size();
                const auto* last  = arg.data() + arg.size();

                long parsed = 0;

                if (std::from_chars (first, last, parsed).ec == std::errc{} && parsed > 0)
                    return parsed;

                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    /*  The string-valued sibling, for --device= and --child=. Kept separate
        rather than templated: two short functions read better than one clever
        one, in code nobody maintains.
    */
    inline std::optional<std::string> textFor (int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg { argv[i] };

            if (arg.size() > flag.size() && arg.compare (0, flag.size(), flag) == 0)
                return std::string { arg.substr (flag.size()) };
        }

        return std::nullopt;
    }

    inline bool hasFlag (int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
            if (std::string_view { argv[i] } == flag)
                return true;

        return false;
    }

    //==============================================================================
    /*  WHY THE THREE CORE ARGUMENTS ARE MANDATORY AND HAVE NO DEFAULTS
        ---------------------------------------------------------------
        --tracks, --sample-rate and --buffer are REQUIRED, and there is no
        fallback value for any of them anywhere in this tree. The default fixed
        track count and the target sample rates / buffer sizes are two of the
        author's open Phase 0 decisions (devplan:49-50). A "sensible default"
        written here would be an ANSWER to a question he has not answered, and
        it would then get read back out of this file as though it were one.

        argv is the only place a number like this is allowed to live in Go.dot
        right now, and it is only allowed here because these programs are
        throwaway and never migrate into src/. Do not helpfully add defaults.
    */
    struct Args
    {
        long tracks     = 0;
        long sampleRate = 0;
        long buffer     = 0;
    };

    inline std::optional<Args> parseArgs (int argc, char** argv)
    {
        const auto t = valueFor (argc, argv, "--tracks=");
        const auto s = valueFor (argc, argv, "--sample-rate=");
        const auto b = valueFor (argc, argv, "--buffer=");

        if (! t || ! s || ! b)
            return std::nullopt;

        return Args { *t, *s, *b };
    }

    /*  Prints the shared half of every spike's usage block to stderr and returns
        2, so a spike's usage() is its own first line plus this. `criterion` is
        PRD 6.1's wording for that spike, verbatim - the same text that heads the
        source file, because the PRD is the single source and both merely quote
        it.
    */
    inline int usage (std::string_view programName,
                      std::string_view criterion,
                      std::string_view extraFlags = {})
    {
        std::cerr << programName << " - a Go.dot Phase 0 validation spike (PRD 6.1)\n\n"
                  << "usage: " << programName
                  << " --tracks=N --sample-rate=N --buffer=N" << extraFlags << "\n\n"
                     "The first three are REQUIRED and have no defaults. The fixed track count\n"
                     "and the target sample rates / buffer sizes are open author decisions\n"
                     "(devplan:49-50); the spikes take them on the command line so that no value\n"
                     "for either is recorded anywhere in the source tree.\n\n"
                  << "Pass criterion (PRD 6.1):\n  " << criterion << "\n\n"
                  << "Exit: 0 invariant held, 1 VIOLATED, 2 usage, 3 harness could not measure.\n"
                     "The verdict a human acts on is the report in docs/spikes/.\n";

        return usageError;
    }

    //==============================================================================
    /*  Engine bring-up with no audio hardware.

        autoInitialiseDeviceManager() returning false is what stops the Engine
        constructor opening a device (tracktion_Engine.cpp:68). Everything then
        runs through EnginePlayer's HostedAudioDeviceInterface instead, which is
        deterministic and block-driven. Pattern from TE's own
        examples/TestRunner/TestRunner.h:81-95.

        describeWaveDevices is a settable std::function because exactly one spike
        (#1, bus routing) needs to carve the hosted device into channel pairs,
        and the other six must not be perturbed by it.
    */
    struct SpikeEngineBehaviour : public tracktion::engine::EngineBehaviour
    {
        bool autoInitialiseDeviceManager() override        { return false; }
        bool enableReadAheadForTimeStretchNodes() override  { return true; }

        std::function<void (std::vector<tracktion::engine::WaveDeviceDescription>&,
                            juce::AudioIODevice&, bool)> describeWaveDevicesFn;

        bool isDescriptionOfWaveDevicesSupported() override
        {
            return describeWaveDevicesFn != nullptr;
        }

        void describeWaveDevices (std::vector<tracktion::engine::WaveDeviceDescription>& descs,
                                  juce::AudioIODevice& device, bool isInput) override
        {
            if (describeWaveDevicesFn)
                describeWaveDevicesFn (descs, device, isInput);
        }
    };

    //==============================================================================
    /*  Property storage that cannot touch the author's real application data.

        TE's TestPropertyStorage (TestRunner.h:100-129) calls
        getAppCacheFolder().deleteRecursively() in BOTH its constructor and its
        destructor, against whatever folder the base class computes from the app
        name. Under a name that collides with a real installed application that
        is data loss on the developer's own machine - and these spikes run on the
        machine that also runs Waveform.

        So we do not delete what the base class chose. We OVERRIDE where it
        looks, into a uniquely-named directory under the system temp folder, and
        clean up only that. Nothing outside it is reachable.
    */
    class SpikePropertyStorage : public tracktion::engine::PropertyStorage
    {
    public:
        explicit SpikePropertyStorage (juce::String appName)
            : PropertyStorage (appName),
              root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("go.dot-spike-" + juce::Uuid().toDashedString()))
        {
            root.createDirectory();
            root.getChildFile ("cache").createDirectory();
            root.getChildFile ("prefs").createDirectory();
        }

        ~SpikePropertyStorage() override
        {
            root.deleteRecursively (false);
        }

        juce::File getAppCacheFolder() override  { return root.getChildFile ("cache"); }
        juce::File getAppPrefsFolder() override  { return root.getChildFile ("prefs"); }

        void removeProperty (tracktion::engine::SettingID) override {}
        juce::var getProperty (tracktion::engine::SettingID, const juce::var& defaultValue) override { return defaultValue; }
        void setProperty (tracktion::engine::SettingID, const juce::var&) override {}
        std::unique_ptr<juce::XmlElement> getXmlProperty (tracktion::engine::SettingID) override { return {}; }
        void setXmlProperty (tracktion::engine::SettingID, const juce::XmlElement&) override {}

        void removePropertyItem (tracktion::engine::SettingID, juce::StringRef) override {}
        juce::var getPropertyItem (tracktion::engine::SettingID, juce::StringRef, const juce::var& defaultValue) override { return defaultValue; }
        void setPropertyItem (tracktion::engine::SettingID, juce::StringRef, const juce::var&) override {}
        std::unique_ptr<juce::XmlElement> getXmlPropertyItem (tracktion::engine::SettingID, juce::StringRef) override { return {}; }
        void setXmlPropertyItem (tracktion::engine::SettingID, juce::StringRef, const juce::XmlElement&) override {}

        juce::File root;
    };

    //==============================================================================
    /*  RAII engine. Construct one on the stack in main() and nothing else.

        ScopedJuceInitialiser_GUI opens no X display on Linux - it only reaches
        MessageManager::getInstance() - which is why the Linux CI job installs no
        xvfb. Same reasoning as src/wfg/engine/Boot.cpp:93-97.
    */
    struct HeadlessEngine
    {
        HeadlessEngine()
        {
            auto owned = std::make_unique<SpikeEngineBehaviour>();
            behaviour = owned.get();

            engine = std::make_unique<tracktion::engine::Engine> (
                std::make_unique<SpikePropertyStorage> ("go.dot-spike"),
                std::make_unique<tracktion::engine::UIBehaviour>(),
                std::move (owned));
        }

        tracktion::engine::Engine& operator*()  const { return *engine; }
        tracktion::engine::Engine* operator->() const { return engine.get(); }

        juce::ScopedJuceInitialiser_GUI juceInit;
        SpikeEngineBehaviour* behaviour = nullptr;   // owned by engine
        std::unique_ptr<tracktion::engine::Engine> engine;
    };

    //==============================================================================
    /*  Bounded replacement for test_utilities::waitForFileToBeMapped.

        TE's version (tracktion_EnginePlayer.h:180-192) is a bare for(;;) with a
        100 ms sleep and NO deadline. A file that never maps hangs the process
        forever - on a private, billed repository that is a six-hour job which
        looks like a slow one. Spikes therefore pass an EMPTY filesToMap to
        createEnginePlayer and map explicitly through this instead.

        Returns false on timeout; the caller exits harnessError.
    */
    inline bool mapFileWithDeadline (const tracktion::engine::AudioFile& af,
                                     std::chrono::milliseconds deadline = std::chrono::seconds (30))
    {
        if (af.engine == nullptr)
            return false;

        const auto giveUpAt = std::chrono::steady_clock::now() + deadline;

        for (;;)
        {
            if (af.engine->getAudioFileManager().cache.hasMappedReader (af, 0))
                return true;

            if (std::chrono::steady_clock::now() >= giveUpAt)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    }

    //==============================================================================
    /*  createEnginePlayer, with the file-mapping wait bounded.

        This replicates test_utilities::createEnginePlayer's sequence exactly
        (tracktion_EnginePlayer.h:161-176) rather than calling it, for one
        reason: the ORDER matters and the wait must be ours.

          player ctor -> dispatchPendingUpdatesSynchronously
                      -> ensureContextAllocated
                      -> map the files          <- TE's unbounded loop lives here
                      -> transport.play (false)

        A file is not mapped until the playback context exists, so mapping
        before constructing the player simply never succeeds - it times out and
        the spike reports a harness error for a problem that is purely ordering.
        And mapping after play() has already started races the thing being
        measured. So: same order as TE, bounded wait, and `mappedOk` tells the
        caller which of the two failure kinds it got.
    */
    inline std::unique_ptr<tracktion::engine::test_utilities::EnginePlayer>
    createPlayerWithDeadline (tracktion::engine::Edit& edit,
                              tracktion::engine::HostedAudioDeviceInterface::Parameters params,
                              const std::vector<tracktion::engine::AudioFile>& filesToMap,
                              bool& mappedOk,
                              std::chrono::milliseconds deadline = std::chrono::seconds (30))
    {
        mappedOk = true;

        auto player = std::make_unique<tracktion::engine::test_utilities::EnginePlayer> (
            edit.engine, std::move (params));

        edit.dispatchPendingUpdatesSynchronously();
        edit.getTransport().ensureContextAllocated();

        for (const auto& af : filesToMap)
        {
            if (! mapFileWithDeadline (af, deadline))
            {
                mappedOk = false;
                break;
            }
        }

        edit.getTransport().play (false);

        return player;
    }

    //==============================================================================
    /*  Launch a clip in a slot, at the earliest beat the engine can honour.

        TE does NOT quantise for you, and LaunchHandle::play({}) is not an
        "immediately" shorthand: an empty optional reaches code that dereferences
        it, which in a Debug build is
            optional(429) : Assertion failed: operator->() called on empty optional
        and in a Release build is undefined. The caller must supply a
        MonotonicBeat, which only exists once the playback context has a sync
        point - i.e. after the transport is rolling and at least one block has
        been processed.

        Shape taken from TE's own reference implementation, examples/DemoRunner/
        demos/ClipLauncherDemo.h:156-182, minus the quantisation offset:
        quantisation is not what any of these spikes is measuring, and adding it
        would put a tempo-grid dependency in the middle of a launch-timing
        measurement.

        Returns false if there is no handle or no sync point yet, so a caller can
        tell "could not launch" from "launched and nothing happened".
    */
    inline bool launchAtNextWholeBeat (tracktion::engine::Clip& clip)
    {
        auto handle = clip.getLaunchHandle();

        if (handle == nullptr)
            return false;

        auto* context = clip.edit.getTransport().getCurrentPlaybackContext();

        if (context == nullptr)
            return false;

        const auto syncPoint = context->getSyncPoint();

        if (! syncPoint)
            return false;

        /*  Quantise to the next WHOLE beat rather than launching at the raw
            sync point, and this is what makes a launch reproducible.

            The sync point advances with wall-clock-influenced transport start,
            not purely with processed blocks, so two identical runs in the same
            process ask for launch at slightly different beats. For a full-scale
            sine that is a phase shift, and a phase shift between two runs shows
            up as a ~6 dBFS sample-wise difference - which reads exactly like the
            "crossfade tax on already-playing material" this spike is looking
            for, and is not. It cost two sweeps to notice, because the drift is
            small enough to land on the same beat most of the time.

            At 60 bpm one beat is a second and jitter is sub-millisecond, so
            snapping to the next whole beat is deterministic with enormous
            margin. It is also what the product does anyway: PRD 3.4, "Launches
            quantise to the tick: every message belonging to one GO leaves in the
            same frame."
        */
        const auto current = syncPoint->monotonicBeat.v.inBeats();
        const tracktion::engine::MonotonicBeat launchAt
            { tracktion::BeatPosition::fromBeats (std::floor (current) + 1.0) };

        handle->play (launchAt);
        return true;
    }

    //==============================================================================
    /*  Paces a render to wall-clock real time.

        EnginePlayer::process advances the graph as fast as the CPU allows, which
        is MUCH faster than real time. TE's audio-file readers are background
        threads sized for real-time playback, so a free-running render can outrun
        them and produce silence or stale data - which looks exactly like an
        engine defect and is not one.

        This matters for what a spike is allowed to CONCLUDE. "TE cannot route 64
        channels at 96 kHz" and "TE cannot route 64 channels at 96 kHz when asked
        to render them in a fraction of real time" are different sentences, and
        only the second is supported by a free-running harness.

        Construct one before the block loop and call waitForBlock() after each
        process() call. Costs exactly as much wall clock as the audio is long,
        which is why it is opt-in.
    */
    class RealTimePacer
    {
    public:
        RealTimePacer (double sampleRate, int blockSize)
            : blockDuration (std::chrono::duration<double> (blockSize / sampleRate)),
              startedAt (std::chrono::steady_clock::now())
        {}

        void waitForBlock()
        {
            ++blocksDone;
            const auto due = startedAt + std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                                             blockDuration * blocksDone);

            for (;;)
            {
                const auto now = std::chrono::steady_clock::now();

                if (now >= due)
                    return;

                std::this_thread::sleep_for (std::min (std::chrono::duration_cast<std::chrono::microseconds> (due - now),
                                                       std::chrono::microseconds (500)));
            }
        }

    private:
        std::chrono::duration<double> blockDuration;
        std::chrono::steady_clock::time_point startedAt;
        long long blocksDone = 0;
    };

    //==============================================================================
    /*  Counts audio-graph rebuilds.

        EditNodeBuilder::insertOptionalLastStageNode is a public static hook
        (tracktion_EditNodeBuilder.h:44) that TE calls unconditionally on every
        graph build (.cpp:1935). Installing a counting lambda that returns its
        argument unchanged is a non-invasive rebuild counter, and it is the
        instrument the whole polyphony verdict rests on (PRD 9.2).

        TWO THINGS THAT WILL BITE:
          * it fires once PER OUTPUT DEVICE per build, and is skipped when the
            device is used as an insert. Only DELTAS against a baseline mean
            anything; an absolute count does not.
          * it is a global static. The destructor RESTORES the previous
            std::function rather than clearing it, so nesting is safe and a spike
            cannot leave the hook armed for whatever runs next.

        Validate the instrument before trusting a zero from it: trigger a known
        rebuild (Clip::setOffset is in Edit::TreeWatcher's restart list,
        tracktion_Edit.cpp:147-150) and confirm the count moves.
    */
    class RebuildCounter
    {
    public:
        RebuildCounter()
            : previous (tracktion::engine::EditNodeBuilder::insertOptionalLastStageNode)
        {
            tracktion::engine::EditNodeBuilder::insertOptionalLastStageNode =
                [this] (std::unique_ptr<tracktion::graph::Node> n)
                {
                    count.fetch_add (1, std::memory_order_relaxed);
                    return n;
                };
        }

        ~RebuildCounter()
        {
            tracktion::engine::EditNodeBuilder::insertOptionalLastStageNode = previous;
        }

        RebuildCounter (const RebuildCounter&) = delete;
        RebuildCounter& operator= (const RebuildCounter&) = delete;

        int total() const  { return count.load (std::memory_order_relaxed); }
        void mark()        { baselineValue = total(); }
        int delta() const  { return total() - baselineValue; }

    private:
        std::function<std::unique_ptr<tracktion::graph::Node> (std::unique_ptr<tracktion::graph::Node>)> previous;
        std::atomic<int> count { 0 };
        int baselineValue = 0;
    };

    //==============================================================================
    /*  Test material, so that every spike generates its audio the same way and
        two reports' numbers stay comparable.

        Semantics match tracktion_graph's getSinFile / getTransientFile, which is
        what these replace (see the note at the top of this file): a full-scale
        sine, and a single non-zero sample at an exact frame with silence either
        side. The transient one is what makes a position measurable to the sample
        - TE's own PDC test locates it the same way, with a 5-sample tolerance.
    */
    inline std::unique_ptr<juce::TemporaryFile> writeWav (const juce::AudioBuffer<float>& buffer,
                                                          double sampleRate)
    {
        auto file = std::make_unique<juce::TemporaryFile> (".wav");

        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream (file->getFile().createOutputStream());

        if (stream == nullptr)
            return {};

        std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (stream.get(), sampleRate,
                                    static_cast<unsigned int> (buffer.getNumChannels()),
                                    24, {}, 0));

        if (writer == nullptr)
            return {};

        stream.release();   // the writer owns it now
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        writer.reset();     // flush before the file is handed back

        return file;
    }

    inline std::unique_ptr<juce::TemporaryFile> makeToneFile (double sampleRate,
                                                              double lengthSeconds,
                                                              int numChannels,
                                                              float frequency)
    {
        const auto numSamples = static_cast<int> (sampleRate * lengthSeconds);
        juce::AudioBuffer<float> buffer (numChannels, numSamples);

        const auto phaseInc = juce::MathConstants<double>::twoPi
                                * static_cast<double> (frequency) / sampleRate;

        for (int c = 0; c < numChannels; ++c)
        {
            auto* d = buffer.getWritePointer (c);

            for (int i = 0; i < numSamples; ++i)
                d[i] = static_cast<float> (std::sin (phaseInc * i));
        }

        return writeWav (buffer, sampleRate);
    }

    inline std::unique_ptr<juce::TemporaryFile> makeTransientFile (double sampleRate,
                                                                   double lengthSeconds,
                                                                   double transientAtSeconds,
                                                                   float value,
                                                                   int numChannels)
    {
        const auto numSamples = static_cast<int> (sampleRate * lengthSeconds);
        const auto at = static_cast<int> (sampleRate * transientAtSeconds);

        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        buffer.clear();

        if (at >= 0 && at < numSamples)
            for (int c = 0; c < numChannels; ++c)
                buffer.setSample (c, at, value);

        return writeWav (buffer, sampleRate);
    }

    /*  Frame index of the first sample whose magnitude exceeds `threshold`, or
        nullopt. The instrument for every "did it land where we said" question.
    */
    inline std::optional<choc::buffer::FrameCount>
    findFirstNonZeroFrame (choc::buffer::ChannelArrayView<float> view, float threshold = 1.0e-4f)
    {
        for (choc::buffer::FrameCount f = 0; f < view.getNumFrames(); ++f)
            for (choc::buffer::ChannelCount c = 0; c < view.getNumChannels(); ++c)
                if (std::abs (view.getSample (c, f)) > threshold)
                    return f;

        return {};
    }

    //==============================================================================
    /*  Model edits must happen with the message manager locked: most of the
        model, and AutomatableParameter in particular, assert on
        currentThreadHasLockedMessageManager(). A MISSING lock does not fail the
        build and does not fail CI - jassertfalse only breaks under a debugger
        (juce_PlatformDefs.h:164) - it produces a quietly WRONG measurement.

        So no spike takes the lock by hand; they call this.
    */
    template <typename Fn>
    auto withMessageLock (Fn&& fn) -> decltype (fn())
    {
        const juce::MessageManagerLock mml;
        return fn();
    }

    //==============================================================================
    /*  Result reporting. key=value, one per line, then a single VERDICT line.

        The banner exists so the "How it was run" block of a spike report can be
        copy-pasted rather than retyped, and so a number in a report can never be
        separated from the build that produced it. Debug numbers and Release
        numbers are not the same numbers.
    */
    class Report
    {
    public:
        Report (std::string_view spikeName, int argc, char** argv)
        {
            /*  Unbuffered on purpose. TE and EnginePlayer use bare assert()
                in a few places (tracktion_EnginePlayer.h:41,164), which in a
                Debug build aborts the process - and an aborted process loses
                whatever is still sitting in stdout's buffer. A spike that
                dies must still show how far it got, or debugging it means
                guessing. Costs nothing at these output volumes.
            */
            std::cout << std::unitbuf;

            std::cout << "spike: " << spikeName << "\n"
                      << "tracktion: " << tracktion::engine::Engine::getVersion() << "\n"
                      << "juce: " << juce::SystemStats::getJUCEVersion() << "\n"
                     #if JUCE_DEBUG
                      << "build: Debug\n"
                     #else
                      << "build: Release\n"
                     #endif
                      << "os: " << juce::SystemStats::getOperatingSystemName() << "\n";

            std::cout << "argv:";

            for (int i = 0; i < argc; ++i)
                std::cout << ' ' << argv[i];

            std::cout << "\n";
        }

        template <typename T>
        void value (std::string_view key, const T& v)
        {
            std::cout << key << '=' << v << '\n';
        }

        /*  Returns the exit code, so a spike's last line reads
                return report.verdict (passed, "...");
        */
        int verdict (bool passed, std::string_view oneSentence)
        {
            std::cout << "VERDICT: " << (passed ? "PASS" : "FAIL") << " - " << oneSentence
                      << std::endl;

            return passed ? ok : violated;
        }

        int cannotMeasure (std::string_view why)
        {
            std::cout << "VERDICT: HARNESS-ERROR - " << why << std::endl;
            return harnessError;
        }
    };
}
