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
    Where a fader sits for a level, and what a level is for a place on a fader.

    NOT A STRAIGHT LINE, because decibels are not what a hand expects. A linear
    -120..+12 scale puts unity at nine tenths of the way up and spends its whole
    bottom half between silence and inaudible, so the useful part of the throw -
    the few dB either side of where a cue actually sits - is a couple of pixels.
    Every console in the world bends it, and this bends it the way WFS-DIY's
    does: four points, straight between them.

      fraction 0.00 = -120 dB   silence, and the bottom of the throw
      fraction 0.20 =  -60 dB   the bottom fifth covers sixty dB nobody rides
      fraction 0.85 =    0 dB   unity high up, where a hand finds it
      fraction 1.00 =  +12 dB   the headroom a quiet recording needs

    The points are *(proposed)* - they are a feel rather than a fact, and moving
    them is four numbers.

    SILENCE IS EXACT. -120 dB is how this document spells "no sound at all"
    rather than "very quiet", so the bottom of the throw has to land on it
    exactly and `faderText` says so in words rather than as a number nobody
    reads as silence.
*/

#include <string>

namespace wfg::client::model
{
    /** Silence, as the document spells it. */
    inline constexpr double silenceDb = -120.0;

    /** The loudest anything may be asked for, as `media/level` declares. */
    inline constexpr double loudestDb = 12.0;

    /** Where on the throw, from 0 at the bottom to 1 at the top, a level sits. */
    double fractionForDb (double decibels);

    /** And back: what a place on the throw means. Clamped into the range. */
    double dbForFraction (double fraction);

    /*  The number beside the fader, always drawn: §4.8, colour is never the
        sole carrier of information, and a fader's POSITION is a colour-like
        fact. One decimal, through `osc::formatDouble` so a French locale reads
        it, and the word for silence at the bottom. */
    std::string faderText (double decibels);

    /*  A wheel click or an arrow, `fine` for the shift-held tenth. Answers the
        new level, clamped. Steps in dB rather than in fractions, because what
        somebody means by "a bit louder" is a decibel and not a pixel. */
    double stepDb (double decibels, int clicks, bool fine);
}
