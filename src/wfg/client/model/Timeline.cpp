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

#include <wfg/client/model/Timeline.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        double secondsAt (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return osc::parseDouble (text (snapshot, address)).value_or (0.0);
        }

        /*  HOW FAR A GROUP REACHES FROM ITS OWN ENTRY, worked out from its
            members (author, 2026-09-22: "can you resolve nested groups?").

            A GROUP'S LENGTH IS PUBLISHED NOWHERE. The engine has it -
            `ShowWalk::groupLength` - but it lives inside the static walk and
            reaches no client, so a nested group was drawn as a start with no
            end. It does not have to be: everything the sum is made of IS
            published, one level at a time, so the client can do the same
            arithmetic by walking down.

            THE SAME GUARDS AS THE ENGINE'S, and they are the point rather than
            a formality - each one is a reason the answer cannot be known:

              - a header or a footer: a header runs first and blocks, and an
                operator may be inside it;
              - a manual sequence: a person between the members, and a person
                is not a duration;
              - `loops` other than one, `play` N of M, or a shuffle: how many
                and which is not decided until the round is drawn;
              - any member whose own length is unknown.

            IT EXCLUDES THE GROUP'S OWN PRE-WAIT, which is where this parts
            company with the engine's version - see the note in the header. A
            bar's left edge is already its pre-wait; adding it to the length
            too would draw every nested group that much too long. */
        constexpr int deepest = 16;

        bool lengthOf (const tree::TreeSnapshot&, const std::string&, const std::string&,
                       double&, int);

        bool groupSpan (const tree::TreeSnapshot& snapshot, const std::string& id,
                        double& span, int depth)
        {
            span = 0.0;

            //  A show cannot nest this far without somebody having made a loop.
            if (depth >= deepest)
                return false;

            const auto base = "/godot/cue/" + id + "/";

            if (! text (snapshot, base + "header").empty()
                 || ! text (snapshot, base + "footer").empty())
                return false;

            auto mode = text (snapshot, base + "mode");

            if (mode.empty())
                mode = "sequence";

            const auto advance = text (snapshot, base + "advance");

            if (mode != "timeline" && advance != "auto")
                return false;

            const auto rounds = static_cast<int> (osc::parseDouble (
                text (snapshot, base + "loops")).value_or (1.0));

            const auto plays = static_cast<int> (osc::parseDouble (
                text (snapshot, base + "play")).value_or (0.0));

            auto selection = text (snapshot, base + "selection");

            if (selection.empty())
                selection = "sequential";

            if (rounds < 1 || plays != 0 || selection != "sequential")
                return false;

            const auto timeline = mode == "timeline";
            auto running = 0.0;
            auto furthest = 0.0;

            for (const auto& child : words (text (snapshot, base + "order")))
            {
                const auto member = "/godot/cue/" + child + "/";

                double length = 0.0;

                if (! lengthOf (snapshot, child, text (snapshot, member + "kind"),
                                length, depth + 1))
                    return false;

                const auto preWait = secondsAt (snapshot, member + "preWait");
                const auto begins = (timeline ? 0.0 : running) + preWait;

                furthest = std::max (furthest, begins + length);

                if (! timeline)
                    running = begins + length + secondsAt (snapshot, member + "postWait");
            }

            span = furthest * static_cast<double> (rounds);
            return true;
        }

        /*  HOW LONG A CUE TAKES, and the honest nought when nobody knows.

            A media cue's `duration` is read from its file when the show opens;
            a fade's and a stop's is a number somebody decided; a group's is the
            sum above; everything else takes no time at all. */
        bool lengthOf (const tree::TreeSnapshot& snapshot, const std::string& id,
                       const std::string& kind, double& length, int depth)
        {
            length = 0.0;

            if (kind == "group")
                return groupSpan (snapshot, id, length, depth);

            if (kind == "media" || kind == "fade" || kind == "transport")
            {
                length = secondsAt (snapshot, "/godot/cue/" + id + "/duration");
                return length > 0.0;
            }

            //  A memo, an osc, a midi or a start cue is an instant, and that is known.
            return true;
        }
    }

    TimelineReading readTimeline (const tree::TreeSnapshot& snapshot, const std::string& groupId)
    {
        TimelineReading out;

        if (groupId.empty())
            return out;

        const auto base = "/godot/cue/" + groupId + "/";

        out.groupId = groupId;
        out.groupName = text (snapshot, base + "name");
        out.mode = text (snapshot, base + "mode");
        out.parent = text (snapshot, base + "parent");

        if (text (snapshot, base + "kind") != "group")
        {
            out.notice = "Only a group has members to arrange.";
            return out;
        }

        if (out.mode.empty())
            out.mode = "sequence";

        /*  ONLY A TIMELINE IS ARRANGED BY DRAGGING. In a sequence a member
            starts when the one before it finishes, so where a bar sits is
            arithmetic rather than a decision and moving it would move
            everything after it without being asked. */
        out.draggable = out.mode == "timeline";

        const auto order = words (text (snapshot, base + "order"));

        if (order.empty())
        {
            out.notice = "This group has no members yet.";
            return out;
        }

        auto running = 0.0;

        for (const auto& id : order)
        {
            const auto member = "/godot/cue/" + id + "/";

            Bar bar;
            bar.id = id;
            bar.name = text (snapshot, member + "name");
            bar.kind = text (snapshot, member + "kind");
            bar.colour = text (snapshot, member + "colour");
            bar.preWait = secondsAt (snapshot, member + "preWait");
            bar.postWait = secondsAt (snapshot, member + "postWait");
            bar.isGroup = bar.kind == "group";
            bar.lengthKnown = lengthOf (snapshot, id, bar.kind, bar.length, 0);

            /*  A TIMELINE SCHEDULES EVERYTHING AT ENTRY and each member's
                pre-wait is its offset from that moment (§3.6); a sequence is a
                running sum, and one unknown length ends the arithmetic for
                everything after it - the same rule the engine's own walk
                applies, and for the same reason. */
            if (out.mode == "timeline")
            {
                bar.at = bar.preWait;
            }
            else
            {
                bar.at = running + bar.preWait;

                if (bar.lengthKnown)
                    running = bar.at + bar.length + bar.postWait;
            }

            out.span = std::max (out.span, bar.ends());
            out.bars.push_back (std::move (bar));
        }

        /*  NEVER NOUGHT, or the axis would have no width and every bar would be
            drawn at the same pixel. Ten seconds is a scene's worth of room to
            drag into when everything in the group is an instant. */
        if (! (out.span > 0.0))
            out.span = 10.0;

        return out;
    }

    double preWaitFor (double wantedStart)
    {
        //  A member cannot begin before the group it is inside of.
        return std::max (0.0, wantedStart);
    }

    std::vector<SnapTarget> snapTargets (const std::vector<Bar>& bars,
                                         const std::string& draggedId)
    {
        std::vector<SnapTarget> out;

        //  The group's own entry, which is the one instant that is always there.
        out.push_back ({ 0.0, {}, "start" });

        for (const auto& bar : bars)
        {
            if (bar.id == draggedId)
                continue;

            const auto name = bar.name.empty() ? bar.id : bar.name;

            out.push_back ({ bar.at, name, "start" });

            /*  AN END NOBODY KNOWS IS NOT A PLACE. A group's length never
                reaches a client, and a media file this build could not read
                has none either - offering their "end" would be offering the
                same instant as their start, wearing a different word. */
            if (bar.lengthKnown && bar.length > 0.0)
                out.push_back ({ bar.ends(), name, "end" });
        }

        return out;
    }

    std::optional<Snapped> snapTo (double wantedStart, double length, bool lengthKnown,
                                   const std::vector<SnapTarget>& targets,
                                   double tolerance)
    {
        if (! (tolerance > 0.0))
            return std::nullopt;

        std::optional<Snapped> best;
        auto nearest = tolerance;

        /*  BOTH EDGES OF THE DRAGGED BAR against every target, which is how
            all three of the author's relations fall out of one comparison:
            its start against a start, its start against an end, its end
            against an end. */
        const auto consider = [&] (double edge, const char* which, const SnapTarget& target)
        {
            const auto apart = std::abs (edge - target.seconds);

            if (apart >= nearest)
                return;

            nearest = apart;

            //  What to write is always the START, however the match was made.
            const auto at = preWaitFor (wantedStart + (target.seconds - edge));

            const auto where = target.name.empty()
                                 ? std::string ("the group's start")
                                 : "\"" + target.name + "\"'s " + target.edge;

            best = Snapped { at, std::string ("its ") + which + " to " + where };
        };

        for (const auto& target : targets)
        {
            consider (wantedStart, "start", target);

            if (lengthKnown && length > 0.0)
                consider (wantedStart + length, "end", target);
        }

        return best;
    }
}
