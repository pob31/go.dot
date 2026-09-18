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

#include <wfg/client/model/RunModel.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <unordered_map>

namespace wfg::client::model
{
    namespace
    {
        /*  A run's parent chain cannot be longer than the show is deep, but a
            client reading a tree it did not build does not get to assume that:
            the same cap the cue list uses, for the same reason. */
        constexpr int deepestNesting = 64;

        std::string at (const tree::TreeSnapshot& snapshot,
                        const std::string& runId, const char* name)
        {
            return text (snapshot, "/godot/run/" + runId + "/" + name);
        }

        /*  Seconds with one decimal, written by hand rather than through a
            locale - the same rule the cue list's time columns follow, so the
            two panes spell a number the same way. */
        std::string seconds (const std::string& raw)
        {
            if (raw.empty())
                return {};

            const auto value = osc::parseDouble (raw);

            if (! value.has_value())
                return {};

            const auto tenths = static_cast<long long> (*value * 10.0
                                                          + (*value < 0 ? -0.5 : 0.5));
            const auto whole = tenths / 10;
            const auto fraction = tenths % 10;

            return std::to_string (whole) + "." + std::to_string (fraction < 0 ? -fraction : fraction);
        }
    }

    std::string RunRow::mark() const
    {
        if (state == "playing") return "▶";    // a filled triangle: sounding
        if (state == "armed")   return "○";    // a ring: ready, not let go

        return {};
    }

    bool RunRow::launched() const noexcept
    {
        /*  The four states before a launch, named rather than inferred: a run
            that has not been let go has no position to report, and drawing the
            zero it publishes would be drawing a time nothing happened at. */
        return state != "preparing" && state != "waiting"
            && state != "armed" && state != "failed";
    }

    std::vector<RunRow> readRuns (const tree::TreeSnapshot& snapshot)
    {
        const auto order = words (text (snapshot, "/godot/run/order"));

        std::vector<RunRow> rows;
        rows.reserve (order.size());

        //  Parents first, so a depth can be walked without re-reading the tree.
        std::unordered_map<std::string, std::string> parentOf;

        for (const auto& id : order)
            parentOf.emplace (id, at (snapshot, id, "parent"));

        for (const auto& id : order)
        {
            RunRow row;
            row.id = id;
            row.cueId = at (snapshot, id, "cue");
            row.kind = at (snapshot, id, "kind");
            row.state = at (snapshot, id, "state");
            row.error = at (snapshot, id, "error");
            row.round = at (snapshot, id, "round");
            row.pruned = ! at (snapshot, id, "pruned").empty()
                      && at (snapshot, id, "pruned") != "false";
            row.asserted = flag (snapshot, "/godot/run/" + id + "/asserted") == Flag::yes;

            if (const auto late = osc::parseDouble (at (snapshot, id, "late")); late.has_value())
                row.late = static_cast<int> (*late);

            /*  THE CUE'S NAME, not the run's identifier: a run id is eight
                characters the engine drew and nobody recognises, and what an
                operator is looking for in this pane is which cue that is. */
            if (! row.cueId.empty())
                row.cueName = text (snapshot, "/godot/cue/" + row.cueId + "/name");

            if (row.launched())
                row.position = seconds (at (snapshot, id, "position"));

            for (auto up = parentOf.find (id);
                 up != parentOf.end() && ! up->second.empty() && row.depth < deepestNesting;
                 up = parentOf.find (up->second))
            {
                ++row.depth;
            }

            rows.push_back (std::move (row));
        }

        return rows;
    }
}
