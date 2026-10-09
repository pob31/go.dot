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
    A DCA'S TRIM AS A PICTURE'S OPACITY (namespace draft 37.5, WE).

    The author's ends: "-inf -> 0% opacity to 0dB -> 100%. Don't go over 100%
    even if the fader is above 0dB." Between them, the author's choice: the
    picture follows the fader's TRAVEL - how far up the show's fader the trim
    sits, over how far up nought dB sits - so half way up the fader looks half
    way faded. On the show's law (the panel's, not a surface's): 92 % at -6 dB,
    75 % at -20, 49 % at -40, 24 % at -60, nothing at -120.

    A factor 0..1 that multiplies the opacity a run is at. Pure.
*/

#include <wfg/engine/surface/FaderCurve.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace wfg::video
{
    inline double opacityForTrim (double decibels) noexcept
    {
        if (std::isnan (decibels))
            return 1.0;

        if (decibels >= 0.0)
            return 1.0;

        const auto unity = surface::fractionForDb (0.0);

        if (! (unity > 0.0))
            return 1.0;

        return std::clamp (surface::fractionForDb (decibels) / unity, 0.0, 1.0);
    }

    /*  THE CURVE ON A DCA MARK (namespace draft §50, ABU, proposed): the
        factor the fader's travel gives, bent by the mark's `dcaCurve` in %.
        Nought is the straight travel; above nought the picture comes in fast
        at first - at 100 the factor's fourth root - and below nought slowly at
        first, at -100 its fourth power. Nothing and all of it stay where they
        are, so a fader at the bottom still hides the picture and one at the
        top still shows all of it. */
    inline double shapedOpacity (double factor, double curve) noexcept
    {
        if (std::isnan (factor))
            return 1.0;

        const auto clamped = std::clamp (factor, 0.0, 1.0);

        if (std::isnan (curve) || clamped <= 0.0 || clamped >= 1.0)
            return clamped;

        return std::pow (clamped, std::pow (4.0, -std::clamp (curve, -100.0, 100.0) / 100.0));
    }

    /*  A TINT AS THE TREE SAYS IT (namespace draft §38, WR): "#RRGGBB", in
        capitals, as a fill's `paint` is written. */
    inline std::string tintText (std::uint32_t rgb)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out = "#";

        for (int shift = 20; shift >= 0; shift -= 4)
            out += digits[(rgb >> shift) & 0xfu];

        return out;
    }
}
