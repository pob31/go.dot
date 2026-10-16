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

#include <wfg/engine/binding/Scaling.h>

#include <wfg/engine/surface/FaderCurve.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wfg::binding
{
    namespace
    {
        constexpr auto unbounded = std::numeric_limits<double>::infinity();

        double lowOf (const TargetRange& range) noexcept  { return range.hasMinimum ? range.minimum : -unbounded; }
        double highOf (const TargetRange& range) noexcept { return range.hasMaximum ? range.maximum : unbounded; }

        bool hasBothEnds (const TargetRange& range) noexcept
        {
            return range.hasMinimum && range.hasMaximum && range.maximum > range.minimum;
        }

        /*  THE TARGET'S OWN LAW, as `dialTurned` reads it off the row: a level
            (decibels down to silence) along the fader's travel, a frequency
            and a hundredfold width in ratios, anything else in a line. */
        enum class Law { line, level, ratio };

        Law lawOf (const TargetRange& range) noexcept
        {
            const auto low = lowOf (range);
            const auto high = highOf (range);

            if (range.unit == "dB" && low <= surface::faderSilenceDb)
                return Law::level;

            if ((range.unit == "Hz" && low > 0.0) || (low > 0.0 && high < unbounded && high / low >= 50.0))
                return Law::ratio;

            return Law::line;
        }

        double clampedTo (double value, const TargetRange& range) noexcept
        {
            return std::clamp (value, lowOf (range), highOf (range));
        }

        double rounded (double value, const TargetRange& range) noexcept
        {
            return range.integer ? std::round (value) : value;
        }
    }

    double travelOf (const Scale& scale, double input) noexcept
    {
        const auto span = scale.inHigh - scale.inLow;

        if (std::abs (span) < 1.0e-12 || ! std::isfinite (input))
            return 0.0;

        const auto travel = std::clamp ((input - scale.inLow) / span, 0.0, 1.0);
        return scale.invert ? 1.0 - travel : travel;
    }

    double inputFor (const Scale& scale, double travel) noexcept
    {
        const auto t = std::clamp (scale.invert ? 1.0 - travel : travel, 0.0, 1.0);
        return scale.inLow + t * (scale.inHigh - scale.inLow);
    }

    double absoluteValue (const Scale& scale, double travel, const TargetRange& range) noexcept
    {
        const auto t = std::clamp (travel, 0.0, 1.0);

        if (! scale.ownRange || ! hasBothEnds (range))
            return clampedTo (rounded (scale.outLow + t * (scale.outHigh - scale.outLow), range), range);

        const auto low = range.minimum;
        const auto high = range.maximum;

        switch (lawOf (range))
        {
            case Law::level:
                return clampedTo (surface::dbForFraction (t), range);

            case Law::ratio:
                return clampedTo (low * std::pow (high / low, t), range);

            case Law::line:
                break;
        }

        return clampedTo (rounded (low + t * (high - low), range), range);
    }

    double travelFor (const Scale& scale, double value, const TargetRange& range) noexcept
    {
        if (! scale.ownRange || ! hasBothEnds (range))
        {
            const auto span = scale.outHigh - scale.outLow;
            return std::abs (span) < 1.0e-12 ? 0.0 : std::clamp ((value - scale.outLow) / span, 0.0, 1.0);
        }

        const auto low = range.minimum;
        const auto high = range.maximum;

        switch (lawOf (range))
        {
            case Law::level:
                return std::clamp (surface::fractionForDb (value), 0.0, 1.0);

            case Law::ratio:
                return value <= 0.0 ? 0.0 : std::clamp (std::log (value / low) / std::log (high / low), 0.0, 1.0);

            case Law::line:
                break;
        }

        return std::clamp ((value - low) / (high - low), 0.0, 1.0);
    }

    double relativeValue (const Scale& scale, double current, int steps, const TargetRange& range) noexcept
    {
        return surface::dialTurned (range, current, scale.invert ? -steps : steps, surface::FaderLaw::generic);
    }

    int midiSteps (int value) noexcept
    {
        if (value >= 1 && value <= 63)
            return value;

        if (value >= 65 && value <= 127)
            return -(value - 64);

        return 0;
    }

    double deflectionOf (const Scale& scale, double input) noexcept
    {
        auto deflection = travelOf (scale, input) * 2.0 - 1.0;
        const auto zone = std::clamp (scale.deadZone, 0.0, 0.5);

        if (std::abs (deflection) <= zone)
            return 0.0;

        const auto past = (std::abs (deflection) - zone) / (1.0 - zone);
        deflection = deflection < 0.0 ? -past : past;
        return std::clamp (deflection, -1.0, 1.0);
    }

    double rateValue (const Scale& scale, double current, double deflection, double seconds,
                      const TargetRange& range) noexcept
    {
        if (std::abs (deflection) < 1.0e-12 || seconds <= 0.0)
            return current;

        const auto span = (scale.ownRange && hasBothEnds (range)) ? range.maximum - range.minimum
                                                                   : std::abs (scale.outHigh - scale.outLow);

        const auto moved = current + deflection * scale.speed * span * seconds;
        return clampedTo (rounded (moved, range), range);
    }

    bool pickUp (Pickup& pickup, double wanted, double current, double tolerance) noexcept
    {
        if (pickup.caught)
        {
            pickup.lastWanted = wanted;
            return true;
        }

        const auto near = std::abs (wanted - current) <= tolerance;
        const auto crossed = pickup.lastWanted.has_value()
                               && ((*pickup.lastWanted <= current && wanted >= current)
                                     || (*pickup.lastWanted >= current && wanted <= current));

        pickup.lastWanted = wanted;
        pickup.caught = near || crossed;
        return pickup.caught;
    }
}
