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

#include <wfg/engine/timecode/CountLine.h>

#include <algorithm>
#include <cmath>

namespace wfg::timecode
{
    namespace
    {
        //  The most pairs the line keeps: three minutes of ticks.
        constexpr std::size_t mostPairs = 9000;
    }

    bool CountLine::feed (std::int64_t sample, std::int64_t nanos, int rate)
    {
        if (rate <= 0 || sample < 0)
            return false;

        if (rate != rateFed)
        {
            forget();
            rateFed = rate;
        }

        const auto at = static_cast<double> (nanos) * 1.0e-6;
        const auto count = static_cast<double> (sample);
        const auto perMillisecond = static_cast<double> (rate) / 1000.0;
        auto slipped = false;
        auto apart = false;

        /*  A SLIP OF GO.DOT'S CLOCK under the steady one - an interface that
            dropped a buffer and lost its samples, a count that paused or
            reset: the last `slipPairs` pairs, together, further from the line
            than their scatter ever puts them. A block's scatter is uniform, so
            eight pairs average to within a tenth of a block of the line, while
            one dropped buffer moves every one of them a whole block; and the
            line, fitted through two minutes of pairs, has not yet moved for
            the few that slipped. The line starts again from this pair, young
            for a second. Not while the line is young: its scatter is not known
            yet. Found by measuring (TG.6): read through a ClockEstimate, which
            follows a slip in a few pairs and rings, a fifty-millisecond slip
            wrote forty-five `running` of the wall clock in half a minute at
            speeds up to a quarter of a percent, and one dropped buffer of 512
            samples sixteen at up to six hundred parts per million; told here,
            either is one record, at the speed the clock had. */
        if (fitted.valid && fitted.residual >= 0.0)
        {
            const auto off = count - fitted.positionAt (at);

            recentOffs[recentNext] = off;
            recentAt[recentNext] = at;
            recentNext = (recentNext + 1) % recentOffs.size();
            recentCount = std::min (recentCount + 1, recentOffs.size());

            if (nanos >= quietUntil && recentCount == recentOffs.size())
            {
                auto sum = 0.0;
                auto lowest = recentOffs.front();
                auto highest = recentOffs.front();

                for (const auto value : recentOffs)
                {
                    sum += value;
                    lowest = std::min (lowest, value);
                    highest = std::max (highest, value);
                }

                const auto scatter = std::max (fitted.residual, 0.1 * perMillisecond)
                                       / std::sqrt (static_cast<double> (recentOffs.size()));

                /*  AND TOGETHER MEANS AT ONE DISTANCE: a count that slipped
                    stands a whole block or more off the line for good, its
                    pairs as close to each other as they ever were to the line -
                    a block's width, four times the line's scatter. A count that
                    is only LATE is not one: an interface's callback or the
                    hosted pump held back while the machine is loaded falls
                    further behind with every tick it does not come, then runs
                    the blocks it owes back to back and is on the line again,
                    its pairs strung out over tens or hundreds of milliseconds.
                    Started again from those, the line would stand where the
                    count was late, and again where it caught up, and every
                    moment read through it would move with it. Measured: the
                    hosted pump of a Debug engine, its machine busy on every
                    core, fell 590 ms behind for a second and a half and then
                    caught up; CodeWatchTests' count line, told that as a slip,
                    read a moment 584 ms from where the crystal put it. */
                const auto saysSlip = std::abs (sum / static_cast<double> (recentOffs.size()))
                                        > std::max (slipMilliseconds * perMillisecond, slipSigmas * scatter)
                                      && highest - lowest <= std::max (slipMilliseconds * perMillisecond,
                                                                       slipWidths * fitted.residual);

                //  AND FOR AS LONG AS THIS LINE HOLDS A SLIP BEFORE IT BELIEVES IT (`lateHoldSeconds`).
                if (! saysSlip)
                {
                    slipSince = -1;
                }
                else if (slipSince < 0)
                {
                    slipSince = nanos;
                    slipFrom = *std::min_element (recentAt.begin(), recentAt.end());
                }

                if (saysSlip && nanos - slipSince >= slipHold)
                {
                    /*  THE SLOPE KEPT ACROSS IT is the line's without the pairs
                        that slipped, which a block's slip leaves inside the
                        line's own scatter and so fitted through until the slip
                        was seen: seven pairs a block off at the newest end of
                        the window tilt it by parts per million. */
                    while (pairs.size() > 2 && pairs.back().sample >= slipFrom)
                        pairs.pop_back();

                    fitted = linefit::through (pairs);
                    restart();
                    slipped = true;
                }
            }

            /*  ONE PAIR FAR OFF THE LINE, not yet a slip, is not fitted
                through: a tick thread preempted between reading the count and
                reading the clock, an interface whose block came late. Six times
                the line's own scatter and more than three milliseconds is no
                block's noise, and a late tick's pair is not far off at all -
                the count and the moment are read together. If more follow it
                is a slip, told above from these same distances. */
            apart = ! slipped && nanos >= quietUntil
                      && (slipSince >= 0
                          || std::abs (off) > std::max (slipMilliseconds * perMillisecond, slipSigmas * fitted.residual));
        }

        //  A LINE STARTING - the first pair, or the first after a slip - is young for a second.
        if (pairs.empty())
            quietUntil = nanos + static_cast<std::int64_t> (quietSeconds * 1.0e9);

        if (apart)
            return slipped;

        pairs.push_back ({ at, count });

        while (pairs.size() > 2 && (pairs.front().sample < at - pairSeconds * 1000.0 || pairs.size() > mostPairs))
            pairs.pop_front();

        /*  Fitted at every pair while it is young, then twice a second: a
            straight line through two minutes of pairs moves by nothing worth
            the arithmetic in between, and is read off where it stands. Two
            minutes because its slope is the interface's crystal against the
            steady clock, which does not change, and every sample is read off
            it: through half a minute of 512-sample blocks it was good to ten
            parts per million, and the wall clock's speed wandered with it -
            four to fifteen `running` an hour, measured (TG.6), against two to
            five. */
        if (! fitted.valid || at - pairs.front().sample < quietSeconds * 1000.0 || at - fittedAt >= fitSeconds * 1000.0)
        {
            fitted = linefit::through (pairs);
            fittedAt = at;
        }

        return slipped;
    }

    void CountLine::restart() noexcept
    {
        /*  A SLIP MOVES THE COUNT, NOT ITS RATE: the line after it reads at
            the slope this one had, as well as it was known. */
        if (fitted.valid)
        {
            auto error = 0.0;
            keptSlope = slopeNow (error);
            keptError = error;
        }

        pairs.clear();
        fitted = {};
        fittedAt = 0.0;
        recentNext = 0;
        recentCount = 0;
        slipSince = -1;
    }

    void CountLine::forget() noexcept
    {
        restart();
        quietUntil = 0;
        keptSlope = 0.0;
        keptError = 0.0;
    }

    double CountLine::slopeNow (double& error) const noexcept
    {
        const auto prior = keptSlope > 0.0 ? keptSlope : static_cast<double> (rateFed) / 1000.0;
        const auto priorError = keptSlope > 0.0 ? keptError : prior * priorPartsPerMillion * 1.0e-6;

        //  Two pairs say nothing of their scatter, so nothing of how well they know their slope.
        if (! fitted.valid || fitted.residual < 0.0)
        {
            error = priorError;
            return prior;
        }

        /*  EACH SLOPE BY HOW WELL IT IS KNOWN - inverse variance, the line's
            from its own scatter (LineFit.h) - so the nominal stands while a
            few blocks say little, and gives way as minutes of them say more:
            a 512-sample line's slope is known to a part in a thousand after
            a second, to twenty parts per million after eighteen. */
        const auto fitError = fitted.slopeError (fitted.residual);
        const auto weights = fitError * fitError + priorError * priorError;

        if (! (weights > 0.0))
        {
            error = 0.0;
            return fitted.slope;
        }

        const auto onPrior = fitError * fitError / weights;
        error = fitError * priorError / std::sqrt (weights);
        return fitted.slope + onPrior * (prior - fitted.slope);
    }

    double CountLine::sampleAt (std::int64_t nanos) const noexcept
    {
        const auto at = static_cast<double> (nanos) * 1.0e-6;
        auto error = 0.0;
        const auto slope = slopeNow (error);

        //  One pair since the line started: through it.
        if (! fitted.valid)
            return pairs.empty() ? 0.0 : pairs.back().position + slope * (at - pairs.back().sample);

        //  THROUGH THE PAIRS' MIDDLE, where a line through them is surest whatever its slope.
        return fitted.positionAt (fitted.meanSample) + slope * (at - fitted.meanSample);
    }
}
