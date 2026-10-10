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

#include "MediaEdit.h"

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace wfg::doc
{
    namespace
    {
        constexpr double sameSecond = 1.0e-4;   // two lane points the lane cannot tell apart

        bool same (double a, double b) noexcept
        {
            return std::abs (a - b) < sameInstant;
        }
    }

    //==============================================================================
    std::vector<double> sectionStarts (const std::vector<Section>& sections)
    {
        std::vector<double> starts;
        starts.reserve (sections.size());

        double at = 0.0;

        for (const auto& section : sections)
        {
            starts.push_back (at);
            at += section.length();
        }

        return starts;
    }

    double editedLength (const std::vector<Section>& sections)
    {
        double length = 0.0;

        for (const auto& section : sections)
            length += section.length();

        return length;
    }

    bool isContinuousJoin (const Section& before, const Section& after) noexcept
    {
        return same (before.out, after.in);
    }

    double crossfadeInto (const std::vector<Section>& sections, std::size_t index) noexcept
    {
        if (index == 0 || index >= sections.size())
            return 0.0;

        return std::max (0.0, sections[index].crossfade);
    }

    bool isIdentityEdit (const std::vector<Section>& sections, double sourceLength) noexcept
    {
        if (sections.empty())
            return true;

        if (sections.size() != 1)
            return false;

        const auto& only = sections.front();

        return same (only.in, 0.0) && same (only.out, sourceLength) && same (only.trimDb, 0.0);
    }

    std::string editText (const std::vector<Section>& sections)
    {
        std::string text;

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto& section = sections[i];

            text += osc::formatDouble (section.in) + ' ' + osc::formatDouble (section.out) + ' '
                  + osc::formatDouble (section.trimDb) + ' '
                  + osc::formatDouble (crossfadeInto (sections, i)) + ';';
        }

        return text;
    }

    std::optional<Place> placeOf (const std::vector<Section>& sections, double editedSecond) noexcept
    {
        if (sections.empty() || editedSecond < 0.0)
            return std::nullopt;

        double at = 0.0;

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto next = at + sections[i].length();

            if (editedSecond < next || (i + 1 == sections.size() && same (editedSecond, next)))
                return Place { i, sections[i].in + (editedSecond - at) };

            at = next;
        }

        return std::nullopt;
    }

    //==============================================================================
    namespace
    {
        /*  A stretch of a section's material, in seconds of the file. */
        struct Stretch
        {
            double from = 0.0;
            double to = 0.0;
        };

        /*  What is left of [in, out) once [a, b) is taken out of it: nothing,
            one stretch, or two. */
        std::vector<Stretch> remainder (double in, double out, const std::optional<Stretch>& taken)
        {
            if (! taken.has_value())
                return { { in, out } };

            std::vector<Stretch> left;

            if (taken->from > in)
                left.push_back ({ in, taken->from });

            if (taken->to < out)
                left.push_back ({ taken->to, out });

            return left;
        }
    }

    std::vector<TimeRun> timeMap (const std::vector<Section>& before, const std::vector<Section>& after)
    {
        /*  BY IDENTIFIER FIRST: a section kept by the edit is the same section,
            whatever happened to its edges or its place, and the material it has
            in both its old and its new extent is one run. THEN BY MATERIAL,
            among what that left: a split's second half is a new identifier over
            material the old section had, and a join's survivor holds material
            the section it swallowed had; both are the identity on the
            timeline and must read as such, so an old section's uncovered
            material is matched to a new section's uncovered material where
            the two are the same stretch of the file. */
        const auto oldStarts = sectionStarts (before);
        const auto newStarts = sectionStarts (after);

        std::map<std::string, std::size_t> placeAfter;

        for (std::size_t i = 0; i < after.size(); ++i)
            placeAfter.emplace (after[i].id, i);

        std::vector<TimeRun> runs;
        std::vector<std::optional<Stretch>> oldCovered (before.size());
        std::vector<std::optional<Stretch>> newCovered (after.size());

        for (std::size_t i = 0; i < before.size(); ++i)
        {
            const auto found = placeAfter.find (before[i].id);

            if (found == placeAfter.end())
                continue;

            const auto j = found->second;
            const auto& was = before[i];
            const auto& is = after[j];

            const auto a = std::max (was.in, is.in);
            const auto b = std::min (was.out, is.out);

            if (b > a)
            {
                runs.push_back ({ oldStarts[i] + (a - was.in), newStarts[j] + (a - is.in), b - a });
                oldCovered[i] = Stretch { a, b };
                newCovered[j] = Stretch { a, b };
            }
            else
            {
                oldCovered[i] = Stretch { was.in, was.in };   // matched, nothing shared: its material is gone
            }
        }

        for (std::size_t i = 0; i < before.size(); ++i)
        {
            for (const auto& left : remainder (before[i].in, before[i].out, oldCovered[i]))
            {
                for (std::size_t j = 0; j < after.size(); ++j)
                {
                    for (const auto& free : remainder (after[j].in, after[j].out, newCovered[j]))
                    {
                        const auto a = std::max (left.from, free.from);
                        const auto b = std::min (left.to, free.to);

                        if (b > a)
                            runs.push_back ({ oldStarts[i] + (a - before[i].in), newStarts[j] + (a - after[j].in), b - a });
                    }
                }
            }
        }

        std::sort (runs.begin(), runs.end(),
                   [] (const TimeRun& x, const TimeRun& y) { return x.oldFrom < y.oldFrom; });

        return runs;
    }

    std::vector<TimeRun> timeMapToFile (const std::vector<Section>& sections)
    {
        const auto starts = sectionStarts (sections);
        std::vector<TimeRun> runs;

        for (std::size_t i = 0; i < sections.size(); ++i)
            if (sections[i].length() > 0.0)
                runs.push_back ({ starts[i], sections[i].in, sections[i].length() });

        return runs;
    }

    std::optional<double> carrySecond (const std::vector<TimeRun>& runs, double second) noexcept
    {
        for (const auto& run : runs)
            if (second >= run.oldFrom && second < run.oldFrom + run.length)
                return run.newFrom + (second - run.oldFrom);

        /*  The end of a run, where no run begins: the last point of the
            material, which a strict "inside" would drop. */
        for (const auto& run : runs)
            if (same (second, run.oldFrom + run.length))
                return run.newFrom + run.length;

        return std::nullopt;
    }

    double carryToCut (const std::vector<TimeRun>& runs, double second) noexcept
    {
        const TimeRun* before = nullptr;
        const TimeRun* after = nullptr;

        for (const auto& run : runs)
        {
            if (run.oldFrom + run.length <= second)
            {
                if (before == nullptr || run.oldFrom + run.length > before->oldFrom + before->length)
                    before = &run;
            }
            else if (after == nullptr || run.oldFrom < after->oldFrom)
            {
                after = &run;
            }
        }

        if (before != nullptr)
            return before->newFrom + before->length;

        if (after != nullptr)
            return after->newFrom;

        return 0.0;
    }

    double carried (const std::vector<TimeRun>& runs, double second) noexcept
    {
        if (const auto kept = carrySecond (runs, second))
            return *kept;

        return carryToCut (runs, second);
    }

    //==============================================================================
    std::vector<LanePoint> carryLane (const std::vector<LanePoint>& points, const std::vector<TimeRun>& runs)
    {
        std::vector<LanePoint> moved;
        moved.reserve (points.size());

        for (const auto& point : points)
            if (const auto second = carrySecond (runs, point.seconds))
                moved.push_back ({ *second, point.levelDb });

        std::stable_sort (moved.begin(), moved.end(),
                          [] (const LanePoint& a, const LanePoint& b) { return a.seconds < b.seconds; });

        std::vector<LanePoint> climbing;
        climbing.reserve (moved.size());

        for (const auto& point : moved)
            if (climbing.empty() || point.seconds > climbing.back().seconds + sameSecond)
                climbing.push_back (point);

        return climbing;
    }

    std::string writeLaneText (const std::vector<LanePoint>& points)
    {
        std::string text;

        for (const auto& point : points)
        {
            if (! text.empty())
                text.push_back (' ');

            text += osc::formatDouble (point.seconds) + ' ' + osc::formatDouble (point.levelDb);
        }

        return text;
    }

    std::string carryLaneText (std::string_view text, const std::vector<TimeRun>& runs)
    {
        const auto lane = readLevelLane (text);

        if (! lane.problem.empty())
            return std::string (text);

        return writeLaneText (carryLane (lane.points, runs));
    }

    std::vector<CarriedRange> carryRanges (const std::vector<CarriedRange>& ranges, const std::vector<TimeRun>& runs)
    {
        std::vector<CarriedRange> out;
        out.reserve (ranges.size());

        for (const auto& range : ranges)
        {
            CarriedRange moved { range.id, carried (runs, range.in), carried (runs, range.out), false };
            moved.removed = ! (moved.out > moved.in + sameInstant);
            out.push_back (moved);
        }

        return out;
    }

    double carryStartOffset (double offset, const std::vector<TimeRun>& runs) noexcept
    {
        if (! (offset > 0.0))
            return 0.0;

        return std::max (0.0, carried (runs, offset));
    }

    //==============================================================================
    std::vector<Section> clampCrossfades (std::vector<Section> sections)
    {
        /*  The first section's crossfade is not heard and is left alone; for
            the pair constraint it counts as nothing. */
        const auto heard = [&sections] (std::size_t i) { return i == 0 ? 0.0 : std::max (0.0, sections[i].crossfade); };

        for (std::size_t i = 1; i < sections.size(); ++i)
            sections[i].crossfade = std::min (std::max (0.0, sections[i].crossfade), 2.0 * std::max (0.0, sections[i].in));

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto length = std::max (0.0, sections[i].length());
            const auto own = heard (i);
            const auto next = i + 1 < sections.size() ? heard (i + 1) : 0.0;

            if (own / 2.0 + next / 2.0 <= length)
                continue;

            /*  The larger shrinks to what the other leaves; if that is still
                too much the other goes down to the section's length. */
            if (own >= next)
            {
                sections[i].crossfade = std::max (0.0, 2.0 * (length - next / 2.0));

                if (i + 1 < sections.size() && next / 2.0 > length)
                    sections[i + 1].crossfade = 2.0 * length;
            }
            else
            {
                sections[i + 1].crossfade = std::max (0.0, 2.0 * (length - own / 2.0));

                if (i > 0 && own / 2.0 > length)
                    sections[i].crossfade = 2.0 * length;
            }
        }

        return sections;
    }

    std::string whyNotSections (const std::vector<Section>& sections, double trimLow, double trimHigh)
    {
        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto& section = sections[i];
            const auto here = "section " + std::to_string (i + 1);

            if (! std::isfinite (section.in) || ! std::isfinite (section.out)
                  || ! std::isfinite (section.trimDb) || ! std::isfinite (section.crossfade))
                return here + ": a value that is not a number";

            if (section.in < 0.0)
                return here + ": its in point is before the file's start";

            if (! (section.out > section.in))
                return here + ": its out point is not after its in point";

            if (section.trimDb < trimLow || section.trimDb > trimHigh)
                return here + ": a trim outside " + osc::formatDouble (trimLow) + ".." + osc::formatDouble (trimHigh) + " dB";

            if (section.crossfade < 0.0)
                return here + ": a crossfade below nought";
        }

        return {};
    }
}
