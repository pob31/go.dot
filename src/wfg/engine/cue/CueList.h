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

    GROUPS ARE OPAQUE SIBLINGS, and that is a PHASE 1 CHOICE rather than a
    conclusion. PRD §3.6 says the pointer descends into a manual sequence group,
    and Phase 3 will implement that; a Phase 1 group has no runtime behaviour to
    descend into, and the namespace draft's `standby.set` constraint already
    says the cue must be a top-level child. So traversal steps over a group as
    one sibling, and the test that asserts it is named for the choice rather
    than for a rule.

    FOCUS IS RUNTIME AND RESOLVED, NOT STORED AND MAINTAINED (author, 2026-09-06).
    The development plan puts the focus model in Phase 3 and the namespace draft
    puts a published `/godot/list/focus` node in this one; the author settled it
    at the smallest thing that makes `standby.next` unambiguous - engine state,
    not a published node, not written to state.xml. Which means it needs no
    maintenance: it is a request that falls back to the first list whenever the
    request names nothing, so creating and deleting lists cannot leave it
    pointing at a list that is gone. Phase 3 publishes it when parallel lists
    give it something to be exclusive about.
*/

#include <wfg/engine/document/ShowDocument.h>

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
    std::string nextOf (const juce::ValueTree& list, const std::string& current);
    std::string previousOf (const juce::ValueTree& list, const std::string& current);

    /** True when `cueId` is one of `list`'s immediate children. */
    bool isTopLevelChild (const juce::ValueTree& list, const std::string& cueId);

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
            mistake rather than a coincidence. */
        bool request (const doc::ShowDocument& document, const std::string& wanted);

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
        const std::string& requested() const noexcept { return requestedId; }

    private:
        std::string requestedId;
    };

    //==============================================================================
    /*  The address of a list's standby node, which is where every write to it
        goes - the commands, a client's `node.set`, and the load path all use
        this one spelling. */
    std::string standbyAddressOf (const std::string& listId);
}
