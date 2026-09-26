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
#include <wfg/engine/plugin/PluginLoad.h>
#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/plugin/TestGainState.h>

#include <spatcore/rt/RtThreadPriority.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#if JUCE_MAC
/*  DECLARED HERE because JUCE does not declare it in a header - Console.cpp
    has the same line and the reason: runDispatchLoop is [NSApp run], and it
    runs against nothing at all unless the application object was made.
    The macOS CI job found it the honest way: the child said ready, its
    loop returned at once, and it left before answering a block. */
namespace juce { void initialiseNSApplication(); }
#endif

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

        bool hasFlag (const std::vector<std::string>& args, const char* name)
        {
            return std::find (args.begin(), args.end(), std::string (name)) != args.end();
        }

        /** The one sentence the parent will read, then the flag it waits on. */
        void reportFailure (region::Header& header, const std::string& sentence)
        {
            std::memset (header.problem, 0, sizeof (header.problem));
            std::snprintf (header.problem, sizeof (header.problem), "%s", sentence.c_str());
            header.childFailed.store (1, std::memory_order_release);
        }

        /*  THE MAIN THREAD'S WAIT, for either mode: the message loop runs -
            a VST3 needs one to initialise, to take state and for whatever it
            times itself with - and this timer stops it when the parent says
            leave or has gone. A plain loop with a stop, because the modal
            `runDispatchLoopUntil` is compiled out of this build. */
        struct ExitWatch final : juce::Timer
        {
            ExitWatch (region::Header* headerToWatch, std::int64_t parentToWatch)
                : header (headerToWatch), parentPid (parentToWatch) {}

            void timerCallback() override
            {
                if (header != nullptr && header->childShouldExit.load (std::memory_order_acquire) != 0)
                {
                    stopTimer();
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }

                if (parentPid > 0 && ++ticks % 10 == 0 && ! process::isAlive (parentPid))
                {
                    stopTimer();
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                }
            }

            region::Header* header;
            std::int64_t parentPid;
            int ticks = 0;
        };

        /*  A LANE'S STATE LOAD, between the message thread that loads it and
            the worker that plays the lane (the author's decision of
            2026-09-25: a cue's whole state loaded onto its voice). `gate` is
            nought while the worker owns the instance; the loader sets one to
            ask for it, and the worker answers two - after which it never
            touches that instance, answering its blocks DRY (the audio left as
            it came) so no block is missed, until the loader sets nought.
            `epoch` moves after each load: the worker then sets every value on
            top again. Atomics and nothing else - the worker is the child's
            real-time thread. */
        struct LaneControl
        {
            std::atomic<int> gate { 0 };
            std::atomic<std::uint32_t> epoch { 0 };
        };

        /** The lanes of a region, resolved once. */
        struct Lanes
        {
            Lanes (region::Header& headerToUse, void* base)
                : header (headerToUse),
                  channels (static_cast<int> (header.channels.load (std::memory_order_relaxed))),
                  maxSamples (static_cast<int> (header.maxSamples.load (std::memory_order_relaxed))),
                  count (static_cast<int> (header.lanes.load (std::memory_order_relaxed))),
                  control (std::make_unique<LaneControl[]> (static_cast<std::size_t> (std::max (1, count))))
            {
                laneOf.reserve (static_cast<std::size_t> (count));

                for (int i = 0; i < count; ++i)
                    laneOf.push_back (region::laneAt (base, channels, maxSamples, i));
            }

            /*  The worker's side of the gate, before anything else about a
                lane: grants a load that asks, and while one runs answers the
                lane's blocks dry. True when the lane is parked this pass. */
            bool parked (int i, std::uint64_t request, std::uint64_t& answered) const
            {
                auto& gate = control[static_cast<std::size_t> (i)].gate;
                const auto g = gate.load (std::memory_order_acquire);

                if (g == 0)
                    return false;

                if (g == 1)
                    gate.store (2, std::memory_order_release);

                if (request > answered)
                {
                    answered = request;
                    laneOf[static_cast<std::size_t> (i)]->responseSeq.store (request, std::memory_order_release);
                }

                return true;
            }

            /*  Real-time priority for the answering thread, the same class the
                engine's own audio thread has; the period is the block's. */
            void takePriority() const
            {
                const auto rate = std::max<std::uint32_t> (1, header.sampleRate.load (std::memory_order_relaxed));
                const auto block = std::max<std::uint32_t> (1, header.blockSize.load (std::memory_order_relaxed));
                const auto periodMs = 1000.0 * static_cast<double> (block) / static_cast<double> (rate);
                spatcore::rt::setCurrentThreadAudioPriority (periodMs, periodMs * 0.5);
            }

            /*  THE SPIN POLICY (plan decision 14): hot while any lane is switched
                in, one-millisecond polls otherwise. */
            void restAfterIdlePass (bool anyRequest) const
            {
                if (! anyRequest && header.wantSpin.load (std::memory_order_relaxed) == 0)
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }

            region::Header& header;
            int channels, maxSamples, count;
            std::vector<region::Lane*> laneOf;
            std::unique_ptr<LaneControl[]> control;
        };

        //======================================================================
        /*  THE TEST GAIN, spike 07's child grown up: one instance per lane,
            each a gain that rests at the baseline, with the kill switch on p1.
            What every test in CI runs against, on every platform. */
        struct TestGainInstance
        {
            float gain = 0.5f;
            std::uint32_t paramsSeen = 0;
            std::uint64_t answered = 0;
            std::uint32_t epochSeen = 0;

            /*  WHAT ITS STATE SAYS (TestGainState): where Gain rests when the
                cue does not say, and Pad, which is no parameter. Written by
                the loader while the lane is parked, read by the worker after
                the epoch moves. */
            float restingGain = 0.5f;
            float padFactor = 1.0f;
        };

        struct TestGainWorker
        {
            explicit TestGainWorker (Lanes& lanesToUse) : lanes (lanesToUse)
            {
                instances.resize (static_cast<std::size_t> (lanes.count));

                const auto baseline0 = lanes.header.baseline[0].load (std::memory_order_relaxed);

                for (auto& instance : instances)
                    instance.restingGain = baseline0;
            }

            /*  THE MESSAGE THREAD'S LOAD, with the lane parked: the file's
                words, or the preset's own for an empty path. A delay in the
                file is slept here - which is how a test makes a slow plugin. */
            std::string loadState (int i, const std::string& path)
            {
                TestGainState state;
                std::string problem;

                if (! path.empty())
                {
                    const juce::File file { juce::String::fromUTF8 (path.c_str()) };

                    if (file.existsAsFile())
                        state = TestGainState::fromText (file.loadFileAsString().toStdString());
                    else
                        problem = "the cue's state file is not in the bundle: " + path;
                }

                if (state.loadDelayMs > 0)
                    juce::Thread::sleep (state.loadDelayMs);

                auto& instance = instances[static_cast<std::size_t> (i)];
                instance.restingGain = problem.empty() ? state.gain : lanes.header.baseline[0].load (std::memory_order_relaxed);
                instance.padFactor = problem.empty() ? state.padFactor() : 1.0f;
                return problem;
            }

            void run (const std::atomic<bool>& stop)
            {
                lanes.takePriority();

                while (! stop.load (std::memory_order_relaxed))
                {
                    auto any = false;

                    for (int i = 0; i < lanes.count; ++i)
                    {
                        auto* lane = lanes.laneOf[static_cast<std::size_t> (i)];
                        auto& instance = instances[static_cast<std::size_t> (i)];
                        const auto request = lane->requestSeq.load (std::memory_order_acquire);

                        if (lanes.parked (i, request, instance.answered))
                        {
                            any = true;
                            continue;
                        }

                        if (request <= instance.answered)
                            continue;

                        any = true;

                        //  A STATE LOADED UNDER IT: every value again, on top.
                        if (const auto epoch = lanes.control[static_cast<std::size_t> (i)].epoch.load (std::memory_order_acquire);
                            epoch != instance.epochSeen)
                        {
                            instance.epochSeen = epoch;
                            instance.paramsSeen = lane->paramRevision.load (std::memory_order_acquire) - 1u;
                        }

                        if (const auto revision = lane->paramRevision.load (std::memory_order_acquire);
                            revision != instance.paramsSeen)
                        {
                            instance.paramsSeen = revision;

                            const auto p0 = lane->params[0].load (std::memory_order_relaxed);
                            instance.gain = p0 < 0.0f ? instance.restingGain : std::clamp (p0, 0.0f, 1.0f);

                            /*  THE KILL SWITCH (plan decision 15). */
                            if (lane->params[1].load (std::memory_order_relaxed) >= 0.5f)
                                std::abort();
                        }

                        const auto numChannels = std::min<int> (lanes.channels, static_cast<int> (lane->numChannels.load (std::memory_order_relaxed)));
                        const auto numSamples = std::min<int> (lanes.maxSamples, static_cast<int> (lane->numSamples.load (std::memory_order_relaxed)));
                        auto* audio = region::audioOf (lane);

                        const auto factor = instance.gain * instance.padFactor;

                        for (int channel = 0; channel < numChannels; ++channel)
                        {
                            auto* samples = audio + channel * lanes.maxSamples;

                            for (int n = 0; n < numSamples; ++n)
                                samples[n] *= factor;
                        }

                        instance.answered = request;
                        lane->responseSeq.store (request, std::memory_order_release);
                    }

                    lanes.restAfterIdlePass (any);
                }
            }

            Lanes& lanes;
            std::vector<TestGainInstance> instances;
        };

        //======================================================================
        /*  A REAL PLUGIN, through JUCE's format manager (PR 9a.7): one instance
            per lane, made on the message thread, the preset applied, the
            baseline read back, and a worker that feeds them.

            THE CATALOGUE IS READ OFF THE FIRST INSTANCE - what a description
            does not carry and only an instance knows (§17.7): every
            parameter's name, its seven-character name, its label, its default,
            whether it is stepped and its step texts, a bipolar guess, and its
            value text at a hundred and one points. */
        Catalogue catalogueOf (juce::AudioPluginInstance& instance, const juce::PluginDescription& description)
        {
            Catalogue out;
            out.identifier = description.createIdentifierString().toStdString();
            out.name = description.name.toStdString();
            out.latencySamples = instance.getLatencySamples();

            const auto& parameters = instance.getParameters();
            const auto count = std::min (parameters.size(), region::maxParams);

            for (int i = 0; i < count; ++i)
            {
                auto* parameter = parameters[i];

                if (parameter == nullptr)
                {
                    out.params.push_back ({});
                    continue;
                }

                Parameter p;
                p.name = parameter->getName (64).toStdString();
                p.shortName = parameter->getName (7).toStdString();
                p.unit = parameter->getLabel().toStdString();
                p.defaultValue = std::clamp (parameter->getDefaultValue(), 0.0f, 1.0f);
                p.discrete = parameter->isDiscrete() || parameter->isBoolean();
                p.steps = p.discrete ? std::max (2, parameter->getNumSteps()) : 0;

                if (p.discrete)
                    for (const auto& step : parameter->getAllValueStrings())
                        p.stepText.push_back (step.toStdString());

                for (int n = 0; n <= 100; ++n)
                    p.text[static_cast<std::size_t> (n)] = parameter->getText (static_cast<float> (n) / 100.0f, 32).toStdString();

                p.bipolar = Catalogue::guessBipolar (p.defaultValue, p.text.front(), p.text.back());
                out.params.push_back (std::move (p));
            }

            return out;
        }

        struct RealHost
        {
            /** Message thread. False with a sentence when the plugin will not come up. */
            bool create (const juce::PluginDescription& description, const juce::String& bundle,
                         int instanceCount, int channels, double sampleRate, int blockSize,
                         const juce::File& preset, std::string& problem)
            {
                addFormatFor (manager, description, bundle);

                for (int i = 0; i < instanceCount; ++i)
                {
                    /*  THE SAME MAKING AS THE EDITING HELPER'S (PluginLoad.h):
                        the voice's width, the preparation, the preset - so a
                        cue edited in the plugin's own window starts where a
                        voice starts. */
                    auto instance = makeInsertInstance (manager, description, channels, sampleRate,
                                                        blockSize, preset, problem);

                    if (instance == nullptr)
                        return false;

                    width = std::max ({ width, channels, instance->getTotalNumInputChannels(),
                                        instance->getTotalNumOutputChannels() });
                    instances.push_back (std::move (instance));
                }

                scratch.setSize (width, blockSize, false, true, true);
                return true;
            }

            /** Message thread, after create: what the header carries. */
            void report (region::Header& header, int channels)
            {
                juce::ignoreUnused (channels);
                auto& first = *instances.front();
                const auto& parameters = first.getParameters();
                const auto count = std::min (parameters.size(), region::maxParams);

                header.latencySamples.store (static_cast<std::uint32_t> (std::max (0, first.getLatencySamples())),
                                             std::memory_order_relaxed);
                header.paramCount.store (static_cast<std::uint32_t> (count), std::memory_order_relaxed);

                for (int i = 0; i < count; ++i)
                    header.baseline[i].store (parameters[i] != nullptr ? std::clamp (parameters[i]->getValue(), 0.0f, 1.0f) : 0.0f,
                                              std::memory_order_relaxed);

                baseline.assign (static_cast<std::size_t> (count), 0.0f);

                for (int i = 0; i < count; ++i)
                    baseline[static_cast<std::size_t> (i)] = header.baseline[i].load (std::memory_order_relaxed);

                /*  EVERY LANE RESTS WHERE THE PRESET LEFT IT, until a cue's
                    whole state puts it elsewhere; and the preset's own state
                    is kept, for a cue with none of its own after one with -
                    no state is a state too. */
                laneBaseline.assign (instances.size(), baseline);
                first.getStateInformation (initialState);
            }

            /*  A CUE'S WHOLE STATE onto one lane's instance: MESSAGE THREAD,
                where a VST3 takes its state, with the lane parked so the
                worker is not in it. The file, or the preset's own state for an
                empty path or one that is not there - and then every value it
                left is where that lane's unmentioned parameters rest. */
            std::string loadState (int i, const std::string& path)
            {
                auto& instance = *instances[static_cast<std::size_t> (i)];
                std::string problem;
                juce::MemoryBlock bytes;

                if (! path.empty() && ! juce::File (juce::String::fromUTF8 (path.c_str())).loadFileAsData (bytes))
                    problem = "the cue's state file is not in the bundle: " + path;

                const auto& chosen = (path.empty() || ! problem.empty()) ? initialState : bytes;
                instance.setStateInformation (chosen.getData(), static_cast<int> (chosen.getSize()));

                const auto& parameters = instance.getParameters();
                auto& resting = laneBaseline[static_cast<std::size_t> (i)];

                for (std::size_t p = 0; p < resting.size(); ++p)
                    if (auto* parameter = parameters[static_cast<int> (p)])
                        resting[p] = std::clamp (parameter->getValue(), 0.0f, 1.0f);

                return problem;
            }

            /** Message thread, before the worker: releases in reverse. */
            void teardown()
            {
                for (auto& instance : instances)
                    if (instance != nullptr)
                        instance->releaseResources();

                instances.clear();
            }

            struct LaneState
            {
                std::uint32_t paramsSeen = 0;
                std::uint32_t resetSeen = 0;
                std::uint64_t answered = 0;
                std::uint32_t epochSeen = 0;
                std::vector<float> applied;
            };

            /** The worker thread. */
            void run (Lanes& lanes, const std::atomic<bool>& stop)
            {
                lanes.takePriority();

                std::vector<LaneState> state (static_cast<std::size_t> (lanes.count));

                for (auto& s : state)
                    s.applied.assign (baseline.size(), -2.0f);

                juce::MidiBuffer midi;

                while (! stop.load (std::memory_order_relaxed))
                {
                    auto any = false;

                    for (int i = 0; i < lanes.count && i < static_cast<int> (instances.size()); ++i)
                    {
                        auto* lane = lanes.laneOf[static_cast<std::size_t> (i)];
                        auto& s = state[static_cast<std::size_t> (i)];
                        auto& instance = *instances[static_cast<std::size_t> (i)];
                        const auto request = lane->requestSeq.load (std::memory_order_acquire);

                        if (lanes.parked (i, request, s.answered))
                        {
                            any = true;
                            continue;
                        }

                        if (request <= s.answered)
                            continue;

                        any = true;

                        /*  A STATE LOADED UNDER IT: forget what was set, and set
                            every value again on top - preallocated, a fill and
                            nothing more on this thread. */
                        if (const auto epoch = lanes.control[static_cast<std::size_t> (i)].epoch.load (std::memory_order_acquire);
                            epoch != s.epochSeen)
                        {
                            s.epochSeen = epoch;
                            std::fill (s.applied.begin(), s.applied.end(), -2.0f);
                            s.paramsSeen = lane->paramRevision.load (std::memory_order_acquire) - 1u;
                        }

                        /*  Values first, so the block is processed with what
                            the tick thread last wrote; only what moved is set,
                            because a plugin may smooth every set it is given. */
                        if (const auto revision = lane->paramRevision.load (std::memory_order_acquire);
                            revision != s.paramsSeen)
                        {
                            s.paramsSeen = revision;
                            const auto& parameters = instance.getParameters();

                            for (std::size_t p = 0; p < baseline.size(); ++p)
                            {
                                const auto value = lane->params[p].load (std::memory_order_relaxed);
                                const auto target = value < 0.0f ? laneBaseline[static_cast<std::size_t> (i)][p]
                                                                 : std::clamp (value, 0.0f, 1.0f);

                                if (std::abs (target - s.applied[p]) > 1.0e-7f && parameters[static_cast<int> (p)] != nullptr)
                                {
                                    parameters[static_cast<int> (p)]->setValue (target);
                                    s.applied[p] = target;
                                }
                            }
                        }

                        if (const auto resetSeq = lane->resetSeq.load (std::memory_order_acquire);
                            resetSeq != s.resetSeen)
                        {
                            s.resetSeen = resetSeq;
                            instance.reset();
                        }

                        const auto numChannels = std::min<int> (lanes.channels, static_cast<int> (lane->numChannels.load (std::memory_order_relaxed)));
                        const auto numSamples = std::min<int> ({ lanes.maxSamples, scratch.getNumSamples(),
                                                                 static_cast<int> (lane->numSamples.load (std::memory_order_relaxed)) });
                        auto* audio = region::audioOf (lane);

                        scratch.clear();

                        for (int channel = 0; channel < numChannels; ++channel)
                            scratch.copyFrom (channel, 0, audio + channel * lanes.maxSamples, numSamples);

                        /*  A view of the scratch at the block's length: the
                            plugin sees the width it asked for and the length
                            the parent sent. */
                        juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), numSamples);
                        midi.clear();
                        instance.processBlock (block, midi);

                        for (int channel = 0; channel < numChannels; ++channel)
                            std::copy_n (scratch.getReadPointer (channel), numSamples, audio + channel * lanes.maxSamples);

                        s.answered = request;
                        lane->responseSeq.store (request, std::memory_order_release);
                    }

                    lanes.restAfterIdlePass (any);
                }
            }

            juce::AudioPluginFormatManager manager;
            std::vector<std::unique_ptr<juce::AudioPluginInstance>> instances;
            std::vector<float> baseline;
            std::vector<std::vector<float>> laneBaseline;
            juce::MemoryBlock initialState;
            juce::AudioBuffer<float> scratch;
            int width = 0;
        };

        /*  A CUE'S WHOLE STATE, LOADED (the author's decision of 2026-09-25).
            On the message thread, because that is where a VST3 takes its
            state, every five milliseconds: a lane whose request moved asks
            the worker for its instance (the gate), and once the worker has
            let go - answering that lane dry, missing nothing - the state goes
            in, the epoch moves so every value is set again on top, the lane
            is given back, and the parent is answered with how it went. Lanes
            load one after another; that is the honest cost. */
        struct StateLoader final : juce::Timer
        {
            using Load = std::function<std::string (int lane, const std::string& path)>;

            StateLoader (Lanes& lanesToUse, Load loadToUse)
                : lanes (lanesToUse), load (std::move (loadToUse)),
                  pending (static_cast<std::size_t> (std::max (1, lanes.count)))
            {
            }

            void timerCallback() override
            {
                for (int i = 0; i < lanes.count; ++i)
                    service (i);
            }

            void service (int i)
            {
                auto* lane = lanes.laneOf[static_cast<std::size_t> (i)];
                auto& control = lanes.control[static_cast<std::size_t> (i)];
                auto& wait = pending[static_cast<std::size_t> (i)];

                if (! wait.active)
                {
                    const auto request = lane->stateRequestSeq.load (std::memory_order_acquire);

                    if (request <= lane->stateDoneSeq.load (std::memory_order_relaxed))
                        return;

                    lane->statePath[region::pathChars - 1] = 0;
                    wait = { true, request, std::string (lane->statePath) };
                    control.gate.store (1, std::memory_order_release);
                    return;
                }

                //  Not let go yet: the worker grants within a pass, idle or not.
                if (control.gate.load (std::memory_order_acquire) != 2)
                    return;

                const auto began = std::chrono::steady_clock::now();
                const auto problem = load (i, wait.path);
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now() - began);

                control.epoch.fetch_add (1, std::memory_order_release);
                control.gate.store (0, std::memory_order_release);

                std::memset (lane->stateProblem, 0, sizeof (lane->stateProblem));
                std::snprintf (lane->stateProblem, sizeof (lane->stateProblem), "%s", problem.c_str());
                lane->stateFailed.store (problem.empty() ? 0u : 1u, std::memory_order_relaxed);
                lane->stateLoadMicros.store (static_cast<std::uint32_t> (std::min<long long> (micros.count(), 0xffffffffLL)),
                                             std::memory_order_relaxed);
                lane->stateDoneSeq.store (wait.seq, std::memory_order_release);
                wait.active = false;
            }

            struct Waiting
            {
                bool active = false;
                std::uint64_t seq = 0;
                std::string path;
            };

            Lanes& lanes;
            Load load;
            std::vector<Waiting> pending;
        };

        /*  `wfg plugin-host --catalogue-only --description=<file>`: one
            instance, the catalogue on stdout as JSON, and out. No region, no
            parent. What `wfg plugins --catalogue=<identifier>` runs. */
        int runCatalogueOnly (const std::vector<std::string>& args)
        {
            /*  The test child's is built in, and answers with no JUCE at all -
                which is what lets a test drive this verb on a CI runner. */
            if (optionFrom (args, "--plugin") == Catalogue::testGainIdentifier())
            {
                std::printf ("%s\n", Catalogue::testGain().toJson().c_str());
                std::fflush (stdout);
                return 0;
            }

            juce::ScopedJuceInitialiser_GUI juceForTheChild;

            juce::PluginDescription description;
            juce::String bundle;
            std::string problem;

            if (! readDescription (optionFrom (args, "--description"), description, bundle, problem))
            {
                std::fprintf (stderr, "wfg plugin-host: %s\n", problem.c_str());
                return 2;
            }

            RealHost host;

            if (! host.create (description, bundle, 1, 2, 48000.0, 256, {}, problem))
            {
                std::fprintf (stderr, "wfg plugin-host: %s\n", problem.c_str());
                return 4;
            }

            const auto catalogue = catalogueOf (*host.instances.front(), description);
            host.teardown();

            std::printf ("%s\n", catalogue.toJson().c_str());
            std::fflush (stdout);
            return 0;
        }
    }

    //==============================================================================
    int runPluginHost (const std::vector<std::string>& args)
    {
        if (hasFlag (args, "--catalogue-only"))
            return runCatalogueOnly (args);

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

        const auto channels = static_cast<int> (header.channels.load (std::memory_order_relaxed));
        const auto maxSamples = static_cast<int> (header.maxSamples.load (std::memory_order_relaxed));
        const auto laneCount = static_cast<int> (header.lanes.load (std::memory_order_relaxed));

        if (mapping.getSize() < region::regionBytes (channels, maxSamples, laneCount))
        {
            reportFailure (header, "the shared region is shorter than its header says");
            return 3;
        }

        /*  JUCE up for the whole of the child's life, on this thread, which
            is therefore the message thread: where a plugin is made and takes
            its state, and where the loop below runs for its sake. */
        juce::ScopedJuceInitialiser_GUI juceForTheChild;

        Lanes lanes (header, mapping.getData());
        std::atomic<bool> stop { false };
        std::thread answering;
        std::unique_ptr<TestGainWorker> testGain;
        std::unique_ptr<RealHost> real;

        if (identifier == Catalogue::testGainIdentifier())
        {
            /*  THE REPORT, before ready: what the parent reads into the table
                and what a value nobody set rests at. */
            const auto catalogue = Catalogue::testGain();
            header.latencySamples.store (static_cast<std::uint32_t> (catalogue.latencySamples), std::memory_order_relaxed);
            header.paramCount.store (static_cast<std::uint32_t> (catalogue.params.size()), std::memory_order_relaxed);

            for (std::size_t i = 0; i < catalogue.params.size() && i < static_cast<std::size_t> (region::maxParams); ++i)
                header.baseline[i].store (catalogue.params[i].defaultValue, std::memory_order_relaxed);

            /*  Its catalogue too, so the parent's pickup runs in CI. */
            if (const auto cataloguePath = optionFrom (args, "--catalogue-file"); ! cataloguePath.empty())
            {
                juce::File (juce::String (cataloguePath)).replaceWithText (juce::String (catalogue.toJson()), false, false, "\n");
                header.catalogueReady.store (1, std::memory_order_release);
            }

            testGain = std::make_unique<TestGainWorker> (lanes);
            answering = std::thread ([&testGain, &stop] { testGain->run (stop); });
        }
        else
        {
            juce::PluginDescription description;
            juce::String bundle;
            std::string problem;

            if (! readDescription (optionFrom (args, "--description"), description, bundle, problem))
            {
                reportFailure (header, problem);
                return 4;
            }

            if (description.createIdentifierString().toStdString() != identifier)
            {
                reportFailure (header, "the description does not name " + identifier);
                return 4;
            }

            real = std::make_unique<RealHost>();
            const auto preset = optionFrom (args, "--preset");

            if (! real->create (description, bundle, laneCount, channels,
                                static_cast<double> (std::max<std::uint32_t> (1, header.sampleRate.load (std::memory_order_relaxed))),
                                maxSamples, preset.empty() ? juce::File() : juce::File (juce::String (preset)), problem))
            {
                reportFailure (header, problem);
                return 4;
            }

            real->report (header, channels);

            /*  THE CATALOGUE, beside the region, once: the parent reads it
                into the machine's cache when the flag goes up. */
            if (const auto cataloguePath = optionFrom (args, "--catalogue-file"); ! cataloguePath.empty())
            {
                const auto catalogue = catalogueOf (*real->instances.front(), description);
                juce::File (juce::String (cataloguePath)).replaceWithText (juce::String (catalogue.toJson()), false, false, "\n");
                header.catalogueReady.store (1, std::memory_order_release);
            }

            answering = std::thread ([&real, &lanes, &stop] { real->run (lanes, stop); });
        }

        /*  THE CUES' WHOLE STATES, loaded on this thread for whichever kind
            of child this is - made before ready, so a request can never
            arrive with nobody to take it. */
        StateLoader loader (lanes, [&testGain, &real] (int lane, const std::string& path)
        {
            return testGain != nullptr ? testGain->loadState (lane, path) : real->loadState (lane, path);
        });

        header.childReady.store (1, std::memory_order_release);

       #if JUCE_MAC
        juce::initialiseNSApplication();
       #endif

        ExitWatch watch (&header, parentPid);
        watch.startTimer (100);
        loader.startTimer (5);
        juce::MessageManager::getInstance()->runDispatchLoop();
        loader.stopTimer();
        watch.stopTimer();

        /*  A READY CHILD NEVER LEAVES BEFORE IT IS TOLD. Should the dispatch
            loop return with nobody having asked - a platform where it could
            not run - the plain poll of PR 9a.6 keeps the worker answering
            until the parent says leave or has gone. */
        auto lastParentCheck = std::chrono::steady_clock::now();

        while (header.childShouldExit.load (std::memory_order_acquire) == 0)
        {
            const auto now = std::chrono::steady_clock::now();

            if (parentPid > 0 && now - lastParentCheck >= std::chrono::seconds (1))
            {
                lastParentCheck = now;

                if (! process::isAlive (parentPid))
                    break;
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }

        stop.store (true, std::memory_order_relaxed);

        if (answering.joinable())
            answering.join();

        if (real != nullptr)
            real->teardown();

        return 0;
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
