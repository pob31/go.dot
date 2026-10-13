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
    A STRAIGHT LINE THROUGH WHAT A WATCHER HEARD (namespace draft §60, TG.5 and
    TG.6).

    Both watchers - CodeWatch for LTC and MTC, ClockWatch for the wall clock -
    hold a window of points, a source's position against Go.dot's sample, and
    need the same three things of it: the line through them by least squares,
    where it puts the source at a sample and how fast it says the source runs,
    and how far each can be trusted. Written once, here, out of CodeWatch, so
    the two watchers fit their lines by the same arithmetic and a mend to one
    is a mend to both.

    THE ERRORS ARE THE TEXTBOOK'S, for points scattered `sigma` about a line:
    the slope known to sigma over the root of the samples' spread, a position
    to sigma times the root of one over the count plus its distance from the
    points' middle squared over that spread. The caller says what sigma is -
    the residual once there are three points, an assumed jitter before - since
    that depends on what was heard, not on the line.

    PURE AND INLINE: no clock, no JUCE; the window's storage is the caller's.
*/

#include <cmath>
#include <cstddef>
#include <deque>

namespace wfg::timecode::linefit
{
    //  One thing heard: a position of the source (frames, or milliseconds of the day) at a Go.dot sample.
    struct Point
    {
        double sample = 0.0;
        double position = 0.0;
    };

    /*  The fitted line: through (`sample`, `position`), `slope` positions per
        sample, with what the errors need - the points' count, their mean
        sample and spread, and the residual about the line (negative until
        three points, when two say nothing of their scatter). */
    struct Line
    {
        bool valid = false;
        double sample = 0.0;
        double position = 0.0;
        double slope = 0.0;
        double meanSample = 0.0;
        double spread = 0.0;
        double residual = -1.0;
        std::size_t count = 0;

        double positionAt (double atSample) const noexcept { return position + slope * (atSample - sample); }

        //  How far the slope can be trusted, for points `sigma` about the line.
        double slopeError (double sigma) const noexcept
        {
            return spread > 0.0 ? sigma / std::sqrt (spread) : 0.0;
        }

        //  How far the line's position at a sample can be trusted, likewise.
        double positionError (double atSample, double sigma) const noexcept
        {
            if (! valid || count == 0 || ! (spread > 0.0))
                return 0.0;

            const auto away = atSample - meanSample;
            return sigma * std::sqrt (1.0 / static_cast<double> (count) + away * away / spread);
        }
    };

    /*  THE LINE THROUGH A WINDOW of points, or an invalid one for fewer than
        two or for points all on one sample. Summed about the newest point, so
        the sums are of small numbers however many hours of samples the clock
        has counted. */
    inline Line through (const std::deque<Point>& window) noexcept
    {
        Line line;
        const auto count = window.size();

        if (count < 2)
            return line;

        const auto& reference = window.back();
        auto sumX = 0.0;
        auto sumY = 0.0;

        for (const auto& point : window)
        {
            sumX += point.sample - reference.sample;
            sumY += point.position - reference.position;
        }

        const auto n = static_cast<double> (count);
        const auto meanX = sumX / n;
        const auto meanY = sumY / n;
        auto spread = 0.0;
        auto together = 0.0;

        for (const auto& point : window)
        {
            const auto dx = point.sample - reference.sample - meanX;
            const auto dy = point.position - reference.position - meanY;
            spread += dx * dx;
            together += dx * dy;
        }

        if (! (spread > 0.0))
            return line;

        const auto slope = together / spread;
        auto leftOver = 0.0;

        for (const auto& point : window)
        {
            const auto dx = point.sample - reference.sample - meanX;
            const auto dy = point.position - reference.position - meanY;
            const auto off = dy - slope * dx;
            leftOver += off * off;
        }

        line.valid = true;
        line.sample = reference.sample;
        line.position = reference.position + meanY - slope * meanX;
        line.slope = slope;
        line.meanSample = reference.sample + meanX;
        line.spread = spread;
        line.count = count;
        line.residual = count >= 3 ? std::sqrt (leftOver / (n - 2.0)) : -1.0;
        return line;
    }
}
