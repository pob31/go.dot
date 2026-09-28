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
    ONE VOICE'S SPEED, ON THE AUDIO SIDE (namespace draft §22.4).

    The Runner decides a voice's speed on the tick thread and places each change
    a launch horizon ahead, as a breakpoint on the beat axis - at this beat, this
    speed. This is where those breakpoints cross to the audio thread, and where
    the audio thread keeps them: a queue of sixty-four one way (the Looper's
    shape, Looper.h), drained once a block before the graph runs, into a
    RateClock the voice's slots ask while it runs.

    THE CLOCK IS NEVER RESET. A new cue on the voice is a step at its launch -
    the speed held until then, then the new one - never a clean slate, because a
    slate wiped the moment the message arrived would be wiped under whatever the
    voice is still playing: the last blocks of the cue before, fading out, would
    be read against a history that no longer exists. Placed ahead and never
    rewritten, the history a block plays is the one it was always going to have.

    A breakpoint that arrives for a moment already played is placed at the
    block's start instead, and counted: the horizon exists so that never
    happens, and a count that moves says the horizon was wrong.

    Vendor-free: the slots that read it are adaptors in AudioHost.cpp, the one
    file that knows Tracktion.
*/

#include <wfg/engine/clock/RateClock.h>
#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace wfg::audio
{
    class RateVoice
    {
    public:
        static constexpr int inboxSize = 64;

        /** At this beat (monotonic, the launch handles' axis), this speed. */
        struct Breakpoint
        {
            double at = 0.0;
            double rate = 1.0;
        };

        /*  Begins at beat nought, at one: the identity, which is what a voice
            plays at until somebody places anything. */
        RateVoice() noexcept { rateClock.start (0.0, 1.0); }

        RateVoice (const RateVoice&) = delete;
        RateVoice& operator= (const RateVoice&) = delete;

        /*  Tick thread. False when the queue is full, which it cannot be unless
            the audio thread has stopped draining it: two breakpoints a tick
            against a drain every block. */
        bool post (Breakpoint breakpoint) noexcept
        {
            const auto tail = inboxTail.load (std::memory_order_relaxed);

            if (tail - inboxHead.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (inboxSize))
                return false;

            inbox[tail % static_cast<std::uint32_t> (inboxSize)] = breakpoint;
            inboxTail.store (tail + 1, std::memory_order_release);
            return true;
        }

        /*  Audio thread, once a block, before the graph runs: every breakpoint
            posted since the last block goes into the clock, then the past before
            this block is let go - the clock keeps the point in force at the
            block's start, which is all a read from here on can need. */
        void drain (double blockStartBeat) noexcept WFG_AUDIO_THREAD
        {
            for (;;)
            {
                const auto head = inboxHead.load (std::memory_order_relaxed);

                if (head == inboxTail.load (std::memory_order_acquire))
                    break;

                const auto breakpoint = inbox[head % static_cast<std::uint32_t> (inboxSize)];
                inboxHead.store (head + 1, std::memory_order_release);

                auto at = breakpoint.at;

                if (at < blockStartBeat)
                {
                    at = blockStartBeat;
                    late.fetch_add (1, std::memory_order_relaxed);
                }

                if (rateClock.size() > 0)
                    at = std::max (at, rateClock.back().at);

                if (! rateClock.place (at, breakpoint.rate))
                    refused.fetch_add (1, std::memory_order_relaxed);
            }

            rateClock.forgetBefore (blockStartBeat);
        }

        /*  The audio thread's, while the graph runs: the slots read it between
            one drain and the next, on the thread that drains it. */
        const RateClock& clock() const noexcept { return rateClock; }

        /*  Any thread, for a test or a readout: breakpoints that arrived after
            their moment had played, and ones the clock had no room for. */
        std::uint32_t lateCount() const noexcept     { return late.load (std::memory_order_relaxed); }
        std::uint32_t refusedCount() const noexcept  { return refused.load (std::memory_order_relaxed); }

    private:
        std::array<Breakpoint, inboxSize> inbox {};
        std::atomic<std::uint32_t> inboxHead { 0 }, inboxTail { 0 };
        RateClock rateClock;
        std::atomic<std::uint32_t> late { 0 }, refused { 0 };
    };
}
