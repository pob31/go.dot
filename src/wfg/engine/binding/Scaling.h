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
    HOW A CONTROL'S MOVEMENT BECOMES A NUMBER (namespace draft §74, AQX, AQZ).

    Three ways a control moves (PRD §3.16) and three laws:

    ABSOLUTE - a fader, a knob: where it is, read as a fraction of its travel
    between `inLow` and `inHigh`, is where the target is. Over the target's own
    range by the target's own law - a level along a fader's travel, a frequency
    in ratios, the law the master dial turns by (SurfacePages.h) - or in a
    straight line between `outLow` and `outHigh`. A control with no motor
    PICKS the value up (AQZ): it moves nothing until it crosses where the value
    is, or lands within a step of it, so a knob left anywhere does not make a
    level jump.

    RELATIVE - an endless encoder: each step turns the target as one detent of
    the master dial turns it.

    RATE - a pedal, a joystick: how far it is pushed from its middle is how fast
    the target moves, `speed` of its range a second at a full push; nothing
    inside the dead zone; let go in the middle, it holds where it is.

    Pure: every function takes what it needs and answers, so the laws are
    tested with no port, no clock and no tree.
*/

#include <wfg/engine/surface/SurfacePages.h>

#include <optional>
#include <string>
#include <string_view>

namespace wfg::binding
{
    /*  WHAT THE TARGET IS: its type, its range and its unit, read off its node
        - the master dial's `DialRange` and the same reading. */
    using TargetRange = surface::DialRange;

    /*  THE SCALING A BINDING CARRIES, its rows as numbers. */
    struct Scale
    {
        double inLow = 0.0;
        double inHigh = 1.0;
        bool ownRange = true;
        double outLow = 0.0;
        double outHigh = 1.0;
        bool invert = false;
        double speed = 0.5;
        double deadZone = 0.1;
    };

    /*  WHERE ON ITS TRAVEL THE CONTROL IS, nought to one, from what it sent:
        `inLow` is nought and `inHigh` one, clamped, and turned over when the
        binding is inverted. A travel of no length is nought. */
    double travelOf (const Scale& scale, double input) noexcept;

    /*  ABSOLUTE: the target's value for a place on the control's travel (from
        `travelOf`). Its own range by its own law when it has one and the
        binding asks for it, else the straight line from outLow to outHigh;
        a whole number rounded; always within the target's range. */
    double absoluteValue (const Scale& scale, double travel, const TargetRange& range) noexcept;

    /*  AND BACK, for feedback (ARC): where on the control's travel the target's
        value puts it, nought to one - before `inLow`, `inHigh` and invert are
        undone by `inputFor`. */
    double travelFor (const Scale& scale, double value, const TargetRange& range) noexcept;

    /*  THE CONTROL'S OWN VALUE for a place on its travel: `travelOf` undone. */
    double inputFor (const Scale& scale, double travel) noexcept;

    /*  RELATIVE: the target turned `steps` detents of the master dial, the
        other way when inverted. */
    double relativeValue (const Scale& scale, double current, int steps, const TargetRange& range) noexcept;

    /*  A MIDI RELATIVE VALUE AS STEPS: 1 to 63 up by that many, 65 to 127 down
        by the value less 64; nought and 64 no step. */
    int midiSteps (int value) noexcept;

    /*  RATE: how far the control is pushed from its middle, minus one to one,
        with the dead zone taken out and what is left spread back over the
        travel - so a push just past the dead zone is a crawl, not a jump. The
        middle of the travel between inLow and inHigh is rest: a pedal resting
        at nought wants `inLow` at minus its full push. */
    double deflectionOf (const Scale& scale, double input) noexcept;

    /*  AND HOW FAR THE TARGET MOVES in `seconds` at that deflection: `speed` of
        its span a second at a full push, its span being its own range or the
        line from outLow to outHigh. The new value, within the range. */
    double rateValue (const Scale& scale, double current, double deflection, double seconds,
                      const TargetRange& range) noexcept;

    /*  PICKUP (AQZ), one binding's: what the control asked for last time, and
        whether it has caught the value. */
    struct Pickup
    {
        std::optional<double> lastWanted;
        bool caught = false;

        void reset() noexcept { lastWanted.reset(); caught = false; }
    };

    /*  WHETHER A CONTROL'S NEW WANT MAY BE WRITTEN: caught already; or it lands
        within `tolerance` of the value now; or, between its last want and this
        one, it crossed the value. Caught from then on, until `reset` - the
        value moved by something else, the binding armed again. */
    bool pickUp (Pickup& pickup, double wanted, double current, double tolerance) noexcept;
}
