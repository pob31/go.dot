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

#include <wfg/engine/clock/TickClock.h>

namespace wfg
{
    std::optional<int> TickClock::samplesPerTickFor (int rateInHz) noexcept
    {
        if (rateInHz <= 0 || rateInHz % rateHz != 0)
            return std::nullopt;

        return rateInHz / rateHz;
    }

    std::optional<TickClock> TickClock::create (int rateInHz) noexcept
    {
        const auto perTick = samplesPerTickFor (rateInHz);

        if (! perTick.has_value())
            return std::nullopt;

        return TickClock (rateInHz, *perTick);
    }

    //==============================================================================
    std::int64_t TickClock::sampleForTick (std::int64_t index) const noexcept
    {
        return sampleAnchor
             + (index - tickAnchor) * static_cast<std::int64_t> (currentSamplesPerTick);
    }

    std::int64_t TickClock::ticksReachedAt (std::int64_t samples) const noexcept
    {
        if (samples < sampleAnchor)
            return tickAnchor - 1;

        /*  Both operands are non-negative here, so the division truncates the
            way flooring wants. That is the whole reason for the branch above:
            C++ integer division truncates TOWARDS ZERO, so a negative numerator
            would round the wrong way and report a tick as reached one boundary
            too early. */
        const auto since = samples - sampleAnchor;
        return tickAnchor + since / static_cast<std::int64_t> (currentSamplesPerTick);
    }

    //==============================================================================
    bool TickClock::rebase (std::int64_t atTick, int newSampleRate) noexcept
    {
        const auto perTick = samplesPerTickFor (newSampleRate);

        if (! perTick.has_value() || atTick < tickAnchor)
            return false;

        /*  Where that tick sits is computed BEFORE the ratio changes, and then
            becomes the new anchor. That is what keeps the sequence continuous:
            the tick the change happens on is at the same sample either side of
            it, and only the ticks after it move. */
        sampleAnchor = sampleForTick (atTick);
        tickAnchor = atTick;
        currentRate = newSampleRate;
        currentSamplesPerTick = *perTick;

        return true;
    }
}
