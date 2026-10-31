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

#include <wfg/client/model/FingerDrag.h>

#include <wfg/client/model/Scrub.h>

#include <algorithm>

namespace wfg::client::model
{
    double outsideBand (double at, double low, double high) noexcept
    {
        if (at < low)
            return low - at;

        if (at > high)
            return at - high;

        return 0.0;
    }

    FingerDrag::FingerDrag (double startPlace, double startUp) noexcept
        : at (std::clamp (startPlace, 0.0, 1.0)), lastUp (startUp)
    {
    }

    double FingerDrag::moveTo (double up, double off, double length, double unit) noexcept
    {
        lastRate = Scrub::rateFor (off, unit);

        if (length > 0.0)
            at = std::clamp (at + (up - lastUp) / length * lastRate, 0.0, 1.0);

        lastUp = up;
        return at;
    }
}
