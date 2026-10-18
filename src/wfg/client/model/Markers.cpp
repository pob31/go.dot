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

#include <wfg/client/model/Markers.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/marker/";
    }

    std::vector<MarkerRow> readMarkers (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        /*  ONE PASS, gathering by identifier, as `rangesByCue` does: a marker
            whose `cue` has not arrived in this pass is nobody's and is not
            drawn over this file. */
        std::map<std::string, MarkerRow> found;
        std::map<std::string, std::string> ownerOf;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);
            const auto reading = text (node);

            auto& row = found[id];
            row.id = id;

            if (name == "cue")       ownerOf[id] = reading;
            else if (name == "name") row.name = reading;
            else if (name == "at")   row.at = osc::parseDouble (reading).value_or (0.0);
        }

        std::vector<MarkerRow> out;

        for (auto& [id, row] : found)
            if (const auto owner = ownerOf.find (id); owner != ownerOf.end() && owner->second == cueId)
                out.push_back (row);

        std::stable_sort (out.begin(), out.end(), [] (const MarkerRow& a, const MarkerRow& b) { return a.at < b.at; });
        return out;
    }

    std::string nextMarkerName (const std::vector<MarkerRow>& markers)
    {
        constexpr std::string_view word = "Marker ";
        auto highest = 0;

        for (const auto& marker : markers)
        {
            if (marker.name.rfind (word, 0) != 0)
                continue;

            const auto number = marker.name.substr (word.size());

            if (! number.empty() && number.size() < 7
                  && std::all_of (number.begin(), number.end(), [] (char c) { return c >= '0' && c <= '9'; }))
                highest = std::max (highest, std::stoi (number));
        }

        return std::string (word) + std::to_string (highest + 1);
    }

    std::vector<double> marksOf (const std::vector<MarkerRow>& markers, const std::vector<RangeRow>& ranges,
                                 const std::vector<SectionRow>& sections, const std::vector<double>& cuts,
                                 double startOffset, double length)
    {
        std::vector<double> all;

        if (length > 0.0)
        {
            all.push_back (0.0);
            all.push_back (length);
        }

        if (ranges.empty() && startOffset > 0.0)
            all.push_back (startOffset);

        for (const auto& range : ranges)
        {
            all.push_back (range.in);
            all.push_back (range.out);
        }

        const auto starts = sectionStarts (sections);

        for (std::size_t k = 0; k < starts.size() && k < sections.size(); ++k)
        {
            all.push_back (starts[k]);
            all.push_back (starts[k] + sections[k].length());
        }

        all.insert (all.end(), cuts.begin(), cuts.end());

        for (const auto& marker : markers)
            all.push_back (marker.at);

        std::sort (all.begin(), all.end());

        std::vector<double> marks;

        for (const auto at : all)
            if (std::isfinite (at) && at >= 0.0 && (marks.empty() || ! sameInstant (at, marks.back())))
                marks.push_back (at);

        return marks;
    }

    std::optional<std::pair<double, double>> intervalAround (const std::vector<double>& marks, double seconds)
    {
        /*  The first mark after the second; the one before that is its start. A
            second ON a mark is in the interval that begins there. */
        const auto after = std::upper_bound (marks.begin(), marks.end(), seconds);

        if (after == marks.begin() || after == marks.end())
            return std::nullopt;

        return std::make_pair (*(after - 1), *after);
    }

    std::pair<double, double> spanOver (std::pair<double, double> selection, std::pair<double, double> interval)
    {
        return { std::min (selection.first, interval.first), std::max (selection.second, interval.second) };
    }

    std::string spanRefusal (const std::vector<RangeRow>& ranges, double startOffset,
                             std::optional<double> start, std::optional<double> end)
    {
        if (start && end && ! (*end > *start + 0.001))
            return "the end would not be after the start";

        if (ranges.empty())
        {
            if (end && ! (*end > start.value_or (startOffset) + 0.001))
                return "the playhead is before where the cue starts - an end has to come after it";

            return {};
        }

        const auto left = std::any_of (ranges.begin(), ranges.end(), [&start, &end] (const RangeRow& range)
        {
            return (! start || range.out > *start + 0.001) && (! end || range.in < *end - 0.001);
        });

        if (left || (start && end))
            return {};

        return start ? "the playhead is past the end of the last slice - nothing would be left to play"
                     : "the playhead is before the first slice begins - nothing would be left to play";
    }

    int rangesAfterSlicing (const std::vector<RangeRow>& ranges, double from, double to, int count)
    {
        auto list = ranges;

        if (list.empty())
            list.push_back ({});    // the whole file, which holds the selection

        auto kept = 0;

        for (const auto& range : list)
        {
            const auto touched = ranges.empty() || (range.out > from + 0.001 && range.in < to - 0.001);

            if (! touched)
                ++kept;
            else if (ranges.empty() || (range.in < from - 0.001 && range.out > to + 0.001))
                kept += 2;
            else if (range.in < from - 0.001 || range.out > to + 0.001)
                ++kept;
        }

        return kept + count;
    }
}
