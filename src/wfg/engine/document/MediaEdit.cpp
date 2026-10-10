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

        constexpr double halfPi = 1.5707963267948966;

        double finiteOr (double value, double otherwise) noexcept
        {
            return std::isfinite (value) ? value : otherwise;
        }
    }

    Section sectionFromNode (const juce::ValueTree& node)
    {
        Section section;
        section.id = node[juce::Identifier ("id")].toString().toStdString();
        section.in = static_cast<double> (node.getProperty ("in", 0.0));
        section.out = static_cast<double> (node.getProperty ("out", 0.0));
        section.trimDb = static_cast<double> (node.getProperty ("trim", 0.0));
        section.fadeIn = static_cast<double> (node.getProperty ("fadeIn", defaultFade));
        section.fadeOut = static_cast<double> (node.getProperty ("fadeOut", defaultFade));
        section.fadeInCurve = static_cast<double> (node.getProperty ("fadeInCurve", 0.0));
        section.fadeOutCurve = static_cast<double> (node.getProperty ("fadeOutCurve", 0.0));
        section.gap = static_cast<double> (node.getProperty ("gap", 0.0));
        return section;
    }

    //==============================================================================
    std::vector<double> sectionStarts (const std::vector<Section>& sections)
    {
        std::vector<double> starts;
        starts.reserve (sections.size());

        double at = 0.0;

        for (const auto& section : sections)
        {
            //  A gap under a millisecond is none, as a join says (isJoin).
            if (finiteOr (section.gap, 0.0) >= sameInstant)
                at += section.gap;

            starts.push_back (at);
            at += section.length();
        }

        return starts;
    }

    double editedLength (const std::vector<Section>& sections)
    {
        if (sections.empty())
            return 0.0;

        return sectionStarts (sections).back() + sections.back().length();
    }

    bool isJoin (const std::vector<Section>& sections, std::size_t index) noexcept
    {
        return index > 0 && index < sections.size() && ! (finiteOr (sections[index].gap, 0.0) >= sameInstant);
    }

    bool isContinuousJoin (const Section& before, const Section& after) noexcept
    {
        return same (before.out, after.in);
    }

    //==============================================================================
    std::vector<Fades> heardFades (const std::vector<Section>& sections)
    {
        const auto held = clampFades (sections);
        std::vector<Fades> out (held.size());

        for (std::size_t k = 0; k < held.size(); ++k)
        {
            auto& fades = out[k];
            const auto joinIn = isJoin (held, k);
            const auto joinOut = isJoin (held, k + 1);

            fades.inCentred = joinIn;
            fades.outCentred = joinOut;
            fades.plainIn = joinIn && isContinuousJoin (held[k - 1], held[k]);
            fades.plainOut = joinOut && isContinuousJoin (held[k], held[k + 1]);
            fades.in = fades.plainIn ? 0.0 : held[k].fadeIn;
            fades.out = fades.plainOut ? 0.0 : held[k].fadeOut;
            fades.inCurve = held[k].fadeInCurve;
            fades.outCurve = held[k].fadeOutCurve;
        }

        return out;
    }

    double fadeGain (double progress, double curve, bool picture) noexcept
    {
        const auto p = std::clamp (finiteOr (progress, 0.0), 0.0, 1.0);
        const auto base = picture ? p : std::sin (p * halfPi);
        return std::pow (base, std::exp2 (-std::clamp (finiteOr (curve, 0.0), -1.0, 1.0)));
    }

    double heardFrom (double start, const Fades& fades) noexcept
    {
        return start - (fades.inCentred ? fades.in / 2.0 : 0.0);
    }

    double heardTo (double start, double length, const Fades& fades) noexcept
    {
        return start + length + (fades.outCentred ? fades.out / 2.0 : 0.0);
    }

    double fadeWeight (const Fades& fades, double start, double length, double t, bool picture) noexcept
    {
        auto weight = 1.0;

        if (fades.in > 0.0)
        {
            const auto from = fades.inCentred ? start - fades.in / 2.0 : start;

            if (t < from)
                return 0.0;

            if (t < from + fades.in)
                weight *= fadeGain ((t - from) / fades.in, fades.inCurve, picture);
        }
        else if (t < start)
        {
            return 0.0;
        }

        const auto end = start + length;

        if (fades.out > 0.0)
        {
            const auto from = fades.outCentred ? end - fades.out / 2.0 : end - fades.out;

            if (t >= from + fades.out)
                return 0.0;

            if (t >= from)
                weight *= fadeGain (1.0 - (t - from) / fades.out, fades.outCurve, picture);
        }
        else if (t >= end)
        {
            return 0.0;
        }

        return weight;
    }

    bool isIdentityEdit (const std::vector<Section>& sections, double sourceLength) noexcept
    {
        if (sections.empty())
            return true;

        if (sections.size() != 1)
            return false;

        const auto& only = sections.front();
        const auto heard = heardFades (sections).front();

        return same (only.in, 0.0) && same (only.out, sourceLength) && same (only.trimDb, 0.0)
            && ! (only.gap >= sameInstant) && ! (heard.in > 0.0) && ! (heard.out > 0.0);
    }

    std::string editText (const std::vector<Section>& sections)
    {
        std::string text;
        const auto heard = heardFades (sections);
        const auto starts = sectionStarts (sections);
        auto end = 0.0;

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto& section = sections[i];
            const auto& fades = heard[i];

            //  The gap as the timeline has it.
            const auto gap = starts[i] - end;

            text += osc::formatDouble (section.in) + ' ' + osc::formatDouble (section.out) + ' '
                  + osc::formatDouble (section.trimDb) + ' ' + osc::formatDouble (gap) + ' '
                  + osc::formatDouble (fades.in) + ' ' + osc::formatDouble (fades.out) + ' '
                  + osc::formatDouble (fades.in > 0.0 ? fades.inCurve : 0.0) + ' '
                  + osc::formatDouble (fades.out > 0.0 ? fades.outCurve : 0.0) + ';';

            end = starts[i] + section.length();
        }

        return text;
    }

    std::optional<Place> placeOf (const std::vector<Section>& sections, double editedSecond) noexcept
    {
        if (sections.empty() || editedSecond < 0.0)
            return std::nullopt;

        const auto starts = sectionStarts (sections);

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto at = starts[i];
            const auto next = at + sections[i].length();

            //  In the silence before it: no place in the file.
            if (editedSecond < at)
                return std::nullopt;

            if (editedSecond < next || (i + 1 == sections.size() && same (editedSecond, next)))
                return Place { i, sections[i].in + (editedSecond - at) };
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

            /*  THE SILENCE BEFORE IT (55.9), lined up by how far its material
                moved: a trim into the silence leaves the silence where it was,
                a ripple carries it with the section. What the old gap and the
                new have in common, so lined up, is one run. */
            const auto moved = (newStarts[j] - is.in) - (oldStarts[i] - was.in);
            const auto gapOf = [] (const Section& s) { return finiteOr (s.gap, 0.0) >= sameInstant ? s.gap : 0.0; };
            const auto oldGapFrom = oldStarts[i] - gapOf (was) + moved;
            const auto newGapFrom = newStarts[j] - gapOf (is);
            const auto gapFrom = std::max (oldGapFrom, newGapFrom);
            const auto gapTo = std::min (oldStarts[i] + moved, newStarts[j]);

            if (gapTo > gapFrom)
                runs.push_back ({ gapFrom - moved, gapFrom, gapTo - gapFrom });
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
    std::vector<Section> clampFades (std::vector<Section> sections)
    {
        for (auto& section : sections)
        {
            section.fadeIn = std::max (0.0, finiteOr (section.fadeIn, 0.0));
            section.fadeOut = std::max (0.0, finiteOr (section.fadeOut, 0.0));
            section.fadeInCurve = std::clamp (finiteOr (section.fadeInCurve, 0.0), -1.0, 1.0);
            section.fadeOutCurve = std::clamp (finiteOr (section.fadeOutCurve, 0.0), -1.0, 1.0);
        }

        const auto plain = [&sections] (std::size_t k)
        {
            return isJoin (sections, k) && isContinuousJoin (sections[k - 1], sections[k]);
        };

        /*  A FADE IN CENTRED ON A JOIN reaches back half its length before
            the in point: nothing is there before the file's start. */
        for (std::size_t k = 1; k < sections.size(); ++k)
            if (isJoin (sections, k) && ! plain (k))
                sections[k].fadeIn = std::min (sections[k].fadeIn, 2.0 * std::max (0.0, sections[k].in));

        /*  WHAT THE TWO TAKE OF THE SECTION must fit inside it: half of a
            centred fade, all of one inside, nothing of one not heard. The
            larger shrinks to what the other leaves; if that is still too much
            the other goes down to the section's length. */
        for (std::size_t k = 0; k < sections.size(); ++k)
        {
            auto& section = sections[k];
            const auto length = std::max (0.0, section.length());
            const auto inShare = plain (k) ? 0.0 : isJoin (sections, k) ? 0.5 : 1.0;
            const auto outShare = plain (k + 1) ? 0.0 : isJoin (sections, k + 1) ? 0.5 : 1.0;
            auto takenIn = section.fadeIn * inShare;
            auto takenOut = section.fadeOut * outShare;

            if (takenIn + takenOut <= length)
                continue;

            if (takenIn >= takenOut)
            {
                takenOut = std::min (takenOut, length);
                takenIn = std::max (0.0, length - takenOut);
            }
            else
            {
                takenIn = std::min (takenIn, length);
                takenOut = std::max (0.0, length - takenIn);
            }

            if (inShare > 0.0)
                section.fadeIn = takenIn / inShare;

            if (outShare > 0.0)
                section.fadeOut = takenOut / outShare;
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
                  || ! std::isfinite (section.trimDb) || ! std::isfinite (section.fadeIn)
                  || ! std::isfinite (section.fadeOut) || ! std::isfinite (section.fadeInCurve)
                  || ! std::isfinite (section.fadeOutCurve) || ! std::isfinite (section.gap))
                return here + ": a value that is not a number";

            if (section.in < 0.0)
                return here + ": its in point is before the file's start";

            if (! (section.out > section.in))
                return here + ": its out point is not after its in point";

            if (section.trimDb < trimLow || section.trimDb > trimHigh)
                return here + ": a trim outside " + osc::formatDouble (trimLow) + ".." + osc::formatDouble (trimHigh) + " dB";

            if (section.fadeIn < 0.0 || section.fadeOut < 0.0)
                return here + ": a fade below nought";

            if (section.gap < 0.0)
                return here + ": a gap below nought";

            if (std::abs (section.fadeInCurve) > 1.0 || std::abs (section.fadeOutCurve) > 1.0)
                return here + ": a curve outside -1..1";
        }

        return {};
    }
}
