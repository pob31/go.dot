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
    LTC, READ ON THE AUDIO THREAD (namespace draft §60, TG.3; AIL, AIM, AIQ,
    AIR).

    A time group whose source is LTC starts its members on the very sample the
    incoming code reaches their time, so the code is read where its samples
    are: on the audio thread, over the one logical input `sync/ltcIn` names,
    right after AudioHost fills the input tap. Each frame read comes out with
    the sample its bit 0 began on, in Go.dot's own count and to a fraction of
    a sample, and goes to the tick thread through an outbox; nothing here
    decides what a frame means (TG.5 does).

    THE EDGES. Biphase mark is read by its changes of level, which are found as
    crossings of the line's MIDDLE with HYSTERESIS: the line must go beyond
    half its half-swing on the other side before a crossing counts, so a wobble
    or the noise riding on a plateau is never an edge. The middle and the swing
    come from two peak followers, one for the top of the line and one for the
    bottom, each taking a new peak at once and letting go towards the line over
    a tenth of a second: the middle is where the line is centred whatever DC it
    carries, and the swing is the code's own level, so a code at -40 dBFS reads
    as a code at full scale does. A line that has never moved, or has fallen
    quiet, is at REST: there the first excursion beyond the hysteresis is an
    edge either way, which is how the first frame of a code that starts from
    silence is read whole.

    EACH EDGE PLACED BY ITS AREA, NOT BY ITS CROSSING. The crossing, linear
    between the two samples it falls between, finds the edge; it does not place
    it well enough. An edge rounded by a rise time of a sample or two is not a
    straight line between samples, and where the straight line crosses moves
    by a seventh of a sample as the edge slides across a sample - more than
    TG.5 can be given. So the edge is placed by what the line holds around it:
    for a change from level `L0` to level `L1` at moment `t`, the samples from
    `a` to `b` add up to `L0 * (t - a) + L1 * (b - t)` whatever the rounding,
    which a linear filter only delays - and so `t = b - sum (x - L0) / (L1 -
    L0)`, exact to the filter's constant delay at every fraction of a sample.
    The stretch summed reaches half way to the crossings either side, so it
    holds the whole of this edge and nothing of the next, and the edge is
    placed once the next one is found - or once the stretch has passed with
    none, at the end of a code. `L0` is the plateau the edge left, taken when
    it is found; `L1` the one it reached, taken when it is placed - by then the
    line has stood on it. A crossing out of rest leaves the rest level; a code
    that falls back to rest - a stop, which ends on a frame boundary - makes
    one last edge to the middle, which is what closes the last bit of its last
    frame, as the next frame's first edge would have.

    THE BITS. The interval between two placed edges is half a bit or a bit,
    told apart at three quarters of a TRACKED bit period: two halves are a one,
    a whole is a nought, and a half left over when a whole one comes is
    dropped - the reader was half a bit out of step, and the whole one puts it
    back. The period is found from the first intervals - a half next to a
    whole, which every frame holds, or forty alike in a row, which only noughts
    make - and the intervals heard before it was found are read again by it,
    so the first frame of a code is not lost to finding it; then each interval
    accepted moves it a sixteenth of the way, so a code played fast or slow is
    followed. It is held between the periods 23.976 to 30 frames a second
    allow at half to twice speed, and an interval under a third of it or over
    one and a half loses it and starts finding it again.

    THE FRAMES. The last eighty bits are kept with the edges that began and
    ended each. Bits 64 to 79 of a frame are the sync word, 0011 1111 1111
    1101 in the order they are sent: the eighty bits ending in it are a frame
    read FORWARDS, and the eighty beginning with it mirrored - 1011 1111 1111
    1100 - are a frame played BACKWARDS, whose bit 0 arrived last. Each field
    is binary-coded decimal, least significant bit first, and a frame whose
    digits are not digits, or whose label no rate has, is not a frame. Its
    parity - an even count of ones, the polarity bit's purpose - is said, not
    judged.

    A FRAME'S START is the moment of the edge that began its bit 0: read
    forwards, the first edge of its first bit; read backwards, the edge that
    closed the last bit to arrive, which is the same edge met from the other
    side. As a whole sample and the fraction after it.

    THE OUTBOX, sixty-four frames one way: LtcGenerator's inbox the other way
    round, a single producer and a single consumer, two atomics and no lock.
    A full outbox drops the NEWEST frame and counts it - the frames already
    waiting are older and the reader is told it lost some. Once a frame has
    been read and none follows for two of its own periods, one `lost` entry
    says the code went quiet, at the moment those two periods ran out; what
    a silence means - a stop, a dropout - is TG.5's (AIR).

    `prepare` resets everything and moves a generation on, as LtcGenerator's
    does - everything but the outbox, which the tick may be taking from while
    the audio restarts: emptying it from here would race that take. Instead
    every entry carries the generation it was read under, so the tick thread
    can let go of what an audio restart made meaningless.

    Vendor-free, allocation-free and lock-free on the audio thread, the only
    caller of `process`; `take` is the tick thread's, into a vector the caller
    owns and reserved once.
*/

#include <wfg/engine/rt/RtCheck.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wfg::timecode
{
    /*  ONE FRAME HEARD, or that the code went quiet. `startSample` and
        `startFraction` are where its bit 0 began, in Go.dot's count;
        `bitPeriod` is its own length in samples over eighty, so a code
        played fast reads short. A `lost` entry carries only where the
        silence was declared, in `startSample` and `startFraction`. */
    struct LtcHeard
    {
        Label label;
        bool drop = false;
        bool reverse = false;
        bool parityOk = false;
        bool lost = false;
        std::int64_t startSample = 0;
        float startFraction = 0.0f;
        double bitPeriod = 0.0;
        std::uint64_t generation = 0;
    };

    /*  THE RATE AN INCOMING CODE RUNS AT (AIL), from what a reader can know of
        it: the largest frame number its frames have carried, its drop-frame
        flag, and the length of its frames in samples at a sample rate. The
        frame numbers give what the labels count to - 23 means 24, 24 means 25,
        29 means 30 - and the length tells 23.976 from 24 and 29.97 from 30,
        which a label cannot: the frames must run within 500 parts per million
        of the one rate the labels and the flag leave, or nothing is said. A
        largest frame below 23 has not shown what the labels count to yet, a
        drop flag at 24 or 25 is no rate, and a drop-frame code running at
        30.000 is not 29.97 drop-frame: each answers nothing, and TG.5 reads at
        the show's rate (AHA). PURE: the tick thread calls it. */
    inline std::optional<Rate> detectRate (int largestFrame, bool dropFrame, double framePeriodSamples,
                                           int sampleRate) noexcept
    {
        if (sampleRate <= 0 || ! (framePeriodSamples > 0.0))
            return std::nullopt;

        const auto measured = static_cast<double> (sampleRate) / framePeriodSamples;

        const auto near = [measured] (Rate rate)
        {
            const auto nominal = static_cast<double> (numOf (rate)) / static_cast<double> (denOf (rate));
            return std::abs (measured / nominal - 1.0) < 500.0e-6;
        };

        //  Exactly one of two candidates, or nothing.
        const auto oneOf = [&near] (Rate first, Rate second) -> std::optional<Rate>
        {
            const auto isFirst = near (first);
            const auto isSecond = near (second);

            if (isFirst != isSecond)
                return isFirst ? first : second;

            return std::nullopt;
        };

        switch (largestFrame)
        {
            case 23:
                if (dropFrame)
                    return std::nullopt;

                return oneOf (Rate::fps24, Rate::fps23976);

            case 24:
                if (dropFrame || ! near (Rate::fps25))
                    return std::nullopt;

                return Rate::fps25;

            case 29:
                if (dropFrame)
                    return near (Rate::fps2997df) ? std::optional<Rate> { Rate::fps2997df } : std::nullopt;

                return oneOf (Rate::fps30, Rate::fps2997);

            default:
                return std::nullopt;
        }
    }

    class LtcDecoder
    {
    public:
        static constexpr int outboxSize = 64;

        //  The slowest rate read, as the generator's slowest drawn.
        static constexpr int lowestSampleRate = 8000;

        //  Below this the line is silence, whatever its peak followers say: -66 dBFS.
        static constexpr double floorLevel = 0.0005;

        LtcDecoder() = default;
        LtcDecoder (const LtcDecoder&) = delete;
        LtcDecoder& operator= (const LtcDecoder&) = delete;

        /*  At the start of the audio, and never while it runs: everything
            read so far forgotten, the generation moved on. A rate below the
            lowest read - nought, for a host that has stopped - reads nothing.
            The outbox keeps what is waiting in it, each entry stamped with the
            generation it was read under. */
        void prepare (int newSampleRate) noexcept
        {
            generationCount.fetch_add (1, std::memory_order_acq_rel);
            samplesPerSecond = newSampleRate >= lowestSampleRate ? newSampleRate : 0;

            if (samplesPerSecond > 0)
            {
                const auto rate = static_cast<double> (samplesPerSecond);

                shortestBit = rate / (30.0 * 80.0 * 2.0);
                longestBit = rate * 1001.0 / (24000.0 * 80.0 * 0.5);
                widestReach = std::min (0.5 * longestBit, static_cast<double> (ringSize - 8) / 3.0);
                releaseStep = 1.0 - std::exp (-1.0 / (releaseSeconds * rate));
                restStep = 1.0 - std::exp (-1.0 / (restSeconds * rate));
            }

            reset();
        }

        /*  THE AUDIO THREAD, once a block, over the input the code comes in
            on: `numSamples` of it, the first of them sample `blockStart` of
            Go.dot's count. A block that does not follow the last one - the
            count moved under it - starts the line again. */
        void process (const float* samples, int numSamples, std::int64_t blockStart) noexcept WFG_AUDIO_THREAD
        {
            if (samples == nullptr || numSamples <= 0 || samplesPerSecond <= 0)
                return;

            if (primed && blockStart != nextSample)
                reset();

            for (int i = 0; i < numSamples; ++i)
                step (static_cast<double> (samples[i]), blockStart + i);

            nextSample = blockStart + numSamples;
        }

        /*  THE LINE FORGOTTEN, the generation kept: a new input to read, or a
            jump in the count. The audio thread's, or `prepare`'s. */
        void reset() noexcept
        {
            primed = false;
            nextSample = 0;
            hi = lo = rest = previous = 0.0;
            side = 0;
            inBand = false;
            bandEntry = 0.0;
            lastUp = lastDown = 0.0;
            lastCrossing = 0.0;
            haveCrossing = false;
            edgeWaiting = false;
            waiting = {};
            haveEdge = false;
            lastEdge = 0.0;
            locked = false;
            bitLength = 0.0;
            pendingCount = 0;
            orphan = false;
            orphanStart = 0.0;
            arrivals = 0;
            sinceFrame = 0;
            lostArmed = false;
            lostAt = 0.0;
        }

        /*  THE TICK THREAD: every entry waiting, oldest first, into `out`,
            which is cleared first and is the caller's - reserved once, so
            nothing allocates here in a show's steady state. How many. */
        std::size_t take (std::vector<LtcHeard>& out)
        {
            out.clear();

            auto head = outHead.load (std::memory_order_relaxed);
            const auto tail = outTail.load (std::memory_order_acquire);

            for (; head != tail; ++head)
                out.push_back (outbox[head % static_cast<std::uint32_t> (outboxSize)]);

            outHead.store (head, std::memory_order_release);
            return out.size();
        }

        /*  Any thread. The frames a full outbox dropped, never reset; and how
            many times `prepare` has run. */
        std::uint32_t droppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }
        std::uint64_t generation() const noexcept   { return generationCount.load (std::memory_order_relaxed); }

    private:
        static constexpr std::size_t ringSize = 1024;
        static constexpr std::uint64_t ringMask = ringSize - 1;
        static constexpr std::size_t pendingSize = 128;
        static constexpr int frameBits = 80;

        //  The hysteresis: how far past the middle, in halves of the swing.
        static constexpr double hysteresis = 0.5;

        //  How fast the peak followers let go, and how fast the rest level follows a quiet line.
        static constexpr double releaseSeconds = 0.1;
        static constexpr double restSeconds = 0.005;

        //  A half or a whole bit, and what is neither.
        static constexpr double halfOrWhole = 0.75;
        static constexpr double shortest = 0.3;
        static constexpr double longest = 1.5;

        //  How far each accepted interval moves the period.
        static constexpr double following = 1.0 / 16.0;

        //  Finding the period: a half beside a whole, or a run of wholes alike.
        static constexpr double pairLow = 1.5;
        static constexpr double pairHigh = 2.6;
        static constexpr std::size_t aliked = 40;
        static constexpr double alikeSpread = 1.3;

        //  Bits 64 to 79 in the order they are sent.
        static constexpr std::array<bool, 16> syncWord { false, false, true, true, true, true, true, true,
                                                         true, true, true, true, true, true, false, true };

        //  An edge found and not yet placed.
        struct Edge
        {
            double crossing = 0.0;

            //  The level it left, and +1 up or -1 down.
            double from = 0.0;
            int direction = 0;

            //  How far either side of its crossing it may reach: half way to the one before.
            double reach = 0.0;

            //  A fall back to rest: it goes to the middle, not to a plateau.
            bool toMiddle = false;
        };

        struct Interval
        {
            double start = 0.0;
            double end = 0.0;
        };

        struct Bit
        {
            bool one = false;
            double start = 0.0;
            double end = 0.0;
        };

        //==============================================================================
        /*  ONE SAMPLE: an edge found against the middle and the swing as they
            stood before it, then the peak followers moved, then whatever is
            due by it - an edge's placing, a fall into rest, the line gone
            quiet, the `lost` its silence owes. */
        void step (double x, std::int64_t n) noexcept
        {
            ring[static_cast<std::size_t> (static_cast<std::uint64_t> (n) & ringMask)] = static_cast<float> (x);
            const auto at = static_cast<double> (n);

            if (! primed)
            {
                //  The line starts where it stands, so a DC offset is never an edge.
                hi = lo = rest = previous = x;
                primed = true;
                return;
            }

            const auto middle = 0.5 * (hi + lo);
            const auto band = std::max (floorLevel, hysteresis * 0.5 * (hi - lo));

            if (side == 0)
            {
                const auto away = x - rest;

                if (away > band || away < -band)
                {
                    const auto direction = away > 0.0 ? 1 : -1;
                    const auto level = rest + static_cast<double> (direction) * band;
                    found (at - 1.0 + fractionOf (previous, x, level), direction, rest, false, n);
                }
                else
                {
                    rest = settle (rest + restStep * (x - rest), x);
                }
            }
            else
            {
                const auto now = x - middle;
                const auto before = previous - middle;

                if (before <= 0.0 && now > 0.0)
                    lastUp = at - 1.0 + fractionOf (before, now, 0.0);

                if (before >= 0.0 && now < 0.0)
                    lastDown = at - 1.0 + fractionOf (before, now, 0.0);

                if (side < 0 && now > band)
                {
                    found (lastUp, 1, lo, false, n);
                }
                else if (side > 0 && now < -band)
                {
                    found (lastDown, -1, hi, false, n);
                }
                else if (std::abs (now) < band)
                {
                    /*  INSIDE THE BAND, which a code only passes through. Where
                        it came in is half way from the plateau to the middle:
                        a fall to rest crosses there as a whole edge crosses
                        the middle. Still inside once it would have reached
                        half way to the next edge, the code has fallen quiet. */
                    if (! inBand)
                    {
                        inBand = true;
                        bandEntry = at - 1.0 + fractionOf (std::abs (before), std::abs (now), band);
                    }
                    else if (at >= bandEntry + std::max (reachFrom (bandEntry), 0.25 * currentBit()) + 1.0)
                    {
                        fallQuiet (x, n);
                    }
                }
                else
                {
                    inBand = false;
                }
            }

            hi = x > hi ? x : settle (hi - releaseStep * (hi - x), x);
            lo = x < lo ? x : settle (lo - releaseStep * (lo - x), x);
            previous = x;

            //  The edge waiting, placed once its stretch has passed with no edge after it.
            if (edgeWaiting && at >= waiting.crossing + 2.0 * waiting.reach + 1.0)
                place (waiting.reach, n);

            //  No crossing for a bit and a half: the line stands still, at a plateau.
            if (side != 0 && at - lastCrossing > longest * (locked ? bitLength : longestBit) + 1.0)
            {
                if (edgeWaiting)
                    place (waiting.reach, n);

                goQuiet (x);
            }

            if (lostArmed && at >= lostAt)
            {
                lostArmed = false;

                LtcHeard heard;
                heard.lost = true;
                setStart (heard, lostAt);
                post (heard);
            }
        }

        /*  A follower within a billionth of the line is on it: a long silence
            is never left to creep towards nought through the denormals. */
        static double settle (double follower, double x) noexcept
        {
            return std::abs (follower - x) < 1.0e-9 ? x : follower;
        }

        /*  Where between two samples, `a` then `b`, the line crossed
            `level`: nought at the first, one at the second. */
        static double fractionOf (double a, double b, double level) noexcept
        {
            const auto span = b - a;

            if (! (std::abs (span) > 0.0))
                return 1.0;

            return std::clamp ((level - a) / span, 0.0, 1.0);
        }

        //  The bit the line is read by: the period followed, or the shortest there can be.
        double currentBit() const noexcept
        {
            return locked ? bitLength : shortestBit;
        }

        //  Half way back to the last crossing, held to the widest stretch the ring keeps.
        double reachFrom (double crossing) const noexcept
        {
            if (! haveCrossing)
                return widestReach;

            return std::min (widestReach, 0.5 * (crossing - lastCrossing));
        }

        /*  AN EDGE FOUND at `crossing`: the one waiting is placed now, its
            stretch reaching half way to this one, and this one waits for the
            next. */
        void found (double crossing, int direction, double from, bool toMiddle, std::int64_t n) noexcept
        {
            if (edgeWaiting)
                place (std::min (waiting.reach, 0.5 * (crossing - waiting.crossing)), n);

            waiting.crossing = crossing;
            waiting.from = from;
            waiting.direction = direction;
            waiting.reach = reachFrom (crossing);
            waiting.toMiddle = toMiddle;
            edgeWaiting = true;

            lastCrossing = crossing;
            haveCrossing = true;
            side = direction;
            inBand = false;
        }

        /*  THE CODE FELL BACK TO REST, and has stayed there longer than any
            edge takes to pass the band: the edge waiting placed, then the fall
            itself as an edge to the middle, then the line at rest where it
            stands. */
        void fallQuiet (double x, std::int64_t n) noexcept
        {
            const auto from = side > 0 ? hi : lo;
            const auto crossing = bandEntry;

            found (crossing, -side, from, true, n);
            place (waiting.reach, n);
            goQuiet (x);
        }

        void goQuiet (double x) noexcept
        {
            side = 0;
            rest = x;
            inBand = false;
            haveCrossing = false;
            haveEdge = false;
            orphan = false;
            sinceFrame = 0;
            pendingCount = 0;
        }

        /*  THE EDGE WAITING, PLACED BY ITS AREA over `reach` either side of
            its crossing - the samples the ring still holds and the line has
            reached. Where the stretch is too short, the level it went to too
            close to the one it left, or the answer outside the stretch, the
            crossing stands. */
        void place (double reach, std::int64_t n) noexcept
        {
            edgeWaiting = false;

            const auto& edge = waiting;
            auto first = static_cast<std::int64_t> (std::ceil (edge.crossing - reach));
            auto end = static_cast<std::int64_t> (std::floor (edge.crossing + reach)) + 1;

            first = std::max (first, n - static_cast<std::int64_t> (ringSize) + 1);
            end = std::min (end, n + 1);

            auto when = edge.crossing;

            if (end - first >= 2)
            {
                const auto to = edge.toMiddle ? 0.5 * (hi + lo) : (edge.direction > 0 ? hi : lo);
                const auto span = to - edge.from;

                if (std::abs (span) > floorLevel)
                {
                    double sum = 0.0;

                    for (auto s = first; s < end; ++s)
                        sum += static_cast<double> (ring[static_cast<std::size_t> (static_cast<std::uint64_t> (s) & ringMask)])
                                 - edge.from;

                    const auto placed = static_cast<double> (end) - sum / span;

                    if (placed >= static_cast<double> (first) && placed <= static_cast<double> (end))
                        when = placed;
                }
            }

            placedEdge (when);
        }

        //==============================================================================
        //  The intervals between placed edges.
        void placedEdge (double when) noexcept
        {
            if (! haveEdge)
            {
                haveEdge = true;
                lastEdge = when;
                return;
            }

            const auto start = lastEdge;
            lastEdge = when;
            interval (start, when);
        }

        void interval (double start, double end) noexcept
        {
            if (locked)
            {
                const auto length = end - start;

                if (length >= shortest * bitLength && length <= longest * bitLength)
                {
                    classify (start, end);
                    return;
                }

                //  Neither half nor whole: the period is lost, and found again from here.
                locked = false;
                orphan = false;
                sinceFrame = 0;
                pendingCount = 0;
            }

            const auto length = end - start;

            if (length < 0.4 * shortestBit || length > 1.25 * longestBit)
            {
                pendingCount = 0;
                return;
            }

            if (pendingCount == pendingSize)
            {
                for (std::size_t i = 1; i < pendingSize; ++i)
                    pending[i - 1] = pending[i];

                --pendingCount;
            }

            pending[pendingCount++] = { start, end };

            if (! findPeriod())
                return;

            /*  FOUND: what was heard while finding it is read by it, so the
                first frame of a code is read from its first edge. */
            const auto count = pendingCount;
            pendingCount = 0;

            for (std::size_t i = 0; i < count && locked; ++i)
                classify (pending[i].start, pending[i].end);
        }

        bool findPeriod() noexcept
        {
            const auto lengthOf = [this] (std::size_t i) { return pending[i].end - pending[i].start; };
            double candidate = 0.0;

            if (pendingCount >= 2)
            {
                const auto last = lengthOf (pendingCount - 1);
                const auto before = lengthOf (pendingCount - 2);
                const auto larger = std::max (last, before);
                const auto smaller = std::min (last, before);

                if (smaller > 0.0 && larger >= pairLow * smaller && larger <= pairHigh * smaller)
                    candidate = larger;
            }

            if (! (candidate > 0.0) && pendingCount >= aliked)
            {
                auto smallest = lengthOf (pendingCount - aliked);
                auto largest = smallest;
                double sum = 0.0;

                for (auto i = pendingCount - aliked; i < pendingCount; ++i)
                {
                    smallest = std::min (smallest, lengthOf (i));
                    largest = std::max (largest, lengthOf (i));
                    sum += lengthOf (i);
                }

                if (largest <= alikeSpread * smallest)
                    candidate = sum / static_cast<double> (aliked);
            }

            if (! (candidate >= 0.9 * shortestBit && candidate <= 1.1 * longestBit))
                return false;

            bitLength = std::clamp (candidate, shortestBit, longestBit);
            locked = true;
            orphan = false;
            return true;
        }

        //  Half a bit or a whole one, against the period being followed.
        void classify (double start, double end) noexcept
        {
            const auto length = end - start;

            if (length < halfOrWhole * bitLength)
            {
                bitLength += following * (2.0 * length - bitLength);

                if (orphan)
                {
                    orphan = false;
                    pushBit (true, orphanStart, end);
                }
                else
                {
                    orphan = true;
                    orphanStart = start;
                }
            }
            else
            {
                bitLength += following * (length - bitLength);
                orphan = false;
                pushBit (false, start, end);
            }

            bitLength = std::clamp (bitLength, shortestBit, longestBit);
        }

        //==============================================================================
        //  The `index`th of the last eighty bits in the order they arrived, nought the oldest.
        const Bit& arrived (int index) const noexcept
        {
            const auto position = arrivals - static_cast<std::uint64_t> (frameBits) + static_cast<std::uint64_t> (index);
            return bits[static_cast<std::size_t> (position % static_cast<std::uint64_t> (frameBits))];
        }

        void pushBit (bool one, double start, double end) noexcept
        {
            bits[static_cast<std::size_t> (arrivals % static_cast<std::uint64_t> (frameBits))] = { one, start, end };
            ++arrivals;

            if (sinceFrame < frameBits)
                ++sinceFrame;

            if (sinceFrame < frameBits)
                return;

            auto forwards = true;
            auto backwards = true;

            for (int i = 0; i < 16; ++i)
            {
                const auto word = syncWord[static_cast<std::size_t> (i)];
                forwards = forwards && arrived (64 + i).one == word;
                backwards = backwards && arrived (i).one == syncWord[static_cast<std::size_t> (15 - i)];
            }

            if (forwards)
                frame (false);
            else if (backwards)
                frame (true);
        }

        /*  THE EIGHTY BITS AS A FRAME, bit `n` being the `n`th to arrive read
            forwards and the `79 - n`th read backwards. Nothing is said of a
            frame whose digits are not digits. */
        void frame (bool reverse) noexcept
        {
            const auto bitOf = [this, reverse] (int n) { return arrived (reverse ? frameBits - 1 - n : n).one; };

            const auto field = [&bitOf] (int first, int width)
            {
                int value = 0;

                for (int b = 0; b < width; ++b)
                    if (bitOf (first + b))
                        value |= 1 << b;

                return value;
            };

            const auto frameUnits = field (0, 4), frameTens = field (8, 2);
            const auto secondUnits = field (16, 4), secondTens = field (24, 3);
            const auto minuteUnits = field (32, 4), minuteTens = field (40, 3);
            const auto hourUnits = field (48, 4), hourTens = field (56, 2);

            if (frameUnits > 9 || secondUnits > 9 || minuteUnits > 9 || hourUnits > 9
                  || secondTens > 5 || minuteTens > 5)
                return;

            LtcHeard heard;
            heard.label = Label { 10 * hourTens + hourUnits, 10 * minuteTens + minuteUnits,
                                  10 * secondTens + secondUnits, 10 * frameTens + frameUnits };

            if (heard.label.hours > 23 || heard.label.frames > 29)
                return;

            int ones = 0;

            for (int n = 0; n < frameBits; ++n)
                ones += bitOf (n) ? 1 : 0;

            heard.drop = bitOf (10);
            heard.reverse = reverse;
            heard.parityOk = ones % 2 == 0;

            const auto first = arrived (0).start;
            const auto last = arrived (frameBits - 1).end;
            heard.bitPeriod = (last - first) / static_cast<double> (frameBits);
            setStart (heard, reverse ? last : first);

            post (heard);
            sinceFrame = 0;

            //  Two of its own lengths from where it was heard to end, and the code is quiet.
            lostArmed = true;
            lostAt = last + 2.0 * (last - first);
        }

        static void setStart (LtcHeard& heard, double when) noexcept
        {
            const auto whole = std::floor (when);
            heard.startSample = static_cast<std::int64_t> (whole);
            heard.startFraction = static_cast<float> (when - whole);
        }

        //  Into the outbox, or counted when it is full.
        void post (LtcHeard& heard) noexcept
        {
            heard.generation = generationCount.load (std::memory_order_relaxed);

            const auto tail = outTail.load (std::memory_order_relaxed);

            if (tail - outHead.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (outboxSize))
            {
                dropped.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            outbox[tail % static_cast<std::uint32_t> (outboxSize)] = heard;
            outTail.store (tail + 1, std::memory_order_release);
        }

        //==============================================================================
        int samplesPerSecond = 0;
        double shortestBit = 1.0, longestBit = 1.0, widestReach = 1.0;
        double releaseStep = 0.0, restStep = 0.0;

        //  The line: the samples kept, the peak followers, the rest level, the sample before.
        std::array<float, ringSize> ring {};
        bool primed = false;
        std::int64_t nextSample = 0;
        double hi = 0.0, lo = 0.0, rest = 0.0, previous = 0.0;

        //  +1 above the middle, -1 below, nought at rest; and the band's entry.
        int side = 0;
        bool inBand = false;
        double bandEntry = 0.0;

        //  The last crossings of the middle each way, and of the last edge found.
        double lastUp = 0.0, lastDown = 0.0;
        double lastCrossing = 0.0;
        bool haveCrossing = false;

        Edge waiting;
        bool edgeWaiting = false;

        bool haveEdge = false;
        double lastEdge = 0.0;

        //  The period, and the intervals heard while finding it.
        bool locked = false;
        double bitLength = 0.0;
        std::array<Interval, pendingSize> pending {};
        std::size_t pendingCount = 0;

        //  Half a one waiting for its other half.
        bool orphan = false;
        double orphanStart = 0.0;

        std::array<Bit, static_cast<std::size_t> (frameBits)> bits {};
        std::uint64_t arrivals = 0;
        int sinceFrame = 0;

        bool lostArmed = false;
        double lostAt = 0.0;

        std::array<LtcHeard, static_cast<std::size_t> (outboxSize)> outbox {};
        std::atomic<std::uint32_t> outHead { 0 }, outTail { 0 };
        std::atomic<std::uint32_t> dropped { 0 };
        std::atomic<std::uint64_t> generationCount { 0 };
    };
}
