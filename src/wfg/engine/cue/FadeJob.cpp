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
}
