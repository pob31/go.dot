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
    AN OSC CUE'S CURVES, PLAYING (namespace draft 45, O.4).

    A cue whose messages carry curves runs on a clock of its own: seconds from
    the moment it began sending, counted in ticks so a replay lands on the
    same ones (YW). Each tick the Runner asks each message what its values are
    at the clock's second and writes those that changed (YX) - through the
    same door and queue as any write to a device, so the device's rate cap
    thins it, `tx` off sends nothing and a bundle gathers a source's x, y and z.
    At the end of the duration the last values are written and the run is done
    by its wait; with `loop` the clock goes round instead, until somebody stops
    it.

    A VALUE THE RUNNER HOLDS AND ADVANCES, like an OscJob: it owns nothing and
    touches nothing. The writes are the hook's, never a handler's - they are an
    output of the document and the clock, recomputed on the night and not
    logged, as a fade's level is; the run's start and end are records like any
    other run's.

    The arithmetic below is std-only and vendor-free, so the rule for what a
    message says at a second is checked against numbers rather than a device.
*/

#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::cue
{
    /*  One curve on one value of a message: which value, and its points. */
    struct CurveLane
    {
        std::size_t arg = 0;
        std::vector<doc::LanePoint> points;
    };

    /*  One message of the cue - its own, or a further one - with the curves on
        its values. A message with none is part of the cue all the same: it was
        sent at GO, and it is asked about when the cue is verified. */
    struct CurveTarget
    {
        std::string address;

        /** The message's written values, which a value with no curve keeps. */
        osc::Values base;

        std::vector<CurveLane> lanes;

        /*  What was last asked of it and what the device's door made of that:
            the first is what the next tick compares against, so a value the
            door coerces is not sent again every tick; the second is what a
            verify expects back. */
        osc::Values last;
        osc::Values written;

        /** The ticket of the last write, and the tick it was queued on. */
        std::uint64_t ticket = 0;
        std::int64_t ticketTick = -1;
    };

    struct CurveJob
    {
        /** The run of the OSC cue, and the cue. */
        std::string self;
        std::string cue;

        /*  THE CLOCK: the second it read at `originTick`, one second for every
            fifty ticks after. A seek moves both, which is all a seek is. */
        std::int64_t originTick = 0;
        double originSeconds = 0.0;

        /*  How long the curves play - the cue's `duration`, or the last point
            of the longest curve when that is nought - and whether the clock
            goes round at its end. */
        double duration = 0.0;
        bool loop = false;

        std::vector<CurveTarget> targets;

        /** The document revision the curves were read at. */
        std::uint64_t revision = 0;

        bool finished = false;

        /** The clock's second at a tick, before any loop is taken. */
        double secondsAt (std::int64_t tick) const noexcept
        {
            return originSeconds + static_cast<double> (tick - originTick) / 50.0;
        }
    };

    /*  WHERE IN THE CURVES A SECOND OF THE CLOCK FALLS (YW): round and round
        when the cue loops, its duration's end when it does not and the clock
        has passed it - `ended` then says so. `iteration` counts from one. */
    struct CurvePlace
    {
        double seconds = 0.0;
        bool ended = false;
        int iteration = 1;
    };

    inline CurvePlace placeOnCurves (double clock, double duration, bool loop) noexcept
    {
        CurvePlace place;

        if (! (duration > 0.0))
        {
            place.ended = true;
            return place;
        }

        if (loop)
        {
            const auto rounds = std::floor (clock / duration);
            place.seconds = clock - rounds * duration;
            place.iteration = static_cast<int> (rounds) + 1;
            return place;
        }

        if (! (clock < duration))
        {
            place.seconds = duration;
            place.ended = true;
            return place;
        }

        place.seconds = clock < 0.0 ? 0.0 : clock;
        return place;
    }

    /*  WHAT A MESSAGE SAYS AT A SECOND OF ITS CURVES: its written values, each
        one a curve moves replaced by the curve's value there - kept to the
        written value's type, an integer rounded to the nearest (YR: the value
        itself, in its own units). A curve with no point leaves its value as
        written. */
    inline osc::Values valuesAt (const CurveTarget& target, double seconds)
    {
        auto values = target.base;

        for (const auto& lane : target.lanes)
        {
            if (lane.points.empty() || lane.arg >= values.size())
                continue;

            const auto value = doc::laneValueAt (lane.points, seconds);
            auto& slot = values[lane.arg];

            if (slot.isInt32())
                slot = osc::Value::int32 (static_cast<std::int32_t> (std::lround (value)));
            else if (slot.isInt64())
                slot = osc::Value::int64 (static_cast<std::int64_t> (std::llround (value)));
            else if (slot.isFloat32())
                slot = osc::Value::float32 (static_cast<float> (value));
            else if (slot.isFloat64())
                slot = osc::Value::float64 (value);
        }

        return values;
    }

    /*  THE LONGEST CURVE'S LAST SECOND: what a `duration` of nought means. */
    inline double lastPointOf (const std::vector<CurveTarget>& targets) noexcept
    {
        double last = 0.0;

        for (const auto& target : targets)
            for (const auto& lane : target.lanes)
                if (! lane.points.empty() && lane.points.back().seconds > last)
                    last = lane.points.back().seconds;

        return last;
    }

    /*  Whether any message carries a curve with a point: a cue whose curves
        are all empty is a cue of written values, sent once. */
    inline bool hasCurves (const std::vector<CurveTarget>& targets) noexcept
    {
        for (const auto& target : targets)
            for (const auto& lane : target.lanes)
                if (! lane.points.empty())
                    return true;

        return false;
    }
}
