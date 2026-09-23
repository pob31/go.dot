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
    WHERE A MOTOR FADER SITS FOR A LEVEL, and what a level is for a place on
    its travel - the engine's copy of the curve the desktop client draws.

    A RESTATEMENT, AND DELIBERATELY ONE. The four points below are
    `src/wfg/client/model/Fader.h`'s, which is WFS-DIY's curve: the engine
    links no client (Console.h's ClientHost is the whole contract), so the
    bridge cannot call the client's functions and has to say them again.
    THE TWO MUST MOVE TOGETHER: a hand that sets -6 dB on the D700 and then
    looks at the virtual panel has to find its fader at the same height, and
    tests/SurfaceBridgeTests.cpp checks this file against the client's at
    every half decibel so that moving one and not the other fails a build.

      fraction 0.00 = -120 dB   silence, and the bottom of the throw
      fraction 0.20 =  -60 dB   the bottom fifth covers sixty dB nobody rides
      fraction 0.85 =    0 dB   unity high up, where a hand finds it
      fraction 1.00 =  +12 dB   the headroom a quiet recording needs

    Straight between the points, which is what makes the inverse exact: a
    position the hand sent, turned into decibels and written, comes back as
    the same position - and that is what lets the bridge recognise its own
    fader's echo and not move the motor under the finger that caused it.

    FOURTEEN BITS ON THE WIRE. A Mackie fader is pitch bend, 0..16383, and
    the bottom of the travel is -120 dB exactly: silence is where a parked
    channel lives, and a curve that could not land on it would leave every
    parked fader a hair open.

    std only, and header only: the bridge, its tests and anything else on
    the engine side may include it.
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace wfg::surface
{
    /** Silence, as the document spells it: the bottom of every fader. */
    inline constexpr double faderSilenceDb = -120.0;

    /** The top of the travel, as `run/trim` and `dca/trim` declare it. */
    inline constexpr double faderLoudestDb = 12.0;

    /** The top of a Mackie fader's fourteen bits. The bottom is nought. */
    inline constexpr int faderTop = 16383;

    namespace faderCurve
    {
        /*  The four points, bottom to top: { fraction, dB }. The client's
            `throwPoints` in model/Fader.cpp, restated - see above. */
        inline constexpr std::array<std::pair<double, double>, 4> points
        {{
            { 0.00, faderSilenceDb },
            { 0.20, -60.0 },
            { 0.85, 0.0 },
            { 1.00, faderLoudestDb },
        }};

        inline double between (double from, double to, double howFar) noexcept
        {
            return from + (to - from) * howFar;
        }
    }

    /** Where on the travel, from 0 at the bottom to 1 at the top, a level sits.
        A value that is not a number sits at the bottom. */
    inline double fractionForDb (double decibels) noexcept
    {
        if (! std::isfinite (decibels))
            return 0.0;

        const auto level = std::clamp (decibels, faderSilenceDb, faderLoudestDb);

        for (std::size_t at = 1; at < faderCurve::points.size(); ++at)
        {
            const auto& below = faderCurve::points[at - 1];
            const auto& above = faderCurve::points[at];

            if (level > above.second)
                continue;

            const auto span = above.second - below.second;

            //  A segment of no height would divide by nought; the client's guard.
            if (! (span > 0.0))
                return below.first;

            return faderCurve::between (below.first, above.first, (level - below.second) / span);
        }

        return 1.0;
    }

    /** And back: what a place on the travel means. Clamped into the range. */
    inline double dbForFraction (double fraction) noexcept
    {
        if (! std::isfinite (fraction))
            return faderSilenceDb;

        const auto place = std::clamp (fraction, 0.0, 1.0);

        for (std::size_t at = 1; at < faderCurve::points.size(); ++at)
        {
            const auto& below = faderCurve::points[at - 1];
            const auto& above = faderCurve::points[at];

            if (place > above.first)
                continue;

            const auto span = above.first - below.first;

            if (! (span > 0.0))
                return below.second;

            return faderCurve::between (below.second, above.second, (place - below.first) / span);
        }

        return faderLoudestDb;
    }

    /** A level as a Mackie fader position, 0..16383, rounded to the nearest. */
    inline int fourteenBitForDb (double decibels) noexcept
    {
        const auto position = std::lround (fractionForDb (decibels) * static_cast<double> (faderTop));
        return static_cast<int> (std::clamp (position, 0L, static_cast<long> (faderTop)));
    }

    /** A Mackie fader position as a level. Clamped to 0..16383 first. */
    inline double dbForFourteenBit (int position) noexcept
    {
        const auto clamped = std::clamp (position, 0, faderTop);
        return dbForFraction (static_cast<double> (clamped) / static_cast<double> (faderTop));
    }
}
