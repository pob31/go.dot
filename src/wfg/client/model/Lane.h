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
    A media cue's level lane, as points somebody drags over its waveform
    (namespace draft §20.5).

    THE SAME RULES THE ENGINE JUDGES BY, restated because the boundary forbids
    reaching for them - `model/Curve`'s arrangement exactly: `doc::readLevelLane`
    is the one judge (the write door, `validate`, the Runner), `doc::` is a token
    this half of the program may not name, and the test that keeps the two
    honest asserts every string this writes against the real `readLevelLane`.

    WHAT MAKES A RUN OF DOUBLES A LANE:

      - they pair up as (seconds, level): seconds of the FILE from its start,
        and a level that is an offset on the cue's own (decision CZ);
      - the seconds are nought or more and climb strictly;
      - every level is one a cue may be written at, -120..12.

    NEITHER END IS FIXED, which is the difference from a fade's curve: a lane
    may start and stop anywhere in the file, and beyond its first and last
    points it holds what they say. An EMPTY list is no lane - the cue as
    written - and ONE point is a constant offset.

    THE VERTICAL AXIS IS A FADER'S (decision DD), `model/Fader`'s law, the one
    every strip in this window already moves on: unity high up, the bottom
    fifth for the sixty decibels nobody rides. A lane is a fader somebody drew
    over time, so it is drawn on the throw a hand already knows.

    NOTHING HERE WRITES. Each verb answers what the lane WOULD become, and the
    window sends it - one `node.set` of the whole list per gesture, on release.
*/

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** One point: a second of the file, and the offset on the cue's level there. */
    struct LanePoint
    {
        double seconds = 0.0;
        double levelDb = 0.0;
    };

    /** The address a lane is written at: `/godot/cue/<id>/levelLane`. */
    std::string laneAddress (const std::string& cueId);

    /*  The address a SEND's lane is written at (namespace draft §28):
        `/godot/send/<id>/levelLane`. The same rules, on a send's level. */
    std::string sendLaneAddress (const std::string& sendId);

    /*  The lane the tree publishes for a media cue, in order. Empty for none,
        for a cue that is not media, and for a list that is not pairs. */
    std::vector<LanePoint> readLane (const tree::TreeSnapshot&, const std::string& cueId);

    /*  The lane the tree publishes at an address - a cue's or a send's - in
        order. Empty for none and for a list that is not pairs. */
    std::vector<LanePoint> readLaneAt (const tree::TreeSnapshot&, const std::string& address);

    /*  What the lane asks for at a second of the file: straight in dB between
        points, held beyond both ends, nought for no lane - what the engine
        plays (`doc::laneLevelDb`). */
    double laneLevelAt (const std::vector<LanePoint>&, double seconds);

    /*  Where on the picture, from 0 at the bottom to 1 at the top, a level is
        drawn, and what a height means: the fader's throw (DD). */
    double laneHeightFor (double levelDb);
    double laneLevelForHeight (double height);

    /*  WHICH POINT is under the pointer, or npos. Both axes in the picture's
        own units - seconds of the view and heights of the bar - each divided
        by what counts as near on it, so a point is picked by how close it
        LOOKS and not by which axis has the bigger numbers. */
    std::size_t nearestLanePoint (const std::vector<LanePoint>&, double seconds, double height,
                                  double secondsTolerance, double heightTolerance);

    /** Whether the pointer is on the drawn line, near enough to add a point to it. */
    bool onLaneLine (const std::vector<LanePoint>&, double seconds, double height,
                     double heightTolerance);

    /*  WHERE A DRAGGED POINT WOULD LAND: held strictly between its neighbours
        in time - two points at one second are a jump, which the door refuses -
        nought or later, no later than the file's end when that is known, and
        a level clamped to what a cue may be written at. */
    LanePoint dragLanePoint (const std::vector<LanePoint>&, std::size_t at,
                             double seconds, double levelDb, double fileLength);

    /*  THE LANE WITH THAT POINT MOVED, which is the list a drag or a typed
        number sends: `dragLanePoint`'s answer put in its place. */
    std::vector<LanePoint> withLanePoint (const std::vector<LanePoint>&, std::size_t at,
                                          double seconds, double levelDb, double fileLength);

    /*  THE LANE WITH ONE MORE POINT, at that second, ON THE LINE it already
        draws there - so adding a point changes nothing until somebody moves
        it, which is what makes adding one safe while listening. On a lane
        that has none it is one point at nought: a constant offset of nothing,
        which is still the cue as written. `nullopt` where there is already a
        point at that instant, or the second is outside the file. */
    std::optional<std::vector<LanePoint>> insertLanePoint (const std::vector<LanePoint>&,
                                                           double seconds, double fileLength);

    /*  THE LANE WITHOUT ONE. Any point may go, the ends included - a lane has
        no ends it must keep - and taking the last one away is no lane. */
    std::vector<LanePoint> removeLanePoint (const std::vector<LanePoint>&, std::size_t at);

    /*  The text `node.set` is given, in the spelling `osc::formatDouble`
        writes so a French locale reads it back: a tenth of a millisecond and
        a hundredth of a decibel. Empty for no lane. */
    std::string writeLane (const std::vector<LanePoint>&);

    /** Why this is not a lane, in words; empty when it is one (or is none). */
    std::string whyNotALane (const std::vector<LanePoint>&);

    /*  A level somebody typed: "-6", "-6.5 dB", "+3", or "silence". `nullopt`
        for anything else, so a slip of the keyboard moves nothing. */
    std::optional<double> levelFrom (const std::string& typed);
}
