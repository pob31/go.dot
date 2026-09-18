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

#include <wfg/client/model/LoadToTime.h>

#include <wfg/client/model/Scrub.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace wfg::client::model
{
    std::vector<HistoryStep> readHistory (const std::string& text)
    {
        std::vector<HistoryStep> steps;
        std::size_t at = 0;

        while (at < text.size())
        {
            const auto space = text.find (' ', at);
            const auto word = text.substr (at, space == std::string::npos ? std::string::npos
                                                                            : space - at);
            at = space == std::string::npos ? text.size() : space + 1;

            /*  `<tick>:<cue>:<origin>`, and a word that is not that shape is
                skipped rather than read as half a step: the node is the
                engine's and a client that guessed at it would be inventing a
                thing the list never did. */
            const auto first = word.find (':');
            const auto last = word.rfind (':');

            if (first == std::string::npos || last == first || last + 1 >= word.size())
                continue;

            HistoryStep step;
            step.tick = std::strtoll (word.substr (0, first).c_str(), nullptr, 10);
            step.cue = word.substr (first + 1, last - first - 1);
            step.origin = word[last + 1];

            if (! step.cue.empty())
                steps.push_back (step);
        }

        return steps;
    }

    std::string originWord (char origin)
    {
        switch (origin)
        {
            case 'g': return "GO";
            case 'f': return "by name";
            case 't': return "trigger";
            default:  return std::string (1, origin);
        }
    }

    std::string LoadToTimeReading::nameOf (const std::string& cueId) const
    {
        const auto found = names.find (cueId);
        return found != names.end() && ! found->second.empty() ? found->second : cueId;
    }

    LoadToTimeReading readLoadToTime (const tree::TreeSnapshot& snapshot,
                                      const std::string& listId)
    {
        LoadToTimeReading reading;
        reading.listId = listId;

        if (listId.empty())
            return reading;

        const auto base = "/godot/list/" + listId + "/";
        reading.listName = text (snapshot, base + "name");
        reading.tick = snapshot.tick();
        reading.steps = readHistory (text (snapshot, base + "history"));

        /*  THE AIM, as `list/aim` spells it: the cue and the seconds with a
            space between, empty when nobody is pointing. */
        const auto aim = text (snapshot, base + "aim");
        const auto space = aim.rfind (' ');

        if (space != std::string::npos && space > 0)
        {
            if (const auto offset = osc::parseDouble (aim.substr (space + 1)); offset.has_value())
            {
                reading.aimed = true;
                reading.aimCue = aim.substr (0, space);
                reading.aimOffset = *offset;
            }
        }

        const auto nameFor = [&snapshot, &reading] (const std::string& cueId)
        {
            if (! cueId.empty() && reading.names.count (cueId) == 0)
                reading.names[cueId] = text (snapshot, "/godot/cue/" + cueId + "/name");
        };

        for (const auto& step : reading.steps)
            nameFor (step.cue);

        nameFor (reading.aimCue);

        /*  AND THE ANSWER, which is the one place in this window that parses
            JSON - for the reason the engine gives: a solve changes shape as
            the aim moves, and a subtree of nodes appearing and vanishing
            under a dragged finger would be one no client could subscribe to.
            Read through the engine's own reader, so a number here is the
            number the engine wrote. */
        const auto solved = json::parse (text (snapshot, base + "solve"));

        if (! solved.ok() || ! solved.value->isObject())
            return reading;

        const auto& answer = *solved.value;

        if (const auto* ok = answer.find ("ok"))
            reading.ok = ok->asBool();

        if (const auto* how = answer.find ("how"))
            reading.how = how->asString();

        if (const auto* instant = answer.find ("instant"))
            reading.instant = static_cast<std::int64_t> (instant->asNumber());

        if (const auto* standby = answer.find ("standby"))
        {
            reading.standby = standby->asString();
            nameFor (reading.standby);
        }

        if (const auto* runs = answer.find ("runs"); runs != nullptr && runs->isArray())
        {
            for (const auto& run : runs->asArray())
            {
                PlannedLine line;

                if (const auto* cue = run.find ("cue"))           line.cue = cue->asString();
                if (const auto* when = run.find ("when"))         line.when = when->asString();
                if (const auto* offset = run.find ("offset"))     line.offset = offset->asNumber();
                if (const auto* startsIn = run.find ("startsIn")) line.startsIn = startsIn->asNumber();

                nameFor (line.cue);
                reading.runs.push_back (line);
            }
        }

        if (const auto* confused = answer.find ("confused"); confused != nullptr && confused->isArray())
        {
            for (const auto& why : confused->asArray())
            {
                std::string line;

                if (const auto* reason = why.find ("why"))
                    line = reason->asString();

                if (const auto* cue = why.find ("cue"))
                {
                    nameFor (cue->asString());
                    line += " for " + reading.nameOf (cue->asString());
                }

                if (const auto* took = why.find ("took"))
                    line += ": " + took->asString();

                reading.confused.push_back (line);
            }
        }

        return reading;
    }

    std::vector<StepLine> stepsUnder (const std::vector<HistoryStep>& steps,
                                      const std::string& aimCue, std::int64_t instant)
    {
        std::vector<StepLine> lines;

        if (aimCue.empty())
            return lines;

        /*  THE AIMED CUE'S LATEST FIRING is the clock. Newest first, so the
            first match is the latest; everything before it in the list is
            after it in time. */
        std::size_t own = steps.size();

        for (std::size_t n = 0; n < steps.size(); ++n)
            if (steps[n].cue == aimCue)
            {
                own = n;
                break;
            }

        if (own == steps.size())
            return lines;

        const auto firedAt = steps[own].tick;

        for (std::size_t n = own; n-- > 0;)
        {
            StepLine line;
            line.cue = steps[n].cue;
            line.origin = steps[n].origin;
            line.offset = static_cast<double> (steps[n].tick - firedAt) / 50.0;
            line.undone = steps[n].tick > instant;
            lines.push_back (line);
        }

        return lines;
    }

    std::vector<Row> stepRows (const std::vector<StepLine>& lines, double aimOffset,
                               const std::map<std::string, std::string>& names, int depth)
    {
        std::vector<Row> rows;

        const auto nameOf = [&names] (const std::string& cueId)
        {
            const auto found = names.find (cueId);
            return found != names.end() && ! found->second.empty() ? found->second : cueId;
        };

        Row pointer;
        pointer.rowKind = RowKind::step;
        pointer.pointer = true;
        pointer.offset = aimOffset;
        pointer.name = offsetText (aimOffset);
        pointer.depth = depth;

        auto placed = false;

        for (const auto& line : lines)
        {
            if (! placed && line.offset > aimOffset)
            {
                rows.push_back (pointer);
                placed = true;
            }

            Row row;
            row.rowKind = RowKind::step;
            row.id = line.cue;
            row.name = nameOf (line.cue);
            row.number = originWord (line.origin);
            row.offset = line.offset;
            row.undone = line.undone;
            row.enabled = ! line.undone;
            row.depth = depth;
            rows.push_back (row);
        }

        if (! placed)
            rows.push_back (pointer);

        return rows;
    }

    std::string offsetText (double offset)
    {
        if (offset < 0.0)
            return "before";

        return "+" + clockText (offset);
    }

    std::string agoText (std::int64_t at, std::int64_t now)
    {
        const auto seconds = std::max<std::int64_t> (0, now - at) / 50;

        if (seconds < 60)
            return std::to_string (seconds) + " s ago";

        if (seconds < 3600)
            return std::to_string (seconds / 60) + " min ago";

        return std::to_string (seconds / 3600) + " h " + std::to_string ((seconds % 3600) / 60)
                 + " min ago";
    }
}
