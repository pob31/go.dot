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

#include <wfg/engine/cue/FadeJob.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace wfg::cue
{
    FadeCurve fadeCurveFrom (const std::string& text) noexcept
    {
        /*  Anything unknown is linear rather than a refusal, because this is
            read from a document the grammar has already checked - the enum is a
            closed set there - and a fade that refused to run because somebody
            hand-edited a word would be a cue that does nothing on a show night.
            Linear is the shape that surprises nobody. */
        return text == "sCurve" ? FadeCurve::sCurve : FadeCurve::linear;
    }

    double fadeLevelDb (double fromDb, double toDb, double progress, FadeCurve curve) noexcept
    {
        const auto t = std::clamp (progress, 0.0, 1.0);

        /*  SMOOTHSTEP, which is monotonic on [0,1] and has zero slope at both
            ends - so an sCurve leaves the start without a corner and arrives
            without one either. It cannot overshoot, which matters more than the
            shape: a curve that went past its destination would put a level
            somewhere nobody asked for, and briefly is long enough to hear. */
        const auto shaped = curve == FadeCurve::sCurve ? t * t * (3.0 - 2.0 * t)
                                                       : t;

        /*  INTERPOLATED IN dB. A linear ramp in GAIN spends most of its time in
            the top few decibels and then falls off a cliff; a linear ramp in dB
            is what a hand on a fader does, and what every desk in every theatre
            has trained everyone to expect. */
        return fromDb + (toDb - fromDb) * shaped;
    }

    double fadeLevelDb (double fromDb, const std::vector<doc::FadePoint>& points,
                        double progress) noexcept
    {
        if (points.size() < 2)
            return fromDb;

        const auto t = std::clamp (progress, 0.0, 1.0);

        /*  The segment whose END is the first breakpoint at or after `t`, so
            that a `t` landing exactly on a breakpoint is answered as the end of
            the segment before it - which is that breakpoint's level, exactly. */
        std::size_t end = 1;

        while (end + 1 < points.size() && points[end].t < t)
            ++end;

        const auto& from = points[end - 1];
        const auto& to = points[end];

        /*  The first segment leaves from the run's level, not the drawing's:
            see the header. */
        const auto startDb = end == 1 ? fromDb : from.levelDb;
        const auto span = to.t - from.t;
        const auto along = span > 0.0 ? std::clamp ((t - from.t) / span, 0.0, 1.0) : 1.0;

        /*  Arrives EXACTLY, rather than at a start plus a difference that
            rounding left a hair short: a breakpoint at -120 dB is a silence
            and -119.99 is not. */
        if (along >= 1.0)
            return to.levelDb;

        return startDb + (to.levelDb - startDb) * along;
    }
}
