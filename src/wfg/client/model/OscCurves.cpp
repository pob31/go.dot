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

#include <wfg/client/model/OscCurves.h>

#include <wfg/client/model/OscMessages.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr double apart = 0.001;   // a millisecond, the level lane's

        /*  The last word of an address, and the value's place when the message
            has more than one: what a person reading the menu calls a curve. */
        std::string labelFor (const std::string& address, std::size_t arg, std::size_t of)
        {
            const auto slash = address.find_last_of ('/');
            auto word = slash == std::string::npos ? address : address.substr (slash + 1);

            if (word.empty())
                word = address;

            return of > 1 ? word + " " + std::to_string (arg + 1) : word;
        }

        /*  AN AXIS THAT FRAMES the points and the written value, a tenth of the
            span either side, and a unit either side of a curve that does not
            move at all. */
        CurveAxis framed (const std::vector<LanePoint>& points, double written)
        {
            auto low = written, high = written;

            for (const auto& point : points)
            {
                low = std::min (low, point.levelDb);
                high = std::max (high, point.levelDb);
            }

            const auto span = high - low;

            if (! (span > 0.0))
                return { low - 1.0, high + 1.0, false };

            return { low - span * 0.1, high + span * 0.1, false };
        }
    }

    OscCurvesReading readOscCurves (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        OscCurvesReading out;
        out.cueId = cueId;

        const auto messages = readOscMessages (snapshot, cueId);

        if (messages.rows.empty())
            return out;

        out.locked = messages.locked;
        out.loop = text (snapshot, "/godot/cue/" + cueId + "/loop") == "true";

        const auto armedCue = text (snapshot, "/godot/curves/cue");
        out.armedHere = ! armedCue.empty() && armedCue == cueId;
        out.armedElsewhere = ! armedCue.empty() && armedCue != cueId;
        out.recording = out.armedHere && text (snapshot, "/godot/curves/recording") == "true";
        out.lastPass = text (snapshot, "/godot/curves/pass");

        const auto armedCurves = out.armedHere ? " " + text (snapshot, "/godot/curves/rec") + " " : std::string {};

        auto last = 0.0;

        for (const auto& row : messages.rows)
        {
            const auto values = osc::valuesFromAtoms (row.value).value_or (osc::Values {});

            for (std::size_t arg = 0; arg < row.args.size(); ++arg)
            {
                if (row.args[arg].curveId.empty())
                    continue;

                CurveView view;
                view.id = row.args[arg].curveId;
                view.parentId = row.parentId (cueId);
                view.messageAddress = row.address;
                view.arg = arg;
                view.label = labelFor (row.address, arg, row.args.size());
                view.points = readLaneAt (snapshot, view.pointsAddress());

                if (arg < values.size())
                {
                    view.written = values[arg].asDouble();
                    view.integer = values[arg].isInt32() || values[arg].isInt64();
                }

                /*  THE AXIS: the curve's own range, else the device's for this
                    value, else what the points and the written value cover. */
                const auto* range = snapshot.find ("/godot/curve/" + view.id + "/range");

                if (range != nullptr && range->values.size() == 2
                      && range->values[0].asDouble() < range->values[1].asDouble())
                {
                    view.axis = { range->values[0].asDouble(), range->values[1].asDouble(), true };
                }
                else if (const auto* node = snapshot.find (row.address);
                         node != nullptr && node->rangeOf (arg).hasMinimum && node->rangeOf (arg).hasMaximum
                           && node->rangeOf (arg).minimum < node->rangeOf (arg).maximum)
                {
                    view.axis = { node->rangeOf (arg).minimum, node->rangeOf (arg).maximum, true };
                }
                else
                {
                    view.axis = framed (view.points, view.written);
                }

                if (! view.points.empty())
                    last = std::max (last, view.points.back().seconds);

                view.armed = armedCurves.find (" " + view.id + " ") != std::string::npos;

                if (const auto puck = text (snapshot, view.rowAddress ("axis")); ! puck.empty())
                    view.puckAxis = puck;

                view.puckSpeed = osc::parseDouble (text (snapshot, view.rowAddress ("speed"))).value_or (1.0);
                view.puckInvert = text (snapshot, view.rowAddress ("invert")) == "true";

                if (const auto* ride = snapshot.find ("/godot/curve/" + view.id + "/ride");
                    out.armedHere && ride != nullptr && ! ride->values.empty() && ride->values[0].isNumber())
                    view.ride = ride->values[0].asDouble();

                out.curves.push_back (std::move (view));
            }
        }

        const auto written = osc::parseDouble (text (snapshot, "/godot/cue/" + cueId + "/duration")).value_or (0.0);
        out.duration = written > 0.0 ? written : last;
        out.drawn = std::max ({ out.duration, last, 1.0 });

        return out;
    }

    //==============================================================================
    double heightOnAxis (double value, const CurveAxis& axis) noexcept
    {
        const auto span = axis.high - axis.low;
        return span > 0.0 ? (value - axis.low) / span : 0.5;
    }

    double valueOnAxis (double height, const CurveAxis& axis) noexcept
    {
        return axis.low + height * (axis.high - axis.low);
    }

    std::size_t nearestCurvePoint (const std::vector<LanePoint>& points, double seconds, double height,
                                   const CurveAxis& axis, double secondsTolerance, double heightTolerance)
    {
        auto best = std::numeric_limits<std::size_t>::max();
        auto bestDistance = 1.0;   // inside both tolerances, as a fraction of them

        for (std::size_t n = 0; n < points.size(); ++n)
        {
            const auto across = (points[n].seconds - seconds) / std::max (secondsTolerance, 1.0e-9);
            const auto up = (heightOnAxis (points[n].levelDb, axis) - height) / std::max (heightTolerance, 1.0e-9);
            const auto distance = std::sqrt (across * across + up * up);

            if (distance <= bestDistance)
            {
                best = n;
                bestDistance = distance;
            }
        }

        return best;
    }

    std::vector<LanePoint> withCurvePoint (const std::vector<LanePoint>& points, std::size_t at,
                                           double seconds, double value, const CurveAxis& axis)
    {
        auto out = points;

        if (at >= out.size())
            return out;

        const auto earliest = at > 0 ? out[at - 1].seconds + apart : 0.0;
        const auto latest = at + 1 < out.size() ? out[at + 1].seconds - apart : std::numeric_limits<double>::max();

        out[at].seconds = std::clamp (seconds, earliest, std::max (earliest, latest));
        out[at].levelDb = axis.bounded ? std::clamp (value, axis.low, axis.high) : value;
        return out;
    }

    std::optional<std::vector<LanePoint>> insertCurvePoint (const std::vector<LanePoint>& points, double seconds,
                                                            double written)
    {
        if (seconds < 0.0)
            return std::nullopt;

        for (const auto& point : points)
            if (std::abs (point.seconds - seconds) < apart)
                return std::nullopt;

        const auto value = points.empty() ? written : laneLevelAt (points, seconds);

        auto out = points;
        const auto place = std::lower_bound (out.begin(), out.end(), seconds,
                                             [] (const LanePoint& point, double at) { return point.seconds < at; });
        out.insert (place, LanePoint { seconds, value });
        return out;
    }

    double stepFor (const CurveAxis& axis) noexcept
    {
        const auto span = axis.high - axis.low;

        if (! (span > 0.0))
            return 0.0001;

        //  A ten-thousandth of the axis, down to a power of ten.
        return std::pow (10.0, std::floor (std::log10 (span / 10000.0)));
    }

    std::string writeCurve (const std::vector<LanePoint>& points, double step)
    {
        std::string out;

        const auto rounded = [] (double value, double to)
        {
            return to > 0.0 ? std::round (value / to) * to : value;
        };

        for (const auto& point : points)
        {
            if (! out.empty())
                out += ' ';

            out += osc::formatDouble (rounded (point.seconds, 0.0001));
            out += ' ';
            out += osc::formatDouble (rounded (point.levelDb, step));
        }

        return out;
    }
}
