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
#include <wfg/engine/plugin/GridPlayHead.h>
#include <wfg/engine/plugin/LaneMapping.h>
#include <wfg/engine/plugin/PluginLoad.h>
#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/plugin/ScratchBlock.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/plugin/TestGainState.h>
#include <wfg/engine/plugin/WorkgroupShare.h>
#include <wfg/engine/app/MachinePower.h>

#include <wfg/engine/app/RealtimeRights.h>
#include <wfg/engine/clock/FineWait.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
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
            `runDispatchLoopUntil` is compiled out of this build. It also asks
            the parent, at the same pace, what audio workgroup it shares. */
        struct ExitWatch final : juce::Timer
        {
            ExitWatch (region::Header* headerToWatch, std::int64_t parentToWatch, WorkgroupFollower& workgroupToFollow)
                : header (headerToWatch), parentPid (parentToWatch), workgroup (workgroupToFollow) {}

            void timerCallback() override
            {
                if (header != nullptr && header->childShouldExit.load (std::memory_order_acquire) != 0)
                {
                    stopTimer();
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }

                /*  The first tick asks for the workgroup at once; then once a
                    second, as before, the parent's life. */
                if (ticks++ % 10 != 0)
                    return;

                if (parentPid > 0 && ticks > 1 && ! process::isAlive (parentPid))
                {
                    stopTimer();
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }

                workgroup.poll();
            }

            region::Header* header;
            std::int64_t parentPid;
            WorkgroupFollower& workgroup;
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
            Lanes (region::Header& headerToUse, void* base, std::int64_t parentPid)
                : header (headerToUse),
                  workgroup (parentPid),
                  channels (static_cast<int> (header.channels.load (std::memory_order_relaxed))),
                  maxSamples (static_cast<int> (header.maxSamples.load (std::memory_order_relaxed))),
                  count (static_cast<int> (header.lanes.load (std::memory_order_relaxed))),
                  queueSlots (static_cast<int> (header.queueSlots.load (std::memory_order_relaxed))),
                  control (std::make_unique<LaneControl[]> (static_cast<std::size_t> (std::max (1, count))))
            {
                laneOf.reserve (static_cast<std::size_t> (count));
                answeredAtStart.reserve (static_cast<std::size_t> (count));
                queuedAtStart.reserve (static_cast<std::size_t> (count));

                for (int i = 0; i < count; ++i)
                    laneOf.push_back (region::laneAt (base, channels, maxSamples, i, queueSlots));

                /*  WHAT THE LAST CHILD WAS ASKED AND NEVER ANSWERED IS NOBODY'S
                    BLOCK NOW (namespace draft §139, BHC): a child relaunched
                    into a region finds the request its predecessor died on,
                    and played through a fresh instance that old audio would
                    be the first thing in the plugin when the cue comes back.
                    Answered here unplayed, before any worker runs and long
                    before the parent calls again; and no reset is owed by
                    instances just made. */
                for (auto* lane : laneOf)
                {
                    const auto asked = lane->requestSeq.load (std::memory_order_acquire);
                    answeredAtStart.push_back (asked);
                    lane->responseSeq.store (asked, std::memory_order_release);
                    lane->resetDoneSeq.store (lane->resetSeq.load (std::memory_order_acquire), std::memory_order_release);

                    //  And what was in its queue (namespace draft §149): the same, each of them.
                    const auto queued = lane->queueSeq.load (std::memory_order_acquire);
                    queuedAtStart.push_back (queued);
                    lane->queueDone.store (queued, std::memory_order_release);
                }
            }

            /*  The worker's side of the gate, before anything else about a
                lane: grants a load that asks, and while one runs answers the
                lane's blocks dry. True when the lane is parked this pass. */
            bool parked (int i, std::uint64_t request, std::uint64_t& answered, std::uint64_t& queued) const
            {
                auto& gate = control[static_cast<std::size_t> (i)].gate;
                const auto g = gate.load (std::memory_order_acquire);

                if (g == 0)
                    return false;

                if (g == 1)
                    gate.store (2, std::memory_order_release);

                auto* lane = laneOf[static_cast<std::size_t> (i)];

                if (request > answered)
                {
                    answered = request;
                    lane->responseSeq.store (request, std::memory_order_release);
                }

                /*  And its queue (namespace draft §149, BKC): what was sent
                    before a reset or a state is the last cue's, answered as
                    it came and never played. */
                if (const auto asked = lane->queueSeq.load (std::memory_order_acquire); asked > queued)
                {
                    queued = asked;
                    lane->queueDone.store (asked, std::memory_order_release);
                }

                return true;
            }

            /*  ONE REQUEST AS THE WORKER SEES IT (namespace draft §149): the
                one waiting on the lane itself, or one of its queue's - where
                its audio and its key lie, its shape, and when it was asked. */
            struct Request
            {
                float* audio = nullptr;
                float* key = nullptr;
                int numChannels = 0;
                int numSamples = 0;
                int keyChannels = 0;
                std::int64_t asked = 0;
            };

            Request waiting (int i) const noexcept
            {
                auto* lane = laneOf[static_cast<std::size_t> (i)];
                Request request;
                request.audio = region::audioOf (lane);
                request.key = region::keyOf (lane, channels, maxSamples);
                request.numChannels = static_cast<int> (lane->numChannels.load (std::memory_order_relaxed));
                request.numSamples = static_cast<int> (lane->numSamples.load (std::memory_order_relaxed));
                request.keyChannels = static_cast<int> (lane->keyChannels.load (std::memory_order_relaxed));
                request.asked = lane->requestNanos.load (std::memory_order_relaxed);
                return request;
            }

            /*  The queue's request `number`, in its slot; and the lane told
                where it starts, which is what the instance's playhead reads
                (GridPlayHead) while this thread plays it. */
            Request queuedRequest (int i, std::uint64_t number) const noexcept
            {
                auto* lane = laneOf[static_cast<std::size_t> (i)];
                auto* slot = region::slotAt (lane, channels, maxSamples,
                                             static_cast<int> (number % static_cast<std::uint64_t> (std::max (1, queueSlots))));
                Request request;
                request.audio = region::audioOf (slot);
                request.key = region::keyOf (slot, channels, maxSamples);
                request.numChannels = static_cast<int> (slot->numChannels.load (std::memory_order_relaxed));
                request.numSamples = static_cast<int> (slot->numSamples.load (std::memory_order_relaxed));
                request.keyChannels = static_cast<int> (slot->keyChannels.load (std::memory_order_relaxed));
                request.asked = slot->requestNanos.load (std::memory_order_relaxed);
                lane->blockStartSample.store (slot->blockStartSample.load (std::memory_order_relaxed), std::memory_order_relaxed);
                return request;
            }

            /*  Real-time priority for the answering thread, the same class the
                engine's own audio thread has; the period is the block's. On
                Linux through RealtimeKit where the user's limit refuses
                (app/RealtimeRights.h). */
            void takePriority() const
            {
                const auto rate = std::max<std::uint32_t> (1, header.sampleRate.load (std::memory_order_relaxed));
                const auto block = std::max<std::uint32_t> (1, header.blockSize.load (std::memory_order_relaxed));
                const auto periodMs = 1000.0 * static_cast<double> (block) / static_cast<double> (rate);

                /*  WHICH CLASS, on Windows, where this thread never sleeps
                    while a lane is in (namespace draft §148; app/RealtimeRights.h
                    says why it matters). `WFG_PROXY_WORKER=mmcss|timecritical|away`
                    picks one for a bench or a test; read once, here, before
                    any real-time work. Elsewhere they are the same ask. */
                const auto asked = juce::SystemStats::getEnvironmentVariable ("WFG_PROXY_WORKER", {});
                const auto spinClass = asked == "timecritical" ? app::SpinClass::timeCritical
                                     : asked == "away"         ? app::SpinClass::timeCriticalAway
                                     : asked == "mmcss"        ? app::SpinClass::multimedia
                                                               : defaultSpinClass;
                app::takeRealtimeSpinning (spinClass, periodMs, periodMs * 0.5);
            }

            /*  The class the worker takes when nobody says (namespace draft §148,
                BJR): the plain top of the ordinary range on Windows, and the
                sound's own class everywhere else, where the name changes
                nothing. M89, this laptop, Likyū keyed at 128 samples, the
                three run at one moment for ten minutes: 43 buffers late in
                the multimedia class, 4 here, 4 with the wish for another
                core - which therefore stays a bench's word. */
            static constexpr app::SpinClass defaultSpinClass = app::SpinClass::timeCritical;

            /*  THE INTERFACE'S AUDIO WORKGROUP (macOS, 2026-10-22): the worker,
                at the top of every pass, joins what the parent shares when it
                changed, and the header says whether it is in - written only
                when that moves. */
            void followWorkgroup() noexcept
            {
                const auto in = workgroup.keepUp() ? 1u : 0u;

                if (header.inWorkgroup.load (std::memory_order_relaxed) != in)
                    header.inWorkgroup.store (in, std::memory_order_relaxed);
            }

            /*  THE WAIT BETWEEN REQUESTS (namespace draft §139, BHG; it was
                plan decision 14's spin). The parent cannot wake this thread -
                a wake is a kernel call, and its side is the audio thread - so
                the worker looks for work itself, and how it waits in between
                is the whole of its manners.

                IT USED TO SPIN, hot, for as long as any lane was switched in:
                a core a plugin, and worse than a core. A thread in the
                real-time class that never gives the processor back is exactly
                what a system's scheduler takes it from: on a Mac (M86, M4
                Pro) one buffer in fifteen found this worker gone for five to
                twenty milliseconds, with a gain for a plugin, and not one did
                once it slept between requests - or once it had no real-time
                class at all, which is no answer on a machine that is busy.

                SO IT NAPS: a tenth of a millisecond at a time while a lane is
                in, which is the most a request then waits, and a millisecond
                when none is. For a moment after a request it stays awake,
                because the lanes of one device callback are asked one after
                another and the next is already on its way. Where the buffer
                is so small that a nap would be a real share of a lane's
                allowance, it spins as it always did.

                NOT ON WINDOWS, which cannot nap so briefly: a worker napping
                on a high-resolution timer took a millisecond to answer and
                1.6 at worst - the timer's half millisecond is a whole one by
                the time a thread runs again. There it spins while a lane is
                in, as before, and only its rest when none is uses the finer
                wait.

                BUT WINDOWS DOES PUNISH THE SPIN, in the class the sound's
                threads are given (namespace draft §148). §139 read eight
                seconds of a gain on a quiet machine and found it never late.
                The multimedia class takes a fifth of every ten milliseconds
                back from a thread that used its whole share, and one that
                never sleeps always has: beside sixteen ordinary threads at
                work, 2313 buffers of 22499 came back late, each some two
                milliseconds; beside none but the author at his desk, 26 in
                five minutes - what his session showed. So the worker asks
                for no such class there (`takePriority`): at the plain top of
                the ordinary range nothing takes a share from it, and the
                same rows read nought and a tenth as many. */
            void restAfterIdlePass (bool anyRequest) noexcept
            {
                const auto now = std::chrono::steady_clock::now();

                if (anyRequest)
                {
                    lastAsked = now;
                    return;
                }

                if (header.wantSpin.load (std::memory_order_relaxed) == 0)
                {
                    nap (std::chrono::milliseconds (1));
                    return;
                }

                if (! napsBetweenRequests || now - lastAsked < lingerAfterRequest)
                    return;

                nap (napLength);
            }

            /*  How long it stays awake after a request, for the rest of the
                callback's lanes; and a nap. */
            static constexpr std::chrono::microseconds lingerAfterRequest { 200 };
            static constexpr std::chrono::microseconds napLength { 100 };

            /** A nap no longer than a quarter of what a lane may wait, or none at all; never on Windows. */
            static bool napFits (const region::Header& header) noexcept
            {
               #if defined (_WIN32)
                (void) header;
                return false;
               #else
                const auto rate = std::max<std::uint32_t> (1, header.sampleRate.load (std::memory_order_relaxed));
                const auto block = std::max<std::uint32_t> (1, header.blockSize.load (std::memory_order_relaxed));
                const auto capUs = ProxyLane::laneCapFraction * 1000000.0 * static_cast<double> (block) / static_cast<double> (rate);
                return capUs >= 4.0 * static_cast<double> (napLength.count());
               #endif
            }

            void nap (std::chrono::nanoseconds length) noexcept
            {
               #if defined (_WIN32)
                fineWait.wait (length);
               #else
                std::this_thread::sleep_for (length);
               #endif
            }

            region::Header& header;
            WorkgroupFollower workgroup;
            int channels, maxSamples, count;
            int queueSlots;     ///< how many slots each lane's queue has, nought for none (§149)
            const bool napsBetweenRequests = napFits (header);
            std::chrono::steady_clock::time_point lastAsked {};

           #if defined (_WIN32)
            FineWait fineWait;      ///< the worker thread's alone
           #endif

            std::vector<region::Lane*> laneOf;
            std::vector<std::uint64_t> answeredAtStart;     ///< each lane's request count when this child came up
            std::vector<std::uint64_t> queuedAtStart;       ///< and its queue's
            std::unique_ptr<LaneControl[]> control;
        };

        /*  THE LAYOUT, for the header: the widths, and the words the entry
            says them in, written before ready. */
        void reportLayout (region::Header& header, int inputs, int outputs, const std::string& words)
        {
            header.inputs.store (static_cast<std::uint32_t> (std::max (0, inputs)), std::memory_order_relaxed);
            header.outputs.store (static_cast<std::uint32_t> (std::max (0, outputs)), std::memory_order_relaxed);
            std::memset (header.layout, 0, sizeof (header.layout));
            std::snprintf (header.layout, sizeof (header.layout), "%s", words.c_str());
        }

        /** The lane's channels as pointers: the region is channel-major. */
        constexpr int maxLaneChannels = 64;

        void pointersFrom (float* audio, int channels, int maxSamples,
                           std::array<float*, maxLaneChannels>& into) noexcept
        {
            for (int channel = 0; channel < std::min (channels, maxLaneChannels); ++channel)
                into[static_cast<std::size_t> (channel)] = audio + channel * maxSamples;
        }

        //======================================================================
        /*  THE TEST GAIN, spike 07's child grown up: one instance per lane,
            each a gain that rests at the baseline, with the kill switch on p1.
            What every test in CI runs against, on every platform. */
        struct TestGainInstance
        {
            float gain = 0.5f;
            std::uint32_t paramsSeen = 0;
            std::uint64_t answered = 0;
            std::uint64_t queued = 0;       ///< how many of its queue's requests it has answered (§149)
            std::uint32_t epochSeen = 0;

            /*  WHAT ITS STATE SAYS (TestGainState): where Gain rests when the
                cue does not say, and Pad, which is no parameter. Written by
                the loader while the lane is parked, read by the worker after
                the epoch moves. */
            float restingGain = 0.5f;
            float padFactor = 1.0f;

            /** How long its reset takes, from its state: a slow one made on purpose. */
            int resetDelayMs = 0;

            /*  A LOOK-AHEAD MADE ON PURPOSE (namespace draft §140), from its
                state: the sound given back this many samples late, a line a
                channel, and declared as a limiter declares its own. Sized by
                the loader and cleared by a reset, the lane parked for both. */
            int lookAhead = 0;
            std::vector<float> held;
            std::size_t heldAt = 0;

            /*  LATE ON PURPOSE, in each of the two ways (namespace draft §148),
                from its state: microseconds before the worker takes a request,
                and inside its work. */
            int takeDelayUs = 0;
            int workDelayUs = 0;

            /*  ITS SIDECHAIN, for the ducker (namespace draft §141): how wide
                it is laid out, nought while its cue has no key. Set by a reset,
                the lane parked. */
            int keyWidth = 0;

            /** Whether its blocks are kept for a plugin's window (namespace draft §146). */
            region::TapGate tap;
        };

        struct TestGainWorker
        {
            /*  THE FOUR TEST PLUGINS: the ducker (namespace draft §141) as wide as
                the voice, the gain's own shape with a sidechain beside it; and
                the three of 2026-09-26: the gain as wide as the
                voice; the mono, one in and one out; the widener, one in and
                two out - left the input times the gain, right that at a
                half. The last two take a block only when the cue is mono
                (LaneMapping.h), and pass it dry otherwise. */
            TestGainWorker (Lanes& lanesToUse, const std::string& identifier)
                : lanes (lanesToUse),
                  duck (identifier == Catalogue::testDuckIdentifier()),
                  inputs (identifier == Catalogue::testGainIdentifier() || identifier == Catalogue::testDuckIdentifier()
                            ? lanesToUse.channels : 1),
                  outputs (identifier == Catalogue::testWidenIdentifier() ? 2
                             : identifier == Catalogue::testMonoIdentifier() ? 1 : lanesToUse.channels),
                  work (static_cast<std::size_t> (2 * std::max (1, lanesToUse.maxSamples)), 0.0f)
            {
                instances.resize (static_cast<std::size_t> (lanes.count));

                const auto baseline0 = lanes.header.baseline[0].load (std::memory_order_relaxed);

                for (std::size_t i = 0; i < instances.size(); ++i)
                {
                    instances[i].restingGain = baseline0;
                    instances[i].answered = lanes.answeredAtStart[i];
                    instances[i].queued = lanes.queuedAtStart[i];
                }
            }

            /** How wide a sidechain it offers (§141): the ducker's two, none for the others. */
            int keyOffer() const noexcept { return duck ? 2 : 0; }

            /*  THE SIDECHAIN, switched on or off as the lane's cue wants (§141):
                the ducker's is a flag. Message thread, the lane parked. */
            void layKey (int i, bool on)
            {
                instances[static_cast<std::size_t> (i)].keyWidth = on ? keyOffer() : 0;
            }

            int keyHeld (int i) const
            {
                return instances[static_cast<std::size_t> (i)].keyWidth;
            }

            /*  THE MESSAGE THREAD'S RESET, with the lane parked: a gain holds
                nothing to clear, so only the time a real one takes is played. */
            void resetLane (int i)
            {
                auto& instance = instances[static_cast<std::size_t> (i)];

                if (instance.resetDelayMs > 0)
                    juce::Thread::sleep (instance.resetDelayMs);

                //  What a look-ahead still holds is the last cue's.
                std::fill (instance.held.begin(), instance.held.end(), 0.0f);
                instance.heldAt = 0;
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
                instance.resetDelayMs = problem.empty() ? state.resetDelayMs : 0;
                instance.lookAhead = problem.empty() ? std::clamp (state.lookAhead, 0, 1 << 16) : 0;
                instance.held.assign (static_cast<std::size_t> (instance.lookAhead)
                                          * static_cast<std::size_t> (std::max (1, lanes.channels)), 0.0f);
                instance.heldAt = 0;
                instance.takeDelayUs = problem.empty() ? std::clamp (state.takeDelayUs, 0, 1000000) : 0;
                instance.workDelayUs = problem.empty() ? std::clamp (state.workDelayUs, 0, 1000000) : 0;
                return problem;
            }

            /** Awake and busy for this long: a test's lateness, never a show's. */
            static void spinFor (int microseconds) noexcept
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds (microseconds);

                while (std::chrono::steady_clock::now() < until) {}
            }

            void run (const std::atomic<bool>& stop)
            {
                lanes.takePriority();

                while (! stop.load (std::memory_order_relaxed))
                {
                    lanes.followWorkgroup();
                    auto any = false;

                    for (int i = 0; i < lanes.count; ++i)
                    {
                        auto* lane = lanes.laneOf[static_cast<std::size_t> (i)];
                        auto& instance = instances[static_cast<std::size_t> (i)];
                        const auto request = lane->requestSeq.load (std::memory_order_acquire);

                        if (lanes.parked (i, request, instance.answered, instance.queued))
                        {
                            any = true;
                            continue;
                        }

                        /*  THE ONE THAT WAITS FIRST - it was asked first - then
                            one of the queue's, in the order they were asked
                            (namespace draft §149); one a pass, so the other
                            lanes of this child are looked at in between. */
                        const auto fromQueue = request <= instance.answered;
                        const auto queuedNumber = instance.queued + 1;

                        if (fromQueue && (lanes.queueSlots <= 0
                                            || lane->queueSeq.load (std::memory_order_acquire) < queuedNumber))
                            continue;

                        any = true;
                        const auto asked = fromQueue ? lanes.queuedRequest (i, queuedNumber) : lanes.waiting (i);

                        if (instance.takeDelayUs > 0)
                            spinFor (instance.takeDelayUs);

                        //  TAKEN (namespace draft §148): from here the time is this side's.
                        const auto takenAt = region::steadyNanos();

                        if (instance.workDelayUs > 0)
                            spinFor (instance.workDelayUs);

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

                        const auto numChannels = std::min<int> (lanes.channels, asked.numChannels);
                        const auto numSamples = std::min<int> (lanes.maxSamples, asked.numSamples);
                        auto* audio = asked.audio;

                        /*  WHAT IT WAS GIVEN, kept for the plugin's window while
                            the parent asks (namespace draft §146) - before the
                            gain is put on it in place. */
                        if (instance.tap.open (*lane, numSamples, static_cast<int> (lanes.header.sampleRate.load (std::memory_order_relaxed))))
                            region::tapBlock (lane, lanes.channels, lanes.maxSamples, numChannels, numSamples,
                                              asked.keyChannels, asked.audio, asked.key);

                        const auto factor = instance.gain * instance.padFactor;
                        const lanemap::Shape shape { numChannels, inputs, outputs, lanes.channels };

                        /*  THE DUCKER'S KEY (§141): how loud it is, sample by
                            sample, the louder of its sides. */
                        const auto keyChannels = duck && instance.keyWidth > 0
                                                   ? std::min<int> (region::keyChannelsMax, asked.keyChannels)
                                                   : 0;
                        const auto* key = asked.key;

                        if (inputs == lanes.channels && outputs == lanes.channels)
                        {
                            for (int channel = 0; channel < numChannels; ++channel)
                            {
                                auto* samples = audio + channel * lanes.maxSamples;

                                if (keyChannels > 0)
                                {
                                    for (int n = 0; n < numSamples; ++n)
                                    {
                                        auto level = std::abs (key[n]);

                                        if (keyChannels > 1)
                                            level = std::max (level, std::abs (key[lanes.maxSamples + n]));

                                        samples[n] *= factor * (1.0f - std::min (1.0f, level));
                                    }

                                    continue;
                                }

                                for (int n = 0; n < numSamples; ++n)
                                    samples[n] *= factor;
                            }

                            /*  AND LATE BY ITS LOOK-AHEAD, when its state has one
                                (namespace draft §140): each sample changes places
                                with the one that went in that many before it. */
                            if (const auto length = static_cast<std::size_t> (instance.lookAhead); length > 0)
                            {
                                for (int channel = 0; channel < numChannels; ++channel)
                                {
                                    auto* samples = audio + channel * lanes.maxSamples;
                                    auto* line = instance.held.data() + static_cast<std::size_t> (channel) * length;
                                    auto at = instance.heldAt;

                                    for (int n = 0; n < numSamples; ++n)
                                    {
                                        std::swap (samples[n], line[at]);
                                        at = at + 1 == length ? 0 : at + 1;
                                    }
                                }

                                instance.heldAt = (instance.heldAt + static_cast<std::size_t> (numSamples)) % length;
                            }

                            /*  A mono cue goes into every input (LaneMapping.h),
                                so every output is it at the gain. */
                            if (numChannels == 1)
                                for (int channel = 1; channel < lanes.channels; ++channel)
                                    std::copy_n (audio, numSamples, audio + channel * lanes.maxSamples);
                        }
                        else if (lanemap::takes (shape))
                        {
                            /*  One in: the mono side of the cue. Out: itself
                                times the gain, and for the widener a second side
                                at a half - back into the lane by the rules. */
                            float* sides[2] = { work.data(), work.data() + lanes.maxSamples };

                            for (int n = 0; n < numSamples; ++n)
                            {
                                const auto x = audio[n];
                                sides[0][n] = x * factor;
                                sides[1][n] = x * factor * 0.5f;
                            }

                            pointersFrom (asked.audio, lanes.channels, lanes.maxSamples, lanePointers);
                            lanemap::backInto (sides, shape, lanePointers.data(), numSamples);
                        }

                        region::noteTimesFrom (*lane, asked.asked, takenAt, region::steadyNanos(), app::processorNow());

                        if (fromQueue)
                        {
                            instance.queued = queuedNumber;
                            lane->queueDone.store (queuedNumber, std::memory_order_release);
                        }
                        else
                        {
                            instance.answered = request;
                            lane->responseSeq.store (request, std::memory_order_release);
                        }
                    }

                    lanes.restAfterIdlePass (any);
                }

                lanes.workgroup.leave();
            }

            Lanes& lanes;
            bool duck = false;
            int inputs, outputs;
            std::vector<float> work;
            std::array<float*, maxLaneChannels> lanePointers {};
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
                preparedRate = sampleRate;
                preparedBlock = blockSize;

                for (int i = 0; i < instanceCount; ++i)
                {
                    /*  THE SAME MAKING AS THE EDITING HELPER'S (PluginLoad.h):
                        the layout ladder, the preparation, the preset - so a
                        cue edited in the plugin's own window starts where a
                        voice starts. */
                    auto instance = makeInsertInstance (manager, description, channels, sampleRate,
                                                        blockSize, preset, layout, problem);

                    if (instance == nullptr)
                        return false;

                    width = std::max ({ width, channels, instance->getTotalNumInputChannels(),
                                        instance->getTotalNumOutputChannels() });
                    instances.push_back (std::move (instance));
                }

                /*  ROOM FOR A SIDECHAIN (namespace draft §141): every instance
                    made alike, so the first one's answer holds for all; one
                    the ladder could not switch off is on from the start. */
                width = std::max (width, layout.keyedInputs);
                keyWidth.assign (instances.size(), layout.keyForced ? layout.keyOffer : 0);
                keyIndex.assign (instances.size(), KeyAt {});

                for (std::size_t i = 0; i < instances.size(); ++i)
                    keyChannelIndices (*instances[i], keyWidth[i], keyIndex[i].at);

                scratch.setSize (width, blockSize, false, true, true);
                scratchSamples = blockSize;
                return true;
            }

            int keyOffer() const noexcept { return layout.keyOffer; }

            /*  THE SIDECHAIN, switched on or off as the lane's cue wants
                (namespace draft §141, BHR): MESSAGE THREAD, the lane parked, as
                a reset is - a VST3 is released, laid out and prepared again,
                which it takes only there. Nothing to do for a plugin with no
                sidechain, or one already as asked. */
            void layKey (int i, bool on)
            {
                const auto index = static_cast<std::size_t> (i);

                if (layout.keyOffer <= 0 || layout.keyForced || (keyWidth[index] > 0) == on)
                    return;

                auto& instance = *instances[index];
                instance.releaseResources();
                keyWidth[index] = plugin::layKey (instance, layout, on);
                instance.prepareToPlay (preparedRate, preparedBlock);
                keyChannelIndices (instance, keyWidth[index], keyIndex[index].at);
            }

            int keyHeld (int i) const
            {
                return keyWidth[static_cast<std::size_t> (i)];
            }

            /** Message thread, after create: what the header carries. */
            void report (region::Header& header, int channels)
            {
                juce::ignoreUnused (channels);
                auto& first = *instances.front();
                reportLayout (header, layout.inputs, layout.outputs, layout.words);
                const auto& parameters = first.getParameters();
                const auto count = std::min (parameters.size(), region::maxParams);

                header.latencySamples.store (static_cast<std::uint32_t> (std::max (0, first.getLatencySamples())),
                                             std::memory_order_relaxed);
                header.keyInputs.store (static_cast<std::uint32_t> (std::max (0, layout.keyOffer)), std::memory_order_relaxed);
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

            /*  THE MESSAGE THREAD'S RESET, with the lane parked (namespace
                draft §139, BHC): a VST3's is `setProcessing` and `setActive`
                off and on again, which is the plugin's message thread's to
                call and can take milliseconds - Seventh Heaven's was measured
                at 2.5 to 15. On the worker, before the block, it was every
                cue's first blocks late and every other lane of this child
                kept waiting. */
            void resetLane (int i)
            {
                instances[static_cast<std::size_t> (i)]->reset();
            }

            struct LaneState
            {
                std::uint32_t paramsSeen = 0;
                std::uint64_t answered = 0;
                std::uint64_t queued = 0;       ///< how many of its queue's requests it has answered (§149)
                std::uint32_t epochSeen = 0;
                std::vector<float> applied;

                /** Whether its blocks are kept for the plugin's window (namespace draft §146). */
                region::TapGate tap;
            };

            /** The worker thread. */
            void run (Lanes& lanes, const std::atomic<bool>& stop)
            {
                lanes.takePriority();

                /*  THE PLAYHEAD EACH INSTANCE READS (namespace draft §123, BEJ):
                    the region's clock and its lane's block start, set before
                    the first block and taken off before the instances go. */
                playheads.clear();

                for (int i = 0; i < lanes.count && i < static_cast<int> (instances.size()); ++i)
                {
                    playheads.push_back (std::make_unique<GridPlayHead> (&lanes.header, lanes.laneOf[static_cast<std::size_t> (i)],
                                                                         static_cast<double> (lanes.header.sampleRate.load (std::memory_order_relaxed))));
                    instances[static_cast<std::size_t> (i)]->setPlayHead (playheads.back().get());
                }

                std::vector<LaneState> state (static_cast<std::size_t> (lanes.count));

                for (std::size_t i = 0; i < state.size(); ++i)
                {
                    state[i].applied.assign (baseline.size(), -2.0f);
                    state[i].answered = lanes.answeredAtStart[i];
                    state[i].queued = lanes.queuedAtStart[i];
                }

                /*  NO SUBNORMALS IN THE PLUGINS' ARITHMETIC: a reverb's tail
                    falling through them costs many times a block's usual
                    work on some processors, and the engine's own guard is the
                    parent's thread's, not this one's. */
                const juce::ScopedNoDenormals noDenormals;

                juce::MidiBuffer midi;

                while (! stop.load (std::memory_order_relaxed))
                {
                    lanes.followWorkgroup();
                    auto any = false;

                    for (int i = 0; i < lanes.count && i < static_cast<int> (instances.size()); ++i)
                    {
                        auto* lane = lanes.laneOf[static_cast<std::size_t> (i)];
                        auto& s = state[static_cast<std::size_t> (i)];
                        auto& instance = *instances[static_cast<std::size_t> (i)];
                        const auto request = lane->requestSeq.load (std::memory_order_acquire);

                        if (lanes.parked (i, request, s.answered, s.queued))
                        {
                            any = true;
                            continue;
                        }

                        /*  THE ONE THAT WAITS FIRST - it was asked first - then
                            one of the queue's, in the order they were asked
                            (namespace draft §149); one a pass, so the other
                            lanes of this child are looked at in between. */
                        const auto fromQueue = request <= s.answered;
                        const auto queuedNumber = s.queued + 1;

                        if (fromQueue && (lanes.queueSlots <= 0
                                            || lane->queueSeq.load (std::memory_order_acquire) < queuedNumber))
                            continue;

                        any = true;
                        const auto asked = fromQueue ? lanes.queuedRequest (i, queuedNumber) : lanes.waiting (i);

                        /*  TAKEN (namespace draft §148): how long the request
                            waited for this thread ends here, and what follows -
                            the values, the plugin's block, the copy back - is
                            the time in the plugin's hands. */
                        const auto takenAt = region::steadyNanos();

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

                        const auto numChannels = std::min<int> (lanes.channels, asked.numChannels);
                        const auto numSamples = std::min<int> ({ lanes.maxSamples, scratchSamples, asked.numSamples });

                        /*  BY THE RULES OF LaneMapping.h (2026-09-26): the cue's
                            channels into the main inputs - a mono cue into all
                            of them - every other input fed silence, the main
                            outputs back; a cue wider than the plugin takes, or
                            one it would make narrower, passes dry and whole. */
                        const lanemap::Shape shape { numChannels, layout.inputs, layout.outputs, lanes.channels };

                        if (lanemap::takes (shape))
                        {
                            /*  WHAT IT IS GIVEN, kept for the plugin's window while
                                the parent asks (namespace draft §146): the cue and
                                its key as the lane carries them, before the answer
                                is written back over them. */
                            if (s.tap.open (*lane, numSamples, static_cast<int> (lanes.header.sampleRate.load (std::memory_order_relaxed))))
                                region::tapBlock (lane, lanes.channels, lanes.maxSamples, numChannels, numSamples,
                                                  keyWidth[static_cast<std::size_t> (i)] > 0 ? asked.keyChannels : 0,
                                                  asked.audio, asked.key);

                            pointersFrom (asked.audio, lanes.channels, lanes.maxSamples, lanePointers);
                            fitScratch (scratch, numSamples);
                            scratch.clear();
                            lanemap::feedInto (lanePointers.data(), shape, scratch.getArrayOfWritePointers(), numSamples);

                            /*  AND THE KEY INTO ITS SIDECHAIN (namespace draft §141),
                                when its cue has one and the plugin took it; every
                                other block its sidechain is the silence above. */
                            if (const auto sidechain = keyWidth[static_cast<std::size_t> (i)]; sidechain > 0)
                            {
                                const auto keyChannels = std::min<int> (region::keyChannelsMax, asked.keyChannels);
                                const auto& at = keyIndex[static_cast<std::size_t> (i)].at;
                                auto* key = asked.key;
                                const float* keys[2] = { key, key + lanes.maxSamples };
                                float* into[2] = { nullptr, nullptr };
                                auto usable = 0;

                                for (int channel = 0; channel < sidechain; ++channel)
                                    if (at[channel] >= 0 && at[channel] < scratch.getNumChannels())
                                        into[usable++] = scratch.getWritePointer (at[channel]);

                                lanemap::keyInto (keys, keyChannels, into, usable, numSamples);
                            }

                            /*  The scratch itself, fitted above to the block's
                                length: the plugin sees the width it asked for and
                                the length the parent sent. Not a view of it - a
                                view of 32 channels or more allocates (§142). */
                            midi.clear();
                            instance.processBlock (scratch, midi);

                            lanemap::backInto (scratch.getArrayOfReadPointers(), shape, lanePointers.data(), numSamples);
                        }

                        region::noteTimesFrom (*lane, asked.asked, takenAt, region::steadyNanos(), app::processorNow());

                        if (fromQueue)
                        {
                            s.queued = queuedNumber;
                            lane->queueDone.store (queuedNumber, std::memory_order_release);
                        }
                        else
                        {
                            s.answered = request;
                            lane->responseSeq.store (request, std::memory_order_release);
                        }
                    }

                    lanes.restAfterIdlePass (any);
                }

                for (auto& instance : instances)
                    instance->setPlayHead (nullptr);

                lanes.workgroup.leave();
            }

            juce::AudioPluginFormatManager manager;
            std::vector<std::unique_ptr<juce::AudioPluginInstance>> instances;
            std::vector<std::unique_ptr<GridPlayHead>> playheads;    ///< one an instance (§123, BEJ)
            InsertLayout layout;
            std::array<float*, maxLaneChannels> lanePointers {};
            std::vector<float> baseline;
            std::vector<std::vector<float>> laneBaseline;
            juce::MemoryBlock initialState;
            juce::AudioBuffer<float> scratch;
            int scratchSamples = 0;     ///< the longest block it was made for; it is fitted to each (§142)
            int width = 0;

            /*  EACH INSTANCE'S SIDECHAIN (§141): how wide it is laid out, and
                where its channels sit in the buffer - the message thread's to
                change with the lane parked, the worker's to read. */
            struct KeyAt { int at[2] = { -1, -1 }; };
            std::vector<int> keyWidth;
            std::vector<KeyAt> keyIndex;
            double preparedRate = 48000.0;
            int preparedBlock = 512;
        };

        /*  A CUE'S WHOLE STATE, LOADED (the author's decision of 2026-09-25).
            On the message thread, because that is where a VST3 takes its
            state, every five milliseconds: a lane whose request moved asks
            the worker for its instance (the gate), and once the worker has
            let go - answering that lane dry, missing nothing - the state goes
            in, the epoch moves so every value is set again on top, the lane
            is given back, and the parent is answered with how it went. Lanes
            load one after another; that is the honest cost.

            AND A LANE'S RESET, the same way (namespace draft §139, BHC): the
            worker let go of the instance, the reset made here, the parent
            answered in `resetDoneSeq` - so the other lanes of this child go
            on being answered while it runs, as they do through a load. */
        struct StateLoader final : juce::Timer
        {
            using Load = std::function<std::string (int lane, const std::string& path)>;
            using Reset = std::function<void (int lane)>;

            /*  A RESET IS WHERE A KEY IS LAID OUT (namespace draft §141): the
                sidechain switched on or off as `keyWanted` says, read after the
                count it came with, and how wide it is now told back in
                `keyHeld` before the reset is answered. */

            StateLoader (Lanes& lanesToUse, Load loadToUse, Reset resetToUse)
                : lanes (lanesToUse), load (std::move (loadToUse)), reset (std::move (resetToUse)),
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
                    /*  A RESET ASKED FOR (namespace draft §139, BHC), a state,
                        or both - an arm asks both - under one hold of the
                        lane, the reset first. */
                    const auto request = lane->stateRequestSeq.load (std::memory_order_acquire);
                    const auto stateAsked = request > lane->stateDoneSeq.load (std::memory_order_relaxed);
                    const auto resetAsked = lane->resetSeq.load (std::memory_order_acquire);
                    const auto resetOwed = resetAsked != lane->resetDoneSeq.load (std::memory_order_relaxed);

                    if (! stateAsked && ! resetOwed)
                        return;

                    wait = {};
                    wait.active = true;
                    wait.state = stateAsked;
                    wait.reset = resetOwed;
                    wait.resetSeq = resetAsked;

                    if (stateAsked)
                    {
                        lane->statePath[region::pathChars - 1] = 0;
                        wait.seq = request;
                        wait.path = lane->statePath;
                    }

                    control.gate.store (1, std::memory_order_release);

                    /*  THE WORKER LETS GO WITHIN A PASS - at once when it is
                        answering, a millisecond when it rests - so it is
                        waited for here a moment and not until the next tick:
                        a cue fired cold waits on its reset. */
                    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (2);

                    while (control.gate.load (std::memory_order_acquire) != 2 && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for (std::chrono::microseconds (100));
                }

                //  Not let go yet: the worker grants within a pass, idle or not.
                if (control.gate.load (std::memory_order_acquire) != 2)
                    return;

                if (wait.reset)
                    reset (i);

                std::string problem;
                std::chrono::microseconds micros { 0 };

                if (wait.state)
                {
                    const auto began = std::chrono::steady_clock::now();
                    problem = load (i, wait.path);
                    micros = std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now() - began);
                    control.epoch.fetch_add (1, std::memory_order_release);
                }

                control.gate.store (0, std::memory_order_release);

                if (wait.state)
                {
                    std::memset (lane->stateProblem, 0, sizeof (lane->stateProblem));
                    std::snprintf (lane->stateProblem, sizeof (lane->stateProblem), "%s", problem.c_str());
                    lane->stateFailed.store (problem.empty() ? 0u : 1u, std::memory_order_relaxed);
                    lane->stateLoadMicros.store (static_cast<std::uint32_t> (std::min<long long> (micros.count(), 0xffffffffLL)),
                                                 std::memory_order_relaxed);
                    lane->stateDoneSeq.store (wait.seq, std::memory_order_release);
                }

                //  The count it was made for: one asked meanwhile is made at the next tick.
                if (wait.reset)
                    lane->resetDoneSeq.store (wait.resetSeq, std::memory_order_release);

                wait.active = false;
            }

            struct Waiting
            {
                bool active = false;
                bool state = false;
                bool reset = false;
                std::uint64_t seq = 0;
                std::uint32_t resetSeq = 0;
                std::string path;
            };

            Lanes& lanes;
            Load load;
            Reset reset;
            std::vector<Waiting> pending;
        };

        /*  `wfg plugin-host --catalogue-only --description=<file>`: one
            instance, the catalogue on stdout as JSON, and out. No region, no
            parent. What `wfg plugins --catalogue=<identifier>` runs. */
        int runCatalogueOnly (const std::vector<std::string>& args)
        {
            /*  The test child's is built in, and answers with no JUCE at all -
                which is what lets a test drive this verb on a CI runner. */
            if (Catalogue::isTestIdentifier (optionFrom (args, "--plugin")))
            {
                std::printf ("%s\n", Catalogue::testFor (optionFrom (args, "--plugin")).toJson().c_str());
                std::fflush (stdout);
                return 0;
            }

            juce::ScopedJuceInitialiser_GUI juceForTheChild;

           #if JUCE_MAC
            //  No Dock icon while the plugin is up: part of Go.dot (§138).
            juce::Process::setDockIconVisible (false);
           #endif

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

        if (mapping.getSize() < region::regionBytes (channels, maxSamples, laneCount,
                                                     static_cast<int> (header.queueSlots.load (std::memory_order_relaxed))))
        {
            reportFailure (header, "the shared region is shorter than its header says");
            return 3;
        }

        /*  JUCE up for the whole of the child's life, on this thread, which
            is therefore the message thread: where a plugin is made and takes
            its state, and where the loop below runs for its sake. */
        juce::ScopedJuceInitialiser_GUI juceForTheChild;

        /*  NEVER NAPPED, its timers never coalesced (app/MachinePower.h): a
            child with no window of its own looks idle to the system, and its
            worker answers the audio thread. */
        const app::LatencyCritical latencyCritical { "Go.dot is playing a plugin" };

        Lanes lanes (header, mapping.getData(), parentPid);
        std::atomic<bool> stop { false };
        std::thread answering;
        std::unique_ptr<TestGainWorker> testGain;
        std::unique_ptr<RealHost> real;

        if (Catalogue::isTestIdentifier (identifier))
        {
            /*  THE REPORT, before ready: what the parent reads into the table
                and what a value nobody set rests at. */
            const auto catalogue = Catalogue::testFor (identifier);
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

            testGain = std::make_unique<TestGainWorker> (lanes, identifier);

            const auto width = [] (int n) { return n == 1 ? std::string ("mono") : n == 2 ? std::string ("stereo")
                                                                              : std::to_string (n) + " channels"; };
            reportLayout (header, testGain->inputs, testGain->outputs,
                          width (testGain->inputs) + " in, " + width (testGain->outputs) + " out");

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
        StateLoader loader (lanes, [&testGain, &real, &lanes] (int lane, const std::string& path)
        {
            auto problem = testGain != nullptr ? testGain->loadState (lane, path) : real->loadState (lane, path);

            /*  AND WHAT THE PLUGIN DECLARES NOW (2026-09-26): a state can move
                a look-ahead, and the entry's latency is the largest a voice
                reports. Stored before the loader answers. */
            if (real != nullptr)
                lanes.laneOf[static_cast<std::size_t> (lane)]->latencySamples.store (
                    static_cast<std::uint32_t> (std::max (0, real->instances[static_cast<std::size_t> (lane)]->getLatencySamples())),
                    std::memory_order_relaxed);
            else if (testGain != nullptr)
                lanes.laneOf[static_cast<std::size_t> (lane)]->latencySamples.store (
                    static_cast<std::uint32_t> (testGain->instances[static_cast<std::size_t> (lane)].lookAhead),
                    std::memory_order_relaxed);

            return problem;
        },
        [&testGain, &real, &lanes] (int lane)
        {
            auto& shared = *lanes.laneOf[static_cast<std::size_t> (lane)];
            const auto keyed = shared.keyWanted.load (std::memory_order_relaxed) != 0;

            if (testGain != nullptr)
            {
                testGain->layKey (lane, keyed);
                testGain->resetLane (lane);
                shared.keyHeld.store (static_cast<std::uint32_t> (testGain->keyHeld (lane)), std::memory_order_relaxed);
            }
            else
            {
                real->layKey (lane, keyed);
                real->resetLane (lane);
                shared.keyHeld.store (static_cast<std::uint32_t> (real->keyHeld (lane)), std::memory_order_relaxed);
            }
        });

        /*  A KEY ALREADY WANTED (namespace draft §141): a child relaunched
            under a cue that has one, whose instances are fresh and unkeyed -
            laid out now, before ready, since no reset will come to ask. And
            what the plugin offers, for the parent's menu. */
        header.keyInputs.store (static_cast<std::uint32_t> (testGain != nullptr ? testGain->keyOffer() : real->keyOffer()),
                                std::memory_order_relaxed);

        for (int i = 0; i < lanes.count; ++i)
        {
            auto& shared = *lanes.laneOf[static_cast<std::size_t> (i)];
            const auto keyed = shared.keyWanted.load (std::memory_order_relaxed) != 0;

            if (testGain != nullptr)
            {
                testGain->layKey (i, keyed);
                shared.keyHeld.store (static_cast<std::uint32_t> (testGain->keyHeld (i)), std::memory_order_relaxed);
            }
            else if (i < static_cast<int> (real->instances.size()))
            {
                real->layKey (i, keyed);
                shared.keyHeld.store (static_cast<std::uint32_t> (real->keyHeld (i)), std::memory_order_relaxed);
            }
        }

        header.childReady.store (1, std::memory_order_release);

       #if JUCE_MAC
        juce::initialiseNSApplication();

        /*  NO DOCK ICON (namespace draft §138): one of these runs for every
            plugin a show plays, from inside Go.dot.app, and each was a
            "Go.dot" of its own in the Dock and in Cmd+Tab with no window - a
            tester switching apps met more and more empty Go.dots. */
        juce::Process::setDockIconVisible (false);
       #endif

        ExitWatch watch (&header, parentPid, lanes.workgroup);
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

                lanes.workgroup.poll();
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
