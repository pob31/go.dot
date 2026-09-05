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
        /*  Nothing, deliberately, until PR 1.D brings spatcore in. See the
            header for why this reports false rather than pretending. */
        return false;
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

        std::unique_lock<std::mutex> lock { mutex };

        while (! stopping)
        {
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
