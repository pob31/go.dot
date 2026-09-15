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

#include <wfg/engine/document/FadePoints.h>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::doc
{
    namespace
    {
        FadePoints refused (std::string why)
        {
            FadePoints out;
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

    FadePoints readFadePoints (std::string_view text)
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
                            " breakpoint is a time and a level");

        const auto* level = Schema::instance().attribute ("Fade", "level");

        FadePoints out;
        out.points.reserve (values.size() / 2);

        for (std::size_t k = 0; k < values.size(); k += 2)
        {
            const FadePoint point { values[k], values[k + 1] };
            const auto index = std::to_string (k / 2);

            if (point.t < 0.0 || point.t > 1.0)
                return refused ("breakpoint " + index + ": time " + osc::formatDouble (point.t)
                                + " is outside 0..1 - a time is a fraction of the fade");

            /*  STRICTLY, not merely in order. Two breakpoints at one time are a
                jump drawn as if it were a curve, and a jump in level is a click. */
            if (! out.points.empty() && ! (point.t > out.points.back().t))
                return refused ("breakpoint " + index + ": time " + osc::formatDouble (point.t)
                                + " does not come after " + osc::formatDouble (out.points.back().t));

            if (level != nullptr && ! level->isInRange (point.levelDb))
                return refused ("breakpoint " + index + ": level " + osc::formatDouble (point.levelDb)
                                + " dB is outside " + rangeText (*level));

            out.points.push_back (point);
        }

        /*  THE WHOLE FADE, END TO END. Compared as "not above" and "not below"
            rather than with == so the strict preset's -Wfloat-equal has nothing
            to say; the range check above has already put both inside 0..1. */
        if (out.points.front().t > 0.0)
            return refused ("the first breakpoint is at " + osc::formatDouble (out.points.front().t)
                            + " - a curve starts at 0");

        if (out.points.back().t < 1.0)
            return refused ("the last breakpoint is at " + osc::formatDouble (out.points.back().t)
                            + " - a curve ends at 1");

        return out;
    }
}
