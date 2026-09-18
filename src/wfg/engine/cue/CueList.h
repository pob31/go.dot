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

#pragma once

/*
    The cue list's pointer: where GO will act, and which list it will act on.

    THIS OWNS NO DATA. The standby itself is an attribute of the show - the
    `standby` property of a `<List>`, addressed `/godot/list/<id>/standby`,
    stored since PR 1.3 and restored from state.xml on load. What lives here is
    the arithmetic of MOVING it: which cue comes next, which comes before, and
    which list the argument-less commands mean. A second copy of the pointer
    would be a second thing for a replay to diverge on.

    STANDBY IS AN IDENTIFIER, never an index and never a cue number. A cue
    number is a mutable label renumbered during tech (PRD §3.5) and an index
    silently re-points the moment anything is inserted above it - and
    `cue.create` and `object.move` both already exist. An identifier survives
    both, which is the whole reason objects are addressed by identity.

    IT MOVES ONLY WHEN SOMETHING SAYS SO. PRD §3.5 is explicit that selection
    and scrolling never move it, and Phase 1 has neither - but it also has no
    GO, so in this phase the standby moves for exactly four reasons: one of the
    standby commands, a direct write to its node, a load, and the structural
    repair below. Nothing else touches it.

    THE POINTER MAY STAND INSIDE EVERY GROUP; THE WALK DESCENDS INTO ONLY A
    MANUAL ONE. Two rules, not one, and holding them apart is the whole of
    reading this file.

    THE WALK is the older of the two and is unchanged. PRD §3.6: "manual - a
    member starts on GO. The standby pointer DESCENDS INTO the group; the
    operator is the parent." A timeline group schedules its members at entry and
    an automatic sequence advances itself, so in both the machine is the parent;
    a reader stepping down a list with `next` lands on such a group's own row and
    the following press steps past the whole chain to the next sibling, the
    instant GO is pressed (§3.5). GO on a scene fires the scene, and that is the
    behaviour every show written before this depends on.

    WHERE THE POINTER MAY BE PUT widened on 2026-09-16, asked for by the author
    with the page open. They could move the pointer from group to group but could
    not select a cue within a group to start from that level, "even start all
    cues timelines should move the standby pointer from one cue to the next to
    try each individual cue it contains". So `standby.set` now accepts any
    enabled cue this list holds, at any depth - the members of a timeline or an
    automatic group among them - and once the pointer is inside a group of any
    kind, `next` and `previous` walk that group's members and climb out at its
    ends. GO fires the one cue the pointer is on; starting the group FROM there
    is a second named gesture, which is a later round's.

    Between them the two rules say this: the inside of a group the machine
    parents is somewhere the pointer may be PUT and never somewhere the walk
    carries it into. What that costs where the two meet - stepping out of such a
    group and back - is written out at `stepFrom` in the .cpp, along with why it
    is left alone until somebody has answered it with a show open.

    The reason the old rule gave for refusing all of this was that a pointer
    inside an automatic chain would be a pointer the machine also moves. That was
    about the operator's mental model and not about a race: only GO ever writes
    the standby, and the runner never does. A model is the author's to choose,
    and they have chosen this one.

    Phase 1 stepped over ALL of them and said so where it asserted it - "a
    Phase 1 group has no runtime behaviour to descend into" - and named the test
    for the choice so that this moment would be visible rather than a surprise.

    A HEADER, A FOOTER AND A PERSISTENT SECTION ARE NEVER ENTERED, and that did
    not widen with the rest. They are cue lists a group runs for itself (§3.6);
    the pointer is the operator's position in the show, and the operator does not
    step through a group's preparation. They are what a refusal from the standby
    door now means, and the reason it carries - `not-a-stop` - is named for them.

    FOCUS IS RESOLVED RATHER THAN MAINTAINED, and since PR 3.2 it is also
    PUBLISHED. Phase 1 settled it at the smallest thing that made `standby.next`
    unambiguous: a string on this object, not a node, not written to a file. That
    was the right size for a phase with one list in it.

    Phase 3 has parallel lists, which is what gives focus something to be
    exclusive about, so it is now `/godot/list/focus` - a document attribute a
    client can read, a surface can move, and `state.xml` remembers, on the same
    argument that persisted the standby: a rehearsal reopened where it was left
    is the kinder default, and losing that file costs only where somebody had
    got to.

    What did NOT change is the resolving. It is still a request that falls back
    to the first list whenever it names nothing, so creating and deleting lists
    cannot leave the engine pointed at a list that is gone, and "exactly one list
    is focused whenever a list exists" stays true by construction rather than by
    upkeep.
*/

#include <wfg/engine/document/ShowDocument.h>

#include <wfg/engine/cue/Run.h>

#include <juce_data_structures/juce_data_structures.h>

#include <string>
#include <vector>

namespace wfg::cue
{
    /*  The identifiers of a container's immediate children, in order.

        Read from the ValueTree's child sequence and not from the `order`
        attribute: `order` is `persist=none` and is computed from this same
        sequence when the tree publishes it, so reading it back would be asking
        a derived value what its own source says. */
    std::vector<std::string> childrenOf (const juce::ValueTree& container);

    /*  Where the standby goes next, and where it came from.

        At the ends and from empty they return `current` unchanged - the pointer
        stays put rather than wrapping or arming itself. "Next past the end stays
        put" is the approved plan's; staying put from EMPTY is the author's
        (2026-09-06), and it means only `standby.set` arms a list. There is no
        wrap anywhere, which is what the end-of-list rule is for.

        (Two of Phase 1's sentences stood here until 2026-09-16 and had gone
        false where a reader would trust them most: "the next and previous
        TOP-LEVEL CHILD", which PR 3.4 widened to the manual path, and "disabled
        cues are NOT skipped", which `stops` has filtered out since. Both were
        contradicted three paragraphs further down, so the file disagreed with
        itself about what a press of `next` does. Struck rather than left to be
        read past: this header is the specification.)

        THESE REPLACED A FLAT `nextOf`/`previousOf` in PR 3.4 rather than
        joining them, because two answers to "where does the pointer go" would
        eventually be two DIFFERENT answers - and the one that would have gone
        stale is the one the invariant is checked against.

        It descends into an enabled manual sequence group to its first enabled
        member, steps onto a timeline or automatic group as one sibling and then
        past the whole chain (§3.5), skips disabled cues, never enters a header,
        a footer or a persistent section, and climbs back out to the group's next
        sibling when its members are exhausted.

        AND IT WALKS THE MEMBERS OF A GROUP IT IS ALREADY INSIDE, whatever kind
        of group that is. Since 2026-09-16 `standby.set` can park the pointer on
        a member of a timeline or an automatic group, to try one cue of a scene
        on its own; from there these step through that group's members one at a
        time and climb out at its ends, exactly as they do in a manual one. Only
        the way IN differs, and only for a group the machine parents - the walk
        stops on its row and steps over it.

        THE DOCUMENT IS ENOUGH TO ANSWER THIS. Which way the pointer goes is a
        question about the SHOW - what is a manual group, what is enabled - and
        not about what happens to be running, so a cursor that needed the run
        table would be one that answered differently in a rehearsal from in a
        plotting session. (§3.6's loop rule, where a manual group's pointer
        wraps while rounds remain, is the one place a run does bear on it, and
        that arrives with rounds in PR 3.5.) */
    std::string nextStandby (const juce::ValueTree& list, const std::string& current,
                             const RunTable* runs = nullptr);
    std::string previousStandby (const juce::ValueTree& list, const std::string& current);

    /*  WHERE THE POINTER GOES AFTER A GO, which is `nextStandby`'s answer
        everywhere except at the end of the list: firing the last cue CLEARS the
        pointer rather than leaving it standing on the cue that has just gone
        (author, 2026-09-18).

        THE ARROWS KEEP THE OLD ANSWER, and that difference is the whole reason
        this is a second function rather than an edit to the first. Walking off
        the end is somebody LOOKING, and a pointer that vanished under a keypress
        would be the machine taking their place away. Firing off the end is the
        show being OVER, and an empty pointer is the resting state a list carries
        before anybody armed it (§3.5, §4.6) - so a show that has been run through
        ends where it began, and a second GO is applied and does nothing instead
        of firing the last cue a second time.

        A manual group with rounds left still keeps the pointer, here as there:
        the group is not finished, so neither is the list. */
    std::string standbyAfterFiring (const juce::ValueTree& list, const std::string& current,
                                    const RunTable* runs = nullptr);

    /*  MAY THE POINTER STAND HERE: is `cueId` one of this list's stops - an
        enabled cue it holds, at any depth, that is not inside a header, a footer
        or a persistent section - or the empty string, which is nowhere at all
        and is always legal because an empty pointer is a resting state (§3.5).

        NAMED FOR THE QUESTION SINCE 2026-09-16, because the answer changed. It
        was `isOnManualPath` and it meant "every group between this cue and the
        list is a manual sequence"; the pointer may now be put inside a timeline
        or an automatic group too, so the old name described a rule that is no
        longer the rule - and a name that lies about the rule is worse than no
        comment at all. Its refusal moved with it: `not-manual-path` became
        `reason::notAStop`, "not-a-stop", which is what is actually left to
        refuse.

        BOTH DOORS ASK THIS ONE - `standby.set` and the document's own write door
        - so a client cannot learn one answer from the command and another from
        the node, and so the pointer cannot be put anywhere `next` and `previous`
        could not then walk it away from. It is NOT the same question as where
        the walk would have LANDED it: `standby.set` reaching somewhere the walk
        does not go is the point of this change, not a hole in it. */
    bool mayStandOn (const juce::ValueTree& list, const std::string& cueId);

    /** True when `cueId` is one of `list`'s immediate children. */
    bool isTopLevelChild (const juce::ValueTree& list, const std::string& cueId);

    /** True when `cueId` is anywhere in this list, at any depth - including
        places the pointer may not stand. What tells "another list's cue"
        (`not-in-list`) from "this list's cue, in some group's own header, footer
        or persistent section" (`not-a-stop`), which are two refusals that send
        somebody somewhere different. */
    bool isInList (const juce::ValueTree& list, const std::string& cueId);

    //==============================================================================
    /*  Which list the argument-less standby commands act on.

        Runtime state, held by whoever wires the engine and handed to the
        commands. Not published and not persisted - see the note above.
    */
    class Focus
    {
    public:
        /*  Asks for a list by identifier. False, and nothing changes, when the
            id names no list - including when it names a cue or a mount, since
            an identifier alone is unambiguous and the wrong kind of object is a
            mistake rather than a coincidence.

            The document is not const because this WRITES: focus is an attribute
            of the show's list collection, and it goes through the same single
            door every other attribute does. */
        bool request (doc::ShowDocument& document, const std::string& wanted);

        /** Back to no request, and so to the first list. What deleting the
            focused list leaves behind. */
        void clear (doc::ShowDocument& document);

        /*  The focused list: the requested one if it is still there, otherwise
            the first list in the show, otherwise nothing.

            RESOLVED RATHER THAN MAINTAINED, which is the point. Nothing has to
            remember to move the focus when a list is created or deleted,
            because there is no stored value that can go stale - and "exactly
            one list is focused whenever a list exists" is true by construction
            rather than by upkeep. */
        juce::ValueTree list (const doc::ShowDocument& document) const;

        /** The identifier of that list, or empty when the show has none. */
        std::string listId (const doc::ShowDocument& document) const;

        /** What was last asked for, whether or not it resolves. Tests and
            diagnostics; the engine reads list() instead. */
        std::string requested (const doc::ShowDocument& document) const;
    };

    /** Where the focus lives: `/godot/list/focus`, one attribute of the show's
        collection of lists. Spelled once, because three files write it. */
    std::string focusAddress();

    //==============================================================================
    /*  The address of a list's standby node, which is where every write to it
        goes - the commands, a client's `node.set`, and the load path all use
        this one spelling. */
    std::string standbyAddressOf (const std::string& listId);

    /*  Where the ran-out flag lives, beside the pointer it qualifies. See the
        parameter table's `list,finished`: it is what tells an empty pointer
        nobody has armed from an empty pointer that has been all the way
        through, which the persistent solver has to know and cannot infer. */
    std::string finishedAddressOf (const std::string& listId);
}
