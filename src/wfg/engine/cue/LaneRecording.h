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
    WHAT A PASS ENDS IN (namespace draft §20.9): the hand's level against the
    file's clock, sampled a tick at a time, turned into the lane the pass
    leaves behind.

    Pure, and apart from the Runner for that reason: the Runner samples on the
    tick thread against a sample clock a test does not have, and everything
    after the samples - where a loop's wrap splits them, how many of them a
    straight line can stand for, how they are joined to the curve they
    replace - is arithmetic a test can hand numbers to.

    THE STRETCH A PASS RODE REPLACES THAT STRETCH OF THE LANE (DK), and only
    that: before it and after it the lane is what it was, joined to the ride
    by a straight line of `joinSeconds` at each end (DL) - so the level walks
    onto the ride and back off it rather than stepping, which a hand on a
    fader never does either.

    A LOOP'S WRAP STARTS A NEW SEGMENT. A slice that loops plays the same
    seconds again (§20.1, DA), so a ride across a wrap is two rides over one
    stretch, and the later one is what was heard last: segments are spliced
    in order, each over what the one before left.

    Vendor-free: `doc::LanePoint` is the lane's own point, and nothing here
    needs JUCE.
*/

#include <wfg/engine/document/LevelLane.h>

#include <string>
#include <vector>

namespace wfg::cue
{
    /** One stretch of ride: seconds of the file climbing, and the hand's level. */
    using RideSegment = std::vector<doc::LanePoint>;

    /*  The hand's level at a second of the file, added to the pass. A second
        that FALLS BACK - a loop's wrap - starts a new segment; one that does
        not move (the voice not launched yet, or two ticks inside one block)
        replaces the last sample's level rather than making two points at one
        instant, which the lane's judge refuses. */
    void appendRide (std::vector<RideSegment>& segments, double seconds, double levelDb);

    /*  The segment with every sample a straight line within `toleranceDb`
        can stand for taken out (Ramer-Douglas-Peucker, measured in decibels
        at the sample's second). The first and the last are always kept. */
    RideSegment thinRide (const RideSegment& segment, double toleranceDb);

    /*  THE LANE AFTER A PASS: each segment thinned and put in place of the
        stretch it rode, in order, joined to the lane either side by
        `joinSeconds` - the old curve's own level at the join, so what was
        not ridden is exactly what it was. A join that would reach before the
        file's start is left out. */
    std::vector<doc::LanePoint> spliceRide (std::vector<doc::LanePoint> lane,
                                            const std::vector<RideSegment>& segments,
                                            double joinSeconds, double toleranceDb);

    /*  The text the lane's `node.set` carries, in `osc::formatDouble`'s
        spelling: a tenth of a millisecond and a hundredth of a decibel, as
        the window writes one (`client/model/Lane`). Empty for no lane. */
    std::string laneText (const std::vector<doc::LanePoint>& lane);
}
