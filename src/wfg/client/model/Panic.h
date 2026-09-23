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
    Esc, and Esc again: which of PRD §4.4's two levels a press means.

    The law gives Esc two readings - one press is the graceful abort, a second
    is the immediate one - and only a client can tell them apart, because
    "twice" is a fact about a hand and not about the show. So the reading is
    made here, in one place both the key and the PANIC button ask, and each
    reading is then ONE named command (`run.stopAll`, `run.killAll`): the log
    records which level was reached, and a replay reaches the same one.

    THE SECOND PRESS COUNTS FROM THE FIRST, not from the last: three presses in
    a second are a stop and then a kill, and the third is a kill again - which
    is what a hand hammering the key means, and is applied harmlessly to a
    table that is already stopping. A press long after the last is a first
    press again. Nothing here reads a clock; the caller says what time it is,
    which is what lets a test press twice with no waiting.

    std only, like the rest of model/.
*/

#include <cstdint>

namespace wfg::client::model
{
    class Panic
    {
    public:
        /** How close two presses must be to read as one double press: §4.4's "double Esc".
            A control surface's STOP makes the same reading in the engine
            (`surface::doubleStopTicks`, src/wfg/engine/surface/SurfaceProfile.h),
            which cannot include this file: the two numbers move together. */
        static constexpr std::int64_t doublePressMs = 750;

        /*  Records a press at this time and says whether it is the SECOND
            level - a press within `doublePressMs` of the one before it. */
        bool press (std::int64_t nowMs) noexcept
        {
            const auto isDouble = armed && nowMs - lastMs <= doublePressMs && nowMs >= lastMs;

            armed = true;
            lastMs = nowMs;
            return isDouble;
        }

    private:
        bool armed = false;
        std::int64_t lastMs = 0;
    };
}
