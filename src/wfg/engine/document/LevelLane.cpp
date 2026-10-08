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

#include <wfg/engine/document/LevelLane.h>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::doc
{
    namespace
    {
        LevelLane refused (std::string why)
        {
            LevelLane out;
            out.problem = std::move (why);
            return out;
        }

        /*  `-120..12`, for a message. Read from the row so that the words and
            the check cannot name two different ranges. */
        std::string rangeText (const Attribute& level)
        {
            std::string text;

            if (level.row->hasMin)
                text += osc::formatDouble (level.row->minimum);

            text += "..";

            if (level.row->hasMax)
                text += osc::formatDouble (level.row->maximum);

            return text;
        }
    }

    namespace
    {
        /*  XSD's list form, as Schema::parseList reads it: whitespace between,
            leading and trailing ignored, and nothing at all is zero values.
            nullopt and `problem` set for an element that is not a number. */
        std::optional<std::vector<double>> numbersOf (std::string_view text, std::string& problem)
        {
            std::vector<double> values;
            std::size_t i = 0;

            while (i < text.size())
            {
                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                    ++i;

                const auto start = i;

                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                    ++i;

                if (i == start)
                    break;

                const auto token = text.substr (start, i - start);
                const auto parsed = osc::parseDouble (token);

                if (! parsed)
                {
                    problem = "element " + std::to_string (values.size())
                                + ": expected a number, found \"" + std::string (token) + "\"";
                    return std::nullopt;
                }

                values.push_back (*parsed);
            }

            return values;
        }
    }

    std::optional<LaneRange> readLaneRange (std::string_view text, std::string& problem)
    {
        const auto values = numbersOf (text, problem);

        if (! values.has_value() || values->empty())
            return std::nullopt;

        if (values->size() != 2 || ! ((*values)[0] < (*values)[1]))
        {
            problem = "a range is two numbers, the lowest first";
            return std::nullopt;
        }

        return LaneRange { (*values)[0], (*values)[1] };
    }

    LevelLane readLane (std::string_view text, const std::optional<LaneRange>& range)
    {
        std::string problem;
        const auto values = numbersOf (text, problem);

        if (! values.has_value())
            return refused (problem);

        if (values->empty())
            return {};

        if (values->size() % 2 != 0)
            return refused (std::to_string (values->size()) + " values, an odd number - each"
                            " point is a second and a value");

        LevelLane out;
        out.points.reserve (values->size() / 2);

        for (std::size_t k = 0; k < values->size(); k += 2)
        {
            const LanePoint point { (*values)[k], (*values)[k + 1] };
            const auto index = std::to_string (k / 2);

            if (point.seconds < 0.0)
                return refused ("point " + index + ": second " + osc::formatDouble (point.seconds)
                                + " is before the cue starts");

            if (! out.points.empty() && ! (point.seconds > out.points.back().seconds))
                return refused ("point " + index + ": second " + osc::formatDouble (point.seconds)
                                + " does not come after " + osc::formatDouble (out.points.back().seconds));

            if (range.has_value() && (point.levelDb < range->low || point.levelDb > range->high))
                return refused ("point " + index + ": value " + osc::formatDouble (point.levelDb)
                                + " is outside " + osc::formatDouble (range->low) + ".."
                                + osc::formatDouble (range->high));

            out.points.push_back (point);
        }

        return out;
    }

    double laneValueAt (const std::vector<LanePoint>& points, double seconds) noexcept
    {
        if (points.empty())
            return 0.0;

        if (! (seconds > points.front().seconds))
            return points.front().levelDb;

        if (! (seconds < points.back().seconds))
            return points.back().levelDb;

        /*  The first point after the second asked, by halving: the judge's
            strict climb makes the span below never zero. */
        const auto after = std::upper_bound (points.begin(), points.end(), seconds,
                                             [] (double at, const LanePoint& point) { return at < point.seconds; });
        const auto& to = *after;
        const auto& from = *std::prev (after);
        const auto share = (seconds - from.seconds) / (to.seconds - from.seconds);

        return from.levelDb + share * (to.levelDb - from.levelDb);
    }

    LevelLane readLevelLane (std::string_view text)
    {
        /*  XSD's list form, as Schema::parseList reads it: whitespace between,
            leading and trailing ignored, and nothing at all is zero values. */
        std::vector<double> values;
        std::size_t i = 0;

        while (i < text.size())
        {
            while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                ++i;

            const auto start = i;

            while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                ++i;

            if (i == start)
                break;

            const auto token = text.substr (start, i - start);
            const auto parsed = osc::parseDouble (token);

            if (! parsed)
                return refused ("element " + std::to_string (values.size())
                                + ": expected a number, found \"" + std::string (token) + "\"");

            values.push_back (*parsed);
        }

        if (values.empty())
            return {};

        if (values.size() % 2 != 0)
            return refused (std::to_string (values.size()) + " values, an odd number - each"
                            " point is a second and a level");

        /*  THE CUE'S OWN LEVEL ROW, because an offset of the lane's is a level
            the cue could have been written at: a lane asking for +40 dB would
            be the typo §3.3's ranges exist to catch. */
        const auto* level = Schema::instance().attribute ("Media", "level");

        LevelLane out;
        out.points.reserve (values.size() / 2);

        for (std::size_t k = 0; k < values.size(); k += 2)
        {
            const LanePoint point { values[k], values[k + 1] };
            const auto index = std::to_string (k / 2);

            if (point.seconds < 0.0)
                return refused ("point " + index + ": second " + osc::formatDouble (point.seconds)
                                + " is before the file starts");

            /*  STRICTLY, not merely in order. Two points at one second are a
                jump drawn as if it were a curve, and a jump in level is a click. */
            if (! out.points.empty() && ! (point.seconds > out.points.back().seconds))
                return refused ("point " + index + ": second " + osc::formatDouble (point.seconds)
                                + " does not come after " + osc::formatDouble (out.points.back().seconds));

            if (level != nullptr && ! level->isInRange (point.levelDb))
                return refused ("point " + index + ": level " + osc::formatDouble (point.levelDb)
                                + " dB is outside " + rangeText (*level));

            out.points.push_back (point);
        }

        return out;
    }

    double laneLevelDb (const std::vector<LanePoint>& points, double seconds) noexcept
    {
        if (points.empty())
            return 0.0;

        /*  HELD AT BOTH ENDS. Before the first point the lane says what the
            first point says, and after the last what the last says: a lane
            drawn over the middle of a file does not also decide the rest of it
            is at nought, which would be a step at a point nobody drew. */
        if (! (seconds > points.front().seconds))
            return points.front().levelDb;

        if (! (seconds < points.back().seconds))
            return points.back().levelDb;

        /*  Straight in dB between the two points either side, which is what a
            drawn fade does (§14.6). A lane is short - a few points to a few
            hundred - and read once a tick per sounding cue, so a walk is the
            honest cost; the strict climb the judge insists on is what makes
            the span below never zero. */
        for (std::size_t k = 1; k < points.size(); ++k)
        {
            const auto& to = points[k];

            if (seconds < to.seconds)
            {
                const auto& from = points[k - 1];
                const auto share = (seconds - from.seconds) / (to.seconds - from.seconds);

                return from.levelDb + share * (to.levelDb - from.levelDb);
            }
        }

        return points.back().levelDb;
    }
}
