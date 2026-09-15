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
    A fade somebody drew: `Fade/@points`, read and judged in one place.

    The attribute is a `d*` - a run of doubles, which the schema parses element
    by element - but a run of doubles is not yet a curve. It is a curve when the
    doubles pair up as (t, level), the times climb from 0 to 1, and every level
    is one a fade may reach. None of that is a property of one element, so none
    of it is the schema's; all of it is this file's (namespace §14.6).

    ONE FUNCTION AND THREE CALLERS, which is the reason it is a file. The write
    door refuses a list that is not a curve, `ShowDocument::validate` refuses a
    file that holds one, and the Runner reads the curve it plays from here - so
    the three cannot come to disagree about what a curve is, which is the
    failure a second copy of these rules would eventually produce.

    Vendor-free: consulted from the document layer and the cue layer, and it
    has no reason to know about JUCE.
*/

#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  One breakpoint: a fraction of the fade's duration, and the level in dB
        the fade passes through at that moment. Absolute dB, not an offset from
        where the fade began - plan decision 9. */
    struct FadePoint
    {
        double t = 0.0;
        double levelDb = 0.0;
    };

    struct FadePoints
    {
        /** The breakpoints, in order. Empty for an empty list, or a bad one. */
        std::vector<FadePoint> points;

        /*  Why the list is not a curve, in words that name the element at
            fault; empty when it is one. An EMPTY LIST IS A CURVE, or rather
            the absence of one: `curve` applies, which is how every fade written
            before Phase 5 goes on meaning what it meant. */
        std::string problem;
    };

    /*  The list, parsed and judged. Refuses, and leaves `points` empty:

        - an element that is not a number;
        - an odd count - a breakpoint is a time AND a level;
        - a time outside 0..1, or one that does not climb strictly;
        - a first time that is not 0, or a last that is not 1, so that the
          drawing always covers the whole of the fade and never leaves a stretch
          the curve does not say anything about;
        - a level outside the range `Fade/@level` declares (-120..12 dB), read
          from that row rather than written here a second time. */
    FadePoints readFadePoints (std::string_view text);
}
