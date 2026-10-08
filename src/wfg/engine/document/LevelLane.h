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
    A volume curve somebody drew over a media cue's file: `Media/@levelLane`,
    read, judged and played in one place (namespace draft §20).

    The attribute is a `d*`, and a run of doubles is a lane when the doubles
    pair up as (seconds, level), the seconds climb, and every level is one a
    cue may be asked for. None of that is a property of one element, so none
    of it is the schema's; all of it is this file's - `FadePoints.h`'s
    arrangement, for its reason: ONE FUNCTION AND THREE CALLERS. The write door
    refuses a list that is not a lane, `ShowDocument::validate` refuses a file
    that holds one, and the Runner reads the lane it plays from here, so the
    three cannot come to disagree about what a lane is.

    WHAT IS NOT A FADE'S RULE. A fade's times are fractions that must start at
    0 and end at 1, because a fade has a length and the drawing covers it. A
    lane's times are SECONDS OF THE FILE, and the document does not know how
    long a file is - so neither end is fixed, and before the first point and
    after the last the lane holds what those points say.

    Vendor-free: consulted from the document layer and the cue layer, and it
    has no reason to know about JUCE.
*/

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  One point: a second of the FILE, from its start - not of the cue from
        its GO - and an offset in dB on the cue's level at that second (§20.1,
        decision CZ: nought is the cue as written). */
    struct LanePoint
    {
        double seconds = 0.0;
        double levelDb = 0.0;
    };

    struct LevelLane
    {
        /** The points, in order. Empty for an empty list, or a bad one. */
        std::vector<LanePoint> points;

        /*  Why the list is not a lane, in words that name the element at
            fault; empty when it is one. AN EMPTY LIST IS NO LANE, which is the
            default and every media cue written before there was one: nought
            throughout, the cue exactly as written. */
        std::string problem;
    };

    /*  The list, parsed and judged. Refuses, and leaves `points` empty:

        - an element that is not a number;
        - an odd count - a point is a second AND a level;
        - a negative second - the file starts at nought;
        - a second that does not climb strictly - two points at one instant
          are a jump drawn as if it were a curve, and a jump is a click;
        - a level outside the range `sound,level` declares (-120..12 dB), read
          from that row rather than written here a second time. */
    LevelLane readLevelLane (std::string_view text);

    /*  The offset the lane asks for at a second of the file: straight in dB
        between two points, the first point's level before it, the last one's
        after it, and nought for an empty lane. `points` is a lane as
        `readLevelLane` judges one. */
    double laneLevelDb (const std::vector<LanePoint>& points, double seconds) noexcept;

    //==============================================================================
    /*  AN OSC CUE'S CURVE (namespace draft 45): the same pairs, judged by the
        same rules, with two differences. Its seconds are the CUE'S, counted
        from the moment it begins sending, and its values are the number
        itself in its own units (YR) - so `LanePoint::levelDb` holds a value of
        whatever kind the curve's argument is, and the bounds are the curve's
        own `range` when it has one, and none when it has not. */
    struct LaneRange
    {
        double low = 0.0;
        double high = 0.0;
    };

    /*  A curve's `range` row: empty is no range, two numbers with the first
        below the second are one, anything else is `problem`. */
    std::optional<LaneRange> readLaneRange (std::string_view text, std::string& problem);

    /*  The list, parsed and judged as `readLevelLane` judges a level lane, the
        values held to `range` when one is given. */
    LevelLane readLane (std::string_view text, const std::optional<LaneRange>& range);

    /*  What the curve says at a second: straight between the two points either
        side, the first point's value before it, the last one's after it, and
        nought for no points. The points found by halving, since a recorded
        curve may hold thousands. */
    double laneValueAt (const std::vector<LanePoint>& points, double seconds) noexcept;
}
