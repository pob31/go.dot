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
#include <utility>

namespace wfg::cue
{
    namespace
    {
        /*  WHETHER TWO CLAIMS CAN BE LIVE AT ONCE, and whether that is a proof.

            ONE FUNCTION AND NOT TWO COPIES, because two callers ask it: the
            pairing below, which reports overlaps, and the per-cue marks, which
            answer the direct-out menu. Written twice they would disagree, and
            the disagreement would show as a menu saying FREE about a pair the
            warnings report.

            SECONDS WHERE BOTH ENDS OF BOTH ARE KNOWN, inside one chain of one
            list - and the `timed` guard is load-bearing rather than tidy.
            `from` and `to` are filled even when they mean nothing
            (`ShowWalk.h`), so an ungated compare would read two untimed cues as
            nought against nought and prove that nothing ever overlaps anything,
            which is the one direction this analysis may not be wrong in.

            PROVEN IS THE LOWER BOUNDS TOUCHING, never the upper ones: a claim's
            release row says where it is certainly BACK, not where it is
            certainly HELD. See the header - this is where the three-finite-cues
            -in-a-manual-group case would go wrong. */
        std::optional<Certainty> relate (const SlotUse& a, const SlotUse& b)
        {
            if (a.timed && b.timed && a.chain == b.chain && a.list == b.list)
                return (a.from < b.to && b.from < a.to)
                         ? std::optional<Certainty> (Certainty::proven)
                         : std::nullopt;

            /*  ACROSS LISTS IT IS ALWAYS POSSIBLE AND NEVER PROVEN. Two lists
                can be live at once, so there is something to warn about; and
                nothing orders their rows against each other, so there is no
                arithmetic to prove it with. */
            if (a.list != b.list)
                return Certainty::possible;

            if (a.firstRow <= b.mustLast && b.firstRow <= a.mustLast)
                return Certainty::proven;

            if (a.firstRow <= b.lastRow && b.firstRow <= a.lastRow)
                return Certainty::possible;

            return std::nullopt;
        }

        /*  WHICH BLOCKER TO NAME, when several are in the way of one cue on one
            output. The menu has one mark per row and room for one name, so this
            picks it rather than publishing them all - which on a 500-cue manual
            list would be a hundred thousand pairs.

            Proven beats possible, then same-list beats cross-list, then the
            NEAREST one at or before this cue's row. Nearest is the point: "may
            still be on it: Q34 Rain" is something to act on, where the first
            cue of the show would be true of the whole back half and mean
            nothing. */
        bool preferable (const OutMark& candidate, const SlotUse& theirs,
                         const OutMark& held, const SlotUse& heldSpan)
        {
            if (candidate.certainty != held.certainty)
                return candidate.certainty == Certainty::proven;

            if (theirs.list != heldSpan.list)
                return false;

            return theirs.firstRow > heldSpan.firstRow;
        }
    }

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
        marksByCue.clear();
        overlapText.clear();
        referenceText = document.warnings();

        const Reader read;
        Walk walk { read, durations };

        /*  WHICH BUSES ARE DIRECT OUTS, gathered once. A mix channel is never
            a claim - many cues arriving at one mix is what a mix is for - so
            the word has to be known before any claim is made, and it is a
            fact about the rig rather than about the cue. `Rack` holds
            channels and not buses, so it is skipped the way every other walk
            of `<Audio>` skips it. */
        std::map<std::string, bool> isDirectOut;

        if (const auto audioNode = document.root().getChildWithName (juce::Identifier ("Audio"));
            audioNode.isValid())
            for (const auto& bus : audioNode)
                if (bus.hasType ("Bus"))
                {
                    const auto word = read.text (bus, "bus", "kind");

                    isDirectOut[bus[juce::Identifier ("id")].toString().toStdString()]
                        = word.empty() || word == "direct";
                }

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

        /*  A SPAN PER ENABLED MEDIA CUE, claim or no claim. The lazy
            `releaseRowFor` this loop used to do was right while the only
            question was "do two claims overlap"; the menu asks a second one -
            "what is busy where THIS cue plays" - of a cue that may hold
            nothing at all, so every cue needs its range whether or not it
            uses it. */
        std::vector<std::pair<std::string, SlotUse>> spans;

        for (const auto& cue : walk.placed)
        {
            if (cue.element != "Media" || ! read.flag (cue.node, "cue", "enabled"))
                continue;

            const auto release = releaseRowFor (cue);

            SlotUse span;
            span.cue = cue.id;
            span.list = cue.list;
            span.firstRow = cue.row;
            span.lastRow = release.first;
            span.until = release.second;
            span.timed = cue.timed;
            span.chain = cue.chain;
            span.from = cue.from;
            span.to = cue.to;

            /*  THE LOWER BOUND, and the one line in this file that decides
                whether a menu tells the truth. A cue that ends on its own is
                over by the time a later manual step is reached (`Walk::
                unbounded`'s own words), so rows prove nothing about it and its
                certain extent is the row it starts on. One that never ends on
                its own is still going at every row up to its release, and that
                is a fact rather than a possibility. */
            const auto endless = walk.unbounded.find (cue.id);

            span.mustLast = (endless != walk.unbounded.end() && endless->second)
                              ? span.lastRow : span.firstRow;

            spans.push_back ({ cue.id, span });

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

                auto use = span;
                use.kind = ResourceKind::slot;
                use.slot = slot;
                use.shared = read.flag (child, owner, "shared");

                liveRanges.push_back (use);
            }

            /*  AND THE DIRECT OUT, which is an attribute rather than a child:
                a cue lands on one output or none. Only a direct out is a
                claim; a cue aimed at a mix channel is a document somebody
                should look at (`warnings()` says so) and not a contention. */
            const auto out = read.text (cue.node, "media", "directOut");

            if (! out.empty())
                if (const auto found = isDirectOut.find (out);
                    found != isDirectOut.end() && found->second)
                {
                    auto use = span;
                    use.kind = ResourceKind::directOut;
                    use.slot = out;
                    use.shared = read.flag (cue.node, "media", "sharedOut");

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

                    const auto how = relate (first, second);

                    if (! how.has_value())
                        continue;

                    /*  ONE PAIR IS ONE WARNING however many destinations carry
                        it: a cue with two feeds into one slot is a document
                        somebody should look at, not two sentences about the
                        same two cues. */
                    if (! reported.insert (perSlot.first + " " + first.cue
                                             + " " + second.cue).second)
                        continue;

                    intersections.push_back ({ first.kind, perSlot.first,
                                               first.cue, second.cue, *how });

                    /*  AND THE SENTENCE SAYS WHICH KIND IT IS ABOUT, because
                        the remedy differs: a slot is shared by marking a Feed
                        or an Insert, an output by marking the cue. A sentence
                        naming the wrong one sends somebody to the wrong panel. */
                    overlapText.push_back (
                        first.kind == ResourceKind::directOut
                          ? "/Show/Audio/Bus[" + perSlot.first + "]: cues " + first.cue
                              + " and " + second.cue + " can both be on it; they will sum, and "
                                "marking either cue's sharedOut says that was meant"
                          : "/Show/.../Slot[" + perSlot.first + "]: cues " + first.cue
                              + " and " + second.cue + " can both be holding it; mark either "
                                "Feed or Insert shared if that is meant");
                }
            }
        }

        /*  AND WHAT THE DIRECT-OUT MENU MARKS, per cue.

            THE OTHER HALF OF THE SAME ANALYSIS. The pairing above answers "are
            these two in each other's way"; this answers "what is in the way of
            THIS cue", which is the question somebody has when the menu is open
            - including for a cue that claims nothing yet, which is every cue
            before its output is picked.

            NOT FILTERED BY `shared`. That mark silences a complaint, and this
            is not one: which outputs carry sound while this cue plays is a
            fact, and a designer who said they meant the sharing did not stop
            it being true. `usageOf` has no `shared` test either, for the same
            reason, and these are its cue-side twin.

            INDEXED BY BUS so the inner loop is the claims on one output rather
            than every claim in the show, and capped at one mark per output so
            the published text cannot grow with the square of the cue count. */
        std::map<std::string, std::vector<std::size_t>> claimsByBus;

        for (std::size_t index = 0; index < liveRanges.size(); ++index)
            if (liveRanges[index].kind == ResourceKind::directOut)
                claimsByBus[liveRanges[index].slot].push_back (index);

        for (const auto& [cueId, span] : spans)
        {
            for (const auto& [busId, claims] : claimsByBus)
            {
                std::optional<OutMark> best;
                std::optional<SlotUse> bestSpan;

                for (const auto index : claims)
                {
                    const auto& theirs = liveRanges[index];

                    if (theirs.cue == cueId)
                        continue;

                    const auto how = relate (span, theirs);

                    if (! how.has_value())
                        continue;

                    const OutMark candidate { busId, theirs.cue, *how };

                    if (! best.has_value()
                         || preferable (candidate, theirs, *best, *bestSpan))
                    {
                        best = candidate;
                        bestSpan = theirs;
                    }
                }

                if (best.has_value())
                    marksByCue[cueId].push_back (*best);
            }
        }
    }

    //==============================================================================
    namespace
    {
        /** `<bus> <cue>` pairs, space-separated, for one certainty. */
        std::string markText (const std::vector<OutMark>& marks, Certainty wanted)
        {
            std::string out;

            for (const auto& mark : marks)
            {
                if (mark.certainty != wanted)
                    continue;

                if (! out.empty())
                    out += ' ';

                out += mark.bus + " " + mark.cue;
            }

            return out;
        }
    }

    std::string SlotAnalysis::busyOutsOf (const std::string& cueId) const
    {
        const auto found = marksByCue.find (cueId);

        return found == marksByCue.end() ? std::string {}
                                         : markText (found->second, Certainty::proven);
    }

    std::string SlotAnalysis::maybeOutsOf (const std::string& cueId) const
    {
        const auto found = marksByCue.find (cueId);

        return found == marksByCue.end() ? std::string {}
                                         : markText (found->second, Certainty::possible);
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
