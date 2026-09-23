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

#include <wfg/engine/plugin/PluginHostChild.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/plugin/SharedRegion.h>

#include <spatcore/rt/RtThreadPriority.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace wfg::plugin
{
    namespace
    {
        std::string optionFrom (const std::vector<std::string>& args, const char* name)
        {
            const std::string prefix = std::string (name) + "=";

            for (const auto& arg : args)
                if (arg.rfind (prefix, 0) == 0)
                    return arg.substr (prefix.size());

            return {};
        }

        /** The one sentence the parent will read, then the flag it waits on. */
        void reportFailure (region::Header& header, const std::string& sentence)
        {
            std::memset (header.problem, 0, sizeof (header.problem));
            std::snprintf (header.problem, sizeof (header.problem), "%s", sentence.c_str());
            header.childFailed.store (1, std::memory_order_release);
        }

        //======================================================================
        /*  THE TEST GAIN, spike 07's child grown up: one instance per lane,
            each a gain that rests at the baseline, with the kill switch on p1.
            What every test in CI runs against, on every platform. */
        struct TestGainInstance
        {
            float gain = 0.5f;
            std::uint32_t paramsSeen = 0;
            std::uint32_t resetSeen = 0;
            std::uint64_t answered = 0;
        };

        struct TestGainWorker
        {
            TestGainWorker (region::Header& headerToUse, void* base)
                : header (headerToUse),
                  channels (static_cast<int> (header.channels.load (std::memory_order_relaxed))),
                  maxSamples (static_cast<int> (header.maxSamples.load (std::memory_order_relaxed))),
                  lanes (static_cast<int> (header.lanes.load (std::memory_order_relaxed)))
            {
                instances.resize (static_cast<std::size_t> (lanes));
                laneOf.reserve (static_cast<std::size_t> (lanes));

                for (int i = 0; i < lanes; ++i)
                    laneOf.push_back (region::laneAt (base, channels, maxSamples, i));
            }

            /** Runs until told to stop. The worker thread. */
            void run (const std::atomic<bool>& stop)
            {
                /*  Real-time priority for the answering thread, the same class
                    the engine's own audio thread has; the period is the block's. */
                const auto rate = std::max<std::uint32_t> (1, header.sampleRate.load (std::memory_order_relaxed));
                const auto block = std::max<std::uint32_t> (1, header.blockSize.load (std::memory_order_relaxed));
                const auto periodMs = 1000.0 * static_cast<double> (block) / static_cast<double> (rate);
                spatcore::rt::setCurrentThreadAudioPriority (periodMs, periodMs * 0.5);

                const auto baseline0 = header.baseline[0].load (std::memory_order_relaxed);

                while (! stop.load (std::memory_order_relaxed))
                {
                    auto any = false;

                    for (int i = 0; i < lanes; ++i)
                    {
                        auto* lane = laneOf[static_cast<std::size_t> (i)];
                        auto& instance = instances[static_cast<std::size_t> (i)];
                        const auto request = lane->requestSeq.load (std::memory_order_acquire);

                        if (request <= instance.answered)
                            continue;

                        any = true;

                        /*  Values first, so the block is processed with what
                            the tick thread last wrote. */
                        if (const auto revision = lane->paramRevision.load (std::memory_order_acquire);
                            revision != instance.paramsSeen)
                        {
                            instance.paramsSeen = revision;

                            const auto p0 = lane->params[0].load (std::memory_order_relaxed);
                            instance.gain = p0 < 0.0f ? baseline0 : std::clamp (p0, 0.0f, 1.0f);

                            /*  THE KILL SWITCH (plan decision 15). */
                            if (lane->params[1].load (std::memory_order_relaxed) >= 0.5f)
                                std::abort();
                        }

                        /*  A gain has no state to reset; the request is
                            acknowledged so the sequence is honoured. */
                        instance.resetSeen = lane->resetSeq.load (std::memory_order_relaxed);

                        const auto numChannels = std::min<int> (channels, static_cast<int> (lane->numChannels.load (std::memory_order_relaxed)));
                        const auto numSamples = std::min<int> (maxSamples, static_cast<int> (lane->numSamples.load (std::memory_order_relaxed)));
                        auto* audio = region::audioOf (lane);

                        for (int channel = 0; channel < numChannels; ++channel)
                        {
                            auto* samples = audio + channel * maxSamples;

                            for (int n = 0; n < numSamples; ++n)
                                samples[n] *= instance.gain;
                        }

                        instance.answered = request;
                        lane->responseSeq.store (request, std::memory_order_release);
                    }

                    /*  THE SPIN POLICY (plan decision 14): hot while any lane
                        is switched in, one-millisecond polls otherwise. */
                    if (! any && header.wantSpin.load (std::memory_order_relaxed) == 0)
                        std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }
            }

            region::Header& header;
            int channels, maxSamples, lanes;
            std::vector<region::Lane*> laneOf;
            std::vector<TestGainInstance> instances;
        };
    }

    //==============================================================================
    int runPluginHost (const std::vector<std::string>& args)
    {
        const auto regionPath = optionFrom (args, "--region");
        const auto identifier = optionFrom (args, "--plugin");
        const auto parentPid = static_cast<std::int64_t> (std::atoll (optionFrom (args, "--parent-pid").c_str()));

        if (regionPath.empty() || identifier.empty())
        {
            std::fprintf (stderr, "wfg plugin-host: --region=<path> and --plugin=<identifier> are required\n");
            return 2;
        }

        const juce::File regionFile { juce::String (regionPath) };
        juce::MemoryMappedFile mapping (regionFile, juce::MemoryMappedFile::readWrite, false);

        if (mapping.getData() == nullptr || mapping.getSize() < region::headerBytes())
        {
            std::fprintf (stderr, "wfg plugin-host: could not map %s\n", regionPath.c_str());
            return 3;
        }

        auto& header = *region::headerOf (mapping.getData());

        /*  A region from another revision of SharedRegion.h is refused before
            a byte of it is believed; there is no sentence to write into a
            header whose shape is not known. */
        if (! region::looksValid (header))
        {
            std::fprintf (stderr, "wfg plugin-host: the region at %s is not laid out as this build expects\n",
                          regionPath.c_str());
            return 3;
        }

        const auto bytesNeeded = region::regionBytes (static_cast<int> (header.channels.load (std::memory_order_relaxed)),
                                                      static_cast<int> (header.maxSamples.load (std::memory_order_relaxed)),
                                                      static_cast<int> (header.lanes.load (std::memory_order_relaxed)));

        if (mapping.getSize() < bytesNeeded)
        {
            reportFailure (header, "the shared region is shorter than its header says");
            return 3;
        }

        if (identifier != Catalogue::testGainIdentifier())
        {
            reportFailure (header, "this build hosts only " + std::string (Catalogue::testGainIdentifier())
                                       + "; a real plugin arrives with PR 9a.7");
            return 4;
        }

        /*  THE REPORT, before ready: what the parent reads into the table and
            what a value nobody set rests at. */
        const auto catalogue = Catalogue::testGain();
        header.latencySamples.store (static_cast<std::uint32_t> (catalogue.latencySamples), std::memory_order_relaxed);
        header.paramCount.store (static_cast<std::uint32_t> (catalogue.params.size()), std::memory_order_relaxed);

        for (std::size_t i = 0; i < catalogue.params.size() && i < static_cast<std::size_t> (region::maxParams); ++i)
            header.baseline[i].store (catalogue.params[i].defaultValue, std::memory_order_relaxed);

        std::atomic<bool> stop { false };
        TestGainWorker worker (header, mapping.getData());
        std::thread answering ([&worker, &stop] { worker.run (stop); });

        header.childReady.store (1, std::memory_order_release);

        /*  The main thread waits to be told, or for the parent to vanish. */
        auto exitCode = 0;
        auto lastParentCheck = std::chrono::steady_clock::now();

        for (;;)
        {
            if (header.childShouldExit.load (std::memory_order_acquire) != 0)
                break;

            const auto now = std::chrono::steady_clock::now();

            if (parentPid > 0 && now - lastParentCheck >= std::chrono::seconds (1))
            {
                lastParentCheck = now;

                if (! process::isAlive (parentPid))
                {
                    exitCode = 0;
                    break;
                }
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }

        stop.store (true, std::memory_order_relaxed);
        answering.join();
        return exitCode;
    }

    bool runPluginHostIfAsked (int argc, char** argv, int& exitCode)
    {
        if (argc < 2 || std::strcmp (argv[1], pluginHostVerb) != 0)
            return false;

        std::vector<std::string> args;

        for (int i = 2; i < argc; ++i)
            args.emplace_back (argv[i]);

        exitCode = runPluginHost (args);
        return true;
    }
}
