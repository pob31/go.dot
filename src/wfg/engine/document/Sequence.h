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
    A container's SEQUENCE: which of its children are members, and where the
    next one goes.

    A group's children are not all cues. A <Header>, a <Footer> and a
    <Persistent> section sit among them, each carrying an identifier of its own,
    and none of them is a member of anything: a header is what runs BEFORE the
    members (§3.6), a footer what runs after, a section what outlives a go
    (§3.29). So a container holds two sequences at once — the raw children
    juce::ValueTree keeps, and the members a show is made of — and in any group
    with a header the two are different lengths and the same child sits at two
    different numbers.

    ONLY ONE OF THEM IS PUBLISHED. `/godot/cue/<id>/order` and
    `/godot/list/<id>/order` list the members; the console renders a row per
    entry; and nothing anywhere in the namespace says where the <Header> element
    sits among its siblings. A client counting rows is therefore counting
    members, and it has no other number it could be counting.

    WHICH IS WHY AN INDEX IN A COMMAND IS A MEMBER POSITION. `object.move <id>
    <parent> <index>` and `cue.create <parent> <index> …` handed their index
    straight to juce::ValueTree, so the engine read a raw child index where the
    client had sent a member position. Measured on the live engine (2026-09-16)
    on a group whose children were [Header, Media, Osc, Group, Footer]: the
    inspector's ▼ on the Osc sent index 1, the Osc swapped places with the
    <Header> element, and NO MEMBER MOVED. A button that silently does nothing -
    and every drag about to be built on top of it would have inherited it.

    ONE RULE, ONE PLACE. The predicate was the parameter tree's, inside
    `orderOf`, where it decides what `order` says — and the door translating an
    index now has to ask exactly the same question. A second copy of it is how a
    publisher and a door come to disagree about what a show says, so it lives
    here and `orderOf` asks it too. Kept in `document/` beside the other pure
    rules: it takes a tree and answers a question about it, holds nothing, and
    changes nothing.
*/

#include <juce_data_structures/juce_data_structures.h>

#include <limits>

namespace wfg::doc
{
    /*  Whether this child is one of its parent's members — one of the things
        `order` names, and one of the positions a command's index counts.

        An identified child that is not a header, a footer or a persistent
        section. It asks the element name because the element name is what the
        distinction IS: there is no "member" flag on a child to read, and adding
        one would be a second place to keep correct and a way for a loaded file
        to arrive saying something impossible.

        A <TRIGGER> IS COUNTED, which is worth knowing before it surprises
        somebody. A trigger carries an identifier, may be a child of a group,
        and is not one of the three named elements — so `orderOf` lists it
        today, and so does this. Whether it SHOULD is a separate question, and
        an open one: the tree walk that publishes `/godot/cue/<id>/index` skips
        triggers, so `order` and `index` already disagree about a group that
        holds one. The answer this file gives is deliberately "what `order`
        publishes" and not "what a person would call a cue", because the index
        in a command is a position in the one sequence the client can see.
        Changing it means changing `order`, this, and the walk together, in a PR
        whose subject that is. */
    bool isSequenceChild (const juce::ValueTree& child);

    /*  The raw child index at which a child sits so that it takes member
        position `position`: the number juce::ValueTree wants, worked out from
        the number a client sent.

        It answers the raw index of the member that holds that position now — so
        a child added there pushes that member along and takes its place — and
        `parent.getNumChildren()` when the position is at or past the end, which
        appends. A position of nought on a group whose first child is its
        <Header> therefore answers 1: after the header, before the first member,
        which is where member nought belongs.

        COMPUTED OVER THE PARENT'S CHILDREN AS THEY STAND, the child being moved
        included if it is already there. That is what makes the same answer
        serve `ValueTree::moveChild`, whose index is where the child ENDS UP
        rather than where it is inserted; the arithmetic is worked through at
        the call site in `ShowDocument::move`, which is where a reader will be
        standing when they need it.

        A negative position answers like nought rather than refusing: the doors
        refuse one before they get here, and a rule that answers a question
        should not also be a second place that judges it. */
    int rawIndexForPosition (const juce::ValueTree& parent, int position);

    /*  A position no sequence can reach: "after the last member, whatever this
        parent holds".

        The creates that append rather than place pass it — a route, a trigger,
        a range, a rack channel, a header — and it says in a word what
        `parent.getNumChildren()` used to say by accident. It also says the only
        honest thing about the two creates whose child is NOT a member: a
        header has no member position, so it asks for none, and lands at the end
        of the children exactly as it always did. */
    constexpr int endOfSequence = std::numeric_limits<int>::max();
}
