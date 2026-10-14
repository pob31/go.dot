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

#include <wfg/engine/timecode/ClockWatch.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::timecode
{
    namespace
    {
        constexpr std::int64_t millisecondsPerDay = 86'400'000;

        /*  THE WALL CLOCK'S ADVANCE BETWEEN TWO READINGS, THE NEAR WAY ROUND
            THE DIAL, as `cue::clockCrossings` takes it: 23:59:59.990 to
            00:00:00.010 is twenty milliseconds on, 19:30:05 to 19:30:04 a
            second back, never a day less a second. An advance of half a day or
            more reads as the other way round, and is a step either way. */
        std::int64_t nearWay (std::int64_t from, std::int64_t to) noexcept
        {
            auto advance = (to - from) % millisecondsPerDay;

            if (advance >= millisecondsPerDay / 2)
                advance -= millisecondsPerDay;
            else if (advance < -millisecondsPerDay / 2)
                advance += millisecondsPerDay;

            return advance;
        }

        double squared (double value) noexcept { return value * value; }

        /*  A reading of the millisecond `m` is a clock somewhere between `m`
            and `m + 1`: the system clock is read by truncating it. Its middle
            is the guess with no bias, and without it every map would be half
            a millisecond late. */
        constexpr double readingMiddle = 0.5;

        //  The most readings the clock's line keeps: forty seconds of ticks.
        constexpr std::size_t mostPoints = 2048;
    }

    //==========================================================================
    void ClockWatch::feedClock (std::int64_t sample, std::int64_t nanos, int rate)
    {
        if (rate <= 0 || sample < 0)
            return;

        if (rate != sampleRate)
        {
            forget();
            sampleRate = rate;
        }

        /*  THE COUNT'S LINE (CountLine.h), which says when Go.dot's clock
            slipped under the steady one: it starts again from this pair and
            readings wait a second for it, and the clock's line starts again
            from the first reading heard after (`hear`). */
        if (count.feed (sample, nanos, rate))
            slipped = true;
    }

    void ClockWatch::setLatency (std::int64_t samples) noexcept
    {
        latency = std::max<std::int64_t> (0, samples);
    }

    void ClockWatch::forget() noexcept
    {
        count.forget();
        slipped = false;
        havePrevious = false;
        previous = {};
        previousPosition = 0.0;
        pending.reset();
        current = CodeState::absent;
        window.clear();
        live = {};
        written = {};
        lastCheck = 0.0;
        longRun = 0.0;
    }

    //==========================================================================
    void ClockWatch::hear (const WallReading& reading)
    {
        if (reading.millisecondOfDay < 0 || reading.millisecondOfDay >= millisecondsPerDay
              || sampleRate <= 0 || ! count.line().valid || reading.steadyNanos < count.youngUntil())
            return;

        //  The Go.dot sample at the moment the wall clock was read, off the count's line.
        const auto sample = count.line().positionAt (static_cast<double> (reading.steadyNanos) * 1.0e-6);

        if (! havePrevious)
        {
            //  A slip before the first reading is only where the count's line started.
            slipped = false;
            havePrevious = true;
            previous = reading;
            previousPosition = static_cast<double> (reading.millisecondOfDay) + readingMiddle;
            restart ({ sample, previousPosition }, CodeTurn::jumped);
            return;
        }

        /*  HOW FAR THE WALL CLOCK WENT BEYOND THE STEADY ONE since the reading
            before: nothing but the wall clock's millisecond when time passed,
            whatever the tick did meanwhile - a slow tick is late on both. */
        const auto wall = nearWay (previous.millisecondOfDay, reading.millisecondOfDay);
        const auto steady = static_cast<double> (reading.steadyNanos - previous.steadyNanos) * 1.0e-6;
        const auto step = static_cast<double> (wall) - steady;
        const auto position = previousPosition + static_cast<double> (wall);

        previous = reading;
        previousPosition = position;

        const Point point { sample, position };

        //  A STEP: `jumped`, the line started again where it landed.
        if (std::abs (step) > stepMilliseconds)
        {
            restart (point, CodeTurn::jumped);
            return;
        }

        //  A NUDGE: the same, said as a re-anchor - fitted through, it would read as a speed.
        if (std::abs (step) > nudgeMilliseconds)
        {
            restart (point, CodeTurn::running);
            return;
        }

        /*  GO.DOT'S CLOCK MOVED UNDER A WALL CLOCK THAT DID NOT, and the count's
            line has started again on the new count (`feedClock`): the readings
            before are on another line, so this one starts again whatever the
            distance. More than `stepMilliseconds` from the line last written,
            the clock is as far from where the map puts it as if the wall clock
            had stepped, and a chaser is told the same way - `jumped`; over the
            drift a record is written at, a re-anchor; under it, nothing said. */
        if (std::exchange (slipped, false) && written.valid)
        {
            const auto off = std::abs (position - (written.position + written.slope * (sample - written.began)));

            restart (point, off > stepMilliseconds   ? std::optional<CodeTurn> (CodeTurn::jumped)
                          : off > driftMilliseconds ? std::optional<CodeTurn> (CodeTurn::running)
                                                    : std::nullopt);
            return;
        }

        window.push_back (point);
    }

    void ClockWatch::restart (Point point, std::optional<CodeTurn> turn)
    {
        window.clear();
        window.push_back (point);
        live = {};

        //  A jump not yet said stays one: a nudge after it is part of the same move.
        if (turn.has_value() && (! pending.has_value() || *turn == CodeTurn::jumped))
            pending = turn;
    }

    //==========================================================================
    void ClockWatch::trim()
    {
        if (window.empty())
            return;

        const auto horizon = window.back().sample - windowSeconds * static_cast<double> (sampleRate);

        while (window.size() > 2 && (window.front().sample < horizon || window.size() > mostPoints))
            window.pop_front();
    }

    void ClockWatch::fit()
    {
        live = linefit::through (window);
        longRun = 0.0;

        /*  HOW FAR THE LINE CAN BE TRUSTED, HONESTLY. The textbook's errors
            (LineFit.h) take the readings' scatter as independent from one to
            the next, and a millisecond's rounding is; but what the count's
            line leaves of the blocks is not - it is fitted again twice a
            second, and each fit moves the samples of every reading after it
            together. A line through such readings is less sure than their
            scatter says, and a speed weighed or snapped on the textbook's
            error wanders from one record to the next: measured with the
            ClockEstimate this watcher first read its samples through, whose
            slow loop made neighbouring readings wrong together for a fifth of
            a second, by tens of parts per million; with the count's line,
            two to seven `running` an hour without this against two to five
            with it. So the scatter is measured as it is over the long run -
            the residuals averaged a second at a time, the spread of those
            averages scaled back up - and the larger of the two is what the
            errors are counted in. */
        if (! live.valid || window.size() < 3 * batchReadings)
            return;

        auto batchSum = 0.0;
        auto batchCount = std::size_t { 0 };
        auto sumOfMeans = 0.0;
        auto sumOfSquares = 0.0;
        auto batches = std::size_t { 0 };

        for (const auto& point : window)
        {
            batchSum += point.position - live.positionAt (point.sample);

            if (++batchCount == batchReadings)
            {
                const auto mean = batchSum / static_cast<double> (batchReadings);
                sumOfMeans += mean;
                sumOfSquares += mean * mean;
                ++batches;
                batchSum = 0.0;
                batchCount = 0;
            }
        }

        const auto m = static_cast<double> (batches);
        const auto variance = (sumOfSquares - sumOfMeans * sumOfMeans / m) / (m - 1.0);

        if (variance > 0.0)
            longRun = std::sqrt (variance * static_cast<double> (batchReadings));
    }

    double ClockWatch::nominalSlope() const noexcept
    {
        return sampleRate > 0 ? 1000.0 / static_cast<double> (sampleRate) : 0.0;
    }

    double ClockWatch::sigma() const noexcept
    {
        if (live.residual < 0.0)
            return jitterAssumedMilliseconds;

        return std::max ({ live.residual, longRun, jitterFloorMilliseconds });
    }

    double ClockWatch::jitterMilliseconds() const noexcept
    {
        return live.valid && live.residual >= 0.0 ? live.residual : 0.0;
    }

    double ClockWatch::snapped (double slope, double error) const noexcept
    {
        const auto perUnit = nominalSlope();

        if (! (perUnit > 0.0))
            return slope;

        if (std::abs (slope / perUnit - 1.0) <= std::max (unitTolerance, 3.0 * error / perUnit))
            return perUnit;

        return slope;
    }

    double ClockWatch::keptSlope() const noexcept
    {
        /*  A STEP MOVES THE CLOCK, NOT ITS RATE: the speed the line had is the
            speed after it, and a first reading, which has none, runs at one -
            until ten seconds of readings say otherwise. */
        return written.valid && written.slope > 0.0 ? written.slope : nominalSlope();
    }

    //==========================================================================
    CodeSaid ClockWatch::say (CodeTurn turn, double slope)
    {
        /*  THE ANCHOR: the whole millisecond the clock is in at the newest
            reading, on the line, and the sample the line reaches it on - the
            first at or after the moment. */
        const auto& newest = window.back();
        const auto here = live.valid ? live.positionAt (newest.sample) : newest.position;
        const auto whole = std::floor (here);
        const auto began = slope > 0.0 ? newest.sample + (whole - here) / slope : newest.sample;
        const auto unit = wallClockUnit();

        written.valid = true;
        written.map.state = CodeState::running;
        written.map.unit = unit;
        written.map.frame = codemap::wrapped (static_cast<std::int64_t> (whole), unit.perDay);
        written.map.sample = static_cast<std::int64_t> (std::ceil (began - 1.0e-6));
        written.map.speed = slope / nominalSlope();
        written.map.sampleRate = sampleRate;
        written.position = whole;
        written.began = began;
        written.slope = slope;
        written.positionError = live.valid ? live.positionError (newest.sample, sigma()) : jitterAssumedMilliseconds;
        written.latency = latency;

        current = CodeState::running;

        //  HEARD the output's latency before it plays (AIQ).
        CodeSaid said;
        said.what = turn;
        said.map = written.map;
        said.map.sample -= latency;
        return said;
    }

    std::optional<CodeSaid> ClockWatch::update()
    {
        if (sampleRate <= 0 || window.empty())
            return std::nullopt;

        //  THE FIRST READING, A STEP, A NUDGE, OR GO.DOT'S CLOCK MOVED: said at once.
        if (pending.has_value())
        {
            const auto turn = *pending;
            pending.reset();
            trim();
            fit();
            return say (turn, keptSlope());
        }

        if (current != CodeState::running)
            return std::nullopt;

        //  THE INTERFACE CHANGED: the same line, its heard sample moved.
        if (latency != written.latency)
        {
            trim();
            fit();
            return say (CodeTurn::running, written.slope);
        }

        /*  ASKED TWICE A SECOND, NOT FIFTY TIMES: the clock drifts a few
            microseconds in half a second, a step is caught by its own reading
            at once, and a line through half a minute of readings is not worth
            fitting every tick. */
        const auto newest = window.back().sample;
        const auto perSecond = static_cast<double> (sampleRate);

        if (newest - lastCheck < checkSeconds * perSecond)
            return std::nullopt;

        lastCheck = newest;
        trim();
        fit();

        /*  NOT BEFORE TEN SECONDS OF READINGS since the line started: a young
            line, through readings each a millisecond's rounding and a block's
            leftover from the truth, swings by more than the drift a record is
            written at, and the anchor said at its first reading stands
            meanwhile. */
        if (! live.valid || newest - window.front().sample < settleSeconds * perSecond)
            return std::nullopt;

        /*  HOW FAR THE CLOCK IS FROM THE LINE LAST WRITTEN, at the newest
            reading, in its own milliseconds. */
        const auto here = live.positionAt (newest);
        const auto since = newest - written.began;
        const auto fromLine = here - (written.position + written.slope * since);

        /*  THE SPEED WORTH WRITING, as CodeWatch weighs it: the window's, or -
            when they agree within their errors - that weighed with the speed
            shown since the line was written, as good as the time it covers is
            long. A clock slewed fifty parts per million is learnt to a part in
            ten million within the hour. */
        auto slope = live.slope;
        auto error = live.slopeError (sigma());

        if (since > 0.0)
        {
            const auto longSlope = written.slope + fromLine / since;
            const auto longError = std::sqrt (squared (live.positionError (newest, sigma())) + squared (written.positionError))
                                     / since;

            if (std::abs (slope - longSlope) <= 3.0 * std::sqrt (squared (error) + squared (longError)))
            {
                if (! (longError > 0.0))
                {
                    slope = longSlope;
                    error = 0.0;
                }
                else if (error > 0.0)
                {
                    const auto weightFit = 1.0 / squared (error);
                    const auto weightLong = 1.0 / squared (longError);
                    slope = (slope * weightFit + longSlope * weightLong) / (weightFit + weightLong);
                    error = 1.0 / std::sqrt (weightFit + weightLong);
                }
            }
        }

        if (std::abs (fromLine) > driftMilliseconds)
            return say (CodeTurn::running, snapped (slope, error));

        return std::nullopt;
    }

    std::optional<CodeSaid> ClockWatch::resay()
    {
        if (sampleRate <= 0 || window.empty())
            return std::nullopt;

        //  Not yet said at all, or a turn still owed: that, as a jump.
        if (pending.has_value() || current != CodeState::running)
        {
            pending.reset();
            trim();
            fit();
            return say (CodeTurn::jumped, keptSlope());
        }

        /*  ITS OWN LINE, said in full: where the readings put the clock now, at
            the speed it wrote last - the best it knows. */
        trim();
        fit();
        return say (CodeTurn::jumped, written.slope);
    }
}
