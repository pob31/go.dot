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
    The thread that turns a sample counter into ticks, and the engine's only
    caller of processTick().

    NOT juce::Timer, and the reason is measured rather than assumed. Spike 05
    (docs/spikes/spike05-param-50hz.md) put a 20 ms juce::Timer on an idle
    message thread and found a lateness floor of 0.76 ms at the median and
    2.60 ms at the 99th percentile with nothing else happening - that is the
    instrument's own noise, before any work. It also has to share the message
    thread with everything JUCE puts there. This schedules itself on a steady
    clock, on a thread of its own, and confirms against the sample counter
    before it commits to a tick.

    WHAT IT ACTUALLY WAITS FOR is the counter, not the wall clock. The wall
    clock only says roughly when to look. A tick is due when the sample counter
    has reached that tick's sample position, so the audio side decides when time
    passes and this thread merely notices - which is what keeps the tick index
    locked to audio rather than to how well the OS scheduled anything.

    IT NEVER SKIPS. Ticks are processed one at a time, in order, with no gaps,
    however far behind it falls: the tick index is the event log's ordering key
    and a gap in it would be a gap in the record of the show. When several ticks
    come due at once - one long block, one scheduling stall - they are processed
    back to back with no waiting in between. The first one drains the event
    queue, so the ones behind it usually have nothing to do and cost almost
    nothing.

    IT MEASURES ITS OWN LATENESS, in samples, and keeps the worst. That number
    is the honest report of everything above: the block size, the scheduler, the
    dummy clock's own pacing, and the work each tick did. It becomes
    /godot/engine/lateness and /godot/engine/latenessMax, which is where an
    operator would look when a show feels loose.
*/

#include <wfg/engine/clock/SampleClock.h>
#include <wfg/engine/clock/TickClock.h>

/*  The whole definition, not a forward declaration: AfterTick names
    Engine::TickResult by value. */
#include <wfg/engine/Engine.h>

#include <atomic>
#include <functional>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace wfg
{
    class Engine;

    //==============================================================================
    /*  What the tick thread should do next, given where the counter is.

        Split out as a pure function on purpose. Everything interesting about
        the schedule - a tick straddling a block, several ticks due after one
        long one, a rate change part way through - is arithmetic, and testing
        arithmetic through a thread means testing the OS scheduler at the same
        time and calling the result flaky. The thread below is a loop around
        this and holds no logic of its own. */
    struct TickAction
    {
        enum class Kind { process, wait };

        Kind kind = Kind::wait;

        /** The tick this is about: the one to process, or the one being waited
            for. */
        std::int64_t tick = 0;

        /** process: samples between that tick's position and where the counter
            actually is. Zero or more, never negative. */
        std::int64_t lateness = 0;

        /** wait: samples that still have to elapse before it comes due. */
        std::int64_t shortfall = 0;
    };

    TickAction nextTickAction (const TickClock& clock,
                               std::int64_t lastProcessed,
                               std::int64_t samplesNow) noexcept;

    //==============================================================================
    /*  Raises the calling thread towards real-time scheduling.

        PHASE 1 DOES NOTHING AND RETURNS FALSE, which is the honest answer
        rather than a convenient one. spatcore's rt/RtThreadPriority.h has the
        real thing - MMCSS "Pro Audio" through a runtime-loaded avrt on Windows,
        a mach time-constraint policy on macOS, SCHED_FIFO on Linux - and it
        arrives with PR 1.D, which pins spatcore as a submodule.

        A shim that returned true would be worse than no shim at all: the tick
        thread would report a scheduling guarantee it does not have, and the
        first person to investigate a late show would rule out the right cause
        on the strength of it. */
    bool elevateCurrentThreadForTicking() noexcept;

    //==============================================================================
    class TickThread
    {
    public:
        /** Neither reference may outlive this object. `clock` is whatever
            advances the samples: a DummyAudioClock's in Phase 1, the device
            callback's from Phase 2, a ManualClock's in a test. */
        TickThread (Engine& engineToDrive, const SampleClock& sampleSource,
                    TickClock scheduleToUse);
        ~TickThread();

        TickThread (const TickThread&) = delete;
        TickThread& operator= (const TickThread&) = delete;

        /*  Run on the tick thread immediately after each processTick, before
            the next one is considered.

            THIS IS WHERE THE TREE IS PUBLISHED AND THE PUSHES GO OUT. Both
            belong on this thread and in this order - the snapshot has to be the
            finished answer to the tick that just ran, and a push carrying a
            value from a tick that is still in progress is a push of something
            nobody decided.

            Set before start() and never while running: it is read by the tick
            thread with no synchronisation, exactly like UdpEndpoint's handler
            and for the same reason. A setter that could be called mid-flight
            would be a data race with a very quiet failure mode.

            It is handed the TickResult rather than the tick index, because the
            result carries `soleOrigin` - who caused this tick's changes - and
            echo suppression cannot be done without it.

            The clock knows nothing about what the hook does. It does not
            include a tree header, and `serve` is the only caller that sets
            one. */
        using AfterTick = std::function<void (const Engine::TickResult&)>;

        void setAfterTick (AfterTick hook) { afterTick = std::move (hook); }

        /** Starts at tick 0 and works forwards. Calling it twice does nothing
            the second time. */
        void start();

        /** Stops after the tick in progress and joins. The destructor calls it. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        //======================================================================
        /** The last tick handed to the engine, or -1 before the first. */
        std::int64_t lastTick() const noexcept
        {
            return processed.load (std::memory_order_relaxed);
        }

        /** How many have been processed. Equal to lastTick() + 1, because none
            is ever skipped - which is the point of saying both. */
        std::int64_t ticksProcessed() const noexcept
        {
            return processed.load (std::memory_order_relaxed) + 1;
        }

        /** Samples between the most recent tick's position and where the
            counter was when it ran. */
        std::int64_t lateness() const noexcept
        {
            return lastLateness.load (std::memory_order_relaxed);
        }

        /** The worst since start(). An average would hide the one tick that ran
            40 ms late, and that is the tick somebody noticed. */
        std::int64_t latenessMax() const noexcept
        {
            return maxLateness.load (std::memory_order_relaxed);
        }

        int sampleRate() const noexcept     { return schedule.sampleRate(); }
        int samplesPerTick() const noexcept { return schedule.samplesPerTick(); }

        /** What elevateCurrentThreadForTicking() actually managed. False in
            Phase 1; see the comment on that function. */
        bool hasElevatedPriority() const noexcept
        {
            return elevated.load (std::memory_order_relaxed);
        }

    private:
        AfterTick afterTick;

        void run();

        Engine& engine;
        const SampleClock& samples;
        TickClock schedule;

        std::thread worker;
        std::mutex mutex;
        std::condition_variable wakeUp;
        bool stopping = false;

        std::atomic<bool> running { false };
        std::atomic<bool> elevated { false };
        std::atomic<std::int64_t> processed { -1 };
        std::atomic<std::int64_t> lastLateness { 0 };
        std::atomic<std::int64_t> maxLateness { 0 };
    };
}
