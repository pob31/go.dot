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
    LTC, DRAWN ON THE AUDIO THREAD (namespace draft §59, TC.3; AHI, AHJ, AHK,
    AHL, AHU).

    SMPTE linear timecode is a sound, and Go.dot draws it itself, sample by
    sample, into the one output channel the show names - after that output's
    trim and DCA (AudioHost runs this last), REPLACING whatever the graph mixed
    there: no cue's sound can leak onto the line and no DCA pulled down can cut
    the code, and between runs the channel is silent (AHI).

    THE TICK HANDS OVER THREE THINGS, through an inbox of thirty-two one way
    (RateVoice's shape: a single producer, a single consumer, two atomics and
    no lock): START program P, frame F beginning on sample S - replacing
    whatever program is drawing at S, so a takeover or a jump never leaves the
    line quiet between them (AHL); STOP P at the first frame boundary at or
    after a sample - Esc, a stop cue, a length run out - so a reader never sees
    half a frame (AHK); CUT P on a sample exactly - a double Esc. Each block
    drains the inbox into a short list kept in the order of the samples its
    commands are for, and applies each on its own sample, splitting the block
    there. A command placed ahead waits in that list for as many blocks as it
    has to.

    LATE, OR AS SOON AS CAN BE. A start or a stop placed for a sample this side
    has already drawn arrived late - the launch horizon exists so that never
    happens - and is counted, and applied where it can still be honest: a stop
    at the block's start (and so at the next frame boundary after it), a start
    at the first frame boundary of ITS OWN clock at or after the block's start,
    so a late code still begins on a frame and is still where it would have
    been. A stop or a cut whose sample is nought or less was never placed: an
    Esc is decided on the tick, and takes effect in the block that drains it,
    never counted.

    A STOP TAKES BACK A START NOT YET BEGUN. An Esc pressed inside the launch
    horizon reaches the audio side while the start it ends is still waiting
    for its sample; the program would otherwise begin after the stop meant
    for it had passed it by. So a stop or a cut also drops every start of its
    program that was posted before it and has not begun - and a start posted
    after it is a new one, and plays.

    AND A START IS NOT ENDED BY A STOP POSTED BEFORE IT (TC.7). A jump moves a
    running code by starting its own program again from another frame - the
    same program, so an Esc pressed while the jump waits for its sample stops
    both the old placement and the new one. The old placement's length had
    already posted its stop, for where the old frames would have run out; left
    to land, it would end the code a jump had moved back at the old frame's
    end. So a stop or a cut reaches only a start of its program posted before
    it: one posted after it is the program placed again, which a stop it never
    saw cannot end - as a new program on the MIDI sender's lane is never
    ended by what was posted for the one it replaced.

    THE BIT CLOCK IN WHOLE NUMBERS (§59.1). A run counts half-bits from its
    zero, the first sample of the frame it starts on: the half-bit at sample
    `s` is `floor ((s - zero) * 160 * num / (rate * den))`, the frame that over
    160, the bit half of it modulo 80. That is FrameClock.h's one rule with 160
    parts, and this file never re-derives it: `partAtSample` says where the
    line is at the start of every stretch it draws, and `partStartSample` where
    the next edge falls, each computed afresh from its number. Nothing is a
    double and nothing is a sum of steps, so the code cannot drift from the
    sound beside it however long it runs. A frame's labels come through
    `labelOf` - drop-frame and the wrap at midnight honoured - and its eighty
    bits from `ltcBits`, worked out once at each new frame and kept.

    BIPHASE MARK: the level changes at the start of every bit cell - every even
    half-bit - and in the middle of a cell whose bit is one. A frame holds an
    even number of ones (the encoder's polarity bit), so every frame begins on
    the level the run's first frame began on: the line's level anywhere is
    found by walking at most one frame's 160 half-bits from that frame's start.

    THE EDGE (AHJ). A half-bit's exact moment, `k * rate * den / (160 * num)`,
    falls between two samples as often as on one; the sample that holds it
    takes the area-weighted mean of the two levels, so the edge is placed to
    the fraction of a sample. Then a one-pole low-pass of time constant 40 /
    2.197 microseconds - a 10-90 % rise of 40, SMPTE 12M's 40 +- 10 - rounds
    every edge alike. Its state is reset when a run starts on a silent line,
    whose first edge, at zero, goes from nought up to the level, so the first
    sample that is not nought is zero itself. A run that takes over a drawing
    line keeps the state, and its first edge goes the other way from where the
    line stands, so the takeover is one more edge rather than a glitch.

    THE PRODUCTS NEVER OVERFLOW. FrameClock splits every count into whole
    periods before it multiplies; the one product made here, a half-bit's
    fraction of a sample, is `(k mod 160 * num) * rate * den`, below 160 *
    30000 * 192000 * 1001 = 9.2e14 - a ten-thousandth of what 63 bits hold, a
    day at 192 kHz or a year.

    AN AUDIO RESTART (AHU). `prepare` ends the run, empties the inbox and the
    list, and moves a generation counter on; the Runner reads the counter and
    ends its timecode runs, as a restart ends sound. A command posted under the
    generation before is dropped unseen if it reaches the audio side after it.

    Vendor-free, allocation-free, lock-free: the audio thread renders it, the
    tick thread posts to it, and only the readouts are read anywhere else.
*/

#include <wfg/engine/rt/RtCheck.h>
#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/LtcEncoder.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wfg::timecode
{
    class LtcGenerator
    {
    public:
        static constexpr int inboxSize = 32;

        //  Two half-bits to each of a frame's eighty bit cells.
        static constexpr std::int64_t halfBitsPerFrame = 2 * ltcBitsPerFrame;

        /*  The slowest rate drawn. At 30 frames a second a half-bit is then
            still 1.7 samples, so no sample ever holds two edges - which the
            area-weighting assumes. Any interface is far above it. */
        static constexpr int lowestSampleRate = 8000;

        //  SMPTE 12M's rise time, 10 % to 90 %, and the time constant giving it.
        static constexpr double riseSeconds = 40.0e-6;
        static constexpr double tauSeconds = riseSeconds / 2.197;

        //  -18 dBFS (AHH), the level a channel plays at until it is told one.
        static constexpr float defaultGain = 0.125892541f;

        enum class Verb
        {
            start,
            stop,
            cut
        };

        /*  ONE COMMAND. `sample` is a start's zero - the first sample of frame
            `startFrame`, a frame counted from 00:00:00:00 at `rate` - or the
            sample a stop or a cut is for, nought or less for as soon as can
            be. `program` names the run; nought is no program. */
        struct Command
        {
            Verb verb = Verb::start;
            std::uint64_t program = 0;
            std::int64_t sample = 0;
            std::int64_t startFrame = 0;
            Rate rate = Rate::fps25;

            static Command start (std::uint64_t id, std::int64_t zeroSample, std::int64_t frame, Rate frameRate) noexcept
            {
                return { Verb::start, id, zeroSample, frame, frameRate };
            }

            static Command stop (std::uint64_t id, std::int64_t atSample) noexcept
            {
                return { Verb::stop, id, atSample, 0, Rate::fps25 };
            }

            static Command cut (std::uint64_t id, std::int64_t atSample) noexcept
            {
                return { Verb::cut, id, atSample, 0, Rate::fps25 };
            }
        };

        LtcGenerator() = default;
        LtcGenerator (const LtcGenerator&) = delete;
        LtcGenerator& operator= (const LtcGenerator&) = delete;

        /*  Tick thread. False when the inbox is full, which it cannot be
            unless the audio thread has stopped draining it, or for program
            nought. The command is stamped with the generation it was posted
            under. */
        bool post (const Command& command) noexcept
        {
            if (command.program == 0)
                return false;

            const auto tail = inboxTail.load (std::memory_order_relaxed);

            if (tail - inboxHead.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (inboxSize))
                return false;

            inbox[tail % static_cast<std::uint32_t> (inboxSize)] = { command, generationCount.load (std::memory_order_acquire) };
            inboxTail.store (tail + 1, std::memory_order_release);
            return true;
        }

        /*  Where the code goes - a logical output channel, -1 for none - and
            its peak, linear. Any thread; read once a block. */
        void setOutput (int channel, float gain) noexcept
        {
            outputGain.store (gain, std::memory_order_relaxed);
            outputChannel.store (channel, std::memory_order_relaxed);
        }

        /*  At the start of the audio, and never while it renders: everything
            reset, the generation moved on (AHU). A rate below the lowest drawn
            - nought, for a host that has stopped - draws nothing. */
        void prepare (int newSampleRate) noexcept
        {
            generationCount.fetch_add (1, std::memory_order_acq_rel);
            inboxHead.store (inboxTail.load (std::memory_order_acquire), std::memory_order_release);

            pendingCount = 0;
            run = {};
            drawing.store (0, std::memory_order_relaxed);
            filtered = 0.0;
            lineLevel = 0.0;
            cachedFrame = noFrame;

            samplesPerSecond = newSampleRate >= lowestSampleRate ? newSampleRate : 0;
            step = samplesPerSecond > 0
                     ? 1.0 - std::exp (-1.0 / (static_cast<double> (samplesPerSecond) * tauSeconds))
                     : 0.0;
        }

        /*  THE AUDIO THREAD, once a block, after the outputs' gains: the inbox
            drained, every command applied on its sample, and the output
            channel's `numSamples` REPLACED - the code, or silence while
            nothing runs. With no output, or one past the block's channels,
            nothing is written and the clock runs on regardless, so an output
            given back mid-run picks the code up where it has got to.
            `blockStart` is the block's first sample on Go.dot's count. */
        void render (float* const* channels, int numChannels, int numSamples, std::int64_t blockStart) noexcept WFG_AUDIO_THREAD
        {
            if (numSamples <= 0)
                return;

            drain (blockStart);

            if (samplesPerSecond <= 0)
                return;

            const auto channel = outputChannel.load (std::memory_order_relaxed);
            float* out = channels != nullptr && channel >= 0 && channel < numChannels ? channels[channel] : nullptr;
            /*  AT REST, the ordinary block of a show: no run, nothing waiting,
                and the low-pass already let go to nought - which `silence`
                makes exact - so the channel is nought and nothing needs
                working out sample by sample. */
            if (run.id == 0 && pendingCount == 0 && ! (std::abs (filtered) > 0.0))
            {
                if (out != nullptr)
                    std::fill (out, out + numSamples, 0.0f);

                lineLevel = 0.0;
                return;
            }

            const auto gain = static_cast<double> (outputGain.load (std::memory_order_relaxed));
            const auto blockEnd = blockStart + numSamples;

            for (auto at = blockStart; at < blockEnd;)
            {
                settle (at);

                /*  To the next thing that happens: a command's sample, the
                    run's end, or the block's. `settle` has applied everything
                    due at `at`, so each lies past it. */
                auto until = blockEnd;

                if (pendingCount > 0)
                    until = std::min (until, pending[0].at);

                if (run.id != 0)
                {
                    until = std::min (until, run.endSample);
                    draw (out, blockStart, at, until, gain);
                }
                else
                {
                    silence (out, blockStart, at, until);
                }

                at = until;
            }

            //  A run that ends on the block's end has ended: said now, not a block late.
            if (run.id != 0 && run.endSample <= blockEnd)
                end();
        }

        /*  Any thread. Commands that reached the audio side after their
            sample (`sync/ltcLate`), never reset; the program the line is
            drawing, nought for none; and how many times `prepare` has run. */
        std::uint32_t lateCount() const noexcept      { return late.load (std::memory_order_relaxed); }
        std::uint64_t currentProgram() const noexcept { return drawing.load (std::memory_order_relaxed); }
        std::uint64_t generation() const noexcept     { return generationCount.load (std::memory_order_relaxed); }

    private:
        static constexpr std::size_t pendingSize = 2 * static_cast<std::size_t> (inboxSize);
        static constexpr std::int64_t never = std::numeric_limits<std::int64_t>::max();
        static constexpr std::int64_t noFrame = std::numeric_limits<std::int64_t>::min();

        //  Below this the silent line is nought: a tail is not left to fade
        //  for ever through the denormals.
        static constexpr double quiet = 1.0e-9;

        struct Slot
        {
            Command command;
            std::uint64_t generation = 0;
        };

        //  A command drained, the sample it is applied on, and the order it came in.
        struct Pending
        {
            Command command;
            std::int64_t at = 0;
            std::uint64_t order = 0;
        };

        /*  THE RUN THE LINE IS DRAWING, the audio thread's alone. A stop sets
            where it ends: `endSample`, its last sample's successor, and - for
            a stop on a frame boundary - `endHalfBit`, the half-bit that would
            have begun there, whose edge is drawn down to nought at its exact
            moment rather than up to the next level. */
        struct Run
        {
            std::uint64_t id = 0;
            std::int64_t zero = 0;
            std::int64_t startFrame = 0;
            Rate rate = Rate::fps25;

            //  The level of every frame's first half-bit: +1 or -1.
            double polarity = 1.0;

            std::int64_t endSample = never;
            std::int64_t endHalfBit = never;

            //  The order its start came in: a stop or a cut that came before it is not its (TC.7).
            std::uint64_t startOrder = 0;

            //  A half-bit's exact moment is `k * multiplier / divisor` samples after zero.
            std::int64_t multiplier = 1;
            std::int64_t divisor = 1;
        };

        //  The next change of level: its first sample, and where in the sample before it it lies.
        struct Edge
        {
            std::int64_t sample = 0;

            //  Whether the moment falls inside the sample before `sample`, and
            //  how much of that sample comes before it.
            bool inside = false;
            double before = 0.0;

            double level = 0.0;
        };

        //==============================================================================
        void drain (std::int64_t blockStart) noexcept
        {
            const auto current = generationCount.load (std::memory_order_relaxed);

            while (pendingCount < pendingSize)
            {
                const auto head = inboxHead.load (std::memory_order_relaxed);

                if (head == inboxTail.load (std::memory_order_acquire))
                    break;

                const auto slot = inbox[head % static_cast<std::uint32_t> (inboxSize)];
                inboxHead.store (head + 1, std::memory_order_release);

                if (slot.generation != current || samplesPerSecond <= 0)
                    continue;

                place ({ slot.command, whenOf (slot.command, blockStart), arrivals++ });
            }
        }

        //  The sample a command is applied on, counting the late ones.
        std::int64_t whenOf (const Command& command, std::int64_t blockStart) noexcept
        {
            switch (command.verb)
            {
                case Verb::start:
                {
                    if (command.sample >= blockStart)
                        return command.sample;

                    late.fetch_add (1, std::memory_order_relaxed);

                    const auto frame = firstFrameAtOrAfter (blockStart - command.sample, samplesPerSecond, command.rate);
                    return command.sample + frameStartSample (frame, samplesPerSecond, command.rate);
                }

                case Verb::stop:
                case Verb::cut:
                {
                    if (command.sample <= 0)
                        return blockStart;

                    if (command.sample < blockStart)
                    {
                        late.fetch_add (1, std::memory_order_relaxed);
                        return blockStart;
                    }

                    return command.sample;
                }
            }

            return blockStart;
        }

        //  Into the list by its sample; after those already there for the same one.
        void place (const Pending& item) noexcept
        {
            auto index = pendingCount;

            while (index > 0 && pending[index - 1].at > item.at)
            {
                pending[index] = pending[index - 1];
                --index;
            }

            pending[index] = item;
            ++pendingCount;
        }

        /*  EVERYTHING DUE AT `at`, in order: the run's end first - a stop that
            lands here ends the run before a start for the same sample begins
            the next - then each command, until nothing more is. */
        void settle (std::int64_t at) noexcept
        {
            for (;;)
            {
                if (run.id != 0 && run.endSample <= at)
                {
                    end();
                    continue;
                }

                if (pendingCount > 0 && pending[0].at <= at)
                {
                    const auto item = pending[0];

                    for (std::size_t n = 1; n < pendingCount; ++n)
                        pending[n - 1] = pending[n];

                    --pendingCount;
                    apply (item, at);
                    continue;
                }

                break;
            }
        }

        void apply (const Pending& item, std::int64_t at) noexcept
        {
            const auto& command = item.command;

            switch (command.verb)
            {
                case Verb::start:
                {
                    const auto onSilence = run.id == 0;

                    run = {};
                    run.id = command.program;
                    run.startOrder = item.order;
                    run.zero = command.sample;
                    run.startFrame = command.startFrame;
                    run.rate = command.rate;
                    run.multiplier = static_cast<std::int64_t> (samplesPerSecond) * denOf (command.rate);
                    run.divisor = halfBitsPerFrame * numOf (command.rate);

                    /*  ON A SILENT LINE the first edge goes up from nought, the
                        low-pass starting clean; TAKING OVER a drawing one, the
                        other way from where it stands, the low-pass kept. */
                    run.polarity = (onSilence || lineLevel < 0.0) ? 1.0 : -1.0;

                    if (onSilence)
                        filtered = 0.0;

                    drawing.store (run.id, std::memory_order_relaxed);
                    return;
                }

                case Verb::stop:
                {
                    dropStartsOf (command.program, item.order);

                    if (run.id != command.program || item.order < run.startOrder)
                        return;

                    const auto frame = firstFrameAtOrAfter (at - run.zero, samplesPerSecond, run.rate);
                    const auto boundary = run.zero + frameStartSample (frame, samplesPerSecond, run.rate);

                    if (boundary < run.endSample)
                    {
                        run.endSample = boundary;
                        run.endHalfBit = frame * halfBitsPerFrame;
                    }

                    return;
                }

                case Verb::cut:
                {
                    dropStartsOf (command.program, item.order);

                    if (run.id == command.program && item.order >= run.startOrder && at < run.endSample)
                        run.endSample = at;

                    return;
                }
            }
        }

        //  The starts of `program` posted before a stop or a cut of it, which it takes back.
        void dropStartsOf (std::uint64_t program, std::uint64_t postedBefore) noexcept
        {
            std::size_t kept = 0;

            for (std::size_t n = 0; n < pendingCount; ++n)
            {
                const auto item = pending[n];

                if (item.command.verb == Verb::start && item.command.program == program && item.order < postedBefore)
                    continue;

                pending[kept++] = item;
            }

            pendingCount = kept;
        }

        void end() noexcept
        {
            run = {};
            lineLevel = 0.0;
            drawing.store (0, std::memory_order_relaxed);
        }

        //==============================================================================
        /*  The line with no run: the low-pass letting go of the last edge, to
            nought. */
        void silence (float* out, std::int64_t blockStart, std::int64_t from, std::int64_t until) noexcept
        {
            for (auto s = from; s < until; ++s)
            {
                filtered -= step * filtered;

                if (std::abs (filtered) < quiet)
                    filtered = 0.0;

                if (out != nullptr)
                    out[s - blockStart] = static_cast<float> (filtered);
            }

            lineLevel = 0.0;
        }

        /*  THE CODE, from `from` to `until`. Where the line is at `from` comes
            from the one rule afresh; from there it is stepped edge to edge,
            each edge's sample from the one rule too. */
        void draw (float* out, std::int64_t blockStart, std::int64_t from, std::int64_t until, double gain) noexcept
        {
            auto half = partAtSample (from - run.zero, halfBitsPerFrame, samplesPerSecond, run.rate);
            auto level = levelAt (half);
            auto edge = edgeInto (half + 1, level);

            for (auto s = from; s < until; ++s)
            {
                auto in = level;
                auto after = level;

                if (edge.inside && s + 1 == edge.sample)
                {
                    in = edge.before * level + (1.0 - edge.before) * edge.level;
                    after = edge.level;
                }

                filtered += step * (in * gain - filtered);

                if (out != nullptr)
                    out[s - blockStart] = static_cast<float> (filtered);

                lineLevel = after;

                if (s + 1 == edge.sample && s + 1 < until)
                {
                    ++half;
                    level = edge.level;
                    edge = edgeInto (half + 1, level);
                }
            }
        }

        /*  The edge into half-bit `k`: its first sample by the one rule, and
            the fraction of the sample before that its exact moment leaves on
            the old level - the remainder of `k * multiplier` over the divisor,
            `k` reduced first so the product stays small. */
        Edge edgeInto (std::int64_t k, double levelBefore) noexcept
        {
            Edge edge;
            edge.sample = run.zero + partStartSample (k, halfBitsPerFrame, samplesPerSecond, run.rate);

            const auto rest = ((k % run.divisor) * run.multiplier) % run.divisor;
            edge.inside = rest != 0;
            edge.before = static_cast<double> (rest) / static_cast<double> (run.divisor);
            edge.level = levelInto (k, levelBefore);
            return edge;
        }

        /*  The level half-bit `k` is drawn at, the one before it at
            `levelBefore`: nought where a stop ends the run, the other level at
            a cell's start, and in a cell's middle the other level for a one. */
        double levelInto (std::int64_t k, double levelBefore) noexcept
        {
            if (k >= run.endHalfBit)
                return 0.0;

            if (k % 2 == 0)
                return -levelBefore;

            const auto& bits = bitsOf (run.startFrame + k / halfBitsPerFrame);
            return ltcBit (bits, static_cast<int> ((k / 2) % ltcBitsPerFrame)) ? -levelBefore : levelBefore;
        }

        /*  The level of half-bit `half`, walked from its frame's start, which
            is the run's polarity in every frame. */
        double levelAt (std::int64_t half) noexcept
        {
            const auto& bits = bitsOf (run.startFrame + half / halfBitsPerFrame);
            const auto within = half % halfBitsPerFrame;
            auto level = run.polarity;

            for (std::int64_t j = 1; j <= within; ++j)
                if (j % 2 == 0 || ltcBit (bits, static_cast<int> (j / 2)))
                    level = -level;

            return level;
        }

        //  A frame's eighty bits, worked out once at each new frame.
        const LtcBits& bitsOf (std::int64_t frame) noexcept
        {
            if (frame != cachedFrame || run.rate != cachedRate)
            {
                cachedBits = ltcBits (labelOf (frame, run.rate), run.rate);
                cachedFrame = frame;
                cachedRate = run.rate;
            }

            return cachedBits;
        }

        //==============================================================================
        std::array<Slot, static_cast<std::size_t> (inboxSize)> inbox {};
        std::atomic<std::uint32_t> inboxHead { 0 }, inboxTail { 0 };

        std::array<Pending, pendingSize> pending {};
        std::size_t pendingCount = 0;
        std::uint64_t arrivals = 0;

        Run run;

        LtcBits cachedBits {};
        std::int64_t cachedFrame = noFrame;
        Rate cachedRate = Rate::fps25;

        int samplesPerSecond = 0;
        double step = 0.0;

        //  The low-pass's state, and the level the line was at after the last sample drawn.
        double filtered = 0.0;
        double lineLevel = 0.0;

        std::atomic<int> outputChannel { -1 };
        std::atomic<float> outputGain { defaultGain };

        std::atomic<std::uint32_t> late { 0 };
        std::atomic<std::uint64_t> drawing { 0 };
        std::atomic<std::uint64_t> generationCount { 0 };
    };
}
