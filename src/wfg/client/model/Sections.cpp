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
#include <cmath>
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
            else if (name == "crossfade") row.crossfade = number (reading, 0.01);
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
            starts.push_back (at);
            at += section.length();
        }

        return starts;
    }

    double editedLength (const std::vector<SectionRow>& sections)
    {
        double length = 0.0;

        for (const auto& section : sections)
            length += section.length();

        return length;
    }

    bool continuousJoin (const SectionRow& before, const SectionRow& after) noexcept
    {
        return std::abs (before.out - after.in) < joinInstant;
    }

    double heardCrossfade (const std::vector<SectionRow>& sections, std::size_t index) noexcept
    {
        if (index == 0 || index >= sections.size())
            return 0.0;

        if (continuousJoin (sections[index - 1], sections[index]))
            return 0.0;

        return std::max (0.0, sections[index].crossfade);
    }

    double clampedCrossfade (const std::vector<SectionRow>& given, std::size_t index, double asked)
    {
        if (index >= given.size())
            return 0.0;

        /*  THE DOOR'S RULE, restated: the first section's is not heard and
            counts as nothing; a crossfade reaches back half its length before
            the in point, so twice that point bounds it; and a section's two
            halves must fit inside it, the larger shrinking first. */
        auto sections = given;
        sections[index].crossfade = asked;

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

        return sections[index].crossfade;
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

            out.push_back ({ i, x0, x1 });
        }

        return out;
    }

    SectionHitResult hitSection (const std::vector<SectionLayout>& layout, double x, double joinGrab) noexcept
    {
        for (const auto& block : layout)
            if (block.index > 0 && std::abs (x - block.x0) <= joinGrab)
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

        double at = 0.0;

        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto next = at + sections[i].length();

            if (editedSecond < next || (i + 1 == sections.size() && editedSecond == next))
                return SectionPlace { i, sections[i].in + (editedSecond - at) };

            at = next;
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
