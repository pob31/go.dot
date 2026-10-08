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
    AN OSC CUE'S CURVES, AS THE FOOT'S EDITOR DRAWS THEM (namespace draft 45,
    O.7): each curve on the cue's own time, against an axis of its own - the
    curve's `range` when it has one, the device's range for that value when it
    describes one, and otherwise what its points and its written value cover.

    THE LEVEL LANE'S GESTURES ON A STRAIGHT AXIS: a point picked by how close
    it looks, held between its neighbours in time when dragged, added on the
    line where the curve already runs - so adding one changes nothing until it
    is moved - and taken away. The values are the number itself (YR), so the
    axis is plain metres, degrees, or whatever the value is, top to bottom.

    std only, and pure: the boundary's rules, and every case checkable against
    numbers.
*/

#include <wfg/client/model/Lane.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  Where a curve is drawn, bottom to top: `low` to `high`. `bounded` says
        it came from a range - the curve's or the device's - which a drag is
        held inside; an axis made from the points is a frame and not a fence. */
    struct CurveAxis
    {
        double low = 0.0;
        double high = 1.0;
        bool bounded = false;
    };

    struct CurveView
    {
        std::string id;

        /** What it is created under and moves a value of: the cue, or a further message. */
        std::string parentId;
        std::string messageAddress;
        std::size_t arg = 0;

        /*  What the editor's menu calls it: the address's last word, and which
            value when the message has several - "positionX", "xyz 2". */
        std::string label;

        std::vector<LanePoint> points;
        CurveAxis axis;

        /** The message's written value at that place: what an empty curve plays. */
        double written = 0.0;
        bool integer = false;

        /*  ARMED FOR RECORDING (O.9), and what it rides while its cue is armed:
            the device's report or a hand's value once one has latched it in a
            pass, its own curve where the clock is until then. */
        bool armed = false;
        std::optional<double> ride;

        std::string pointsAddress() const { return "/godot/curve/" + id + "/points"; }
    };

    struct OscCurvesReading
    {
        std::string cueId;
        std::vector<CurveView> curves;

        /*  How long the curves play - the cue's `duration`, or the longest
            curve's last point - and how much time the editor draws, which is
            that or a second, whichever is longer, so a curve with no point yet
            has somewhere to put one. */
        double duration = 0.0;
        double drawn = 1.0;
        bool loop = false;
        bool locked = false;

        /*  RECORDING (namespace draft 45, O.9): whether this cue is the one
            armed - or another is, which a click here would take the arming
            from - whether a pass is running, and what the last one ended in. */
        bool armedHere = false;
        bool armedElsewhere = false;
        bool recording = false;
        std::string lastPass;
    };

    /** The cue's curves, its own message's first, in one pass over the snapshot. */
    OscCurvesReading readOscCurves (const tree::TreeSnapshot&, const std::string& cueId);

    //==============================================================================
    /** A value's height on the axis, nought at the bottom and one at the top, and back. */
    double heightOnAxis (double value, const CurveAxis&) noexcept;
    double valueOnAxis (double height, const CurveAxis&) noexcept;

    /*  WHICH POINT is under the pointer, or npos: both axes divided by what
        counts as near on each, as the level lane picks its points. */
    std::size_t nearestCurvePoint (const std::vector<LanePoint>&, double seconds, double height,
                                   const CurveAxis&, double secondsTolerance, double heightTolerance);

    /*  THE CURVE WITH ONE POINT MOVED: held a millisecond inside its neighbours
        in time and at nought or later, and inside the axis when the axis is a
        range. */
    std::vector<LanePoint> withCurvePoint (const std::vector<LanePoint>&, std::size_t at,
                                           double seconds, double value, const CurveAxis&);

    /*  THE CURVE WITH ONE MORE POINT, at that second on the line it already
        draws - the written value on a curve with none. nullopt where a point
        already stands within a millisecond. */
    std::optional<std::vector<LanePoint>> insertCurvePoint (const std::vector<LanePoint>&, double seconds,
                                                            double written);

    /*  The text `node.set` is given: a tenth of a millisecond, and each value
        rounded to `step` - a ten-thousandth of the axis, as fine as a hand can
        place and coarse enough that a drag does not write noise. Locale-free. */
    std::string writeCurve (const std::vector<LanePoint>&, double step);

    /** The step `writeCurve` rounds a curve on this axis to. */
    double stepFor (const CurveAxis&) noexcept;
}
