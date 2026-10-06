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

namespace spatcore::rt
{
    class AudioWorkgroupCoordinator;
}

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
    /*  Raises the calling thread towards real-time scheduling, ABOVE THE WINDOW
        AND BELOW THE SOUND (2026-10-06, the author: "raise the thread priority
        on all OSes"). Until then this did nothing, and the show's clock ran at
        the priority of every background task on the machine.

        - Windows: THREAD_PRIORITY_TIME_CRITICAL, the top of the ordinary
          range - JUCE's own "realtime" on Windows. Not MMCSS "Pro Audio",
          which spatcore gives an audio thread: that class sits at 23 to 26,
          above an ASIO driver's TIME_CRITICAL callback, and a tick at work
          could then hold the one thread that makes the sound off a core.
        - macOS: the mach time-constraint policy, a 20 ms period with a 5 ms
          budget (spatcore's rt/RtThreadPriority.h); the thread also joins the
          interface's audio workgroup, see TickThread::setWorkgroups.
        - Linux: SCHED_FIFO at the middle of its range (spatcore), under the
          priorities JACK and PipeWire give their audio threads. A user without
          real-time rights gets false and an unchanged thread.

        TRUE ONLY FOR WHAT TOOK EFFECT. A shim that reported a guarantee it did
        not have would let the first person investigating a late show rule out
        the right cause on the strength of it - which is why Phase 1's stub
        returned false rather than pretending. */
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
        /*  Run on the tick thread immediately BEFORE each processTick, so
            anything it submits is drained by that same tick.

            The distinction is not fussiness. What the Runner submits here is
            what it observed of the audio side - a cue started, a cue ended -
            and submitting it from the AFTER hook would put it in the queue that
            the NEXT tick drains, so the log would say it happened one tick
            after it did. A replay would then reproduce a session twenty
            milliseconds out of step with the one that was recorded, faithfully,
            for ever. */
        using BeforeTick = std::function<void (std::int64_t tick)>;

        void setBeforeTick (BeforeTick hook) { beforeTick = std::move (hook); }

        using AfterTick = std::function<void (const Engine::TickResult&)>;

        void setAfterTick (AfterTick hook) { afterTick = std::move (hook); }

        /*  THE INTERFACE'S AUDIO WORKGROUP, for macOS (2026-10-06, the
            author: "use workgroups on macOS"). The device layer publishes the
            open interface's workgroup into the coordinator whenever the device
            starts or stops; this thread joins it at the top of its loop when it
            changed - one atomic load otherwise - so a device reopened while the
            thread runs (an outage, PRD §6.2) is followed rather than left on a
            workgroup that has gone. Joining tells the scheduler the tick is part
            of the work that keeps the sound on time, which on Apple silicon is
            what keeps it on a performance core beside the IO thread.

            Elsewhere the handle is empty and joining does nothing. Set before
            start() and never while running, like the hooks above; the
            coordinator must outlive the thread. Null means no workgroup. */
        void setWorkgroups (spatcore::rt::AudioWorkgroupCoordinator* coordinator) noexcept
        {
            workgroups = coordinator;
        }

        /** Starts at tick 0 and works forwards. Calling it twice does nothing
            the second time. */
        void start();

        /** Stops after the tick in progress and joins. The destructor calls it. */
        void stop();

        // Keep serving commands and publishing snapshots at the frozen tick,
        // without running the scheduler or advancing any cue's elapsed time.
        void setSuspended (bool value) noexcept { suspended.store (value); wakeUp.notify_all(); }
        bool isSuspended() const noexcept { return suspended.load(); }

        // Only while stopped. Keep tick indices monotonic across a device switch.
        std::int64_t rebaseAudio (int newSampleRate)
        {
            const auto next = lastTick() + 1;
            schedule.rebase (next, newSampleRate);
            return schedule.sampleForTick (next);
        }

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

        /*  The worst since the last time this was asked, and nought again: a
            stall noticed once, when it happened, rather than a maximum that
            stops moving after the first (2026-10-06, the engine log's notes). */
        std::int64_t takeWorstLateness() noexcept
        {
            return worstSince.exchange (0, std::memory_order_relaxed);
        }

        int sampleRate() const noexcept     { return schedule.sampleRate(); }
        int samplesPerTick() const noexcept { return schedule.samplesPerTick(); }

        /** What elevateCurrentThreadForTicking() actually managed on this
            thread; see the comment on that function. */
        bool hasElevatedPriority() const noexcept
        {
            return elevated.load (std::memory_order_relaxed);
        }

    private:
        BeforeTick beforeTick;
        AfterTick afterTick;
        spatcore::rt::AudioWorkgroupCoordinator* workgroups = nullptr;

        void run();

        Engine& engine;
        const SampleClock& samples;
        TickClock schedule;

        std::thread worker;
        std::mutex mutex;
        std::condition_variable wakeUp;
        bool stopping = false;

        std::atomic<bool> running { false };
        std::atomic<bool> suspended { false };
        std::atomic<bool> elevated { false };
        std::atomic<std::int64_t> processed { -1 };
        std::atomic<std::int64_t> lastLateness { 0 };
        std::atomic<std::int64_t> maxLateness { 0 };
        std::atomic<std::int64_t> worstSince { 0 };
    };
}
