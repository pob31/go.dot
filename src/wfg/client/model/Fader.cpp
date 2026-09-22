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

#include <wfg/client/model/Fader.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        /*  The four points, bottom to top. Straight between them, which is
            what makes the inverse exact rather than approximate: a hand that
            drags to a place and reads a number, then types that number back,
            must land on the same place. */
        constexpr std::array<std::pair<double, double>, 4> throwPoints
        {{
            { 0.00, silenceDb },
            { 0.20, -60.0 },
            { 0.85, 0.0 },
            { 1.00, loudestDb },
        }};

        double between (double from, double to, double howFar)
        {
            return from + (to - from) * howFar;
        }
    }

    double fractionForDb (double decibels)
    {
        const auto level = std::clamp (decibels, silenceDb, loudestDb);

        for (std::size_t at = 1; at < throwPoints.size(); ++at)
        {
            const auto& below = throwPoints[at - 1];
            const auto& above = throwPoints[at];

            if (level > above.second)
                continue;

            const auto span = above.second - below.second;

            /*  A segment of no height would divide by nought. It cannot happen
                with the points above, and this is the line that keeps it true
                when somebody moves them. */
            if (! (span > 0.0))
                return below.first;

            return between (below.first, above.first, (level - below.second) / span);
        }

        return 1.0;
    }

    double dbForFraction (double fraction)
    {
        const auto place = std::clamp (fraction, 0.0, 1.0);

        for (std::size_t at = 1; at < throwPoints.size(); ++at)
        {
            const auto& below = throwPoints[at - 1];
            const auto& above = throwPoints[at];

            if (place > above.first)
                continue;

            const auto span = above.first - below.first;

            if (! (span > 0.0))
                return below.second;

            return between (below.second, above.second, (place - below.first) / span);
        }

        return loudestDb;
    }

    std::string faderText (double decibels)
    {
        /*  THE WORD AND NOT THE NUMBER AT THE BOTTOM. "-120.0" reads as a very
            quiet sound; what it means is no sound, and the two are worth
            spelling differently where an operator is deciding whether they
            have muted something. */
        if (decibels <= silenceDb)
            return "-inf";

        /*  ROUNDED BEFORE IT IS FORMATTED, because `formatDouble` writes
            what the double actually is - which for a level dragged to
            somewhere near -6 is seventeen digits of binary residue. A tenth of
            a decibel is finer than anybody sets a send to, and it is what the
            box accepts back. */
        const auto level = std::clamp (decibels, silenceDb, loudestDb);

        return osc::formatDouble (std::round (level * 10.0) / 10.0);
    }

    double stepDb (double decibels, int clicks, bool fine)
    {
        /*  A DECIBEL A CLICK, a tenth with shift. Stepping in dB rather than
            in fractions is what makes a wheel predictable: near the bottom of
            the throw a fraction-step is thirty dB and near the top it is one,
            and nobody means that by "one click louder". */
        const auto by = static_cast<double> (clicks) * (fine ? 0.1 : 1.0);

        return std::clamp (decibels + by, silenceDb, loudestDb);
    }
}
