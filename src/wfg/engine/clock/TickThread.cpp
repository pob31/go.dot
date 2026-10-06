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

#include <wfg/engine/clock/TickThread.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/SampleTime.h>

#include <spatcore/rt/RtThreadPriority.h>
#include <spatcore/rt/AudioWorkgroupCoordinator.h>

#include <chrono>

namespace wfg
{
    namespace
    {
        /*  How early to stop sleeping and start looking.

            A general-purpose scheduler wakes a thread when it gets round to it,
            and on Windows the default granularity is coarser than a 512-sample
            block at 44.1 kHz. Sleeping right up to the deadline would therefore
            hand the whole of that granularity straight to the lateness figure.
            So the wait stops two milliseconds short and the last stretch is
            covered by short polls: the oversleep is bounded by the poll instead
            of by the scheduler.

            Two milliseconds and a quarter of a millisecond, so a 20 ms tick
            costs one long sleep and about eight short ones rather than eighty.
            Waiting in uniform short slices instead would work and would spin
            the CPU up for no benefit anybody can hear. */
        constexpr auto wakeGuard = std::chrono::milliseconds { 2 };
        constexpr auto pollSlice = std::chrono::microseconds { 250 };

        /*  What the macOS time-constraint policy is told: one tick's period,
            and a quarter of it as the work a tick ordinarily does. A tick that
            catches up runs past it now and then, which the policy tolerates;
            one that ran past it every time would be demoted by the kernel,
            which is the failsafe working rather than a fault. */
        constexpr double tickPeriodMs = 20.0;
        constexpr double tickBudgetMs = 5.0;
    }

    //==============================================================================
    TickAction nextTickAction (const TickClock& clock,
                               std::int64_t lastProcessed,
                               std::int64_t samplesNow) noexcept
    {
        TickAction action;
        action.tick = lastProcessed + 1;

        const auto due = clock.sampleForTick (action.tick);

        if (samplesNow >= due)
        {
            action.kind = TickAction::Kind::process;
            action.lateness = samplesNow - due;
            return action;
        }

        action.kind = TickAction::Kind::wait;
        action.shortfall = due - samplesNow;
        return action;
    }

    //==============================================================================
    bool elevateCurrentThreadForTicking() noexcept
    {
       #if defined (_WIN32)
        /*  Not spatcore's MMCSS "Pro Audio", which would put the tick above an
            ASIO callback at TIME_CRITICAL - see the header. */
        return ::SetThreadPriority (::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
       #else
        return spatcore::rt::setCurrentThreadAudioPriority (tickPeriodMs, tickBudgetMs);
       #endif
    }

    //==============================================================================
    TickThread::TickThread (Engine& engineToDrive, const SampleClock& sampleSource,
                            TickClock scheduleToUse)
        : engine (engineToDrive), samples (sampleSource), schedule (scheduleToUse)
    {
    }

    TickThread::~TickThread()
    {
        stop();
    }

    void TickThread::start()
    {
        if (worker.joinable())
            return;

        {
            const std::lock_guard<std::mutex> lock { mutex };
            stopping = false;
        }

        running.store (true, std::memory_order_relaxed);
        worker = std::thread ([this] { run(); });
    }

    void TickThread::stop()
    {
        if (! worker.joinable())
            return;

        {
            const std::lock_guard<std::mutex> lock { mutex };
            stopping = true;
        }

        wakeUp.notify_all();
        worker.join();
        running.store (false, std::memory_order_relaxed);
    }

    //==============================================================================
    void TickThread::run()
    {
        elevated.store (elevateCurrentThreadForTicking(), std::memory_order_relaxed);

        /*  Made, joined and left on this thread, which is what a token asks:
            its destructor at the end of this function leaves the workgroup. */
        juce::WorkgroupToken workgroupToken;
        std::uint32_t workgroupSeen = 0;

        std::unique_lock<std::mutex> lock { mutex };

        while (! stopping)
        {
            if (workgroups != nullptr)
                workgroups->joinIfChanged (workgroupToken, workgroupSeen);

            if (suspended.load())
            {
                lock.unlock();
                const auto at = std::max<std::int64_t> (0, lastTick());
                const auto outcome = engine.processTick (at);
                if (afterTick) afterTick (outcome);
                lock.lock();
                wakeUp.wait_for (lock, std::chrono::milliseconds (20), [this] { return stopping; });
                continue;
            }
            const auto action = nextTickAction (schedule,
                                                processed.load (std::memory_order_relaxed),
                                                samples.samplesElapsed());

            if (action.kind == TickAction::Kind::process)
            {
                /*  Out of the lock while the engine works. A tick can take real
                    time - it applies every command submitted since the last one
                    - and stop() must not be made to queue behind it. */
                lock.unlock();

                lastLateness.store (action.lateness, std::memory_order_relaxed);

                if (action.lateness > maxLateness.load (std::memory_order_relaxed))
                    maxLateness.store (action.lateness, std::memory_order_relaxed);

                /*  Raised here only; `takeWorstLateness` swaps it back to
                    nought from another thread, so a lost race costs one reading
                    of one tick, never a wrong one. */
                if (action.lateness > worstSince.load (std::memory_order_relaxed))
                    worstSince.store (action.lateness, std::memory_order_relaxed);

                if (beforeTick != nullptr)
                    beforeTick (action.tick);

                const auto outcome = engine.processTick (action.tick);

                /*  Before `processed` is advanced, so that a reader which sees
                    the new index knows the tree for it has been published and
                    its pushes sent - not merely that the engine finished. */
                if (afterTick != nullptr)
                    afterTick (outcome);

                /*  Published only after the engine has finished with it, so a
                    reader that sees this index knows that tick's work is done
                    rather than merely started. */
                processed.store (action.tick, std::memory_order_relaxed);

                lock.lock();

                /*  Straight round again with no wait. Several ticks due at once
                    are processed back to back; the first drained the queue, so
                    the rest usually cost nothing. */
                continue;
            }

            const auto remaining = samplesToDuration (action.shortfall, schedule.sampleRate());

            const auto slice = remaining > wakeGuard
                             ? std::chrono::duration_cast<std::chrono::nanoseconds> (remaining - wakeGuard)
                             : std::chrono::duration_cast<std::chrono::nanoseconds> (pollSlice);

            wakeUp.wait_for (lock, slice, [this] { return stopping; });
        }
    }
}
