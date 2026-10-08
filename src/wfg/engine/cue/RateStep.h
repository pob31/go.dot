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
    A PUSH TURNED INTO MOVEMENT (namespace draft 45, O.11, PRD §3.16's rate
    class). A SpaceMouse says how far the puck is pushed, not where a value is:
    held at half, a value moves at half its curve's `speed`, a second at a time;
    let go, it stops where it is. Each tick a curve the puck moves steps by its
    push times its speed times a fiftieth of a second, held inside its bounds.

    THE PUSH, spatcore's `AxisMapping::process` (controllers/ControllerMapping.h)
    in doubles: inverted when asked, nothing inside the dead zone, the rest
    rescaled to run from nought at its edge to one at the stop, then raised to
    the exponent. WFS-DIY's defaults - a dead zone of 0.05, an exponent of one -
    are Go.dot's.

    std only and pure, so the rule is checked against numbers.
*/

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace wfg::cue
{
    inline constexpr double puckDeadZone = 0.05;
    inline constexpr double puckExponent = 1.0;

    /*  How far a push counts, from minus one to one: `raw` is the axis as the
        device gives it, -1..1. Nought inside the dead zone. */
    inline double deflectionOf (double raw, bool inverted, double deadZone = puckDeadZone,
                                double exponent = puckExponent) noexcept
    {
        const auto v = std::clamp (inverted ? -raw : raw, -1.0, 1.0);
        const auto magnitude = std::abs (v);

        if (magnitude < deadZone || ! (deadZone < 1.0))
            return 0.0;

        const auto rescaled = (magnitude - deadZone) / (1.0 - deadZone);
        return (v < 0.0 ? -1.0 : 1.0) * std::pow (rescaled, exponent);
    }

    /*  The low and high a ridden value is held between, when it has them. */
    struct RateBounds
    {
        double low = 0.0;
        double high = 0.0;
    };

    /*  ONE STEP: the value moved by the push at `speed` units a second over
        `seconds`, and held inside `bounds` when there are some. */
    inline double rateStep (double value, double deflection, double speed, double seconds,
                            const std::optional<RateBounds>& bounds) noexcept
    {
        auto next = value + deflection * speed * seconds;

        if (bounds.has_value() && bounds->low < bounds->high)
            next = std::clamp (next, bounds->low, bounds->high);

        return next;
    }

    /*  WHICH OF THE PUCK'S SIX AXES a curve's `axis` names: tx, ty, tz the
        three pushes, rx, ry, rz the three twists - spatcore's order. -1 for
        `none` or a word that is not one of them. */
    inline int puckAxisIndex (std::string_view axis) noexcept
    {
        constexpr std::string_view names[] { "tx", "ty", "tz", "rx", "ry", "rz" };

        for (int at = 0; at < 6; ++at)
            if (axis == names[at])
                return at;

        return -1;
    }
}
