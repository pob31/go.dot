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

#include <wfg/engine/clock/DummyAudioClock.h>

#include <wfg/engine/clock/SampleTime.h>

#include <algorithm>
#include <chrono>

namespace wfg
{
    //==============================================================================
    DummyAudioClock::DummyAudioClock (int sampleRateHz, int samplesPerBlock)
        : rate (std::max (1, sampleRateHz)),
          block (std::max (1, samplesPerBlock))
    {
    }

    DummyAudioClock::~DummyAudioClock()
    {
        stop();
    }

    void DummyAudioClock::start()
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

    void DummyAudioClock::stop()
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
    void DummyAudioClock::run()
    {
        using Clock = std::chrono::steady_clock;

        const auto begin = Clock::now();
        std::int64_t delivered = 0;

        std::unique_lock<std::mutex> lock { mutex };

        while (! stopping)
        {
            /*  The deadline for the NEXT block, measured from the absolute
                start. Adding a delay per iteration instead would let every late
                block push the whole schedule out, and the clock would run
                slower than its own sample rate for the rest of the session
                rather than catching up. */
            const auto due = begin + samplesToDuration ((delivered + 1) * block, rate);

            if (wakeUp.wait_until (lock, due, [this] { return stopping; }))
                break;

            /*  Out of the lock to advance: stop() must not be made to wait
                behind a block, and nothing here touches the shared state the
                mutex protects. */
            lock.unlock();

            source.advance (block);

            const auto late = durationToSamples (
                std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now() - due), rate);

            lastLateness.store (late, std::memory_order_relaxed);

            if (late > maxLateness.load (std::memory_order_relaxed))
                maxLateness.store (late, std::memory_order_relaxed);

            ++delivered;
            blocks.store (delivered, std::memory_order_relaxed);

            lock.lock();
        }
    }
}
