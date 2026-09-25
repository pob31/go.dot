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
    THE TWO MUST MOVE TOGETHER: a hand that sets -6 dB on a Mackie unit and
    then looks at the virtual panel has to find its fader at the same height,
    and tests/SurfaceBridgeTests.cpp checks this file against the client's at
    every half decibel so that moving one and not the other fails a build.
    A SURFACE WITH AN ENGRAVING OF ITS OWN is the exception, and the D700 is
    one (2026-09-25): its faders land on the marks printed beside them, so it
    and the panel agree on the LEVEL and not on the height - see `FaderLaw`.

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
#include <span>
#include <utility>

namespace wfg::surface
{
    /** Silence, as the document spells it: the bottom of every fader. */
    inline constexpr double faderSilenceDb = -120.0;

    /** The top of the travel, as `run/trim` and `dca/trim` declare it. */
    inline constexpr double faderLoudestDb = 12.0;

    /** The top of a Mackie fader's fourteen bits. The bottom is nought. */
    inline constexpr int faderTop = 16383;

    /*  WHICH LAW A SURFACE'S FADERS FOLLOW. A hardware fader has its scale
        ENGRAVED beside it, and a level that does not land on its own mark
        reads as a fault (author, 2026-09-25: "The maximum I see engraved is
        +7dB. When set at 0dB (engraved) it reads -6dB on the screen"). So a
        profile whose engraving is known says so, and the rest - a generic
        Mackie unit, whose legend nobody has measured - keep the curve the
        virtual panel draws. */
    enum class FaderLaw { generic, d700 };

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

        /*  THE D700'S ENGRAVING, measured on the author's unit (2026-09-25):
            the fader was stopped on each engraved mark and the position it
            sent was read back from the session log.

              +7   16383   the top of the travel
               0   ~13040  79.6 %
              -24  ~5600   34.2 %
              -48  ~2095   12.8 %
              -inf     0   the bottom

            BETWEEN THE MARKS, A POWER LAW. The level moves by a nearly steady
            number of decibels per decade of travel - 70.6, 65.4 and 56.2 from
            the top down - which is the shape of a DAW volume law (Bitwig's is
            cubic, 60 dB a decade, and Asparion's own Bitwig script hands the
            fader's position to it unchanged: the D700 knows nothing of
            decibels, its engraving was drawn for a law). The points below
            follow that law in three- to twelve-dB steps, the last segment's
            slope continued to -96 dB and silence at the bottom; straight lines
            between them keep the inverse exact, and stay within 0.2 dB of the
            law above -60 dB. Every engraved mark reads what is engraved. */
        inline constexpr std::array<std::pair<double, double>, 21> d700
        {{
            { 0.0000, faderSilenceDb },
            { 0.0179, -96.0 },
            { 0.0293, -84.0 },
            { 0.0478, -72.0 },
            { 0.0612, -66.0 },
            { 0.0782, -60.0 },
            { 0.1000, -54.0 },
            { 0.1279, -48.0 },
            { 0.1635, -42.0 },
            { 0.2091, -36.0 },
            { 0.2673, -30.0 },
            { 0.3418, -24.0 },
            { 0.4222, -18.0 },
            { 0.5216, -12.0 },
            { 0.5797, -9.0 },
            { 0.6443, -6.0 },
            { 0.7161, -3.0 },
            { 0.7959, 0.0 },
            { 0.8777, 3.0 },
            { 0.9369, 5.0 },
            { 1.0000, 7.0 },
        }};

        using Points = std::span<const std::pair<double, double>>;

        inline Points pointsOf (FaderLaw law) noexcept
        {
            return law == FaderLaw::d700 ? Points { d700 } : Points { points };
        }

        inline double between (double from, double to, double howFar) noexcept
        {
            return from + (to - from) * howFar;
        }
    }

    /** Where on the travel, from 0 at the bottom to 1 at the top, a level sits.
        A value that is not a number sits at the bottom; one above the law's
        top sits at the top (+12 on a D700 is its +7, as far up as it goes). */
    inline double fractionForDb (double decibels, FaderLaw law = FaderLaw::generic) noexcept
    {
        if (! std::isfinite (decibels))
            return 0.0;

        const auto points = faderCurve::pointsOf (law);
        const auto level = std::clamp (decibels, points.front().second, points.back().second);

        for (std::size_t at = 1; at < points.size(); ++at)
        {
            const auto& below = points[at - 1];
            const auto& above = points[at];

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
    inline double dbForFraction (double fraction, FaderLaw law = FaderLaw::generic) noexcept
    {
        if (! std::isfinite (fraction))
            return faderSilenceDb;

        const auto points = faderCurve::pointsOf (law);
        const auto place = std::clamp (fraction, 0.0, 1.0);

        for (std::size_t at = 1; at < points.size(); ++at)
        {
            const auto& below = points[at - 1];
            const auto& above = points[at];

            if (place > above.first)
                continue;

            const auto span = above.first - below.first;

            if (! (span > 0.0))
                return below.second;

            return faderCurve::between (below.second, above.second, (place - below.first) / span);
        }

        return points.back().second;
    }

    /** A level as a Mackie fader position, 0..16383, rounded to the nearest. */
    inline int fourteenBitForDb (double decibels, FaderLaw law = FaderLaw::generic) noexcept
    {
        const auto position = std::lround (fractionForDb (decibels, law) * static_cast<double> (faderTop));
        return static_cast<int> (std::clamp (position, 0L, static_cast<long> (faderTop)));
    }

    /** A Mackie fader position as a level. Clamped to 0..16383 first. */
    inline double dbForFourteenBit (int position, FaderLaw law = FaderLaw::generic) noexcept
    {
        const auto clamped = std::clamp (position, 0, faderTop);
        return dbForFraction (static_cast<double> (clamped) / static_cast<double> (faderTop), law);
    }
}
