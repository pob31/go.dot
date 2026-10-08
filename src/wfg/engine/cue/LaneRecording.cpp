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

#include <wfg/engine/cue/LaneRecording.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        /*  Closer than this in time is one instant: the tick is 20 ms, so two
            samples a microsecond apart are one sample twice. */
        constexpr double sameInstant = 1.0e-6;
    }

    void appendRide (std::vector<RideSegment>& segments, double seconds, double levelDb)
    {
        if (segments.empty() || segments.back().empty())
        {
            if (segments.empty())
                segments.emplace_back();

            segments.back().push_back ({ seconds, levelDb });
            return;
        }

        auto& last = segments.back().back();

        if (std::abs (seconds - last.seconds) < sameInstant)
        {
            last.levelDb = levelDb;
            return;
        }

        if (seconds < last.seconds)
            segments.push_back ({ doc::LanePoint { seconds, levelDb } });
        else
            segments.back().push_back ({ seconds, levelDb });
    }

    RideSegment thinRide (const RideSegment& segment, double toleranceDb)
    {
        if (segment.size() <= 2)
            return segment;

        std::vector<bool> keep (segment.size(), false);
        keep.front() = true;
        keep.back() = true;

        /*  AN EXPLICIT STACK, not recursion: a ten-minute ride is thirty
            thousand samples, and a steady ramp splits one sample at a time -
            a recursion that deep is a crash on the thread GO shares. */
        std::vector<std::pair<std::size_t, std::size_t>> spans { { 0, segment.size() - 1 } };

        while (! spans.empty())
        {
            const auto [from, to] = spans.back();
            spans.pop_back();

            if (to <= from + 1)
                continue;

            const auto& a = segment[from];
            const auto& b = segment[to];
            const auto span = b.seconds - a.seconds;

            auto worst = 0.0;
            auto at = from;

            for (auto k = from + 1; k < to; ++k)
            {
                const auto& p = segment[k];
                const auto onLine = span > 0.0 ? a.levelDb + (b.levelDb - a.levelDb) * (p.seconds - a.seconds) / span
                                               : a.levelDb;
                const auto off = std::abs (p.levelDb - onLine);

                if (off > worst)
                {
                    worst = off;
                    at = k;
                }
            }

            if (worst > toleranceDb)
            {
                keep[at] = true;
                spans.push_back ({ from, at });
                spans.push_back ({ at, to });
            }
        }

        RideSegment out;

        for (std::size_t k = 0; k < segment.size(); ++k)
            if (keep[k])
                out.push_back (segment[k]);

        return out;
    }

    std::vector<doc::LanePoint> spliceRide (std::vector<doc::LanePoint> lane,
                                            const std::vector<RideSegment>& segments,
                                            double joinSeconds, double toleranceDb)
    {
        for (const auto& segment : segments)
        {
            if (segment.empty())
                continue;

            const auto ride = thinRide (segment, toleranceDb);
            const auto first = ride.front().seconds;
            const auto last = ride.back().seconds;

            const auto before = first - joinSeconds;
            const auto after = last + joinSeconds;

            std::vector<doc::LanePoint> out;
            out.reserve (lane.size() + ride.size() + 2);

            for (const auto& point : lane)
                if (point.seconds < before)
                    out.push_back (point);

            /*  THE OLD CURVE'S OWN LEVEL AT EACH JOIN, read before anything is
                replaced, so the lane either side of the ride is exactly what it
                was. Before the file starts there is nothing to join from. */
            if (before > 0.0)
                out.push_back ({ before, doc::laneLevelDb (lane, before) });

            out.insert (out.end(), ride.begin(), ride.end());
            out.push_back ({ after, doc::laneLevelDb (lane, after) });

            for (const auto& point : lane)
                if (point.seconds > after)
                    out.push_back (point);

            lane = std::move (out);
        }

        return lane;
    }

    std::string laneText (const std::vector<doc::LanePoint>& lane)
    {
        std::string out;
        auto previous = -1.0;

        for (const auto& point : lane)
        {
            const auto seconds = std::round (point.seconds * 10000.0) / 10000.0;

            /*  STILL CLIMBING ONCE ROUNDED, or left out: an old point a hair
                before a join would otherwise be written at the join's own
                second, and two points at one instant are what the judge
                refuses - one bad pair would lose the whole ride. */
            if (! (seconds > previous))
                continue;

            previous = seconds;

            if (! out.empty())
                out += ' ';

            out += osc::formatDouble (seconds) + " "
                     + osc::formatDouble (std::round (point.levelDb * 100.0) / 100.0);
        }

        return out;
    }

    std::string curveText (const std::vector<doc::LanePoint>& curve, double step)
    {
        std::string out;
        auto previous = -1.0;

        for (const auto& point : curve)
        {
            const auto seconds = std::round (point.seconds * 10000.0) / 10000.0;

            //  Still climbing once rounded, for `laneText`'s reason.
            if (! (seconds > previous))
                continue;

            previous = seconds;

            if (! out.empty())
                out += ' ';

            const auto value = step > 0.0 ? std::round (point.levelDb / step) * step : point.levelDb;
            out += osc::formatDouble (seconds) + " " + osc::formatDouble (value);
        }

        return out;
    }

    double curveTolerance (double tolerance, double span) noexcept
    {
        if (tolerance > 0.0)
            return tolerance;

        return span > 0.0 ? span / 1000.0 : 0.001;
    }

    std::vector<doc::LanePoint> spliceCurve (const std::vector<doc::LanePoint>& curve,
                                             const std::vector<RideSegment>& segments,
                                             double joinSeconds, double tolerance)
    {
        if (! curve.empty())
            return spliceRide (curve, segments, joinSeconds, tolerance);

        const auto first = std::find_if (segments.begin(), segments.end(),
                                         [] (const RideSegment& segment) { return ! segment.empty(); });

        if (first == segments.end())
            return {};

        return spliceRide (thinRide (*first, tolerance), std::vector<RideSegment> (std::next (first), segments.end()),
                           joinSeconds, tolerance);
    }

    double curveStep (double tolerance) noexcept
    {
        return tolerance > 0.0 ? std::pow (10.0, std::floor (std::log10 (tolerance / 10.0))) : 0.0001;
    }
}
