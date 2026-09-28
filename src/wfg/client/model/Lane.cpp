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

#include <wfg/client/model/Lane.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>

namespace wfg::client::model
{
    namespace
    {
        /*  The closest two points may be in time: a millisecond. The engine's
            door refuses seconds that do not climb STRICTLY, so the window keeps
            them apart rather than finding out afterwards. */
        constexpr double apart = 0.001;
    }

    std::string laneAddress (const std::string& cueId)
    {
        return "/godot/cue/" + cueId + "/levelLane";
    }

    std::vector<LanePoint> readLane (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        std::vector<LanePoint> out;

        if (cueId.empty() || text (snapshot, "/godot/cue/" + cueId + "/kind") != "media")
            return out;

        /*  A LIST NODE, read by its values as a fade's points are: `text`
            answers empty for one, deliberately. */
        if (const auto* node = snapshot.find (laneAddress (cueId)))
        {
            const auto& values = node->values;

            //  A second AND a level, so an odd count is not a lane at all.
            if (values.size() % 2 == 0)
                for (std::size_t at = 0; at + 1 < values.size(); at += 2)
                    out.push_back ({ values[at].getFloat64(), values[at + 1].getFloat64() });
        }

        return out;
    }

    double laneLevelAt (const std::vector<LanePoint>& points, double seconds)
    {
        if (points.empty())
            return 0.0;

        if (! (seconds > points.front().seconds))
            return points.front().levelDb;

        for (std::size_t at = 1; at < points.size(); ++at)
        {
            const auto& before = points[at - 1];
            const auto& after = points[at];

            if (seconds > after.seconds)
                continue;

            const auto span = after.seconds - before.seconds;

            if (! (span > 0.0))
                return after.levelDb;

            /*  STRAIGHT IN dB, which is what the engine plays between points. */
            return before.levelDb
                     + (after.levelDb - before.levelDb) * (seconds - before.seconds) / span;
        }

        return points.back().levelDb;
    }

    double laneHeightFor (double levelDb)
    {
        return fractionForDb (levelDb);
    }

    double laneLevelForHeight (double height)
    {
        return dbForFraction (height);
    }

    std::size_t nearestLanePoint (const std::vector<LanePoint>& points, double seconds, double height,
                                  double secondsTolerance, double heightTolerance)
    {
        auto best = static_cast<std::size_t> (-1);

        if (! (secondsTolerance > 0.0) || ! (heightTolerance > 0.0))
            return best;

        auto nearest = 2.0;

        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto ds = std::abs (points[at].seconds - seconds);
            const auto dh = std::abs (laneHeightFor (points[at].levelDb) - height);

            if (ds > secondsTolerance || dh > heightTolerance)
                continue;

            const auto apartHere = ds / secondsTolerance + dh / heightTolerance;

            if (apartHere < nearest)
            {
                nearest = apartHere;
                best = at;
            }
        }

        return best;
    }

    bool onLaneLine (const std::vector<LanePoint>& points, double seconds, double height,
                     double heightTolerance)
    {
        return std::abs (laneHeightFor (laneLevelAt (points, seconds)) - height) <= heightTolerance;
    }

    LanePoint dragLanePoint (const std::vector<LanePoint>& points, std::size_t at,
                             double seconds, double levelDb, double fileLength)
    {
        if (at >= points.size())
            return {};

        auto low = 0.0;
        auto high = fileLength > 0.0 ? fileLength : seconds;

        if (at > 0)
            low = points[at - 1].seconds + apart;

        if (at + 1 < points.size())
            high = std::min (high, points[at + 1].seconds - apart);

        /*  A POINT HEMMED IN on both sides by neighbours a millisecond apart
            stays where it is rather than clamping into a range that is upside
            down, which `std::clamp` would make undefined. */
        if (high < low)
            high = low = points[at].seconds;

        LanePoint moved;
        moved.seconds = std::clamp (std::max (seconds, 0.0), low, high);
        moved.levelDb = std::clamp (levelDb, silenceDb, loudestDb);

        return moved;
    }

    std::vector<LanePoint> withLanePoint (const std::vector<LanePoint>& points, std::size_t at,
                                          double seconds, double levelDb, double fileLength)
    {
        auto out = points;

        if (at < out.size())
            out[at] = dragLanePoint (points, at, seconds, levelDb, fileLength);

        return out;
    }

    std::optional<std::vector<LanePoint>> insertLanePoint (const std::vector<LanePoint>& points,
                                                           double seconds, double fileLength)
    {
        if (seconds < 0.0 || (fileLength > 0.0 && seconds > fileLength))
            return std::nullopt;

        for (const auto& point : points)
            if (std::abs (point.seconds - seconds) < apart)
                return std::nullopt;

        auto out = points;

        //  ON THE LINE IT ALREADY DRAWS, so adding a point changes nothing yet.
        out.push_back ({ seconds, laneLevelAt (points, seconds) });

        std::stable_sort (out.begin(), out.end(),
                          [] (const LanePoint& a, const LanePoint& b) { return a.seconds < b.seconds; });

        return out;
    }

    std::vector<LanePoint> removeLanePoint (const std::vector<LanePoint>& points, std::size_t at)
    {
        auto out = points;

        if (at < out.size())
            out.erase (out.begin() + static_cast<std::ptrdiff_t> (at));

        return out;
    }

    std::string writeLane (const std::vector<LanePoint>& points)
    {
        std::string out;

        for (const auto& point : points)
        {
            if (! out.empty())
                out += ' ';

            /*  Through the OSC formatter, the one locale-proof one here: a
                French locale must not turn 4.5 into something the engine's
                parser refuses. */
            out += osc::formatDouble (std::round (point.seconds * 10000.0) / 10000.0) + " "
                     + osc::formatDouble (std::round (point.levelDb * 100.0) / 100.0);
        }

        return out;
    }

    std::string whyNotALane (const std::vector<LanePoint>& points)
    {
        for (std::size_t at = 0; at < points.size(); ++at)
        {
            if (points[at].seconds < 0.0)
                return "a point before the file starts";

            if (at > 0 && ! (points[at].seconds > points[at - 1].seconds))
                return "two points at the same moment";

            if (points[at].levelDb < silenceDb || points[at].levelDb > loudestDb)
                return "a level no cue may be written at";
        }

        return {};
    }

    std::optional<double> levelFrom (const std::string& typed)
    {
        std::string word;

        for (const auto c : typed)
            if (std::isspace (static_cast<unsigned char> (c)) == 0)
                word += static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

        //  "dB" after the number is how the window writes one, so it is how one is typed back.
        if (word.size() > 2 && word.compare (word.size() - 2, 2, "db") == 0)
            word.resize (word.size() - 2);

        //  And "-inf" is how it writes silence (`faderText`).
        if (word == "silence" || word == "-inf")
            return silenceDb;

        if (! word.empty() && word.front() == '+')
            word.erase (word.begin());

        const auto level = osc::parseDouble (word);

        if (! level.has_value() || *level < silenceDb || *level > loudestDb)
            return std::nullopt;

        return level;
    }
}
