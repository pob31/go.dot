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
    Where a timecode frame falls on the sample clock: the ONE rule LTC and MTC
    both place their code by (namespace draft §59, TC.2; AHJ, AHO).

    Everything is counted from a run's ZERO, the first sample of the frame it
    starts on, and in whole numbers. A frame cut into `parts` - 1 for the frame
    itself, 4 for MTC's quarter frames, 160 for LTC's half-bits - has its part
    `k` begin at the exact moment `k * den / (parts * num)` seconds after zero,
    and the part's FIRST SAMPLE is the first sample at or after that moment:
    `ceil (k * sampleRate * den / (parts * num))`. Never a double and never a
    sum of steps, so nothing drifts from the sound beside it however long a
    code runs: a part's place is computed afresh from its number each time.

    THE CEILING AND NOT THE FLOOR because it is the exact inverse of what the
    audio side reads at a sample (§59.1): the part sample `s` lies in is
    `floor (s * parts * num / (sampleRate * den))`, and the first sample of
    part `k` is the smallest sample that reading puts in `k`. With the one rule
    for both, a quarter frame and the frame it begins can never disagree by a
    sample - MTC's piece 0 leaves on the very sample LTC's frame begins on, and
    the frame a stop waits for is the frame the line is drawing. A sample at
    the exact moment belongs to the part that begins there.

    THE PRODUCTS NEVER OVERFLOW. A count is split into whole periods and a
    remainder before anything is multiplied, and only the remainder - smaller
    than the divisor - meets the multiplier: exact for as long as
    `parts * num * sampleRate * den` fits in 63 bits, which at 160 parts,
    30000/1001 and 192 kHz is 9.2e17, a tenth of the room. A day at 192 kHz
    is a few times 1e10 samples; nothing here comes near it.

    PURE and inline: the audio thread will call it per block (TC.3) and the
    MTC lane per message (TC.5), so it allocates nothing, locks nothing and
    throws nothing. A sample rate or a part count of nought or less places
    everything on nought rather than dividing by it.
*/

#include <wfg/engine/timecode/Timecode.h>

#include <cstdint>

namespace wfg::timecode
{
    namespace frameclock
    {
        /*  `a * b / c` rounded down, for `b > 0` and `c > 0` and any `a`,
            without forming `a * b`: `a` is split into whole multiples of `c`
            and a remainder in [0, c) first. The remainder's product is never
            negative, so plain division of it is already the floor. */
        inline std::int64_t scaledFloor (std::int64_t a, std::int64_t b, std::int64_t c) noexcept
        {
            auto whole = a / c;
            auto rest = a % c;

            if (rest < 0)
            {
                rest += c;
                --whole;
            }

            return whole * b + rest * b / c;
        }

        //  The same, rounded up.
        inline std::int64_t scaledCeil (std::int64_t a, std::int64_t b, std::int64_t c) noexcept
        {
            auto whole = a / c;
            auto rest = a % c;

            if (rest < 0)
            {
                rest += c;
                --whole;
            }

            return whole * b + (rest * b + c - 1) / c;
        }
    }

    /*  The first sample of part `part` of a run cut into `partsPerFrame` parts
        a frame, counted from the run's zero. Part 0 begins on zero itself. */
    inline std::int64_t partStartSample (std::int64_t part, std::int64_t partsPerFrame,
                                         std::int64_t sampleRate, Rate rate) noexcept
    {
        if (sampleRate <= 0 || partsPerFrame <= 0)
            return 0;

        return frameclock::scaledCeil (part, sampleRate * denOf (rate), partsPerFrame * numOf (rate));
    }

    /*  The part a sample lies in, counted from the run's zero: the inverse of
        `partStartSample`, so `partAtSample (partStartSample (k)) == k` and the
        sample before it lies in `k - 1`. A sample before zero lies in a part
        below nought. */
    inline std::int64_t partAtSample (std::int64_t sample, std::int64_t partsPerFrame,
                                      std::int64_t sampleRate, Rate rate) noexcept
    {
        if (sampleRate <= 0 || partsPerFrame <= 0)
            return 0;

        return frameclock::scaledFloor (sample, partsPerFrame * numOf (rate), sampleRate * denOf (rate));
    }

    /*  The first sample of frame `frame` after the run's zero, and the frame a
        sample lies in: the one-part case, which LTC's frame starts and stops
        (TC.3) and MTC's cycles (TC.5) both read. */
    inline std::int64_t frameStartSample (std::int64_t frame, std::int64_t sampleRate, Rate rate) noexcept
    {
        return partStartSample (frame, 1, sampleRate, rate);
    }

    inline std::int64_t frameAtSample (std::int64_t sample, std::int64_t sampleRate, Rate rate) noexcept
    {
        return partAtSample (sample, 1, sampleRate, rate);
    }

    /*  THE FIRST FRAME THAT BEGINS AT OR AFTER A SAMPLE: where a stop at the
        next frame boundary lands, and where a relocated MTC cycle may start.
        A frame beginning exactly on the sample is that frame; one that began
        before it is not. */
    inline std::int64_t firstFrameAtOrAfter (std::int64_t sample, std::int64_t sampleRate, Rate rate) noexcept
    {
        if (sampleRate <= 0)
            return 0;

        return frameAtSample (sample - 1, sampleRate, rate) + 1;
    }
}
