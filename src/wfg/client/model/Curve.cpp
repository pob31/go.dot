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

#include <wfg/client/model/Curve.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        /*  The closest two breakpoints may be in time. Two at one instant are
            two answers to one question, and the engine's door refuses times
            that do not climb STRICTLY - so the window keeps them apart rather
            than finding out afterwards. */
        constexpr double apart = 0.001;

        double levelBetween (const std::vector<CurvePoint>& points, double t)
        {
            if (points.empty())
                return 0.0;

            if (t <= points.front().t)
                return points.front().levelDb;

            for (std::size_t at = 1; at < points.size(); ++at)
            {
                const auto& before = points[at - 1];
                const auto& after = points[at];

                if (t > after.t)
                    continue;

                const auto span = after.t - before.t;

                if (! (span > 0.0))
                    return after.levelDb;

                /*  STRAIGHT IN dB, which is what the engine plays between
                    breakpoints and what "linear" already means here. */
                return before.levelDb
                         + (after.levelDb - before.levelDb) * (t - before.t) / span;
            }

            return points.back().levelDb;
        }
    }

    CurveReading readCurve (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        CurveReading out;

        if (cueId.empty())
            return out;

        const auto base = "/godot/cue/" + cueId + "/";

        out.cueId = cueId;
        out.cueName = text (snapshot, base + "name");
        out.curve = text (snapshot, base + "curve");
        out.levelDb = osc::parseDouble (text (snapshot, base + "level")).value_or (0.0);
        out.seconds = osc::parseDouble (text (snapshot, base + "duration")).value_or (0.0);

        if (text (snapshot, base + "kind") != "fade")
        {
            out.notice = "Only a fade has a curve to draw.";
            return out;
        }

        /*  A LIST NODE, so its values are read directly: `text` answers empty
            for one, deliberately - four numbers have no single text - and this
            is the first thing in the client that wants them. */
        if (const auto* node = snapshot.find (base + "points"))
        {
            const auto& values = node->values;

            //  A time AND a level, so an odd count is not a curve at all.
            if (values.size() >= 2 && values.size() % 2 == 0)
                for (std::size_t at = 0; at + 1 < values.size(); at += 2)
                    out.points.push_back ({ values[at].getFloat64(),
                                            values[at + 1].getFloat64() });
        }

        return out;
    }

    std::size_t nearestPoint (const std::vector<CurvePoint>& points, double t, double levelDb,
                              double tTolerance, double levelTolerance)
    {
        auto best = static_cast<std::size_t> (-1);
        auto nearest = 2.0;

        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto dt = std::abs (points[at].t - t);
            const auto dl = std::abs (points[at].levelDb - levelDb);

            if (dt > tTolerance || dl > levelTolerance)
                continue;

            /*  MEASURED IN THE FIELD'S OWN UNITS rather than in seconds and
                decibels, which are not comparable: each axis is divided by
                what counts as near on it, so a point is picked by how close it
                looks and not by which axis happens to have bigger numbers. */
            const auto apartHere = dt / tTolerance + dl / levelTolerance;

            if (apartHere < nearest)
            {
                nearest = apartHere;
                best = at;
            }
        }

        return best;
    }

    CurvePoint dragTo (const std::vector<CurvePoint>& points, std::size_t at,
                       double t, double levelDb)
    {
        if (at >= points.size())
            return {};

        CurvePoint moved;
        moved.levelDb = std::clamp (levelDb, quietestDb, loudestFadeDb);

        /*  THE TWO ENDS DO NOT MOVE IN TIME. A curve that began after nought
            or ended before one would leave a stretch of the fade it says
            nothing about, which is what the engine's door refuses - so here
            the ends slide up and down and nowhere else. */
        if (at == 0)
        {
            moved.t = 0.0;
            return moved;
        }

        if (at + 1 == points.size())
        {
            moved.t = 1.0;
            return moved;
        }

        //  And the ones between stay strictly inside their neighbours.
        moved.t = std::clamp (t, points[at - 1].t + apart, points[at + 1].t - apart);
        return moved;
    }

    std::optional<std::vector<CurvePoint>> insertAt (const std::vector<CurvePoint>& points,
                                                     double t, double fromDb, double toDb)
    {
        const auto when = std::clamp (t, 0.0, 1.0);

        /*  A CURVE OUT OF A WORD. A fade with no points plays its `curve`, and
            the first thing drawn has to make a curve the rules accept - which
            is two ends and the point asked for. The ends take the levels the
            fade already has, so the shape it plays does not jump the moment
            somebody touches it. */
        if (points.empty())
        {
            if (when <= apart || when >= 1.0 - apart)
                return std::nullopt;

            const auto middle = fromDb + (toDb - fromDb) * when;

            return std::vector<CurvePoint> { { 0.0, fromDb }, { when, middle }, { 1.0, toDb } };
        }

        for (const auto& point : points)
            if (std::abs (point.t - when) < apart)
                return std::nullopt;

        auto out = points;
        const auto level = levelBetween (points, when);

        //  ON THE LINE IT ALREADY DRAWS, so adding a point changes nothing yet.
        out.push_back ({ when, level });

        std::stable_sort (out.begin(), out.end(),
                          [] (const CurvePoint& a, const CurvePoint& b) { return a.t < b.t; });

        return out;
    }

    std::optional<std::vector<CurvePoint>> removeAt (const std::vector<CurvePoint>& points,
                                                     std::size_t at)
    {
        if (at >= points.size())
            return std::nullopt;

        /*  NEITHER END. The drawing has to cover the whole fade, so taking an
            end away would leave one that does not - and the way to stop having
            a drawn curve at all is to take the middle ones out until two are
            left, and then clear it. */
        if (at == 0 || at + 1 == points.size())
            return std::nullopt;

        auto out = points;
        out.erase (out.begin() + static_cast<std::ptrdiff_t> (at));

        return out;
    }

    std::string writePoints (const std::vector<CurvePoint>& points)
    {
        std::string out;

        for (const auto& point : points)
        {
            if (! out.empty())
                out += ' ';

            /*  Through the OSC formatter, which is the one locale-proof one
                here: a French locale must not turn 0.5 into something the
                engine's parser refuses. */
            out += osc::formatDouble (std::round (point.t * 10000.0) / 10000.0) + " "
                     + osc::formatDouble (std::round (point.levelDb * 100.0) / 100.0);
        }

        return out;
    }

    std::string whyNotACurve (const std::vector<CurvePoint>& points)
    {
        if (points.empty())
            return {};              // not a bad curve: the absence of one

        if (points.size() < 2)
            return "a curve needs a start and an end";

        if (points.front().t > apart)
            return "a curve has to start at the beginning of the fade";

        if (points.back().t < 1.0 - apart)
            return "a curve has to reach the end of the fade";

        for (std::size_t at = 1; at < points.size(); ++at)
            if (points[at].t <= points[at - 1].t)
                return "two breakpoints at the same moment";

        for (const auto& point : points)
            if (point.levelDb < quietestDb || point.levelDb > loudestFadeDb)
                return "a level a fade cannot reach";

        return {};
    }
}
