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

#include <wfg/client/model/Sections.h>

#include <wfg/client/model/Lane.h>
#include <wfg/client/model/Ranges.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/section/";

        /*  A millisecond: the engine's `joinInstant`, what makes a join
            continuous. */
        constexpr double joinInstant = 0.001;

        double number (const std::string& reading, double fallback)
        {
            return osc::parseDouble (reading).value_or (fallback);
        }
    }

    std::vector<SectionRow> readSections (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        /*  ONE PASS, gathering by identifier, as the ranges are read: a
            `childrenOf` per row would walk the whole tree each time. */
        std::map<std::string, SectionRow> found;
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
            const auto reading = node->soleValue().has_value() && node->soleValue()->isString()
                                   ? node->soleValue()->getString()
                                   : node->soleValue().has_value() ? osc::formatDouble (node->soleValue()->asDouble())
                                                                   : std::string {};

            auto& row = found[id];
            row.id = id;

            if (name == "cue")            ownerOf[id] = reading;
            else if (name == "index")     row.index = static_cast<int> (number (reading, 0.0));
            else if (name == "in")        row.in = number (reading, 0.0);
            else if (name == "out")       row.out = number (reading, 0.0);
            else if (name == "trim")      row.trimDb = number (reading, 0.0);
            else if (name == "fadeIn")       row.fadeIn = number (reading, 0.01);
            else if (name == "fadeOut")      row.fadeOut = number (reading, 0.01);
            else if (name == "fadeInCurve")  row.fadeInCurve = number (reading, 0.0);
            else if (name == "fadeOutCurve") row.fadeOutCurve = number (reading, 0.0);
            else if (name == "gap")          row.gap = number (reading, 0.0);
        }

        std::vector<SectionRow> out;

        for (auto& [id, row] : found)
            if (const auto owner = ownerOf.find (id); owner != ownerOf.end() && owner->second == cueId)
                out.push_back (row);

        std::stable_sort (out.begin(), out.end(),
                          [] (const SectionRow& a, const SectionRow& b)
                          {
                              if (a.index != b.index)
                                  return a.index < b.index;

                              return a.id < b.id;
                          });

        return out;
    }

    std::vector<double> sectionStarts (const std::vector<SectionRow>& sections)
    {
        std::vector<double> starts;
        starts.reserve (sections.size());
        double at = 0.0;

        for (const auto& section : sections)
        {
            //  A gap under a millisecond is none, as a join says.
            if (std::isfinite (section.gap) && section.gap >= joinInstant)
                at += section.gap;

            starts.push_back (at);
            at += section.length();
        }

        return starts;
    }

    double editedLength (const std::vector<SectionRow>& sections)
    {
        if (sections.empty())
            return 0.0;

        return sectionStarts (sections).back() + sections.back().length();
    }

    bool isJoin (const std::vector<SectionRow>& sections, std::size_t index) noexcept
    {
        return index > 0 && index < sections.size() && ! (sections[index].gap >= joinInstant);
    }

    bool continuousJoin (const SectionRow& before, const SectionRow& after) noexcept
    {
        return std::abs (before.out - after.in) < joinInstant;
    }

    std::vector<SectionRow> clampFades (std::vector<SectionRow> sections)
    {
        const auto finite = [] (double value) { return std::isfinite (value) ? value : 0.0; };

        for (auto& section : sections)
        {
            section.fadeIn = std::max (0.0, finite (section.fadeIn));
            section.fadeOut = std::max (0.0, finite (section.fadeOut));
            section.fadeInCurve = std::clamp (finite (section.fadeInCurve), -1.0, 1.0);
            section.fadeOutCurve = std::clamp (finite (section.fadeOutCurve), -1.0, 1.0);
        }

        const auto plain = [&sections] (std::size_t k)
        {
            return isJoin (sections, k) && continuousJoin (sections[k - 1], sections[k]);
        };

        /*  THE DOOR'S RULE, restated: a fade in centred on a join reaches back
            half its length before the in point; what a section's two fades take
            of it must fit inside it, the larger shrinking first. */
        for (std::size_t k = 1; k < sections.size(); ++k)
            if (isJoin (sections, k) && ! plain (k))
                sections[k].fadeIn = std::min (sections[k].fadeIn, 2.0 * std::max (0.0, sections[k].in));

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

    std::vector<SectionFades> heardFades (const std::vector<SectionRow>& sections)
    {
        const auto held = clampFades (sections);
        std::vector<SectionFades> out (held.size());

        for (std::size_t k = 0; k < held.size(); ++k)
        {
            auto& fades = out[k];
            fades.inCentred = isJoin (held, k);
            fades.outCentred = isJoin (held, k + 1);
            fades.plainIn = fades.inCentred && continuousJoin (held[k - 1], held[k]);
            fades.plainOut = fades.outCentred && continuousJoin (held[k], held[k + 1]);
            fades.in = fades.plainIn ? 0.0 : held[k].fadeIn;
            fades.out = fades.plainOut ? 0.0 : held[k].fadeOut;
            fades.inCurve = held[k].fadeInCurve;
            fades.outCurve = held[k].fadeOutCurve;
        }

        return out;
    }

    double fadeGain (double progress, double curve, bool picture) noexcept
    {
        const auto p = std::clamp (std::isfinite (progress) ? progress : 0.0, 0.0, 1.0);
        const auto base = picture ? p : std::sin (p * 1.5707963267948966);
        return std::pow (base, std::exp2 (-std::clamp (std::isfinite (curve) ? curve : 0.0, -1.0, 1.0)));
    }

    //==============================================================================
    std::vector<SectionLayout> layoutSections (const std::vector<SectionRow>& sections, const View& view, int width)
    {
        std::vector<SectionLayout> out;
        const auto starts = sectionStarts (sections);

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto x0 = view.xForSeconds (starts[i], width);
            const auto x1 = view.xForSeconds (starts[i] + sections[i].length(), width);

            if (x1 < 0.0 || x0 > static_cast<double> (width))
                continue;

            out.push_back ({ i, x0, x1, isJoin (sections, i) });
        }

        return out;
    }

    SectionHitResult hitSection (const std::vector<SectionLayout>& layout, double x, double joinGrab) noexcept
    {
        for (const auto& block : layout)
            if (block.join && std::abs (x - block.x0) <= joinGrab)
                return { SectionHit::join, block.index };

        for (const auto& block : layout)
            if (x >= block.x0 && x < block.x1)
                return { SectionHit::block, block.index };

        return {};
    }

    int dropSlotFor (const std::vector<SectionRow>& sections, const View& view, int width, std::size_t dragged, double x)
    {
        if (dragged >= sections.size())
            return -1;

        const auto starts = sectionStarts (sections);
        int slot = 0;

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            if (i == dragged)
                continue;

            const auto middle = view.xForSeconds (starts[i] + sections[i].length() / 2.0, width);

            if (middle < x)
                ++slot;
        }

        return slot == static_cast<int> (dragged) ? -1 : slot;
    }

    double crossfadeFromDrag (double joinSeconds, double pointerSeconds) noexcept
    {
        return 2.0 * std::abs (pointerSeconds - joinSeconds);
    }

    //==============================================================================
    double fadeHandleSeconds (const std::vector<SectionRow>& given, std::size_t index, bool inSide)
    {
        if (index >= given.size())
            return 0.0;

        const auto sections = clampFades (given);
        const auto starts = sectionStarts (sections);
        const auto& section = sections[index];

        if (inSide)
            return starts[index] + (isJoin (sections, index) ? section.fadeIn / 2.0 : section.fadeIn);

        const auto end = starts[index] + section.length();
        return end - (isJoin (sections, index + 1) ? section.fadeOut / 2.0 : section.fadeOut);
    }

    double gripBand (int height) noexcept
    {
        return std::clamp (static_cast<double> (height) / 4.0, 6.0, 14.0);
    }

    GripHit hitGrip (const std::vector<SectionRow>& sections, const View& view, int width, int height, double x, double y,
                     double radius, bool volumeShown)
    {
        if (sections.empty() || width <= 0 || height <= 0)
            return {};

        const auto starts = sectionStarts (sections);
        const auto xOf = [&view, width] (double seconds) { return view.xForSeconds (seconds, width); };
        const auto band = gripBand (height);

        //  THE VOLUME, in the middle of each section at the height of its trim.
        if (volumeShown)
            for (std::size_t k = 0; k < sections.size(); ++k)
            {
                const auto middle = xOf (starts[k] + sections[k].length() / 2.0);
                const auto level = static_cast<double> (height) * (1.0 - laneHeightFor (sections[k].trimDb));

                if (std::abs (x - middle) <= radius && std::abs (y - level) <= radius)
                    return { Grip::volume, k };
            }

        GripHit best;
        auto nearest = radius;

        //  A FADE'S LENGTH, in the top band; two on one spot go by the side of the join.
        if (y <= band)
            for (std::size_t k = 0; k < sections.size(); ++k)
                for (const auto inSide : { true, false })
                {
                    const auto at = xOf (fadeHandleSeconds (sections, k, inSide));
                    const auto distance = std::abs (x - at);
                    const auto edge = xOf (inSide ? starts[k] : starts[k] + sections[k].length());
                    const auto onItsSide = inSide ? x >= edge - 0.5 : x <= edge + 0.5;

                    if (distance < nearest || (distance <= nearest && onItsSide))
                    {
                        nearest = distance;
                        best = { inSide ? Grip::fadeIn : Grip::fadeOut, k };
                    }
                }

        //  AN EDGE, at the foot; at a join the incoming section's, which rolls the cut.
        if (y >= static_cast<double> (height) - band)
            for (std::size_t k = 0; k < sections.size(); ++k)
            {
                const auto left = xOf (starts[k]);
                const auto right = xOf (starts[k] + sections[k].length());

                if (std::abs (x - left) < nearest || (std::abs (x - left) <= nearest && isJoin (sections, k)))
                {
                    nearest = std::abs (x - left);
                    best = { Grip::edgeIn, k };
                }

                if (std::abs (x - right) < nearest && ! isJoin (sections, k + 1))
                {
                    nearest = std::abs (x - right);
                    best = { Grip::edgeOut, k };
                }
            }

        return best;
    }

    double fadeFromHandle (const std::vector<SectionRow>& sections, std::size_t index, bool inSide, double seconds)
    {
        if (index >= sections.size())
            return 0.0;

        const auto starts = sectionStarts (sections);

        if (inSide)
        {
            const auto distance = seconds - starts[index];
            return std::max (0.0, isJoin (sections, index) ? 2.0 * distance : distance);
        }

        const auto distance = starts[index] + sections[index].length() - seconds;
        return std::max (0.0, isJoin (sections, index + 1) ? 2.0 * distance : distance);
    }

    double edgeFromHandle (const std::vector<SectionRow>& sections, std::size_t index, bool inSide, double seconds,
                           double fileLength)
    {
        if (index >= sections.size())
            return 0.0;

        const auto starts = sectionStarts (sections);
        const auto& self = sections[index];
        const auto asked = self.in + (seconds - starts[index]);
        auto low = 0.0;
        auto high = std::numeric_limits<double>::max();

        if (inSide)
        {
            high = self.out - joinInstant;

            if (isJoin (sections, index))
                low = std::max (0.0, self.in - (sections[index - 1].length() - joinInstant));
            else
                low = std::max (0.0, self.in - (self.gap >= joinInstant ? self.gap : 0.0));
        }
        else
        {
            low = self.in + joinInstant;

            if (index + 1 < sections.size() && isJoin (sections, index + 1))
            {
                const auto& after = sections[index + 1];
                low = std::max (low, self.out - after.in);
                high = self.out + (after.length() - joinInstant);
            }
            else if (index + 1 < sections.size())
            {
                const auto& after = sections[index + 1];
                high = self.out + (after.gap >= joinInstant ? after.gap : 0.0);
            }
            else if (fileLength > 0.0)
            {
                high = std::max (self.out, fileLength);
            }
        }

        return std::clamp (asked, low, std::max (low, high));
    }

    std::vector<SectionRow> withEdge (std::vector<SectionRow> sections, std::size_t index, bool inSide, double fileSeconds)
    {
        if (index >= sections.size())
            return sections;

        auto& self = sections[index];

        if (inSide)
        {
            const auto delta = fileSeconds - self.in;

            if (isJoin (sections, index))
                sections[index - 1].out += delta;
            else
            {
                const auto gap = (self.gap >= joinInstant ? self.gap : 0.0) + delta;
                self.gap = gap < joinInstant ? 0.0 : gap;
            }

            self.in = fileSeconds;
        }
        else
        {
            const auto delta = fileSeconds - self.out;

            if (index + 1 < sections.size() && isJoin (sections, index + 1))
                sections[index + 1].in += delta;
            else if (index + 1 < sections.size())
            {
                auto& after = sections[index + 1];
                const auto gap = (after.gap >= joinInstant ? after.gap : 0.0) - delta;
                after.gap = gap < joinInstant ? 0.0 : gap;
            }

            self.out = fileSeconds;
        }

        return clampFades (std::move (sections));
    }

    std::vector<SectionRow> withFade (std::vector<SectionRow> sections, std::size_t index, bool inSide, double seconds, bool alone)
    {
        if (index >= sections.size())
            return sections;

        auto& own = inSide ? sections[index].fadeIn : sections[index].fadeOut;
        const auto delta = std::max (0.0, seconds) - own;
        own = std::max (0.0, seconds);

        if (! alone)
        {
            if (inSide && isJoin (sections, index))
                sections[index - 1].fadeOut = std::max (0.0, sections[index - 1].fadeOut + delta);
            else if (! inSide && isJoin (sections, index + 1))
                sections[index + 1].fadeIn = std::max (0.0, sections[index + 1].fadeIn + delta);
        }

        return clampFades (std::move (sections));
    }

    std::vector<SectionRow> withCurve (std::vector<SectionRow> sections, std::size_t index, bool inSide, double curve, bool alone)
    {
        if (index >= sections.size())
            return sections;

        auto& own = inSide ? sections[index].fadeInCurve : sections[index].fadeOutCurve;
        const auto value = std::clamp (curve, -1.0, 1.0);
        const auto delta = value - own;
        own = value;

        if (! alone)
        {
            if (inSide && isJoin (sections, index))
                sections[index - 1].fadeOutCurve = std::clamp (sections[index - 1].fadeOutCurve + delta, -1.0, 1.0);
            else if (! inSide && isJoin (sections, index + 1))
                sections[index + 1].fadeInCurve = std::clamp (sections[index + 1].fadeInCurve + delta, -1.0, 1.0);
        }

        return sections;
    }

    std::pair<double, double> placeLimits (const std::vector<SectionRow>& sections, std::size_t index)
    {
        if (index >= sections.size())
            return { 0.0, 0.0 };

        const auto starts = sectionStarts (sections);
        const auto before = index == 0 ? 0.0 : starts[index - 1] + sections[index - 1].length();
        const auto after = index + 1 < sections.size() ? starts[index + 1] - sections[index].length()
                                                       : std::numeric_limits<double>::max();
        return { before, std::max (before, after) };
    }

    std::vector<SectionRow> withPlace (std::vector<SectionRow> sections, std::size_t index, double seconds)
    {
        if (index >= sections.size())
            return sections;

        const auto starts = sectionStarts (sections);
        const auto length = sections[index].length();
        const auto before = index == 0 ? 0.0 : starts[index - 1] + sections[index - 1].length();
        const auto hasNext = index + 1 < sections.size();
        const auto after = hasNext ? starts[index + 1] : 0.0;

        auto start = std::max (before, seconds);

        if (hasNext)
            start = std::min (start, after - length);

        if (start - before < joinInstant)
            start = before;
        else if (hasNext && after - (start + length) < joinInstant)
            start = after - length;

        const auto gap = start - before;
        sections[index].gap = gap < joinInstant ? 0.0 : gap;

        if (hasNext)
        {
            const auto next = std::max (0.0, after - (start + length));
            sections[index + 1].gap = next < joinInstant ? 0.0 : next;
        }

        return clampFades (std::move (sections));
    }

    std::optional<std::pair<std::size_t, bool>> fadeAt (const std::vector<SectionRow>& given, double seconds, double slack)
    {
        const auto sections = clampFades (given);
        const auto starts = sectionStarts (sections);

        for (std::size_t k = 0; k < sections.size(); ++k)
        {
            const auto start = starts[k];
            const auto end = start + sections[k].length();

            //  The fade in: from half before a join, or from the edge, to its handle.
            const auto inFrom = isJoin (sections, k) ? start - sections[k].fadeIn / 2.0 : start;
            const auto inTo = fadeHandleSeconds (sections, k, true);

            if (seconds >= std::min (inFrom, start - slack) && seconds <= std::max (inTo, start + slack)
                  && (! isJoin (sections, k) || seconds >= start))
                return std::make_pair (k, true);

            const auto outFrom = fadeHandleSeconds (sections, k, false);
            const auto outTo = isJoin (sections, k + 1) ? end + sections[k].fadeOut / 2.0 : end;

            if (seconds >= std::min (outFrom, end - slack) && seconds <= std::max (outTo, end + slack)
                  && (! isJoin (sections, k + 1) || seconds < end))
                return std::make_pair (k, false);
        }

        return std::nullopt;
    }

    std::optional<std::size_t> sectionAt (const std::vector<SectionRow>& sections, double seconds)
    {
        const auto starts = sectionStarts (sections);

        for (std::size_t k = 0; k < sections.size(); ++k)
            if (seconds >= starts[k] && seconds < starts[k] + sections[k].length())
                return k;

        return std::nullopt;
    }

    //==============================================================================
    std::string trimText (double dB)
    {
        const auto text = osc::formatDouble (std::round (dB * 10.0) / 10.0);
        return (dB > 0.0 ? "+" + text : text) + " dB";
    }

    std::optional<double> trimFrom (const std::string& typed)
    {
        return levelFrom (typed);
    }

    std::string sectionLabel (const SectionRow& section, std::size_t index)
    {
        return std::to_string (index + 1) + "  " + timeText (section.in) + "\xE2\x80\x93" + timeText (section.out);
    }

    //==============================================================================
    EditRenderRow readEditRender (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        EditRenderRow row;
        const auto* node = snapshot.find ("/godot/engine/editRender");

        if (node == nullptr || ! node->soleValue().has_value() || ! node->soleValue()->isString())
            return row;

        const auto all = node->soleValue()->getString();
        std::string::size_type from = 0;

        while (from <= all.size())
        {
            const auto end = all.find ('\n', from);
            const auto line = all.substr (from, end == std::string::npos ? std::string::npos : end - from);

            std::vector<std::string> fields;
            std::string::size_type at = 0;

            while (true)
            {
                const auto tab = line.find ('\t', at);
                fields.push_back (line.substr (at, tab == std::string::npos ? std::string::npos : tab - at));

                if (tab == std::string::npos)
                    break;

                at = tab + 1;
            }

            if (fields.size() >= 4 && fields[0] == cueId)
            {
                row.cue = fields[0];
                row.state = fields[1];
                row.percent = static_cast<int> (number (fields[2], 0.0));
                row.problem = fields[3];
                return row;
            }

            if (end == std::string::npos)
                break;

            from = end + 1;
        }

        return row;
    }

    std::string renderWords (const EditRenderRow& row)
    {
        if (row.state == "rendering")
            return "rendering the edit, " + std::to_string (row.percent) + " %";

        if (row.state == "done")
            return row.problem.empty() ? std::string ("the edit is rendered")
                                       : "the edit is rendered, but " + row.problem;

        if (row.state == "failed")
            return "the edit could not be rendered" + (row.problem.empty() ? std::string {} : ": " + row.problem);

        return {};
    }

    //==============================================================================
    std::optional<SectionPlace> placeOf (const std::vector<SectionRow>& sections, double editedSecond) noexcept
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

            //  The end itself is the last section's: the same bits, as the engine asks it.
            const auto atTheEnd = i + 1 == sections.size()
                                    && std::bit_cast<std::uint64_t> (editedSecond) == std::bit_cast<std::uint64_t> (next);

            if (editedSecond < next || atTheEnd)
                return SectionPlace { i, sections[i].in + (editedSecond - at) };
        }

        return std::nullopt;
    }

    std::vector<double> cutsOnTimeline (const std::vector<SectionRow>& sections, const std::vector<double>& fileCuts)
    {
        if (sections.empty())
            return fileCuts;

        std::vector<double> out;
        const auto starts = sectionStarts (sections);

        for (std::size_t j = 0; j < sections.size(); ++j)
            for (const auto cut : fileCuts)
                if (cut > sections[j].in && cut < sections[j].out)
                    out.push_back (starts[j] + (cut - sections[j].in));

        std::sort (out.begin(), out.end());
        return out;
    }
}
