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
    An audio device that is not there: a thread advancing the sample counter in
    blocks, paced to the wall clock.

    Phase 1 has no audio (devplan), and yet almost everything it builds is
    driven by a sample counter that a device would normally advance. This is
    what stands in until Phase 2 replaces it with the real callback, and the
    substitution has to be invisible: TickThread holds a SampleClock and cannot
    tell which kind it got.

    IT IS PACED, NOT FREE-RUNNING, and that is the entire point. A loop calling
    advance() as fast as it can would make every test pass instantly and prove
    nothing about a 50 Hz tick; the tick thread would never wait, so its waiting
    would never be exercised. This delivers block N at start + N × block ÷ rate,
    computed from the ABSOLUTE start rather than by adding a delay each time, so
    one late block does not push every later one out with it.

    IT IS NOT REAL TIME AND DOES NOT PRETEND TO BE. A general-purpose scheduler
    wakes a sleeping thread when it gets round to it: on Windows the default
    timer granularity is coarser than a 512-sample block at 44.1 kHz, and a
    loaded CI runner can be worse anywhere. So this MEASURES ITS OWN LATENESS
    and reports it rather than asserting it is small. A number that says "the
    dummy clock ran 4 ms behind" is worth having; a dummy clock that quietly
    pretended otherwise would make every lateness measurement above it a
    fiction.

    That is also why nothing in the test suite asserts this thing is fast. What
    is asserted is that it is PACED - a free-running loop would deliver
    thousands of blocks where this delivers tens, which is a difference no
    scheduler jitter can disguise.
*/

#include <wfg/engine/clock/AudioClockSource.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace wfg
{
    class DummyAudioClock
    {
    public:
        /** Both must be positive; the rate does not have to divide by 50, since
            it is TickClock that cares about that. */
        DummyAudioClock (int sampleRateHz, int samplesPerBlock);
        ~DummyAudioClock();

        DummyAudioClock (const DummyAudioClock&) = delete;
        DummyAudioClock& operator= (const DummyAudioClock&) = delete;

        /** Starts the pacing thread. Calling it twice does nothing the second
            time. */
        void start();

        /** Stops it and joins. Safe to call twice, and the destructor calls it. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        //======================================================================
        /** The counter this advances. Hand it to TickThread. */
        SampleClock& clock() noexcept             { return source; }
        const SampleClock& clock() const noexcept { return source; }

        int sampleRate() const noexcept { return rate; }
        int blockSize() const noexcept  { return block; }

        //======================================================================
        std::int64_t blocksDelivered() const noexcept
        {
            return blocks.load (std::memory_order_relaxed);
        }

        /** How late the most recent block was against its own deadline, in
            samples. Never negative: a block delivered early is delivered on
            time as far as anything downstream can tell. */
        std::int64_t blockLateness() const noexcept
        {
            return lastLateness.load (std::memory_order_relaxed);
        }

        /** The worst of those since start(). This is the honest headline: an
            average would hide the one block that arrived 30 ms late, and that
            block is the one somebody heard. */
        std::int64_t worstBlockLateness() const noexcept
        {
            return maxLateness.load (std::memory_order_relaxed);
        }

    private:
        void run();

        AudioClockSource source;

        const int rate;
        const int block;

        std::thread worker;
        std::mutex mutex;
        std::condition_variable wakeUp;
        bool stopping = false;

        std::atomic<bool> running { false };
        std::atomic<std::int64_t> blocks { 0 };
        std::atomic<std::int64_t> lastLateness { 0 };
        std::atomic<std::int64_t> maxLateness { 0 };
    };
}
