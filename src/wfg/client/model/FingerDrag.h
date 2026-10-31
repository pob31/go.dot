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
    A finger on a fader (namespace draft §125, BFF, MT.1).

    THE PAGE'S FINGER, which is the Android remote's (§75, ARM; PRD §3.17):
    relative from where it landed - a grab is not a move - the travel along
    the throw moving the cap pixel for pixel while the finger stays on the
    strip, and every `unit` pixels it wanders off the strip sideways halving
    what a pixel moves, smoothly, down to a 256th (`Scrub::rateFor`, the same
    gearing the playhead has). A finger has no Shift key; the distance across
    is its Shift, and nothing jumps when it changes, because the travel is
    summed step by step at whatever rate each step was made at.

    The mouse keeps its own rule - from where it went down, Shift a tenth -
    which every fader in this client already has.

    std only, like the rest of model/.
*/

namespace wfg::client::model
{
    /** Every this many pixels off a strip halves a finger's travel: the page's NUMBER_UNIT_PX. */
    inline constexpr double fingerUnitPx = 48.0;

    /** How far outside a band `at` is: nought within it, else the distance to its nearer edge. */
    double outsideBand (double at, double low, double high) noexcept;

    class FingerDrag
    {
    public:
        FingerDrag() = default;

        /*  Taken at `startPlace` on the throw (nought at the bottom, one at
            the top), the finger at `startUp` pixels - measured upwards, so a
            caller with a downward y passes its negative. */
        FingerDrag (double startPlace, double startUp) noexcept;

        /*  The finger is now at `up`, `off` pixels outside the strip across
            its axis, over a throw `length` pixels long. Answers the place,
            held between nought and one. */
        double moveTo (double up, double off, double length, double unit = fingerUnitPx) noexcept;

        double place() const noexcept { return at; }
        double rate() const noexcept { return lastRate; }

    private:
        double at = 0.0;
        double lastUp = 0.0;
        double lastRate = 1.0;
    };
}
