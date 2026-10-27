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

/*
    THE BEAT CLOCK (namespace draft §123, BEH): the one function three sides
    share. Pinned here as arithmetic: a steady tempo counts straight, a ramp's
    beats are the tempo's integral whatever its shape, and the moment of a beat
    inverts it to the nanosecond.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/cue/BeatClock.h>

using namespace wfg::cue;

TEST_CASE ("beat clock: a steady tempo counts beats straight from its anchor, and a beat's moment inverts it")
{
    BeatClock clock;
    clock.bpmFrom = clock.bpmTo = 120.0;

    CHECK (clock.bpmAt (10.0) == doctest::Approx (120.0));
    CHECK (clock.beatAt (0.0) == doctest::Approx (0.0));
    CHECK (clock.beatAt (1.5) == doctest::Approx (3.0));
    CHECK (clock.secondsOfBeat (3.0) == doctest::Approx (1.5));

    //  Before the anchor the count runs back; after a re-anchor it goes on from the beat kept.
    CHECK (clock.beatAt (-1.0) == doctest::Approx (-2.0));
    clock.anchorBeat = 2.5;
    CHECK (clock.beatAt (1.0) == doctest::Approx (4.5));
    CHECK (clock.secondsOfBeat (4.5) == doctest::Approx (1.0));

    //  A tempo of nothing is read as 120, never divided by.
    BeatClock none;
    none.bpmFrom = none.bpmTo = 0.0;
    CHECK (none.bpmAt (0.0) == doctest::Approx (120.0));
    CHECK (none.secondsOfBeat (2.0) == doctest::Approx (1.0));
}

TEST_CASE ("beat clock: a ramp's beats are the tempo's integral, straight or S-shaped, and the inverse round-trips")
{
    for (const auto shape : { 0.0, 0.5, 1.0 })
    {
        INFO ("shape " << shape);

        BeatClock clock;
        clock.bpmFrom = 120.0;
        clock.bpmTo = 60.0;
        clock.rampStart = 1.0;
        clock.rampLength = 4.0;
        clock.shape = shape;

        const auto numeric = [&clock] (double seconds)
        {
            double sum = 0.0;
            constexpr int steps = 40000;

            for (int n = 0; n < steps; ++n)
                sum += clock.bpmAt (seconds * (n + 0.5) / steps) / 60.0 * seconds / steps;

            return sum;
        };

        for (const auto seconds : { 0.5, 1.0, 2.0, 3.5, 5.0, 7.0 })
        {
            INFO ("at " << seconds);
            CHECK (clock.beatAt (seconds) == doctest::Approx (numeric (seconds)).epsilon (1.0e-6));
            CHECK (clock.secondsOfBeat (clock.beatAt (seconds)) == doctest::Approx (seconds).epsilon (1.0e-9));
        }

        //  Whatever the shape the ramp covers the same beats - four seconds at a mean of 90, six beats -
        //  and passes the middle tempo at its middle.
        CHECK (clock.beatAt (5.0) - clock.beatAt (1.0) == doctest::Approx (6.0));
        CHECK (clock.bpmAt (3.0) == doctest::Approx (90.0));
        CHECK (clock.bpmAt (0.9) == doctest::Approx (120.0));
        CHECK (clock.bpmAt (5.1) == doctest::Approx (60.0));
    }

    //  The S eases out of the old tempo where the straight line has left it already.
    BeatClock straight, eased;
    straight.bpmFrom = eased.bpmFrom = 120.0;
    straight.bpmTo = eased.bpmTo = 60.0;
    straight.rampLength = eased.rampLength = 4.0;
    eased.shape = 1.0;
    CHECK (eased.bpmAt (0.4) > straight.bpmAt (0.4));
    CHECK (eased.bpmAt (3.6) < straight.bpmAt (3.6));
}

TEST_CASE ("beat clock: a length of nought is a step at its moment, and a re-anchor mid-ramp keeps the beat and the tempo of that moment")
{
    BeatClock step;
    step.bpmFrom = 120.0;
    step.bpmTo = 60.0;
    step.rampStart = 2.0;
    step.rampLength = 0.0;
    CHECK (step.bpmAt (1.999) == doctest::Approx (120.0));
    CHECK (step.bpmAt (2.0) == doctest::Approx (60.0));
    CHECK (step.beatAt (3.0) == doctest::Approx (4.0 + 1.0));
    CHECK (step.secondsOfBeat (5.0) == doctest::Approx (3.0));

    BeatClock ramp;
    ramp.bpmFrom = 120.0;
    ramp.bpmTo = 60.0;
    ramp.rampLength = 4.0;
    ramp.shape = 0.3;

    //  As the tempo verb re-anchors: the beat and the tempo of the moment carried over, bit for bit the same clock else.
    const auto at = 1.7;
    BeatClock again = ramp;
    again.anchorBeat = ramp.beatAt (at);
    again.bpmFrom = ramp.bpmAt (at);
    CHECK (again.beatAt (0.0) == doctest::Approx (ramp.beatAt (at)));
    CHECK (again.bpmAt (0.0) == doctest::Approx (ramp.bpmAt (at)));
    CHECK (ramp.sameAs (ramp));
    CHECK_FALSE (ramp.sameAs (again));

    BeatClockState one, two;
    one.clock = ramp;
    two.clock = ramp;
    CHECK (one.sameAs (two));
    two.anchorSample = 5;
    CHECK_FALSE (one.sameAs (two));

    //  The bar holding a beat, bars counted from beat nought.
    BeatClock three;
    three.beatsPerBar = 3;
    CHECK (three.lastBarStartBeat (7.2) == doctest::Approx (6.0));
    CHECK (three.lastBarStartBeat (6.0) == doctest::Approx (6.0));
}
