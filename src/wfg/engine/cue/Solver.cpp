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
#include <limits>
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

        /*  THE TARGET'S OWN PART OF A PLAN: its groups, outermost first, the
            members sounding beside it in its chain, and itself at the offset.
            One function for the two readings - the order's target and every
            step of the history's - so a scene is built the same way whichever
            clock placed it. `offset` is seconds into the target; the caller
            has decided it has fired. */
        void planTarget (const Reader& read, const doc::ShowDocument& document,
                         const std::map<std::string, double>* durations,
                         const Walk& walk, const Placed& target, double offset,
                         const std::vector<std::string>& stopped, Plan& plan)
        {
            const auto wasStopped = [&stopped] (const std::string& cueId)
            {
                return std::find (stopped.begin(), stopped.end(), cueId) != stopped.end();
            };

            /*  A GROUP ALREADY IN THE PLAN IS NOT PUSHED AGAIN: two members of
                one manual scene fired at two GOs share it, and the builder
                would otherwise make the scene twice. */
            const auto planned = [&plan] (const std::string& cueId)
            {
                return std::any_of (plan.runs.begin(), plan.runs.end(),
                                    [&cueId] (const PlannedRun& run) { return run.cue == cueId; });
            };

            //----------------------------------------------------------------------
            /*  AND THE TARGET, if it has fired at all. Its groups come with it,
                outermost first, because a member sounds as part of its scene. */
            {
                for (const auto& groupId : target.ancestors)
                {
                    if (planned (groupId))
                        continue;

                    PlannedRun group;
                    group.cue = groupId;

                    /*  ITS OWN GROUPS, the ones before it in the chain, so that a
                        jump into a scene inside a scene builds the inner group
                        UNDER the outer one. Empty, the builder made every
                        ancestor a top-level run and the scheduler saw two scenes
                        where the show had one inside the other. Found on the way
                        to seating a group at an offset (2026-09-18). */
                    group.ancestors.assign (target.ancestors.begin(),
                                            std::find (target.ancestors.begin(),
                                                       target.ancestors.end(), groupId));
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
                /*  AND THE TARGET MAY BE THE SCENE ITSELF (2026-09-18). A member
                    is placed inside its chain; a group that IS the chain's origin
                    is not timed - nothing placed it - but its members are, and
                    they are counted from its entry. So "the scene, `offset`
                    seconds in" reads the same members against the same clock,
                    with the offset itself as the instant. This is what lets a
                    running group be re-seated at another second of its own
                    timeline, which is what scrubbing a group in the running pane
                    asks for. */
                const auto chain = target.timed ? target.chain
                                 : target.element == "Group" ? target.id
                                                              : std::string {};

                if (! chain.empty())
                {
                    const auto at = target.timed ? target.from + offset : offset;

                    /*  WHAT EACH INNER GROUP WAS FOUND TO BE, by identifier, so
                        that its members can follow it: a group that is DUE will
                        spawn its own members when it fires, so planning them too
                        would have each of them twice; one that is FINISHED has
                        had them, whatever their own seconds say. Groups are placed
                        before their members, so the answer is always there. */
                    std::map<std::string, std::string> innerWhen;

                    for (const auto& entry : walk.placed)
                    {
                        if (entry.id == target.id || ! entry.timed || entry.chain != chain)
                            continue;

                        if (! read.flag (entry.node, "cue", "enabled"))
                            continue;

                        /*  Its own groups are already in the plan, above. */
                        if (std::find (target.ancestors.begin(), target.ancestors.end(),
                                       entry.id) != target.ancestors.end())
                            continue;

                        if (entry.element == "Media" && wasStopped (entry.id))
                            continue;

                        std::string inner;

                        for (const auto& groupId : entry.ancestors)
                            if (const auto seen = innerWhen.find (groupId); seen != innerWhen.end())
                                if (seen->second != planned::sounding)
                                    inner = seen->second;

                        if (inner == planned::due)
                            continue;

                        PlannedRun beside;
                        beside.cue = entry.id;
                        beside.ancestors = entry.ancestors;

                        /*  THE WHOLE CHAIN AND NOT ONLY THE NOISY PART. A jump has
                            to build the scene the scheduler is about to take over,
                            and a member missing from it is one the group will spawn
                            a second time - or, missing from the finished end, a
                            group that thinks it has not started.

                            EVERY KIND, not media alone (2026-09-18): a fade due
                            four seconds after the jump has to be waiting there, or
                            the scene the scheduler takes over never fires it. A
                            fade or a message the instant has already passed is
                            over - what it wrote is in the values and the trims. */
                        if (inner == planned::finished || entry.to <= at)
                            beside.when = planned::finished;
                        else if (at < entry.from)
                        {
                            beside.when = planned::due;
                            beside.startsIn = entry.from - at;
                        }
                        else if (entry.element == "Media")
                            placeInRanges (read, entry.node, at - entry.from, beside, plan.confused);
                        else if (entry.element == "Group")
                            beside.offset = at - entry.from;
                        else
                            beside.when = planned::finished;

                        if (entry.element == "Group")
                            innerWhen[entry.id] = beside.when;

                        plan.runs.push_back (beside);
                    }
                }

                if (target.element == "Media")
                {
                    PlannedRun run;
                    run.cue = target.id;
                    run.ancestors = target.ancestors;

                    if (! materialOf (read, target.node, durations).has_value()
                         && ! target.node.getChildWithName (juce::Identifier ("Range")).isValid())
                        plan.confused.push_back ({ target.id, confusion::unknownLength,
                                                  "the offset asked for, played from the top" });

                    placeInRanges (read, target.node, offset, run, plan.confused);
                    plan.runs.push_back (run);
                }
                else
                {
                    PlannedRun run;
                    run.cue = target.id;
                    run.ancestors = target.ancestors;
                    run.offset = offset;
                    plan.runs.push_back (run);
                }
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
        /*  THE NEXT PLACE THE POINTER MAY STAND, which is not always the next
            row. The pointer may now be PARKED on a member of any group
            (decision X, 2026-09-16), but a jump may not LEAVE it there: a jump
            into the middle of a timeline scene leaves the pointer after the
            SCENE, because the scene is about to fire its third member itself
            and a press landing on it would fire it twice.
            `ShowWalk::Placed::mayLandHere` carries that narrower rule and says
            why it is narrower. */
        for (const auto& entry : walk.placed)
            if (entry.row > target->row && entry.mayLandHere)
            {
                plan.standby = entry.id;
                break;
            }

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

            if (entry.element == "Transport" && read.flag (entry.node, "cue", "enabled"))
                stopped.push_back (read.text (entry.node, "transport", "target"));
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

        if (fired && read.flag (target->node, "cue", "enabled"))
            planTarget (read, document, durations, walk, *target, aim.offset, stopped, plan);

        return plan;
    }

    //==============================================================================
    Plan solveHistory (const doc::ShowDocument& document,
                       const std::map<std::string, double>* durations,
                       const tree::MountTable* mounts,
                       const Aim& aim,
                       const std::vector<Step>& steps)
    {
        /*  WHEN THE AIMED CUE WAS FIRED, most recently. Without that there is
            no clock to read the rest against, and the order is the answer. */
        std::int64_t firedAt = -1;

        for (const auto& step : steps)
            if (step.cue == aim.cue)
                firedAt = step.tick;

        if (firedAt < 0)
            return solve (document, durations, mounts, aim);

        Plan plan;
        plan.aim = aim;
        plan.how = "history";

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

        const auto placedOf = [&walk] (const std::string& cueId) -> const Placed*
        {
            for (const auto& entry : walk.placed)
                if (entry.id == cueId)
                    return &entry;

            return nullptr;
        };

        const auto* target = placedOf (aim.cue);

        if (target == nullptr)
            return plan;

        plan.ok = true;

        /** Whether a cue never ends on its own: a bed, a loop for ever. */
        const auto endless = [&walk] (const std::string& cueId)
        {
            const auto found = walk.unbounded.find (cueId);
            return found != walk.unbounded.end() && found->second;
        };

        /*  THE INSTANT: the aimed cue's step plus the offset, in ticks. "Before
            it fired" is the tick before its step, so its own step - and every
            other step on the same tick - is in the future. */
        const auto ticksIn = aim.offset >= 0.0
                               ? static_cast<std::int64_t> (std::llround (aim.offset * 50.0))
                               : -1;
        plan.instant = firedAt + ticksIn;

        /*  EVERYTHING THAT HAD HAPPENED BY THEN, oldest first, which is the
            order values and stops resolve in: a stop after a fire ends the
            run, a fire after a stop is a new one. */
        std::vector<Step> past;

        for (const auto& step : steps)
            if (step.tick <= plan.instant)
                past.push_back (step);

        std::stable_sort (past.begin(), past.end(),
                          [] (const Step& a, const Step& b) { return a.tick < b.tick; });

        std::vector<std::string> stopped;
        std::map<std::string, std::size_t> writtenAt;
        std::map<std::string, std::size_t> trimmedAt;
        std::string lastGo;

        /*  A CUE FIRED AGAIN REPLACES ITS EARLIER SELF: the runs its earlier
            step planned - itself, and a scene's members - go before the later
            step plans them, so one cue is one run whatever the night did. */
        const auto unplan = [&plan] (const std::string& cueId)
        {
            plan.runs.erase (std::remove_if (plan.runs.begin(), plan.runs.end(),
                                             [&cueId] (const PlannedRun& run)
                                             {
                                                 return run.cue == cueId
                                                     || std::find (run.ancestors.begin(),
                                                                   run.ancestors.end(), cueId)
                                                          != run.ancestors.end();
                                             }),
                             plan.runs.end());
        };

        for (const auto& step : past)
        {
            const auto* entry = placedOf (step.cue);

            if (entry == nullptr || ! read.flag (entry->node, "cue", "enabled"))
                continue;

            const auto seconds = static_cast<double> (plan.instant - step.tick) / 50.0;

            if (step.origin == 'g')
                lastGo = step.cue;

            if (entry->element == "Transport")
            {
                const auto targetCue = read.text (entry->node, "transport", "target");

                if (! targetCue.empty())
                {
                    stopped.push_back (targetCue);
                    unplan (targetCue);
                }

                continue;
            }

            if (entry->element == "Osc")
            {
                const auto address = read.text (entry->node, "osc", "address");

                if (address.empty())
                    continue;

                if (mounts != nullptr)
                    if (const auto* node = mounts->nodeAt (address);
                        node != nullptr && node->kind == tree::Kind::event)
                        continue;

                const auto value = osc::Value::fromAtom (read.text (entry->node, "osc", "value"));

                if (! value.has_value())
                    continue;

                if (const auto seen = writtenAt.find (address); seen != writtenAt.end())
                    plan.values[seen->second] = { address, *value, entry->id };
                else
                {
                    writtenAt[address] = plan.values.size();
                    plan.values.push_back ({ address, *value, entry->id });
                }

                continue;
            }

            if (entry->element == "Fade")
            {
                /*  WHOLE ONCE ITS TIME HAS PASSED, and the part of it that had
                    happened inside it: a fade three seconds into six has moved
                    its target half of the way. Linear, which is the shape a
                    reading can promise without the curve; the run that is
                    built from this plays the real one. */
                const auto targetCue = read.text (entry->node, "fade", "target");
                const auto cue = document.findById (targetCue);

                if (targetCue.empty() || ! cue.isValid())
                    continue;

                const auto whole = read.number (entry->node, "fade", "level")
                                     - read.number (cue, "media", "level");
                const auto duration = read.number (entry->node, "fade", "duration");
                const auto part = duration > 0.0 && seconds < duration ? seconds / duration : 1.0;
                const auto trim = whole * part;

                if (const auto seen = trimmedAt.find (targetCue); seen != trimmedAt.end())
                    plan.trims[seen->second].decibels = trim;
                else
                {
                    trimmedAt[targetCue] = plan.trims.size();
                    plan.trims.push_back ({ targetCue, trim });
                }

                continue;
            }

            if (entry->element != "Media" && entry->element != "Group")
                continue;

            /*  A SOUND THAT HAS RUN OUT BY THE INSTANT IS OVER, and is left
                out rather than planned at its last instant: the order reading
                plans a target the operator has DECLARED live, and the history
                reading knows better. A file whose length this build cannot
                read is planned at the offset and said to be a guess, which is
                what the order does for the same case. */
            if (entry->element == "Media")
            {
                const auto material = materialOf (read, entry->node, durations);
                const auto ranged = entry->node.getChildWithName (juce::Identifier ("Range")).isValid();

                if (material.has_value() && seconds >= *material)
                {
                    unplan (entry->id);
                    continue;
                }

                if (! material.has_value() && ! ranged && ! endless (entry->id))
                    plan.confused.push_back ({ entry->id, confusion::unknownLength,
                                              "the seconds since it was fired, played from there" });
            }

            /*  A TIMED SCENE THAT IS WHOLLY OVER is left out too, by the
                same rule; a manual one, or one the walk cannot time, is
                planned at the offset and its members read as the order would. */
            if (entry->element == "Group")
            {
                auto lastEnd = -1.0;
                auto timed = false;

                for (const auto& member : walk.placed)
                    if (member.timed && member.chain == entry->id)
                    {
                        timed = true;
                        lastEnd = std::max (lastEnd, member.to);
                    }

                if (timed && seconds >= lastEnd && ! endless (entry->id))
                {
                    unplan (entry->id);
                    continue;
                }
            }

            unplan (entry->id);
            planTarget (read, document, durations, walk, *entry, seconds, stopped, plan);
        }

        /*  THE POINTER, where the last GO in the instant's past left it:
            positionally after the cue it fired (§3.5), or after the aimed cue
            when nothing in the past was a GO. */
        const auto* after = placedOf (lastGo.empty() ? aim.cue : lastGo);

        if (after != nullptr)
            for (const auto& entry : walk.placed)
                if (entry.row > after->row && entry.mayLandHere)
                {
                    plan.standby = entry.id;
                    break;
                }

        return plan;
    }

    Plan solveAim (const doc::ShowDocument& document,
                   const std::map<std::string, double>* durations,
                   const tree::MountTable* mounts,
                   const Aim& aim,
                   const std::vector<Step>* steps)
    {
        if (steps != nullptr)
            return solveHistory (document, durations, mounts, aim, *steps);

        return solve (document, durations, mounts, aim);
    }

    //==============================================================================
    Plan solvePersistent (const doc::ShowDocument& document,
                          const std::map<std::string, double>* durations,
                          const tree::MountTable* mounts,
                          const std::string& listId,
                          const std::string& standbyCue,
                          bool ranOut)
    {
        Plan plan;
        plan.aim = { listId, standbyCue, -1.0 };
        plan.standby = standbyCue;

        const Reader read;
        Walk walk { read, durations };

        const auto lists = document.root().getChildWithName (juce::Identifier ("Lists"));
        auto list = juce::ValueTree {};

        for (const auto& candidate : lists)
            if (candidate.getType().toString() == "List"
                 && candidate[idProperty].toString().toStdString() == listId)
                list = candidate;

        if (! list.isValid())
            return plan;

        plan.ok = true;

        const auto section = list.getChildWithName (juce::Identifier ("Persistent"));

        if (! section.isValid())
            return plan;

        /*  WHERE THE POINTER IS, AS A ROW. Everything before it has happened;
            a stop there that names a persistent cue is the decision to end it.
            A pointer on nothing - a list nobody has parked on - is the top of
            the list, so nothing has happened and nothing is suspended.

            UNLESS IT IS EMPTY BECAUSE THE LIST RAN OUT, which is the opposite
            fact and looks exactly the same from here: since 2026-09-18 a GO
            that fires the last cue leaves the pointer nowhere, so the list
            carries `finished` to say which nowhere this is. Past the end,
            EVERYTHING has happened - every stop in the list counts - and a bed
            somebody killed at cue twelve stays killed when the show ends,
            which is the whole reason the flag exists. */
        walk.visitList (list);

        int standbyRow = ranOut ? std::numeric_limits<int>::max() : -1;

        for (const auto& entry : walk.placed)
            if (entry.id == standbyCue)
                standbyRow = entry.row;

        std::vector<std::string> stopped;

        for (const auto& entry : walk.placed)
            if (entry.row < standbyRow && entry.element == "Transport"
                 && read.flag (entry.node, "cue", "enabled"))
                stopped.push_back (read.text (entry.node, "transport", "target"));

        for (const auto& cue : section)
        {
            const auto id = cue[idProperty].toString().toStdString();
            const auto element = cue.getType().toString();

            if (id.empty() || ! read.flag (cue, "cue", "enabled"))
                continue;

            if (std::find (stopped.begin(), stopped.end(), id) != stopped.end())
                continue;

            if (element == "Media" || element == "Midi")
            {
                PlannedRun run;
                run.cue = id;
                run.when = planned::sounding;

                if (element == "Media")
                    placeInRanges (read, cue, 0.0, run, plan.confused);

                plan.runs.push_back (run);
            }
            else if (element == "Osc")
            {
                const auto address = read.text (cue, "osc", "address");

                if (address.empty())
                    continue;

                if (mounts != nullptr)
                    if (const auto* node = mounts->nodeAt (address);
                        node != nullptr && node->kind == tree::Kind::event)
                        continue;

                const auto value = osc::Value::fromAtom (read.text (cue, "osc", "value"));

                if (! value.has_value())
                    continue;

                plan.values.push_back ({ address, *value, id });
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
        out += ", \"how\": " + quoted (how) + ", \"instant\": " + std::to_string (instant);
        out += ", \"aim\": {\"cue\": " + quoted (aim.cue)
                 + ", \"offset\": " + osc::formatDouble (aim.offset) + "}";
        out += ", \"standby\": " + quoted (standby);

        out += ", \"runs\": [";

        for (std::size_t n = 0; n < runs.size(); ++n)
        {
            const auto& run = runs[n];

            out += n == 0 ? "" : ", ";
            out += "{\"cue\": " + quoted (run.cue)
                     + ", \"when\": " + quoted (run.when)
                     + ", \"offset\": " + osc::formatDouble (run.offset)
                     + ", \"startsIn\": " + osc::formatDouble (run.startsIn)
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
