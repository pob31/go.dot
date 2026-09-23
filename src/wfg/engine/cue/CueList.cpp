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

#include <wfg/engine/cue/CueList.h>

#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  The `Lists` container, or an invalid tree. The show always has one -
            ShowDocument's constructor makes it - but a document adopted from a
            file could in principle not, and asking is cheaper than assuming. */
        juce::ValueTree listsContainer (const doc::ShowDocument& document)
        {
            for (const auto& child : document.root())
                if (child.getType().toString() == "Lists")
                    return child;

            return {};
        }
    }

    //==============================================================================
    std::vector<std::string> childrenOf (const juce::ValueTree& container)
    {
        std::vector<std::string> ids;

        if (! container.isValid())
            return ids;

        for (const auto& child : container)
            if (child.hasProperty (idProperty))
                ids.push_back (child[idProperty].toString().toStdString());

        return ids;
    }

    bool isTopLevelChild (const juce::ValueTree& list, const std::string& cueId)
    {
        if (cueId.empty())
            return false;

        const auto ids = childrenOf (list);
        return std::find (ids.begin(), ids.end(), cueId) != ids.end();
    }

    //==============================================================================
    namespace
    {
        /*  Whether a cue is a group the WALK goes inside: a manual sequence.

            THE WALK, and no longer "the pointer", which is the distinction this
            file turns on since 2026-09-16. The pointer may be put inside a group
            of any kind; what only a manual sequence gets is `descendTo` entering
            it when a reader steps onto its row from outside. The two are
            written out at `descendTo` and `findOnPath` below.

            Read from the document with its defaults applied - `mode` defaults
            to `sequence` and `advance` to `manual`, so a group somebody made
            and did not configure is one the operator drives, which is the
            gentler default of the two. An attribute absent from a ValueTree is
            the default and not an empty string, and reading it directly gets
            that wrong; here the elements are checked against the same words the
            table uses. */
        bool isManualSequence (const juce::ValueTree& cue)
        {
            if (! cue.isValid() || cue.getType().toString() != "Group")
                return false;

            const auto mode = cue[juce::Identifier ("mode")].toString();
            const auto advance = cue[juce::Identifier ("advance")].toString();

            return (mode.isEmpty() || mode == "sequence")
                     && (advance.isEmpty() || advance == "manual");
        }

        /*  ABSENT MEANS THE DEFAULT, AND PRESENT MEANS ITS OWN TYPE - which
            is two ways to get this wrong and the reason it is written out.

            The canonical writer omits an attribute holding its default, so a
            cue nobody has disabled has no `enabled` property at all and the
            answer is `true`. And a cue somebody HAS disabled holds a BOOLEAN
            false, not the text "false": every value goes into the tree through
            the schema, typed. Comparing the text would have answered "0" and
            matched nothing, which is exactly what the first version did. */
        /*  A SAMPLER GROUP (PRD §3.27, Phase 6): a hand launches its members
            from strips, so none of them is a place the pointer stands - §3.27
            refuses them to standby-set the way a header's cues are refused,
            and the flag PRD §3.5's widening left on that sentence is honoured
            by building it as written (namespace draft §16.5). The group ITSELF
            is a place: GO on it arms the bank. Absent means `sequence`. */
        bool isSamplerGroup (const juce::ValueTree& cue)
        {
            return cue.isValid() && cue.getType().toString() == "Group"
                     && cue[juce::Identifier ("mode")].toString() == "sampler";
        }

        bool isEnabled (const juce::ValueTree& cue)
        {
            const juce::Identifier enabled { "enabled" };

            return ! cue.hasProperty (enabled) || static_cast<bool> (cue[enabled]);
        }

        /*  Whether this element is a cue at all - not a header, a footer, a
            route, a range or a trigger.

            ASKED THROUGH THE OWNER WORD rather than by listing the kinds, and
            the reason is that the list was here and was wrong: a MIDI cue was
            added in PR 3.11 and the standby pointer could not stand on one,
            which showed up as a saved show refusing to restore its own pointer
            with the standby refusal - `not-a-stop` today, `not-manual-path`
            when it happened - a message about nesting, for a cue at the top
            level of its list.

            `ownerForElement` answers "cue" for every kind there is, because
            that is what decides the ADDRESS a cue is published at; a kind that
            is not in it is a kind nothing else works for either. The same
            generalisation `createTrigger` made, for the same reason, after the
            same list had grown twice and been forgotten twice. */
        bool isCueElement (const juce::ValueTree& node)
        {
            return doc::ShowDocument::ownerForElement (node.getType().toString().toStdString())
                     == "cue";
        }

        /*  The cues the pointer may stand on among a container's children.

            THE WORD THE REST OF THIS FILE IS SPELLED IN, and the one a refusal
            carries out to a client: a cue that is not one of these is refused
            with `not-a-stop`. What it leaves out is everything that is not an
            enabled cue ELEMENT of this container - a <Header>, a <Footer>, a
            <Persistent> section, a route, a range, a trigger - which is why
            those are never entered by anything below: they are not stops, so
            nothing recurses into them. */
        std::vector<juce::ValueTree> stops (const juce::ValueTree& container)
        {
            std::vector<juce::ValueTree> out;

            for (const auto& child : container)
                if (child.hasProperty (idProperty) && isCueElement (child) && isEnabled (child))
                    out.push_back (child);

            return out;
        }

        /*  Where a pointer ARRIVING FROM OUTSIDE lands on this cue: on the cue
            itself, unless it is a manual group, in which case on its first
            member - and so on down, because a manual group's first member may
            be one too.

            An EMPTY manual group has nowhere inside it, so the pointer stands
            on the group row itself: GO there completes it, which is the honest
            thing for a container somebody has not filled in yet.

            MANUAL SEQUENCES ONLY, AND THAT IS NOT AN OVERSIGHT. This is the
            line that keeps GO firing a scene rather than a scene's first cue,
            and the next person to read these four functions will be tempted to
            make them agree with `findOnPath`, which since 2026-09-16 descends
            into every group. They are not the same question and they must not
            give the same answer.

            `findOnPath` answers MAY THE POINTER BE HERE, and the answer is yes
            for any member of any group: the operator may park inside a timeline
            or an automatic group to try one of its cues on its own. This
            answers WHERE DOES THE WALK PUT IT, and for a group the machine
            parents the answer has to be the group's own row. A reader stepping
            down a list expects `next` to land on the scene and GO to fire the
            scene; if this descended too, that press would start the scene's
            first cue alone and every show already written would do something
            different under its operator's hands.

            So the inside of a non-manual group is somewhere the pointer may be
            PUT and never somewhere the walk carries it into. What that costs at
            the far end is written out in `stepFrom`. */
        juce::ValueTree descendTo (const juce::ValueTree& cue)
        {
            if (! isManualSequence (cue))
                return cue;

            const auto inside = stops (cue);

            return inside.empty() ? cue : descendTo (inside.front());
        }

        /** The last place the walk puts a pointer arriving from outside - the
            mirror of `descendTo`, and manual sequences only for the same reason
            it is. */
        juce::ValueTree descendToLast (const juce::ValueTree& cue)
        {
            if (! isManualSequence (cue))
                return cue;

            const auto inside = stops (cue);

            return inside.empty() ? cue : descendToLast (inside.back());
        }

        /*  The cue with this identifier, searched everywhere the pointer is
            allowed to stand: down through every group, at any depth, and never
            into a header, a footer or a persistent section - `stops` leaves
            those out, so nothing here has to name them.

            EVERY GROUP, NOT ONLY THE MANUAL ONES, since 2026-09-16, and this
            one word is the whole of the change. The author asked for it with
            the page open: they could move the pointer from group to group but
            could not select a cue within a group to start from that level, and
            "even start all cues timelines should move the standby pointer from
            one cue to the next to try each individual cue it contains". Making
            the pointer FINDABLE inside a timeline group is enough to make it
            walkable there too, because `stepFrom` already walks `stops (parent)`
            and climbs out at the ends - it never asked what kind of group it was
            standing in, only what kind it was stepping onto.

            THE OLD RULE WAS ABOUT THE MENTAL MODEL, NOT ABOUT A RACE, which is
            why it could be changed by a decision rather than by a mechanism. It
            read "only manual sequences", and the reason recorded for it was that
            a pointer inside an automatic chain would be a pointer the machine
            also moves, and two things moving one pointer is how an operator
            presses GO expecting cue 12 and gets 14. But only GO ever writes the
            standby; the runner never does, and measuring a live engine confirmed
            it. The model was the author's to choose and they have chosen the
            other one: the pointer may stand inside every group, GO fires the one
            cue it is on, and a second named gesture - not in this round - starts
            the group from there.

            What this does NOT change is where the walk LANDS from outside, which
            is `descendTo`'s question and still answers manual sequences only.

            AND IT ASKS NOTHING ABOUT THE CHILD'S KIND before recursing, on the
            same argument that generalised `isCueElement` above: a list of which
            elements can contain cues is a list that grows and is forgotten.
            `stops` of anything that holds no cues is empty and the recursion
            ends there, so the test would only be an optimisation over a handful
            of children, bought with a second place to keep in step. */
        juce::ValueTree findOnPath (const juce::ValueTree& container, const std::string& cueId)
        {
            for (const auto& child : stops (container))
            {
                if (child[idProperty].toString().toStdString() == cueId)
                    return child;

                if (isSamplerGroup (child))
                    continue;

                if (const auto found = findOnPath (child, cueId); found.isValid())
                    return found;
            }

            return {};
        }

        /*  The step after `from` within its own container, descending into what
            it finds; or an invalid tree when there is nothing after it, which is
            what tells the caller to climb.

            `list` is the top, so climbing stops there rather than walking out of
            the show.

            IT NEVER ASKS WHAT KIND OF GROUP IT IS STANDING IN. It walks
            `stops (parent)` whatever the parent is, which is why widening
            `findOnPath` was enough to give the author what they asked for: a
            pointer that can be FOUND inside a timeline group is one this walks
            around inside it, one member at a time, for no new code at all. */
        juce::ValueTree stepFrom (const juce::ValueTree& list, const juce::ValueTree& from,
                                  bool forwards)
        {
            auto cue = from;

            while (cue.isValid() && cue != list)
            {
                const auto parent = cue.getParent();
                const auto siblings = stops (parent);

                const auto at = std::find (siblings.begin(), siblings.end(), cue);

                if (at != siblings.end())
                {
                    if (forwards)
                    {
                        if (at + 1 != siblings.end())
                            return descendTo (*(at + 1));
                    }
                    else if (at != siblings.begin())
                    {
                        return descendToLast (*(at - 1));
                    }
                }

                /*  Exhausted where it was, so it climbs: the next place after
                    the last member of a group is whatever follows the GROUP,
                    and the place before its first member is whatever precedes
                    it.

                    THE WALK NEVER RESTS ON A MANUAL GROUP'S ROW, which is
                    decision M (2026-09-06) seen from the inside: GO at a manual
                    group's row fires its first member, so the row and the first
                    member are one position rather than two, and `descendTo`
                    never leaves the pointer on the row going forwards. Stopping
                    there going backwards would have made the path asymmetric - a
                    press of `previous` followed by `next` would not have come
                    back to where it started. (`standby.set` can still put the
                    pointer on that row, and always could: the row is a stop like
                    any other cue. It is the WALK that steps through it.)

                    IT DOES REST ON ANY OTHER GROUP'S ROW - it has to, or a
                    timeline group could not be armed at all - and that is where
                    the asymmetry went instead. Here is exactly what happens,
                    read off these lines rather than guessed, because it is the
                    first thing anybody will try after 2026-09-16.

                    With the pointer on the FIRST member of a timeline or an
                    automatic group, `previous` finds nothing before it among
                    `stops (group)`, climbs to the group, and takes the group's
                    previous SIBLING - `descendToLast` of it, so that sibling's
                    last member when the sibling is a manual sequence. The
                    group's own row is stepped straight over. When the group is
                    the first child of the list there is nothing before it
                    either, the climb reaches `list`, and the pointer stays put.

                    And `next` from where `previous` just left lands on the
                    group's ROW, not back on the member it came from, because
                    `descendTo` does not enter a group the machine parents. So
                    out of a non-manual group, `previous` then `next` is not a
                    round trip: it leaves the operator one row higher than they
                    started, on the scene rather than in it. Entering is the same
                    shape read the other way - from the row, `next` steps past
                    the whole chain, so the members are reachable by
                    `standby.set` and by walking on from one of them, never by
                    walking in.

                    LEFT EXACTLY AS IT IS, deliberately, and it is a question to
                    answer with the page open and a show on it. Both ways of
                    closing it - resting on the row on the way out of its own
                    members, or refusing to climb out of a group the pointer was
                    put inside - change what `previous` and `next` do somewhere
                    else, and neither is the change the author asked for in this
                    round. */
                cue = parent;
            }

            return {};
        }

        /*  A MANUAL GROUP WITH ROUNDS LEFT KEEPS THE POINTER, which is the one
            question the cursor asks about what is RUNNING rather than about
            what is written.

            It has to. The document says a group loops three times; only the run
            knows it is on round two. Leaving on the last member of round one
            would take the operator out of a scene that has two thirds of itself
            still to play, and their next press would fire whatever follows the
            group while the group was still going.

            The pointer goes to the first member of the round, in document
            order, which is where the next round will begin: a manual group
            plays its members as they are written - shuffling and "play N of M"
            are the machine choosing, and in a manual group the operator is the
            one choosing (PRD 3.6). `wfg validate` warns about a manual group
            that declares either.

            RUN-AWARE AND OPTIONAL, so that every caller that has no run table -
            a validator, a test of the document alone - gets the pure document
            answer and the same behaviour a group with no loops has.

            STILL KEYED ON `isManualSequence`, AND NOT ON "the pointer is inside
            a group", which is the one place in this file where the two did not
            move together on 2026-09-16. This rule is about §3.6's loop: the
            operator is the parent of a manual group, so the pointer has to stay
            in the scene while the scene has rounds to play. A timeline or an
            automatic group runs its own rounds without being asked, so a pointer
            parked inside one to try a single cue is a place somebody chose to
            stand, and holding it there while the machine looped would be the
            machine moving the operator around. It leaves at the last member,
            like any other stop.

            ANSWERED IN ONE PLACE because two things ask it now - the arrows
            and a GO - and this file's own argument against a second `nextOf`
            applies exactly as well to a second copy of this. Empty when
            nothing is holding the pointer, which both callers read as
            "walk on". */
        std::string heldForAnotherRound (const juce::ValueTree& from, const RunTable* runs)
        {
            if (runs == nullptr || ! from.isValid())
                return {};

            const auto group = from.getParent();

            if (! isManualSequence (group))
                return {};

            const auto members = stops (group);

            if (members.empty() || members.back() != from)
                return {};

            const auto groupId = group[idProperty].toString().toStdString();

            if (const auto* run = runs->liveRunOf (groupId))
                if (run->iterations == 0 || run->iteration < run->iterations)
                    return descendTo (members.front())[idProperty].toString().toStdString();

            return {};
        }
    }

    bool isInList (const juce::ValueTree& list, const std::string& cueId)
    {
        if (cueId.empty() || ! list.isValid())
            return false;

        for (const auto& child : list)
        {
            if (child.hasProperty (idProperty)
                  && child[idProperty].toString().toStdString() == cueId)
                return true;

            if (isInList (child, cueId))
                return true;
        }

        return false;
    }

    std::string nextStandby (const juce::ValueTree& list, const std::string& current,
                             const RunTable* runs)
    {
        if (! list.isValid())
            return current;

        const auto from = findOnPath (list, current);

        /*  Nowhere, or somewhere the pointer cannot be: it stays put. Only
            `standby.set` arms a list, so there is nothing here that turns
            "nowhere" into "the first cue" - and no wrap at either end, which is
            what the end-of-list rule is for. */
        if (! from.isValid())
            return current;

        if (const auto held = heldForAnotherRound (from, runs); ! held.empty())
            return held;

        const auto next = stepFrom (list, from, true);
        return next.isValid() ? next[idProperty].toString().toStdString() : current;
    }
    std::string standbyAfterFiring (const juce::ValueTree& list, const std::string& current,
                                    const RunTable* runs)
    {
        if (! list.isValid())
            return current;

        const auto from = findOnPath (list, current);

        /*  Nowhere, or somewhere the pointer cannot be. That is not the end of
            anything - it is a list nobody has armed - so it is left alone, and
            only `standby.set` arms a list. */
        if (! from.isValid())
            return current;

        /*  A group that still has rounds to play is not the end of the list
            either, whether or not it is the last thing in it. */
        if (const auto held = heldForAnotherRound (from, runs); ! held.empty())
            return held;

        const auto next = stepFrom (list, from, true);

        /*  AND THE END OF THE LIST CLEARS THE POINTER rather than leaving it
            standing on the cue that has just gone (author, 2026-09-18: "once
            the last cue of the show has been triggered and the standby has no
            other cue to go to, it should be cleared").

            AN EMPTY POINTER IS THE RESTING STATE and not a special case (§4.6):
            nowhere at all is always legal (§3.5), and it is what a list carries
            before anybody arms it - so a show that has been run through ends
            where it began. What it buys is what an operator would otherwise pay
            for: with the pointer left standing on the last cue, a second GO
            FIRED IT AGAIN. Now a second GO is applied and does nothing, which
            is what the `go` handler's own comment - "an operator at the end of
            a list has not made a mistake" - has claimed all along.

            GETTING IT BACK IS A CLICK, not an arrow. `standby.next` from
            nowhere stays put, deliberately and by the rule above, so both
            clients offer park on a row and that is how a list is armed again. */
        return next.isValid() ? next[idProperty].toString().toStdString() : std::string {};
    }

    std::string previousStandby (const juce::ValueTree& list, const std::string& current)
    {
        if (! list.isValid())
            return current;

        const auto from = findOnPath (list, current);

        if (! from.isValid())
            return current;

        const auto previous = stepFrom (list, from, false);
        return previous.isValid() ? previous[idProperty].toString().toStdString() : current;
    }

    bool mayStandOn (const juce::ValueTree& list, const std::string& cueId)
    {
        return cueId.empty() || findOnPath (list, cueId).isValid();
    }

    //==============================================================================
    /*  The parameter is `wanted` rather than the obvious `listId` because this
        class has a listId() accessor, and GCC's -Wshadow objects to a parameter
        that shadows a member - including a member FUNCTION. MSVC says nothing,
        so it would have been a Linux-only build failure. The same trap already
        cost this project two CI round trips. */
    std::string focusAddress()
    {
        return "/godot/list/focus";
    }

    bool Focus::request (doc::ShowDocument& document, const std::string& wanted)
    {
        const auto node = document.findById (wanted);

        /*  Nothing is written until the request is known good. A rejected
            list.focus must leave the previous focus exactly where it was, and
            assigning first would quietly clear it - a failure the fallback to
            the first list would then hide. */
        if (! node.isValid() || node.getType().toString() != "List")
            return false;

        /*  THROUGH THE DOCUMENT'S ONE WRITE DOOR, which is what changed in PR
            3.2. Focus was a string on this object: engine state, unpublished,
            forgotten on every close. Now it is `/godot/list/focus`, so a client
            can read which list GO acts on, a surface can move it, and a show
            reopens on the list the operator was working in - the same argument
            that persisted the standby, applied to the pointer that says which
            standby is being pointed at. */
        return document.setAttribute (focusAddress(), wanted).ok;
    }

    void Focus::clear (doc::ShowDocument& document)
    {
        document.setAttribute (focusAddress(), {});
    }

    juce::ValueTree Focus::list (const doc::ShowDocument& document) const
    {
        const auto requested = document.getAttribute (focusAddress()).value_or (std::string {});

        if (! requested.empty())
        {
            const auto node = document.findById (requested);

            if (node.isValid() && node.getType().toString() == "List")
                return node;
        }

        /*  The first list in the show. This is what makes "exactly one list is
            focused whenever a list exists" true without anything having to
            maintain it: a request that no longer resolves - the list was
            deleted, or the show was replaced under it - falls back here rather
            than leaving the engine pointed at nothing. */
        const auto lists = listsContainer (document);

        for (const auto& child : lists)
            if (child.getType().toString() == "List" && child.hasProperty (idProperty))
                return child;

        return {};
    }

    std::string Focus::requested (const doc::ShowDocument& document) const
    {
        return document.getAttribute (focusAddress()).value_or (std::string {});
    }

    std::string Focus::listId (const doc::ShowDocument& document) const
    {
        const auto node = list (document);
        return node.isValid() ? node[idProperty].toString().toStdString() : std::string {};
    }

    //==============================================================================
    std::string standbyAddressOf (const std::string& listId)
    {
        return "/godot/list/" + listId + "/standby";
    }

    std::string finishedAddressOf (const std::string& listId)
    {
        return "/godot/list/" + listId + "/finished";
    }
}
