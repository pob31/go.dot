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

#include <wfg/engine/cue/Solver.h>

#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>

#include <algorithm>
#include <cmath>

namespace wfg::cue
{
    namespace
    {
        /*  What a media cue's own material lasts, or nothing when the document
            does not say. The same arithmetic the walk does, asked of one cue
            rather than of a chain. */
        std::optional<double> materialOf (const Reader& read, const juce::ValueTree& cue,
                                          const std::map<std::string, double>* durations)
        {
            double total = 0.0;
            auto anyRange = false;

            for (const auto& child : cue)
            {
                if (child.getType().toString() != "Range")
                    continue;

                anyRange = true;

                const auto passes = read.integer (child, "range", "loops");

                if (passes <= 0)
                    return std::nullopt;

                const auto span = read.number (child, "range", "out")
                                    - read.number (child, "range", "in");

                if (! (span > 0.0))
                    return std::nullopt;

                total += span * static_cast<double> (passes);
            }

            if (anyRange)
                return total;

            if (durations == nullptr)
                return std::nullopt;

            const auto found = durations->find (read.text (cue, "media", "file"));

            if (found == durations->end() || ! (found->second > 0.0))
                return std::nullopt;

            const auto span = found->second - read.number (cue, "media", "startOffset");
            return span > 0.0 ? std::optional<double> (span) : std::nullopt;
        }

        /*  Which range a media cue is in `offset` seconds after it started, and
            which pass of it.

            A CUE WITH RANGES IS A PLAYLIST OVER ONE FILE (§3.24), so this is
            arithmetic over the passes in order rather than a lookup. A range
            that plays for ever swallows everything after it, which is right:
            nothing gets past a bed. */
        void placeInRanges (const Reader& read, const juce::ValueTree& cue, double offset,
                            PlannedRun& out, std::vector<Confusion>& confused)
        {
            auto index = 0;
            auto remaining = offset;

            for (const auto& child : cue)
            {
                if (child.getType().toString() != "Range")
                    continue;

                const auto in = read.number (child, "range", "in");
                const auto span = read.number (child, "range", "out") - in;
                const auto passes = read.integer (child, "range", "loops");

                if (! (span > 0.0))
                {
                    ++index;
                    continue;
                }

                if (passes <= 0)
                {
                    /*  FOR EVER, AND THE POSITION IS OPERATOR-TIMED. Which pass
                        a bed is on, and how far into it, is a fact about how
                        long somebody held the scene - not about the show. §3.24
                        settles this as solve-in-practice: land at the start of
                        the range, pass one, and SAY SO. */
                    out.range = index;
                    out.pass = 1;
                    out.offset = in;

                    confused.push_back ({ out.cue, confusion::endlessRange,
                                          "the start of range " + std::to_string (index)
                                            + ", pass 1" });
                    return;
                }

                const auto whole = span * static_cast<double> (passes);

                if (remaining < whole)
                {
                    const auto pass = static_cast<int> (remaining / span);

                    out.range = index;
                    out.pass = pass + 1;
                    out.offset = in + (remaining - static_cast<double> (pass) * span);
                    return;
                }

                remaining -= whole;
                ++index;
            }

            /*  Past the end of every range: the cue is over, and the caller has
                already decided it is live, so it sits at the last instant it
                had. */
            out.range = index > 0 ? index - 1 : -1;
            out.pass = 1;
            out.offset = offset;
        }

        //======================================================================
        /*  Whether a cue is still going when the walk reaches the target.

            EVERYTHING BEFORE THE TARGET IS AT ITS END STATE, which is §3.13's
            step 1 and is the only honest reading of a manual list: a cue fired
            at an earlier GO has had however long the actor took, so a finite
            one is over. What is NOT over is something that never ends on its
            own - a bed, a group looping for ever - and that is exactly what
            `endsOnItsOwn` already answers for the slot analysis. */
        bool stillGoing (const Walk& walk, const Placed& cue)
        {
            const auto found = walk.unbounded.find (cue.id);
            return found != walk.unbounded.end() && found->second;
        }
    }

    //==============================================================================
    Plan solve (const doc::ShowDocument& document,
                const std::map<std::string, double>* durations,
                const tree::MountTable* mounts,
                const Aim& aim)
    {
        Plan plan;
        plan.aim = aim;

        const Reader read;
        Walk walk { read, durations };

        const auto lists = document.root().getChildWithName (juce::Identifier ("Lists"));
        auto list = juce::ValueTree {};

        for (const auto& candidate : lists)
            if (candidate.getType().toString() == "List"
                 && candidate[idProperty].toString().toStdString() == aim.list)
                list = candidate;

        if (! list.isValid())
            return plan;

        walk.visitList (list);

        const Placed* target = nullptr;

        for (const auto& entry : walk.placed)
            if (entry.id == aim.cue)
                target = &entry;

        if (target == nullptr)
            return plan;

        plan.ok = true;

        /*  THE POINTER LANDS AFTER THE TARGET, positionally (§3.5): a jump puts
            the operator where the next GO would take the show on, which is what
            "take it back to cue 12" leaves them wanting. */
        for (const auto& entry : walk.placed)
            if (entry.row == target->row + 1)
                plan.standby = entry.id;

        /*  Whether the target itself has fired. -1 means the pointer is ON it
            and nothing of it has happened, which is a different position from
            nought seconds in - and the difference is whether its own values are
            set and its own sound is going. */
        const auto fired = aim.offset >= 0.0;

        //----------------------------------------------------------------------
        /*  THE VALUE HALF: one forward pass, last writer wins, the whole list.

            NOT STOPPED AT A STRUCTURAL WAYPOINT, deliberately. A group boundary
            bounds what is RUNNING - the footer has run and nothing it started
            is still going - and bounds nothing at all about what is SET. A
            level somebody chose in act one is still chosen in act three, and a
            walk that stopped at a boundary would quietly lose it. */
        std::map<std::string, std::size_t> writtenAt;

        for (const auto& entry : walk.placed)
        {
            if (entry.row > target->row || (entry.row == target->row && ! fired))
                break;

            if (entry.element != "Osc")
                continue;

            if (! read.flag (entry.node, "cue", "enabled"))
                continue;

            const auto address = read.text (entry.node, "osc", "address");

            if (address.empty())
                continue;

            /*  AN EVENT IS NOT A VALUE (§3.13 step 4). "Fire the pyro" has no
                state to restore to, and a jump that re-sent it would set the
                theatre alight on the way past a cue that already happened. A
                mount that has not been loaded says nothing about its nodes, so
                nothing is excluded - which is the direction that fires, and is
                why a caller with a mount table hands it over. */
            if (mounts != nullptr)
                if (const auto* node = mounts->nodeAt (address);
                    node != nullptr && node->kind == tree::Kind::event)
                    continue;

            const auto value = osc::Value::fromAtom (read.text (entry.node, "osc", "value"));

            if (! value.has_value())
                continue;

            const auto seen = writtenAt.find (address);

            if (seen != writtenAt.end())
            {
                plan.values[seen->second] = { address, *value, entry.id };
                continue;
            }

            writtenAt[address] = plan.values.size();
            plan.values.push_back ({ address, *value, entry.id });
        }

        //----------------------------------------------------------------------
        /*  THE TRIMS: base plus the sum of what the fades did (PR 3.12).

            A fade before the target has completed - the same end-state rule the
            runs follow - so it contributes its whole change. What it changed is
            the difference between where it was told to go and what its target
            cue says, which is the one thing a level can be trimmed BY. */
        std::map<std::string, std::size_t> trimmedAt;

        for (const auto& entry : walk.placed)
        {
            if (entry.row > target->row || (entry.row == target->row && ! fired))
                break;

            if (entry.element != "Fade" || ! read.flag (entry.node, "cue", "enabled"))
                continue;

            const auto targetCue = read.text (entry.node, "fade", "target");

            if (targetCue.empty())
                continue;

            const auto level = read.number (entry.node, "fade", "level");
            const auto cue = document.findById (targetCue);

            if (! cue.isValid())
                continue;

            const auto trim = level - read.number (cue, "media", "level");

            const auto seen = trimmedAt.find (targetCue);

            if (seen != trimmedAt.end())
            {
                plan.trims[seen->second].decibels = trim;
                continue;
            }

            trimmedAt[targetCue] = plan.trims.size();
            plan.trims.push_back ({ targetCue, trim });
        }

        //----------------------------------------------------------------------
        /*  THE RUN HALF, and it is a much shorter list than the value half.

            Everything before the target is at its end state, so what is still
            going is: whatever never ends on its own and nothing stopped, and
            the target's own chain at the offset. */
        std::vector<std::string> stopped;

        for (const auto& entry : walk.placed)
        {
            if (entry.row > target->row)
                break;

            if (entry.element == "Stop" && read.flag (entry.node, "cue", "enabled"))
                stopped.push_back (read.text (entry.node, "stop", "target"));
        }

        const auto wasStopped = [&stopped] (const std::string& cueId)
        {
            return std::find (stopped.begin(), stopped.end(), cueId) != stopped.end();
        };

        for (const auto& entry : walk.placed)
        {
            if (entry.row >= target->row)
                break;

            if (entry.element != "Media" || ! read.flag (entry.node, "cue", "enabled"))
                continue;

            if (! stillGoing (walk, entry) || wasStopped (entry.id))
                continue;

            /*  Still going, and where in it is operator-timed. `placeInRanges`
                lands it at the start of its endless range and says so. */
            PlannedRun run;
            run.cue = entry.id;
            run.ancestors = entry.ancestors;
            placeInRanges (read, entry.node, 0.0, run, plan.confused);
            plan.runs.push_back (run);
        }

        //----------------------------------------------------------------------
        /*  AND THE TARGET, if it has fired at all. Its groups come with it,
            outermost first, because a member sounds as part of its scene. */
        if (fired && read.flag (target->node, "cue", "enabled"))
        {
            for (const auto& groupId : target->ancestors)
            {
                PlannedRun group;
                group.cue = groupId;
                plan.runs.push_back (group);

                const auto node = document.findById (groupId);

                if (node.isValid() && read.text (node, "group", "selection") == "shuffle"
                     && read.integer (node, "group", "seed") == 0)
                    plan.confused.push_back ({ groupId, confusion::drawnOrder,
                                               "the order the show is written in" });

                if (node.isValid() && read.integer (node, "group", "loops") != 1)
                    plan.confused.push_back ({ groupId, confusion::unknownRound,
                                               "round one" });
            }

            /*  AND THE MEMBERS SOUNDING BESIDE IT.

                Inside a timeline group or an automatic sequence there is no
                person between the members - the second starts when the first
                completes, or at its own offset from the group's entry - so the
                document knows exactly what else is going when the target is
                `offset` seconds in. That is the one place where "everything
                before the target is at its end state" is FALSE, and it is false
                because there was no GO in between to make it true.

                A load-to-time into the middle of a scene depends on this
                entirely: without it the jump would land the cue somebody asked
                for and silence everything it was playing against.

                The walk has already done the arithmetic - every cue in a timed
                chain carries its seconds from that chain's entry - so this is a
                comparison rather than a second calculation, which is the whole
                reason that walk is shared with the slot analysis. */
            if (target->timed)
            {
                const auto at = target->from + aim.offset;

                for (const auto& entry : walk.placed)
                {
                    if (entry.id == target->id || ! entry.timed
                         || entry.chain != target->chain)
                        continue;

                    if (entry.element != "Media"
                         || ! read.flag (entry.node, "cue", "enabled"))
                        continue;

                    if (wasStopped (entry.id) || ! (entry.from <= at && at < entry.to))
                        continue;

                    PlannedRun beside;
                    beside.cue = entry.id;
                    beside.ancestors = entry.ancestors;
                    placeInRanges (read, entry.node, at - entry.from, beside, plan.confused);
                    plan.runs.push_back (beside);
                }
            }

            if (target->element == "Media")
            {
                PlannedRun run;
                run.cue = target->id;
                run.ancestors = target->ancestors;

                if (! materialOf (read, target->node, durations).has_value()
                     && ! target->node.getChildWithName (juce::Identifier ("Range")).isValid())
                    plan.confused.push_back ({ target->id, confusion::unknownLength,
                                              "the offset asked for, played from the top" });

                placeInRanges (read, target->node, aim.offset, run, plan.confused);
                plan.runs.push_back (run);
            }
            else
            {
                PlannedRun run;
                run.cue = target->id;
                run.ancestors = target->ancestors;
                run.offset = aim.offset;
                plan.runs.push_back (run);
            }
        }

        return plan;
    }

    //==============================================================================
    namespace
    {
        std::string quoted (const std::string& text)
        {
            std::string out = "\"";

            for (const auto c : text)
            {
                switch (c)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    default:   out += c;      break;
                }
            }

            return out + "\"";
        }
    }

    std::string Plan::toJson() const
    {
        /*  ONE NODE HOLDING A DOCUMENT, rather than a subtree of nodes
            appearing and vanishing under a dragged finger. A solve changes
            shape as the aim moves - three runs, then one - and a namespace that
            grew and shrank at fifty hertz would be a namespace no client could
            subscribe to. */
        std::string out = "{\"ok\": ";
        out += ok ? "true" : "false";
        out += ", \"aim\": {\"cue\": " + quoted (aim.cue)
                 + ", \"offset\": " + osc::formatDouble (aim.offset) + "}";
        out += ", \"standby\": " + quoted (standby);

        out += ", \"runs\": [";

        for (std::size_t n = 0; n < runs.size(); ++n)
        {
            const auto& run = runs[n];

            out += n == 0 ? "" : ", ";
            out += "{\"cue\": " + quoted (run.cue)
                     + ", \"offset\": " + osc::formatDouble (run.offset)
                     + ", \"range\": " + std::to_string (run.range)
                     + ", \"pass\": " + std::to_string (run.pass)
                     + ", \"in\": [";

            for (std::size_t a = 0; a < run.ancestors.size(); ++a)
                out += (a == 0 ? "" : ", ") + quoted (run.ancestors[a]);

            out += "]}";
        }

        out += "], \"values\": [";

        for (std::size_t n = 0; n < values.size(); ++n)
        {
            out += n == 0 ? "" : ", ";
            out += "{\"address\": " + quoted (values[n].address)
                     + ", \"value\": " + quoted (values[n].value.toAtom())
                     + ", \"from\": " + quoted (values[n].writer) + "}";
        }

        out += "], \"trims\": [";

        for (std::size_t n = 0; n < trims.size(); ++n)
        {
            out += n == 0 ? "" : ", ";
            out += "{\"cue\": " + quoted (trims[n].cue)
                     + ", \"dB\": " + osc::formatDouble (trims[n].decibels) + "}";
        }

        out += "], \"confused\": [";

        for (std::size_t n = 0; n < confused.size(); ++n)
        {
            out += n == 0 ? "" : ", ";
            out += "{\"cue\": " + quoted (confused[n].cue)
                     + ", \"why\": " + quoted (confused[n].reason)
                     + ", \"took\": " + quoted (confused[n].took) + "}";
        }

        return out + "]}";
    }
}
