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
    A SPEED THAT MOVES IN STRAIGHT LINES, AND WHERE IT HAS TAKEN THE FILE
    (namespace draft §22.4).

    A media cue's speed is a list of breakpoints on some axis of time: at this
    moment, this speed. Between two breakpoints the speed moves in a straight
    line; after the last it holds. Where the file has got to is the integral of
    the speed - a speed of one moves the file one second a second, a half moves
    it half a second - and a straight line integrates exactly, by the trapezoid,
    so where the file is at any moment is arithmetic, not an estimate.

    WHY ONE CLASS ON BOTH SIDES. The Runner, on the tick thread, has to know
    where the file is - for the playhead, the level lane, and above all for a
    range boundary, which it places inside the launch horizon and must place on
    the sample the file reaches the range's end. Tracktion, on the audio thread,
    has to know where the file is to read it. The two are the same question, so
    they are asked of the same arithmetic over the same breakpoints - samples on
    the tick thread, beats on the audio side, one a fixed linear map of the
    other - and the Runner is never estimating what the audio thread did.

    The breakpoints are placed a launch horizon AHEAD (decision DU), so by the
    time the audio thread plays a stretch of time, every breakpoint that stretch
    will ever have is already in its clock. A breakpoint is never placed in the
    past, and nothing already played is rewritten.

    THE AXIS IS THE CALLER'S. `at` is a time on it, `rate` a plain number (one is
    the file's own speed), and `source` - where the file has got to - is in the
    axis's own units from wherever the clock began. Only DIFFERENCES of source
    mean anything: how far the file moved between a launch and now.

    AT ONE, NOTHING. A stretch in which every breakpoint says exactly one (by bit
    pattern: -Wfloat-equal, and because "nearly one" is not one) is the identity,
    and `isIdentityFrom` says so, so a caller can answer with the exact
    arithmetic it used before any of this existed - a render at one is
    bit-identical to the day before (§22.4).

    Vendor-free, header-only, and allocation-free: the audio thread holds one per
    voice and asks it every block (PRD §4.2). Fixed capacity; a caller that has
    finished with the past says so with `forgetBefore`.
*/

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace wfg
{
    class RateClock
    {
    public:
        static constexpr int capacity = 64;

        /*  One breakpoint: the speed here, and where the file has got to here.
            From it the speed moves in a straight line to the next point's. */
        struct Point
        {
            double at = 0.0;
            double rate = 1.0;
            double source = 0.0;
        };

        /** Exactly one, by bit pattern. */
        static bool isOne (double rate) noexcept
        {
            return std::bit_cast<std::uint64_t> (rate) == std::bit_cast<std::uint64_t> (1.0);
        }

        /*  Begins afresh: one point at `at`, at `rate`, with the file at `at`, so
            a clock nobody moves reads the file exactly where the time is. */
        void start (double at, double rate) noexcept
        {
            points[0] = { at, rate, at };
            count = 1;
        }

        int size() const noexcept                   { return count; }
        const Point& front() const noexcept         { return points[0]; }
        const Point& back() const noexcept          { return points[static_cast<std::size_t> (count - 1)]; }

        /*  From the last point, the speed moves in a straight line to `rate` at
            `at`. A point at the same time as the last one is a step. False, and
            nothing changes, when `at` is before the last point, when the speed
            is below nought or not a number, or when the clock is full. A clock
            nobody started is started. */
        bool place (double at, double rate) noexcept
        {
            if (! (rate >= 0.0) || ! std::isfinite (rate) || ! std::isfinite (at))
                return false;

            if (count == 0)
            {
                start (at, rate);
                return true;
            }

            const auto& last = back();

            if (at < last.at || count >= capacity)
                return false;

            const auto length = at - last.at;
            points[static_cast<std::size_t> (count)] = { at, rate,
                                                        last.source + length * 0.5 * (last.rate + rate) };
            ++count;
            return true;
        }

        /*  The speed at `t`: the straight line between the two points around
            it, the first point's before it, the last point's after. */
        double rateAt (double t) const noexcept
        {
            if (count == 0)
                return 1.0;

            const auto i = segmentOf (t);

            if (i < 0)
                return points[0].rate;

            const auto& p = points[static_cast<std::size_t> (i)];

            if (i + 1 >= count)
                return p.rate;

            const auto& q = points[static_cast<std::size_t> (i + 1)];
            const auto length = q.at - p.at;

            if (! (length > 0.0))
                return q.rate;

            return p.rate + (q.rate - p.rate) * ((t - p.at) / length);
        }

        /*  Where the file has got to at `t`, on the clock's own axis. Before the
            first point it is run backwards at the first point's speed, which no
            caller should need: a launch is always inside what the clock holds. */
        double sourceAt (double t) const noexcept
        {
            if (count == 0)
                return t;

            const auto i = segmentOf (t);

            if (i < 0)
                return points[0].source + (t - points[0].at) * points[0].rate;

            const auto& p = points[static_cast<std::size_t> (i)];
            const auto into = t - p.at;

            if (i + 1 >= count)
                return p.source + into * p.rate;

            return p.source + into * 0.5 * (p.rate + rateAt (t));
        }

        /*  The earliest time, not before the first point, at which the file has
            reached `source`. Nothing when it never will, at the speeds the clock
            holds: a speed held at nought after the last point never reaches
            anything further. */
        std::optional<double> whenSourceReaches (double source) const noexcept
        {
            if (count == 0)
                return source;

            if (source <= points[0].source)
                return points[0].at;

            for (int i = 0; i < count; ++i)
            {
                const auto& p = points[static_cast<std::size_t> (i)];
                const auto need = source - p.source;

                if (i + 1 >= count)
                {
                    if (! (p.rate > 0.0))
                        return std::nullopt;

                    return p.at + need / p.rate;
                }

                const auto& q = points[static_cast<std::size_t> (i + 1)];

                if (q.source < source)
                    continue;

                const auto length = q.at - p.at;

                if (! (length > 0.0))
                    return q.at;

                /*  source(d) = a d + k d² / 2 with k the slope of the speed, solved
                    for d in the form that does not subtract two near-equal
                    numbers when the slope is small: 2n / (a + sqrt(a² + 2kn)). */
                const auto a = p.rate;
                const auto k = (q.rate - p.rate) / length;
                const auto root = std::sqrt (std::max (0.0, a * a + 2.0 * k * need));
                const auto denominator = a + root;

                if (! (denominator > 0.0))
                    return q.at;

                return p.at + std::clamp (2.0 * need / denominator, 0.0, length);
            }

            return std::nullopt;
        }

        /*  Whether, from `t` on, the speed is exactly one everywhere: the point
            in force at `t` and every one after it say one, bit for bit. */
        bool isIdentityFrom (double t) const noexcept
        {
            if (count == 0)
                return true;

            for (int i = std::max (0, segmentOf (t)); i < count; ++i)
                if (! isOne (points[static_cast<std::size_t> (i)].rate))
                    return false;

            return true;
        }

        /*  Lets go of the points nobody will ask about again: every one before
            the last point at or before `t`, which is kept so the stretch from it
            still integrates. */
        void forgetBefore (double t) noexcept
        {
            const auto keep = segmentOf (t);

            if (keep <= 0)
                return;

            std::copy (points.begin() + keep, points.begin() + count, points.begin());
            count -= keep;
        }

    private:
        std::array<Point, capacity> points {};
        int count = 0;

        /*  The index of the last point at or before `t`, or -1 when `t` is before
            them all. From the end: the audio thread asks about now, and now is
            nearly always in the last segment or two. */
        int segmentOf (double t) const noexcept
        {
            for (int i = count - 1; i >= 0; --i)
                if (points[static_cast<std::size_t> (i)].at <= t)
                    return i;

            return -1;
        }
    };
}
