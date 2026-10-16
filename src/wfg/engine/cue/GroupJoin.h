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
    WHAT HAPPENS BETWEEN TWO MEMBERS OF A SEQUENCE (PRD §3.6; namespace draft
    §69), asked one way by the Runner, the walk and the client.

    A sequence group's `join` is one of three words. `letPlay` leaves the
    previous member alone - today's boundary. `gapless` places the next member
    on the previous one's end, or has a manual GO cut what still sounds.
    `crossfade` crosses the two over `joinLength`, shaped by `joinCurve`.

    THE FADES BELONG TO THE MEMBER, NOT TO THE BOUNDARY (ANZ). A member that
    fades in rises from its own launch; a member that fades out falls to the
    sample its fade-out was given - its own end in an automatic sequence, the
    incoming member's launch plus the length in a manual one. So the gain of a
    run is a pure function of the run and a sample, and the arithmetic here
    knows nothing of runs: a start, a length in samples, a curve.

    THE SHAPE IS THE SOUND EDIT'S (§55, `doc::fadeGain`): equal power for a
    sound at curve nought, a straight line for a picture, bent by the curve.
    A fade-out is the fade-in read backwards, so the two halves of a crossfade
    at nought add to one in power.
*/

#include <cstdint>
#include <string_view>

namespace wfg::cue::join
{
    enum class Kind
    {
        letPlay,
        gapless,
        crossfade
    };

    /*  A group's three rows, read: what its join is, how long a crossfade
        lasts and its shape. */
    struct Setting
    {
        Kind kind = Kind::letPlay;
        double seconds = 2.0;
        double curve = 0.0;
    };

    /*  The stored word's kind. Anything else - an old show's absent row, a word
        a later version wrote - leaves the previous member alone, which is what
        every show did before the row existed. */
    Kind kindOf (std::string_view word) noexcept;

    /*  The shortest crossfade: five ticks. A run's level is a tick's term with a
        tick's slew, so anything shorter would be a ramp of a few steps (AOC).
        The row refuses less; this is for what reads a value from elsewhere. */
    constexpr double minimumSeconds = 0.1;

    /*  How much of the run a fade-in lets through at `sample`: nothing before
        `from`, all of it from `from + length` on. A length of nought is a step
        at `from`. */
    double gainIn (std::int64_t sample, std::int64_t from, std::int64_t length, double curve,
                   bool picture) noexcept;

    /*  How much a fade-out lets through at `sample`: all of it before `from`,
        nothing from `from + length` on - the fade-in read backwards. */
    double gainOut (std::int64_t sample, std::int64_t from, std::int64_t length, double curve,
                    bool picture) noexcept;

    /*  A gain in decibels, nothing below the level a voice calls silence. */
    double decibelsOf (double gain) noexcept;
}
