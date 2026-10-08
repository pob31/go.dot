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

#include <wfg/engine/surface/SurfacePages.h>

#include <wfg/engine/surface/SurfaceProfile.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace wfg::surface
{
    namespace
    {
        /*  A NUMBER WITH `decimals` PLACES, digit by digit: the locale's
            point is never asked, so "2.00" is "2.00" under fr_FR as well. */
        void fixed (double value, int decimals, std::string& out)
        {
            auto scale = 1LL;

            for (int i = 0; i < decimals; ++i)
                scale *= 10;

            const auto scaled = std::llround (std::abs (value) * static_cast<double> (scale));

            if (value < 0.0 && scaled != 0)
                out.push_back ('-');

            std::array<char, 24> digits {};
            const auto whole = std::to_chars (digits.data(), digits.data() + digits.size(), scaled / scale);
            out.append (digits.data(), whole.ptr);

            if (decimals <= 0)
                return;

            out.push_back ('.');

            auto fraction = scaled % scale;

            for (auto place = scale / 10; place > 0; place /= 10)
            {
                out.push_back (static_cast<char> ('0' + static_cast<int> (fraction / place)));
                fraction %= place;
            }
        }

        /*  A VALUE ON A STEP'S GRID, and for a step under one through its
            whole reciprocal (namespace draft §47, AAB). The author, 2026-10-09:
            "at times the values have a long trailing number". Rounding to a
            tenth as `round (v / 0.1) * 0.1` gives three times the double
            nearest a tenth, 0.30000000000000004, which the window prints in
            full because it prints the shortest text that reads back exactly;
            `round (v * 10) / 10` gives the double nearest 0.3, which prints as
            "0.3". A step that is not a whole fraction keeps the old reading. */
        double roundedTo (double value, double step) noexcept
        {
            if (step > 0.0 && step < 1.0)
            {
                const auto per = std::round (1.0 / step);

                if (per > 0.0 && std::abs (per * step - 1.0) < 1.0e-9)
                    return std::round (value * per) / per;
            }

            return std::round (value / step) * step;
        }

        /*  THE GRAIN A DIAL TURN IS KEPT TO, for a law that moves by `step` a
            detent with no grid of its own: the power of ten at or below the
            step, and never finer than a whole number once the step is one or
            more - so a 0..100 % row (0.78 a detent) keeps a tenth, a hue (2.8
            degrees) and a position (15.6 %) whole numbers. */
        double grainFor (double step) noexcept
        {
            if (! (step > 0.0) || step >= 1.0)
                return 1.0;

            return std::pow (10.0, std::floor (std::log10 (step)));
        }

        /*  ON THE GRAIN, and moved by at least one of it the way the turn
            went: a grain coarser than a step would otherwise round a detent
            away, and a hand turning a dial that does not move would think it
            broken. */
        double keptToGrain (double value, double moved, int steps, double grain) noexcept
        {
            auto next = roundedTo (moved, grain);

            if (steps != 0 && std::abs (next - value) < grain * 0.5)
                next = roundedTo (value + (steps > 0 ? grain : -grain), grain);

            return next;
        }

        /*  A SPEED TURNED A SEMITONE A DETENT (namespace draft §22.7, EF),
            along the equal-tempered grid, with nought below its lowest step: a
            turn down past that step lands on nought, a turn up from nought
            lands on it. A speed typed off the grid - or kept to four figures,
            which is a hair off it - joins the grid at the first detent, on the
            step the turn meets first, so no detent is ever lost to a rounding.

            AND ON BELOW NOUGHT (namespace draft §41, WX): the grid mirrored -
            a detent down from nought is the lowest step backwards, and each
            after it a semitone faster backwards - wherever the row's range goes
            below nought. Read as rungs: nought is rung nought, the lowest step
            one, each semitone one more, backwards the same rungs below nought;
            a speed between nought and the lowest step stands on half a rung. */
        double speedTurned (double value, int steps, double minimum, double maximum) noexcept
        {
            const auto lowest = static_cast<double> (dialLowestSemitone);
            const auto size = std::abs (value);
            const auto magnitude = size > 0.0 ? std::max (0.5, 12.0 * std::log2 (size) - lowest + 1.0) : 0.0;
            const auto at = value < 0.0 ? -magnitude : magnitude;
            const auto nearest = std::round (at);

            auto rung = nearest + static_cast<double> (steps);

            if (std::abs (at - nearest) > 0.02)
                rung = (steps > 0 ? std::ceil (at) : std::floor (at))
                         + static_cast<double> (steps > 0 ? steps - 1 : steps + 1);

            if (std::abs (rung) < 0.5)
                return std::clamp (0.0, minimum, maximum);

            const auto speed = std::exp2 ((std::abs (rung) - 1.0 + lowest) / 12.0);
            const auto scale = std::pow (10.0, static_cast<double> (dialSpeedFigures - 1)
                                                 - std::floor (std::log10 (speed)));

            return std::clamp (std::copysign (std::round (speed * scale) / scale, rung), minimum, maximum);
        }

        /*  Where a value stands between its ends, nought to one: evenly in
            ratio for a frequency and a width, evenly in decibels for a gain,
            along the fader for a level. */
        double fractionOf (Law law, double value, double minimum, double maximum, FaderLaw fader) noexcept
        {
            switch (law)
            {
                case Law::frequency:
                case Law::width:
                    if (! (minimum > 0.0) || ! (maximum > minimum) || ! (value > 0.0))
                        return 0.0;

                    return std::clamp (std::log (value / minimum) / std::log (maximum / minimum), 0.0, 1.0);

                case Law::gain:
                    if (! (maximum > minimum))
                        return 0.5;

                    return std::clamp ((value - minimum) / (maximum - minimum), 0.0, 1.0);

                case Law::level:
                    return std::clamp (fractionForDb (value, fader), 0.0, 1.0);

                case Law::shape:
                    break;
            }

            return 0.0;
        }
    }

    //==========================================================================
    std::string_view pageWord (Page page) noexcept
    {
        switch (page)
        {
            case Page::show: return "show";
            case Page::eq:   return "eq";
            case Page::send: return "send";
            case Page::fx:   return "fx";
            case Page::loop: return "loop";
        }

        return "show";
    }

    int pageCount (int controls, int rotaries) noexcept
    {
        if (controls <= 0 || rotaries <= 0)
            return 1;

        return std::max (1, (controls + rotaries - 1) / rotaries);
    }

    //==========================================================================
    double turned (Law law, double value, int steps, double minimum, double maximum,
                   FaderLaw fader) noexcept
    {
        const auto detents = static_cast<double> (steps);

        switch (law)
        {
            case Law::frequency:
            {
                const auto moved = std::clamp (value * std::exp2 (detents * pageOctavesPerDetent),
                                               minimum, maximum);

                //  A tenth of a hertz is heard in the bass; above a kilohertz, a hertz is plenty.
                return std::clamp (roundedTo (moved, moved < 1000.0 ? 0.1 : 1.0), minimum, maximum);
            }

            case Law::gain:
                return std::clamp (roundedTo (value + detents * pageGainStepDb, 0.1), minimum, maximum);

            case Law::width:
            {
                const auto moved = std::clamp (value * std::exp2 (detents * pageWidthDoublingsPerDetent),
                                               minimum, maximum);
                return std::clamp (roundedTo (moved, 0.01), minimum, maximum);
            }

            case Law::level:
            {
                /*  ALONG THE FADER, so a detent near nought is a fine step and
                    one near the bottom a coarse one, as a fader's is. Back at
                    the bottom of the travel is silence, spelled as the table
                    spells it. */
                const auto at = std::clamp (fractionForDb (value, fader) + detents * pageLevelTravelPerDetent,
                                            0.0, 1.0);

                if (! (at > 0.0))
                    return std::clamp (faderSilenceDb, minimum, maximum);

                return std::clamp (roundedTo (dbForFraction (at, fader), 0.1), minimum, maximum);
            }

            case Law::shape:
                break;
        }

        return value;
    }

    std::string_view turnedShape (std::string_view current, int steps, std::string_view shelf) noexcept
    {
        if (steps > 0)
            return shelf;

        if (steps < 0)
            return "peak";

        return current;
    }

    std::string_view pressedShape (std::string_view current, std::string_view shelf) noexcept
    {
        return current == "peak" || current.empty() ? shelf : std::string_view ("peak");
    }

    //==========================================================================
    Ring d700RingFor (Law law, double value, double minimum, double maximum, FaderLaw fader) noexcept
    {
        const auto fraction = fractionOf (law, value, minimum, maximum, fader);

        if (law == Law::gain)
            return { static_cast<int> (std::lround (fraction * 127.0)), 1 };

        /*  A LEVEL AT SILENCE IS AN EMPTY RING, and a frequency at the bottom
            of its range is not silence: it keeps one step lit. */
        if (law == Law::level)
            return { static_cast<int> (std::lround (fraction * 127.0)), 2 };

        return { 1 + static_cast<int> (std::lround (fraction * 126.0)), 2 };
    }

    Ring mcuRingFor (Law law, double value, double minimum, double maximum, FaderLaw fader) noexcept
    {
        const auto fraction = fractionOf (law, value, minimum, maximum, fader);

        //  Mackie's boost/cut: position six is the middle, one and eleven the ends.
        if (law == Law::gain)
            return { 1 + static_cast<int> (std::lround (fraction * 10.0)), 1 };

        if (law == Law::level)
            return { static_cast<int> (std::lround (fraction * 11.0)), 2 };

        return { 1 + static_cast<int> (std::lround (fraction * 10.0)), 2 };
    }

    double turnedParameter (double value, int steps, int parameterSteps) noexcept
    {
        const auto at = std::clamp (value, 0.0, 1.0);

        if (parameterSteps >= 2)
        {
            const auto last = static_cast<double> (parameterSteps - 1);
            const auto step = std::clamp (std::lround (at * last) + static_cast<long> (steps), 0L,
                                          static_cast<long> (parameterSteps - 1));
            return static_cast<double> (step) / last;
        }

        return std::clamp (at + static_cast<double> (steps) * pageParameterTravelPerDetent, 0.0, 1.0);
    }

    double dialTurned (const DialRange& range, double value, int steps, FaderLaw fader) noexcept
    {
        constexpr auto unbounded = std::numeric_limits<double>::infinity();

        const auto low = range.hasMinimum ? range.minimum : -unbounded;
        const auto high = range.hasMaximum ? range.maximum : unbounded;
        const auto detents = static_cast<double> (steps);

        if (steps == 0)
            return std::clamp (value, low, high);

        if (range.integer)
            return std::clamp (std::round (value) + detents, low, high);

        /*  A FREQUENCY, where its floor is above nought: a ratio a detent,
            as a band's rotary turns it. The table's ceiling, or none. */
        if (range.unit == "Hz" && low > 0.0)
            return turned (Law::frequency, std::max (value, low), steps, low, high, fader);

        /*  A DECIBEL READS TWO WAYS, and the floor says which: a row that
            reaches silence is a LEVEL and moves along the fader, fine near
            nought and coarse near the bottom; a narrower one is a GAIN,
            boost and cut, half a decibel a detent. */
        if (range.unit == "dB")
            return turned (low <= faderSilenceDb ? Law::level : Law::gain, value, steps, low, high, fader);

        if (range.unit == "s")
        {
            const auto upwards = steps > 0;
            auto at = std::max (value, 0.0);

            //  Detent by detent, so a turn across ten seconds changes its step there.
            for (auto left = std::abs (steps); left > 0; --left)
            {
                const auto coarse = upwards ? at >= dialCoarseFromSeconds - 1.0e-9
                                            : at > dialCoarseFromSeconds + 1.0e-9;
                const auto step = coarse ? dialCoarseSeconds : dialFineSeconds;
                at = roundedTo (at + (upwards ? step : -step), step);
            }

            return std::clamp (std::max (at, 0.0), low, high);
        }

        /*  A SPEED, the unit `x`: a semitone a detent (EF). Its range starts
            at nought, which no law in ratios can reach and the linear one
            below would cross the musical range of in six detents. */
        if (range.unit == "x")
            return speedTurned (value, steps, low, high);

        /*  A WIDTH, or anything else that spans a hundredfold with no unit: in
            ratios, as a band's Q turns. */
        if (low > 0.0 && high / low >= 50.0 && high < unbounded)
            return turned (Law::width, std::max (value, low), steps, low, high, fader);

        /*  Anything else with two ends: a hundred-and-twenty-eighth of its
            travel, kept to the grain that step reads at (AAB). With none: one
            a detent, kept to a thousandth, so a number typed with a long tail
            loses it at the first detent. */
        if (low > -unbounded && high < unbounded && high > low)
        {
            const auto step = (high - low) * pageParameterTravelPerDetent;
            return std::clamp (keptToGrain (value, value + detents * step, steps, grainFor (step)), low, high);
        }

        return std::clamp (keptToGrain (value, value + detents, steps, 0.001), low, high);
    }

    Ring d700ParameterRing (double value, bool bipolar) noexcept
    {
        const auto fraction = std::clamp (value, 0.0, 1.0);

        if (bipolar)
            return { static_cast<int> (std::lround (fraction * 127.0)), 1 };

        return { static_cast<int> (std::lround (fraction * 127.0)), 2 };
    }

    Ring mcuParameterRing (double value, bool bipolar) noexcept
    {
        const auto fraction = std::clamp (value, 0.0, 1.0);

        if (bipolar)
            return { 1 + static_cast<int> (std::lround (fraction * 10.0)), 1 };

        return { static_cast<int> (std::lround (fraction * 11.0)), 2 };
    }

    double loopPointTurned (double seconds, int steps) noexcept
    {
        const auto per = std::abs (steps) >= loopCoarseFromDetents ? loopCoarseSeconds : loopFineSeconds;
        const auto moved = std::max (0.0, seconds + static_cast<double> (steps) * per);

        return std::round (moved * 1000.0) / 1000.0;
    }

    void secondsText (double seconds, bool compact, std::string& out)
    {
        const auto milliseconds = std::llround (std::max (0.0, seconds) * 1000.0);
        const auto rest = milliseconds % 1000;

        out.append (std::to_string (milliseconds / 1000));
        out.push_back ('.');
        out.push_back (static_cast<char> ('0' + rest / 100));
        out.push_back (static_cast<char> ('0' + rest / 10 % 10));
        out.push_back (static_cast<char> ('0' + rest % 10));
        out.append (compact ? "s" : " s");
    }

    Ring d700PositionRing (double seconds, double length) noexcept
    {
        const auto fraction = length > 0.0 ? std::clamp (seconds / length, 0.0, 1.0) : 0.0;
        return { static_cast<int> (std::lround (fraction * 127.0)), 2 };
    }

    Ring mcuPositionRing (double seconds, double length) noexcept
    {
        const auto fraction = length > 0.0 ? std::clamp (seconds / length, 0.0, 1.0) : 0.0;
        return { static_cast<int> (std::lround (fraction * 11.0)), 2 };
    }

    Ring d700ShapeRing (std::string_view shape) noexcept
    {
        return { shape == "peak" || shape.empty() ? 0 : 127, 2 };
    }

    Ring mcuShapeRing (std::string_view shape) noexcept
    {
        return { shape == "peak" || shape.empty() ? 0 : 11, 2 };
    }

    //==========================================================================
    void valueText (Law law, double value, bool compact, std::string& out)
    {
        out.clear();
        const auto space = compact ? "" : " ";

        switch (law)
        {
            case Law::frequency:
            {
                if (value >= 999.95)
                {
                    const auto kilohertz = value / 1000.0;
                    fixed (kilohertz, kilohertz >= 9.995 ? 1 : 2, out);
                    out.append (space);
                    out.append ("kHz");
                    return;
                }

                //  Below a hundred the tenth shows when there is one; above, the hertz.
                const auto tenths = std::llround (value * 10.0);
                fixed (value, value < 99.95 && tenths % 10 != 0 ? 1 : 0, out);
                out.append (space);
                out.append ("Hz");
                return;
            }

            case Law::gain:
                if (std::llround (value * 10.0) > 0)
                    out.push_back ('+');

                fixed (value, 1, out);
                out.append (space);
                out.append ("dB");
                return;

            case Law::width:
                out.append ("Q ");
                fixed (value, value >= 9.995 ? 1 : 2, out);
                return;

            case Law::level:
                if (value <= faderSilenceDb)
                {
                    out.append (compact ? "-inf" : "-inf dB");
                    return;
                }

                fixed (value, 1, out);
                out.append (space);
                out.append ("dB");
                return;

            case Law::shape:
                break;
        }
    }

    std::string_view shapeText (std::string_view shape, bool compact) noexcept
    {
        if (shape == "lowShelf")    return compact ? "LoShelf" : "Lo shelf";
        if (shape == "highShelf")   return compact ? "HiShelf" : "Hi shelf";

        return "Peak";
    }

    //==========================================================================
    bool pageButtonLit (int index, int count, std::int64_t tick) noexcept
    {
        if (count <= 1)
            return true;

        const auto blinks = static_cast<std::int64_t> (std::clamp (index + 1, 1, 7));
        const auto period = pageBlinkOnTicks + pageBlinkOffTicks;
        const auto phase = ((tick % pageBlinkCycleTicks) + pageBlinkCycleTicks) % pageBlinkCycleTicks;

        if (phase >= blinks * period)
            return false;

        return phase % period < pageBlinkOnTicks;
    }
}
