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

#include <wfg/engine/cue/SlotAnalysis.h>

#include <wfg/engine/cue/ShowWalk.h>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <optional>
#include <set>

namespace wfg::cue
{

    //==============================================================================
    void SlotAnalysis::ensureBuilt (const doc::ShowDocument& document,
                                    const std::map<std::string, double>* durations)
    {
        if (builtAt == document.revision() && builtWith == durations && builtAt != 0)
            return;

        builtAt = document.revision();
        builtWith = durations;
        ++rebuildCount;

        liveRanges.clear();
        intersections.clear();
        overlapText.clear();
        referenceText = document.warnings();

        const Reader read;
        Walk walk { read, durations };

        const auto lists = document.root().getChildWithName (juce::Identifier ("Lists"));

        for (const auto& list : lists)
            if (list.getType().toString() == "List")
                walk.visitList (list);

        /*  WHAT THE WALK LEFT, INDEXED ONCE.

            Every question below is asked per claim, and a show may have
            hundreds: "which cue is at row N of this list", "is there a stop cue
            after this one aimed at it", "where does this group end". Answered
            by scanning the walk each time, they are the same list read five
            hundred times over, which on a 500-cue show is the difference
            between an analysis that fits inside an edit and one somebody
            notices.
        */
        std::map<std::string, std::vector<std::string>> rowIds;
        std::map<std::string, std::vector<std::pair<int, std::string>>> stopsByList;

        for (const auto& entry : walk.placed)
        {
            auto& ids = rowIds[entry.list];

            if (ids.size() <= static_cast<std::size_t> (entry.row))
                ids.resize (static_cast<std::size_t> (entry.row) + 1);

            ids[static_cast<std::size_t> (entry.row)] = entry.id;

            if (entry.element == "Stop")
            {
                const auto target = read.text (entry.node, "stop", "target");

                if (! target.empty())
                    stopsByList[entry.list].push_back ({ entry.row, target });
            }
        }

        const auto idAtRow = [&rowIds] (const std::string& list, int row) -> std::string
        {
            const auto found = rowIds.find (list);

            if (found == rowIds.end() || row < 0
                 || static_cast<std::size_t> (row) >= found->second.size())
                return {};

            return found->second[static_cast<std::size_t> (row)];
        };

        /*  WHERE EVERY CLAIM ENDS, in the order the answers are looked for:
            a stop cue somebody wrote, then the innermost enclosing group that
            ends on its own, then the end of the list. */
        const auto releaseRowFor = [&walk, &stopsByList, &idAtRow] (const Placed& cue)
                                     -> std::pair<int, std::string>
        {
            std::optional<int> stopRow;

            const auto stops = stopsByList.find (cue.list);

            if (stops != stopsByList.end())
            {
                for (const auto& stop : stops->second)
                {
                    if (stop.first <= cue.row)
                        continue;

                    const auto aimedHere = stop.second == cue.id
                                            || std::find (cue.ancestors.begin(),
                                                          cue.ancestors.end(),
                                                          stop.second) != cue.ancestors.end();

                    if (aimedHere && (! stopRow.has_value() || stop.first < *stopRow))
                        stopRow = stop.first;
                }
            }

            if (stopRow.has_value())
                return { *stopRow, idAtRow (cue.list, *stopRow) };

            for (auto ancestor = cue.ancestors.rbegin();
                 ancestor != cue.ancestors.rend(); ++ancestor)
            {
                const auto found = walk.extents.find (*ancestor);

                if (found == walk.extents.end() || ! found->second.endsOnItsOwn)
                    continue;

                return { found->second.last, idAtRow (cue.list, found->second.last) };
            }

            const auto last = walk.listExtent.count (cue.list) != 0
                                ? walk.listExtent.at (cue.list) : cue.row;

            return { last, idAtRow (cue.list, last) };
        };

        for (const auto& cue : walk.placed)
        {
            if (cue.element != "Media" || ! read.flag (cue.node, "cue", "enabled"))
                continue;

            std::optional<std::pair<int, std::string>> release;

            for (const auto& child : cue.node)
            {
                const auto element = child.getType().toString();
                const auto isFeed = element == "Feed";
                const auto isInsert = element == "Insert";

                if (! isFeed && ! isInsert)
                    continue;

                const auto owner = isFeed ? "feed" : "insert";
                const auto slot = read.text (child, owner, isFeed ? "slot" : "channel");

                if (slot.empty())
                    continue;

                if (! release.has_value())
                    release = releaseRowFor (cue);

                SlotUse use;
                use.slot = slot;
                use.cue = cue.id;
                use.list = cue.list;
                use.firstRow = cue.row;
                use.lastRow = release->first;
                use.until = release->second;
                use.shared = read.flag (child, owner, "shared");
                use.timed = cue.timed;
                use.chain = cue.chain;
                use.from = cue.from;
                use.to = cue.to;

                liveRanges.push_back (use);
            }
        }

        /*  AND WHICH OF THEM CAN BE LIVE AT ONCE.

            Seconds where both ends of the pair are inside one chain, rows where
            they are in one list and not, and always where they are in two lists
            - two lists can be live at once and nothing orders their rows
            against each other.

            PAIRED WITHIN A SLOT AND NOT ACROSS THE SHOW, which is the whole
            difference between quadratic in the claims of one slot and quadratic
            in the claims of the show. Two cues on different slots were never
            going to be a pair, and comparing them to find that out is the work
            an index removes.
        */
        std::map<std::string, std::vector<std::size_t>> bySlot;

        for (std::size_t index = 0; index < liveRanges.size(); ++index)
            bySlot[liveRanges[index].slot].push_back (index);

        std::set<std::string> reported;

        for (const auto& perSlot : bySlot)
        {
            const auto& indices = perSlot.second;

            for (std::size_t a = 0; a < indices.size(); ++a)
            {
                for (std::size_t b = a + 1; b < indices.size(); ++b)
                {
                    const auto& first = liveRanges[indices[a]];
                    const auto& second = liveRanges[indices[b]];

                    if (first.cue == second.cue || first.shared || second.shared)
                        continue;

                    bool clashes = true;

                    if (first.timed && second.timed && first.chain == second.chain
                         && first.list == second.list)
                        clashes = first.from < second.to && second.from < first.to;
                    else if (first.list == second.list)
                        clashes = first.firstRow <= second.lastRow
                                    && second.firstRow <= first.lastRow;

                    if (! clashes)
                        continue;

                    /*  ONE PAIR IS ONE WARNING however many destinations carry
                        it: a cue with two feeds into one slot is a document
                        somebody should look at, not two sentences about the
                        same two cues. */
                    if (! reported.insert (perSlot.first + " " + first.cue
                                             + " " + second.cue).second)
                        continue;

                    intersections.push_back ({ perSlot.first, first.cue, second.cue });

                    overlapText.push_back (
                        "/Show/.../Slot[" + perSlot.first + "]: cues " + first.cue
                          + " and " + second.cue + " can both be holding it; mark either "
                          "Feed or Insert shared if that is meant");
                }
            }
        }
    }

    //==============================================================================
    std::string SlotAnalysis::usageOf (const std::string& slotId) const
    {
        std::string out;

        for (const auto& use : liveRanges)
        {
            if (use.slot != slotId)
                continue;

            if (! out.empty())
                out += ' ';

            out += use.cue + " " + use.until;
        }

        return out;
    }

    std::string SlotAnalysis::overlapsOf (const std::string& slotId) const
    {
        std::string out;

        for (const auto& clash : intersections)
        {
            if (clash.slot != slotId)
                continue;

            if (! out.empty())
                out += ' ';

            out += clash.first + " " + clash.second;
        }

        return out;
    }

    std::string SlotAnalysis::warningText() const
    {
        std::string out;

        for (const auto* list : { &referenceText, &overlapText })
            for (const auto& line : *list)
            {
                if (! out.empty())
                    out += '\n';

                out += line;
            }

        return out;
    }
}
