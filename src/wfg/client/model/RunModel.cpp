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

#include <algorithm>
#include <limits>
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

    bool RunRow::isWaiting() const noexcept
    {
        /*  The two states a run can be counted down in. They are the same
            question from where an operator sits - how long until this does the
            next thing - so they are drawn alike and only the state word says
            which end of the cue it is. */
        return state == "waiting" || state == "postWait";
    }

    /*  THE PANE IN THE ORDER THINGS HAPPENED, with each parent's children
        kept under it. A depth-first walk of the parent chains, siblings
        sorted by when they started and the not-yet-started last, which is
        where the next cue belongs.

        STABLE WITHIN A TIE, so two runs that a single GO started keep the
        order the engine made them in - which is the order their cues are
        written in, and the only tie-break that is not arbitrary. */
    std::vector<RunRow> inShowOrder (const std::vector<RunRow>& rows)
    {
        std::unordered_map<std::string, std::vector<std::size_t>> childrenOfRun;
        std::vector<std::size_t> roots;

        std::unordered_map<std::string, std::size_t> place;

        for (auto index = std::size_t { 0 }; index < rows.size(); ++index)
            place.emplace (rows[index].id, index);

        for (auto index = std::size_t { 0 }; index < rows.size(); ++index)
        {
            const auto parent = rows[index].parentRun;

            /*  A PARENT THAT IS NOT HERE MAKES THIS ROW A ROOT, rather than a
                row nobody walks to. It happens whenever a preparation is
                dropped and something under it is not - and a run left out of
                this pane is a run nobody can kill. */
            if (parent.empty() || place.count (parent) == 0)
                roots.push_back (index);
            else
                childrenOfRun[parent].push_back (index);
        }

        const auto byStart = [&rows] (std::size_t a, std::size_t b)
        {
            /*  Nought is "not started", which sorts LAST rather than first
                - the whole point of the ordering, since that is the next
                cue and the operator's eye goes to the bottom for it. */
            const auto when = [&rows] (std::size_t index)
            {
                return rows[index].started > 0 ? rows[index].started
                                               : std::numeric_limits<std::int64_t>::max();
            };

            return when (a) < when (b);
        };

        std::stable_sort (roots.begin(), roots.end(), byStart);

        for (auto& family : childrenOfRun)
            std::stable_sort (family.second.begin(), family.second.end(), byStart);

        std::vector<RunRow> out;
        out.reserve (rows.size());

        /*  Depth-first, and capped by the row count rather than trusted to
            terminate: a parent chain the engine never makes cyclic is
            still not something a client gets to ASSUME. */
        const auto walk = [&] (auto&& self, std::size_t index) -> void
        {
            if (out.size() >= rows.size())
                return;

            out.push_back (rows[index]);

            if (const auto kids = childrenOfRun.find (rows[index].id); kids != childrenOfRun.end())
                for (const auto child : kids->second)
                    self (self, child);
        };

        for (const auto root : roots)
            walk (walk, root);

        //  Anything a broken chain left out still gets drawn, index the end.
        return out.size() == rows.size() ? out : rows;
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

            /*  THE NUMBERS THE BARS ARE DRAWN FROM, beside the words the row
                already reads. Asked of the CUE for what the document decided -
                the file's length, the waits somebody wrote - and of the RUN for
                where it has got to, which is the split §4.10 makes everywhere
                else in this tree. */
            if (const auto now = osc::parseDouble (at (snapshot, id, "position")); now.has_value())
                row.seconds = *now;

            if (const auto left = osc::parseDouble (at (snapshot, id, "remaining")); left.has_value())
                row.remaining = *left;

            if (const auto began = osc::parseDouble (at (snapshot, id, "started")); began.has_value())
                row.started = static_cast<std::int64_t> (*began);

            row.parentRun = at (snapshot, id, "parent");

            if (! row.cueId.empty())
            {
                const auto cue = "/godot/cue/" + row.cueId + "/";

                row.file = text (snapshot, cue + "file");

                if (const auto len = osc::parseDouble (text (snapshot, cue + "duration"));
                    len.has_value())
                {
                    row.length = *len;
                }

                /*  The wait it is IN, which is the only one worth a bar: the
                    two are the same question from where an operator sits, and
                    the state node is what tells them apart. */
                const auto which = row.state == "postWait" ? "postWait" : "preWait";

                if (const auto total = osc::parseDouble (text (snapshot, cue + which));
                    total.has_value())
                {
                    row.waitTotal = *total;
                }
            }

            for (auto up = parentOf.find (id);
                 up != parentOf.end() && ! up->second.empty() && row.depth < deepestNesting;
                 up = parentOf.find (up->second))
            {
                ++row.depth;
            }

            /*  A PREPARATION IS DRAWN, ring and all. It was hidden for one
                build and the author asked for it back (2026-09-18: "I don't
                mind seeing the armed, preloaded cues ready to fire"): a voice
                reserved for the next cue is something an operator wants to
                see is there. */
            rows.push_back (std::move (row));
        }

        return inShowOrder (rows);
    }
}
