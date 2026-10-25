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
    THE SHOW HELD (PRD §3.34, Pause / Play): whether the block source runs the
    graph this block, and the de-click on the way out and back in.

    WHY THE GRAPH AND NOT EVERY CUE. A run's deadlines are absolute ticks and a
    voice's place is the sample count, and both come from one counter that only
    the graph advances. Not pulling the graph stands every sound, every fade,
    every wait and the tick index still together, as an outage does (PRD §6.2),
    and Play goes on from the same sample with nothing to shift. Pausing each
    cue instead would mean stopping and relaunching every voice at an offset and
    moving every deadline, which is the outage's lesson read backwards.

    THE STATES, as the audio thread lives them:
    - open: pulled, at full level;
    - closing: pulled, ramping down over `rampSamples` (0.1 s, the author's
      de-click, 2026-10-25) - five ticks of ordinary show time;
    - held: NOT pulled - the count stands, and the tick thread drains commands
      at the frozen tick (TickThread, SampleClock::isHeld);
    - muted: pulled at nought - what Esc while paused needs, so footers run
      and stops land with nothing heard;
    - opening: pulled, ramping back up.
    Only the wanted state is asked for (`want`, any thread); the five above
    follow from it and the level, a block at a time.

    THE LIPOGRAM (PRD §4.2): `beginBlock` and `applyGain` run on the audio
    thread - atomics, integers and a loop. The level is the audio thread's
    alone; `prepare` sets it only while no block runs.

    Vendor-free, like the clocks beside it: DummyAudioClock uses it too.
*/

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace wfg
{
    class ShowHold
    {
    public:
        enum class Want : int { open, hold, muted };

        /*  Any thread. `atOnce` skips the ramp: a source that is not sounding
            (an outage, the dummy clock) goes straight to the state. */
        void want (Want next, bool atOnce = false) noexcept
        {
            if (atOnce)
                snap.store (true, std::memory_order_relaxed);

            wish.store (static_cast<int> (next), std::memory_order_release);
        }

        Want wanted() const noexcept { return static_cast<Want> (wish.load (std::memory_order_acquire)); }

        /*  Before the source starts, or while no block runs: the ramp's length
            in samples (0 for none), and the level the wanted state stands at -
            a source started while held starts held. */
        void prepare (int samplesOfRamp) noexcept
        {
            rampSamples = std::max (0, samplesOfRamp);
            blockWant = wanted();
            position = blockWant == Want::open ? rampSamples : 0;
            closes.store (-1, std::memory_order_relaxed);
            snap.store (false, std::memory_order_relaxed);
            heldNow.store (wanted() == Want::hold, std::memory_order_release);
        }

        /*  THE AUDIO THREAD, at the top of every block, with the count the
            block starts on: whether the graph runs. False only once the level
            has reached nought while held. */
        bool beginBlock (std::int64_t blockStart = 0) noexcept
        {
            blockWant = wanted();
            const auto target = blockWant == Want::open ? rampSamples : 0;

            if (snap.exchange (false, std::memory_order_relaxed) || rampSamples == 0)
                position = target;

            /*  WHERE THE COUNT WILL STAND, said as soon as the way down
                begins - the ramp's samples still to go, one a sample - so what
                runs on a clock of its own and draws ahead of the count (the
                renderer, MTC) stops there rather than overshooting and
                stepping back. Unsaid the moment the show is let go. */
            if (blockWant == Want::hold)
            {
                if (closes.load (std::memory_order_relaxed) < 0)
                    closes.store (blockStart + position, std::memory_order_release);
            }
            else
            {
                closes.store (-1, std::memory_order_release);
            }

            const auto pull = ! (blockWant == Want::hold && position == 0);
            heldNow.store (! pull, std::memory_order_release);
            return pull;
        }

        /*  THE AUDIO THREAD, after the graph, on what leaves: the ramp, or
            nought, or nothing at all at full level. Every channel gets the
            same gains, sample for sample. */
        void applyGain (float* const* channels, int numChannels, int numSamples) noexcept
        {
            const auto target = blockWant == Want::open ? rampSamples : 0;

            if (position == target)
            {
                if (blockWant == Want::open)
                    return;

                for (int channel = 0; channel < numChannels; ++channel)
                    if (channels[channel] != nullptr)
                        std::fill (channels[channel], channels[channel] + numSamples, 0.0f);

                return;
            }

            const auto direction = target > position ? 1 : -1;
            const auto scale = 1.0f / static_cast<float> (rampSamples);
            auto reached = position;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto at = position;

                if (auto* data = channels[channel]; data != nullptr)
                    for (int sample = 0; sample < numSamples; ++sample)
                    {
                        if (at != target)
                            at += direction;

                        data[sample] *= static_cast<float> (at) * scale;
                    }
                else
                    at = direction > 0 ? std::min (target, position + numSamples)
                                       : std::max (target, position - numSamples);

                reached = at;
            }

            if (numChannels == 0)
                reached = direction > 0 ? std::min (target, position + numSamples)
                                        : std::max (target, position - numSamples);

            position = reached;
        }

        /*  Any thread: the graph is not being pulled. What the tick thread
            reads, through the sample clock, to drain at the frozen tick. */
        bool held() const noexcept { return heldNow.load (std::memory_order_acquire); }

        /*  Any thread: the count the show stands at once held, on the block
            source's own count - known from the first block of the way down -
            or -1 while it is not being held. */
        std::int64_t closesAt() const noexcept { return closes.load (std::memory_order_acquire); }

        /*  The audio thread's, or a test's between blocks: the level, 0..1. */
        float level() const noexcept
        {
            return rampSamples == 0 ? (blockWant == Want::open ? 1.0f : 0.0f)
                                    : static_cast<float> (position) / static_cast<float> (rampSamples);
        }

    private:
        std::atomic<int> wish { static_cast<int> (Want::open) };
        std::atomic<bool> snap { false };
        std::atomic<bool> heldNow { false };
        std::atomic<std::int64_t> closes { -1 };

        int rampSamples = 0;
        int position = 0;
        Want blockWant = Want::open;
    };
}
