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

    THE POINTER DESCENDS INTO A MANUAL SEQUENCE GROUP, and steps over every
    other kind as one sibling. PRD §3.6: "manual - a member starts on GO. The
    standby pointer DESCENDS INTO the group; the operator is the parent."

    Which is the whole distinction. A timeline group schedules its members at
    entry and an automatic sequence advances itself, so in both the machine is
    the parent and there is nothing for the pointer to do inside: it steps past
    the whole chain to the next sibling, the instant GO is pressed (§3.5). In a
    manual group the operator is the parent, so the pointer has to be able to
    stand on each member in turn.

    Phase 1 stepped over ALL of them and said so where it asserted it - "a
    Phase 1 group has no runtime behaviour to descend into" - and named the test
    for the choice so that this moment would be visible rather than a surprise.

    A HEADER AND A FOOTER ARE NEVER ENTERED. They are cue lists the group runs
    for itself (§3.6); the pointer is the operator's position in the show, and
    the operator does not step through a group's preparation.

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

    /*  The next and previous top-level child of `list`, given where the standby
        is now.

        At the ends and from empty they return `current` unchanged - the pointer
        stays put rather than wrapping or arming itself. "Next past the end
        stays put" is the approved plan's; staying put from EMPTY is the
        author's (2026-09-06), and it means only `standby.set` arms a list.
        There is no wrap anywhere, which is what the end-of-list rule is for.

        Disabled cues are NOT skipped. A disabled cue is still a row in the
        list, and skipping is a running-behaviour decision that Phase 1 has no
        runner to justify; Phase 3 revisits it when a GO that does nothing
        becomes a real failure rather than a hypothetical one. */
    /*  Where the standby goes next, and where it came from.

        THESE REPLACED A FLAT `nextOf`/`previousOf` in PR 3.4 rather than
        joining them, because two answers to "where does the pointer go" would
        eventually be two DIFFERENT answers - and the one that would have gone
        stale is the one the invariant is checked against.

        It descends into an enabled manual sequence group to its first enabled
        member, steps over a timeline or automatic group as one sibling
        (positionally past the whole chain - §3.5), skips disabled cues, never
        enters a header or a footer, and climbs back out to the group's next
        sibling when its members are exhausted.

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

    /*  Whether the pointer may stand on this cue: it belongs to this list, and
        every group between it and the list is a manual sequence.

        The rule §3.5 implies rather than states. A pointer inside an automatic
        chain would be a pointer the machine also moves, and two things moving
        one pointer is how an operator presses GO expecting cue 12 and gets 14. */
    bool isOnManualPath (const juce::ValueTree& list, const std::string& cueId);

    /** True when `cueId` is one of `list`'s immediate children. */
    bool isTopLevelChild (const juce::ValueTree& list, const std::string& cueId);

    /** True when `cueId` is anywhere in this list, at any depth - including
        places the pointer may not stand. What tells "another list's cue" from
        "this list's cue, inside a chain the machine advances". */
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
}
