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

#include <wfg/client/model/Scrub.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace wfg::client::model
{
    void Scrub::begin (const Setup& setup)
    {
        live = true;
        at = setup.position;
        length = setup.length;
        secondsPerPixel = setup.secondsPerPixel;
        unit = setup.unit > 0.0 ? setup.unit : 40.0;
        lastX = setup.x;
        lastRate = 1.0;

        /*  Where the hand took it counts as sent: a grab is not a seek, and
            nothing goes out until the head has moved. */
        sentAt = at;
        sentMs = -1.0e9;
    }

    void Scrub::end()
    {
        live = false;
    }

    double Scrub::rateFor (double distance, double unit) noexcept
    {
        if (! (distance > 0.0) || ! (unit > 0.0))
            return 1.0;

        /*  HALVED PER UNIT, CONTINUOUSLY. A step function would make the
            head jump at each threshold as the hand drifts up; a continuous
            one makes the same drift a smooth change of gearing. Floored so a
            hand at the top of a tall window still moves something. */
        return std::max (1.0 / 256.0, std::pow (0.5, distance / unit));
    }

    double Scrub::clamp (double seconds) const noexcept
    {
        seconds = std::max (0.0, seconds);

        return length > 0.0 ? std::min (seconds, length) : seconds;
    }

    double Scrub::moveTo (double x, double distance)
    {
        if (! live)
            return at;

        lastRate = rateFor (distance, unit);
        at = clamp (at + (x - lastX) * secondsPerPixel * lastRate);
        lastX = x;
        return at;
    }

    double Scrub::push (int direction, double seconds, double distance)
    {
        if (! live || direction == 0 || ! (seconds > 0.0))
            return at;

        lastRate = rateFor (distance, unit);
        at = clamp (at + static_cast<double> (direction) * pushPixelsPerSecond * seconds
                           * secondsPerPixel * lastRate);
        return at;
    }

    bool Scrub::unsent() const noexcept
    {
        /*  Moved since the last send, by more than a rounding: a head that
            came back to where it was sent from has nothing new to say. */
        return std::abs (at - sentAt) > 1.0e-9;
    }

    bool Scrub::due (double nowMs, double intervalMs)
    {
        if (! live || ! unsent() || nowMs - sentMs < intervalMs)
            return false;

        sentMs = nowMs;
        sentAt = at;
        return true;
    }

    bool Scrub::settle()
    {
        if (! live)
            return false;

        const auto moved = unsent();
        sentAt = at;
        return moved;
    }

    //==============================================================================
    std::string clockText (double seconds)
    {
        seconds = std::max (0.0, seconds);

        const auto whole = static_cast<long long> (seconds);
        const auto tenths = static_cast<int> ((seconds - static_cast<double> (whole)) * 10.0);
        const auto hours = whole / 3600;
        const auto minutes = (whole / 60) % 60;
        const auto secs = whole % 60;

        char out[32] = {};

        if (hours > 0)
            std::snprintf (out, sizeof (out), "%lld:%02lld:%02lld.%d", hours, minutes, secs, tenths);
        else
            std::snprintf (out, sizeof (out), "%lld:%02lld.%d", minutes, secs, tenths);

        return out;
    }

    std::string rateText (double rate)
    {
        if (! (rate < 1.0))
            return {};

        const auto denominator = static_cast<long long> (std::llround (1.0 / rate));
        return "1/" + std::to_string (denominator);
    }
}
