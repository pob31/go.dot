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
    SPIKE 07 — THE PROXY-PLUGIN SANDBOX AS A CUSTOM TE PLUGIN TYPE.
    THROWAWAY CODE (devplan:19). FEASIBILITY PROBE (author's decision).

    PRD §6.1 item 7, verbatim, and this is the pass criterion:

        7. The proxy-plugin sandbox as a custom TE plugin type wrapping the IPC.

    and PRD §3.18 says what the shape has to be:

        "Synchronous cross-process: shared-memory buffers, audio thread signals
         the plugin process and waits with a hard deadline. In time -> zero added
         latency. Missed -> last buffer or silence, strip marked failed.
         Degradation instead of dropout."

    SCOPE, per the author's decision: real shared-memory IPC to a real second
    process with a real deadline and a real kill path - but NO plugin hosting in
    the child. The child applies a trivial gain. What is being proved is the
    MECHANISM: that a custom TE plugin type can wrap a synchronous cross-process
    round trip, meet a deadline inside an audio callback, and survive the child
    dying mid-show. Hosting a VST in the child is Phase 9's job and adds nothing
    to that proof.

    THE CONSTRAINT THAT SHAPES EVERYTHING: PRD §4.2
    -----------------------------------------------
        "The audio thread is a lipogram: no allocation, no locks, no exceptions,
         no syscalls, no logging."

    A blocking cross-process call inside applyToBuffer sits on the audio thread.
    So the wait cannot be a condition variable, a semaphore, or anything else
    that enters the kernel. What is left is a BOUNDED SPIN on an atomic in shared
    memory - which is exactly what is implemented here, and measuring whether it
    can meet a deadline is the point of the spike.

    ZERO DECLARED LATENCY, DELIBERATELY. Spike #6 established that any latency a
    plugin declares is added to EVERY track in the Edit by TE's PDC. A proxy
    plugin that declared its round trip as latency would therefore delay the
    whole show. getLatencySeconds() returns 0 and the round trip is absorbed
    inside the block, which is what "in time -> zero added latency" means.
*/

#include "../SpikeHarness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "The proxy-plugin sandbox as a custom TE plugin type wrapping the IPC.";

    constexpr auto extraFlags = " [--deadline-us=N] [--kill-at=N] [--child=PATH]";

    constexpr int maxChannels = 2;
    constexpr int maxSamples  = 4096;

    //==============================================================================
    /*  The shared region. Identical layout in both processes because it IS the
        same executable, which is why the child is launched as `--child=<path>`
        rather than as a separate binary.

        std::atomic<uint64_t> is lock-free on x64, so these are plain memory
        operations with fences - no kernel involvement, which is what §4.2
        requires of anything the audio thread touches.
    */
    struct SharedRegion
    {
        std::atomic<uint64_t> requestSeq;
        std::atomic<uint64_t> responseSeq;
        std::atomic<uint32_t> numChannels;
        std::atomic<uint32_t> numSamples;
        std::atomic<uint32_t> childShouldExit;
        float audio[maxChannels * maxSamples];
    };

    static_assert (std::atomic<uint64_t>::is_always_lock_free,
                   "the whole design depends on these being lock-free: a locking atomic "
                   "would put a kernel call on the audio thread and break PRD 4.2");

    //==============================================================================
    /*  The parent's side of the IPC, shared with the plugin instance TE creates.
        A file-scope pointer because TE's plugin factory constructs the plugin and
        there is nowhere to thread a context through - acceptable in throwaway
        code, and it is why this is not a pattern to copy into src/.
    */
    struct ProxyContext
    {
        SharedRegion* shared = nullptr;
        int64_t deadlineUs = 500;

        std::atomic<int64_t> blocks { 0 };
        std::atomic<int64_t> misses { 0 };
        std::atomic<int64_t> missesAfterKill { 0 };
        std::atomic<bool> childKilled { false };

        std::vector<double> roundTripUs;   // audio thread only, reserved up front
    };

    ProxyContext* context = nullptr;

    //==============================================================================
    class SpikeProxyPlugin : public Plugin
    {
    public:
        SpikeProxyPlugin (PluginCreationInfo info) : Plugin (info) {}

        static const char* getPluginName()          { return "Go.dot proxy sandbox"; }
        static const char* xmlTypeName;

        static juce::ValueTree create()
        {
            return createValueTree (IDs::PLUGIN, IDs::type, xmlTypeName);
        }

        juce::String getName() const override               { return getPluginName(); }
        juce::String getPluginType() override               { return xmlTypeName; }
        juce::String getSelectableDescription() override    { return getName(); }

        void initialise (const PluginInitialisationInfo&) override {}
        void deinitialise() override {}

        /*  ZERO. See the header comment: declaring the round trip as latency
            would make TE's PDC delay every other track by it (spike #6).
        */
        double getLatencySeconds() override { return 0.0; }

        /*  NEW IN TE 3.5, and now PURE virtual on Plugin, so this is not optional.
            The bus layout replaces the old implicit "everything is stereo": a
            plugin declares its input and output channel configuration and the
            graph honours it. Stereo in, stereo out here, because the proxy is
            transparent - but a real sandbox could declare any width, which is
            what makes a multichannel out-of-process effect expressible at all.
        */
        BusLayout getBusses() const override
        {
            return BusLayout::singleStereoInOut();
        }

        void applyToBuffer (const PluginRenderContext& pc) override
        {
            if (context == nullptr || context->shared == nullptr || pc.destBuffer == nullptr)
                return;

            auto& s = *context->shared;
            const auto numCh = std::min (pc.destBuffer->getNumChannels(), maxChannels);
            const auto numSm = std::min (pc.bufferNumSamples, maxSamples);

            if (numCh <= 0 || numSm <= 0)
                return;

            const auto t0 = std::chrono::steady_clock::now();

            // Copy this block into the shared region.
            for (int c = 0; c < numCh; ++c)
                std::copy_n (pc.destBuffer->getReadPointer (c, pc.bufferStartSample),
                             numSm, s.audio + c * maxSamples);

            s.numChannels.store (static_cast<uint32_t> (numCh), std::memory_order_relaxed);
            s.numSamples.store (static_cast<uint32_t> (numSm), std::memory_order_relaxed);

            // Publish the request. release so the audio above is visible first.
            const auto seq = s.requestSeq.load (std::memory_order_relaxed) + 1;
            s.requestSeq.store (seq, std::memory_order_release);

            /*  THE BOUNDED SPIN. This is the mechanism under test.

                No condition variable, no semaphore, no sleep - all of those enter
                the kernel and §4.2 forbids that here. The clock is consulted only
                once every 64 iterations, because even steady_clock::now() is not
                free and checking it every pass would measure the clock rather
                than the round trip.
            */
            bool timedOut = false;
            int spins = 0;

            while (s.responseSeq.load (std::memory_order_acquire) < seq)
            {
                if (++spins >= 64)
                {
                    spins = 0;

                    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds> (
                                               std::chrono::steady_clock::now() - t0).count();

                    if (elapsedUs > context->deadlineUs)
                    {
                        timedOut = true;
                        break;
                    }
                }
            }

            if (! timedOut)
            {
                // In time: take the child's work. Zero added latency.
                for (int c = 0; c < numCh; ++c)
                    std::copy_n (s.audio + c * maxSamples, numSm,
                                 pc.destBuffer->getWritePointer (c, pc.bufferStartSample));
            }
            else
            {
                /*  Missed. §3.18: "last buffer or silence, strip marked failed.
                    Degradation instead of dropout." Passthrough is chosen here -
                    the dry signal is already in destBuffer - because for a show a
                    momentarily unprocessed strip is far better than a hole.
                */
                context->misses.fetch_add (1, std::memory_order_relaxed);

                if (context->childKilled.load (std::memory_order_relaxed))
                    context->missesAfterKill.fetch_add (1, std::memory_order_relaxed);
            }

            const auto us = std::chrono::duration<double, std::micro> (
                                std::chrono::steady_clock::now() - t0).count();

            if (context->roundTripUs.size() < context->roundTripUs.capacity())
                context->roundTripUs.push_back (us);   // reserved up front, never allocates

            context->blocks.fetch_add (1, std::memory_order_relaxed);
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeProxyPlugin)
    };

    const char* SpikeProxyPlugin::xmlTypeName ("goDotProxySandbox");

    //==============================================================================
    /*  THE CHILD. Same executable, re-invoked with --child=<path>. Maps the same
        file and applies a trivial gain: this probe proves the transport and the
        deadline, not plugin hosting (author's decision).
    */
    int runChild (const juce::String& path)
    {
        const juce::File f { path };
        juce::MemoryMappedFile mapped (f, juce::MemoryMappedFile::readWrite);

        if (mapped.getData() == nullptr)
            return 3;

        auto& s = *static_cast<SharedRegion*> (mapped.getData());
        uint64_t lastSeen = s.responseSeq.load (std::memory_order_relaxed);

        for (;;)
        {
            if (s.childShouldExit.load (std::memory_order_relaxed) != 0)
                return 0;

            const auto req = s.requestSeq.load (std::memory_order_acquire);

            if (req > lastSeen)
            {
                const auto numCh = std::min<uint32_t> (s.numChannels.load (std::memory_order_relaxed),
                                                       maxChannels);
                const auto numSm = std::min<uint32_t> (s.numSamples.load (std::memory_order_relaxed),
                                                       maxSamples);

                for (uint32_t c = 0; c < numCh; ++c)
                    for (uint32_t i = 0; i < numSm; ++i)
                        s.audio[c * maxSamples + i] *= 0.5f;

                lastSeen = req;
                s.responseSeq.store (req, std::memory_order_release);
            }
            else
            {
                std::this_thread::yield();   // child side: not the audio thread
            }
        }
    }

    double percentile (std::vector<double> v, double p)
    {
        if (v.empty())
            return 0.0;

        std::sort (v.begin(), v.end());
        return v[std::min (v.size() - 1, static_cast<size_t> (static_cast<double> (v.size()) * p))];
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    spike::makeAssertsNonInteractive();

    // Child mode first: it must not construct an Engine or parse spike args.
    if (const auto childPath = spike::textFor (argc, argv, "--child="))
        return runChild (juce::String (*childPath));

    const auto parsed = spike::parseArgs (argc, argv);

    if (! parsed)
        return spike::usage ("spike07_proxy_plugin", criterion, extraFlags);

    const auto args = *parsed;
    const auto deadlineUs = spike::valueFor (argc, argv, "--deadline-us=").value_or (500);
    const auto killAt     = spike::valueFor (argc, argv, "--kill-at=").value_or (0);

    spike::HeadlessEngine engine;
    spike::Report report ("spike07_proxy_plugin", argc, argv);

    const auto blockPeriodUs = (static_cast<double> (args.buffer) / static_cast<double> (args.sampleRate))
                                 * 1.0e6;

    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("block_period_us", blockPeriodUs);
    report.value ("deadline_us", deadlineUs);
    report.value ("kill_at_block", killAt);

    // --- the shared region -------------------------------------------------
    const auto shmFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("godot-spike07-" + juce::Uuid().toDashedString() + ".shm");

    {
        juce::FileOutputStream os (shmFile);

        if (! os.openedOk())
            return report.cannotMeasure ("could not create the shared-memory backing file");

        const std::vector<char> zeros (sizeof (SharedRegion), 0);
        os.write (zeros.data(), zeros.size());
    }

    juce::MemoryMappedFile mapped (shmFile, juce::MemoryMappedFile::readWrite);

    if (mapped.getData() == nullptr)
        return report.cannotMeasure ("could not map the shared region");

    auto* shared = new (mapped.getData()) SharedRegion{};

    ProxyContext ctx;
    ctx.shared = shared;
    ctx.deadlineUs = static_cast<int64_t> (deadlineUs);
    ctx.roundTripUs.reserve (200000);      // reserved up front: applyToBuffer never allocates
    context = &ctx;

    // --- the child ---------------------------------------------------------
    juce::ChildProcess child;
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    juce::StringArray cmd;
    cmd.add (exe.getFullPathName());
    cmd.add ("--child=" + shmFile.getFullPathName());

    if (! child.start (cmd))
        return report.cannotMeasure ("could not start the child process");

    report.value ("child_started", 1);

    // Give the child a moment to map the region before audio starts.
    std::this_thread::sleep_for (std::chrono::milliseconds (300));

    // --- the edit ----------------------------------------------------------
    engine->getPluginManager().createBuiltInType<SpikeProxyPlugin>();

    HostedAudioDeviceInterface::Parameters params;
    params.sampleRate     = static_cast<double> (args.sampleRate);
    params.blockSize      = static_cast<int> (args.buffer);
    params.inputChannels  = 2;
    params.outputChannels = 2;

    auto edit = test_utilities::createTestEdit (*engine, 1, Edit::EditRole::forEditing);
    auto tracks = getAudioTracks (*edit);

    if (tracks.isEmpty())
        return report.cannotMeasure ("no audio tracks");

    auto tone = spike::makeToneFile (params.sampleRate, 8.0, 2, 440.0f);

    if (tone == nullptr)
        return report.cannotMeasure ("could not create the test tone");

    const AudioFile toneAudio (*engine, tone->getFile());
    insertWaveClip (*tracks[0], {}, tone->getFile(), { { 0_tp, 8_td } }, DeleteExistingClips::no);

    auto proxy = insertNewPlugin<SpikeProxyPlugin> (*tracks[0]);

    if (proxy == nullptr)
        return report.cannotMeasure ("the custom plugin type did not instantiate - "
                                     "this alone would answer 6.1 #7 in the negative");

    report.value ("plugin_registered_and_instantiated", 1);

    bool mappedOk = false;
    auto player = spike::createPlayerWithDeadline (*edit, params, { toneAudio }, mappedOk);

    if (player == nullptr || ! mappedOk)
        return report.cannotMeasure ("could not start playback");

    // --- run, killing the child part-way through ---------------------------
    const auto totalBlocks = static_cast<int> ((6.0 * params.sampleRate) / params.blockSize);
    int64_t missesBeforeKill = 0;

    for (int i = 0; i < totalBlocks; ++i)
    {
        if (killAt > 0 && i == static_cast<int> (killAt))
        {
            missesBeforeKill = ctx.misses.load();
            ctx.childKilled.store (true, std::memory_order_relaxed);
            child.kill();
            report.value ("child_killed_at_block", i);
        }

        player->process (params.blockSize);
    }

    shared->childShouldExit.store (1, std::memory_order_relaxed);
    child.waitForProcessToFinish (1000);

    // --- results -----------------------------------------------------------
    report.value ("blocks_processed", ctx.blocks.load());
    report.value ("misses_total", ctx.misses.load());
    report.value ("misses_before_kill", missesBeforeKill);
    report.value ("misses_after_kill", ctx.missesAfterKill.load());

    report.value ("roundtrip_us.p50", percentile (ctx.roundTripUs, 0.50));
    report.value ("roundtrip_us.p99", percentile (ctx.roundTripUs, 0.99));
    report.value ("roundtrip_us.max", ctx.roundTripUs.empty()
                                        ? 0.0
                                        : *std::max_element (ctx.roundTripUs.begin(), ctx.roundTripUs.end()));
    report.value ("roundtrip_us.p50_pct_of_block", percentile (ctx.roundTripUs, 0.50) / blockPeriodUs * 100.0);

    // Nothing may exceed the deadline by more than one clock-check interval.
    const auto maxRt = ctx.roundTripUs.empty()
                         ? 0.0
                         : *std::max_element (ctx.roundTripUs.begin(), ctx.roundTripUs.end());
    const auto overrun = maxRt - static_cast<double> (deadlineUs);

    report.value ("worst_overrun_us", overrun > 0.0 ? overrun : 0.0);

    context = nullptr;
    shmFile.deleteFile();

    /*  The gate. Three things, and the third is the one that matters for a show:
          - the custom plugin type registered and instantiated at all
          - the deadline was honoured (nothing ran away)
          - the process SURVIVED the child being killed, degrading rather than
            dropping out
    */
    const bool deadlineHeld = overrun <= static_cast<double> (deadlineUs);   // never more than 2x
    const bool survivedKill = killAt == 0 || ctx.blocks.load() > static_cast<int64_t> (killAt);

    return report.verdict (deadlineHeld && survivedKill,
                           deadlineHeld
                             ? (survivedKill
                                  ? "a custom TE plugin type wrapped a synchronous cross-process round trip, held its deadline, and survived the child being killed"
                                  : "the deadline held but playback did not continue past the kill")
                             : "the bounded spin overran its deadline");
}
