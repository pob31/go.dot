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

/*
    WHICH CHILDREN ARE MEMBERS, AND WHERE THE NEXT ONE GOES.

    `document/Sequence.h` is two functions and no state: a predicate that says
    whether a child is one of the things `/godot/cue/<id>/order` publishes, and
    the arithmetic that turns a position in THAT sequence into the raw child
    index juce::ValueTree wants. Both doors that take an index from a client -
    `object.move` and `cue.create` - ask it, and so does `orderOf`, which draws
    the sequence in the first place. That is the whole reason the rule was lifted
    out of the publisher: one rule in one place cannot disagree with itself.

    DocumentTests.cpp asks the COMMANDS whether an operator's gesture lands where
    they meant it to, on one hand-authored group. This file asks the rule
    underneath, with no document, no engine and no parameter tree in the room:
    every shape a container can be in, every position anybody can send, and the
    two ends nobody sends on purpose.

    EVERY SHAPE BELOW IS ONE A REAL FILE REACHES, and it is worth saying how,
    because the shapes are what the arithmetic is actually about. `createRole`
    and `createPersistent` APPEND, so a group given its header before its cues
    carries it first; a group given its header after them carries it last; and a
    group that acquired a header, a footer and then another cue carries both in
    the middle. All three exist in shows that have been edited for a while, and
    the two doors have to be right about all three.

    THE CASES READ THE MEMBERS BACK WITH A PREDICATE OF THEIR OWN rather than
    with `isSequenceChild`. A case that asks the function under test what the
    answer was proves that the function agrees with itself, which is the one
    thing it was never in any danger of. The duplicate is one line long and it is
    deliberate.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/document/Sequence.h>

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace wfg::doc;

namespace
{
    /*  A child spelled the way a show document spells one: an element name, and
        the `id` attribute that is what makes a child something a client can
        name at all.

        The identifiers here are one letter where a document's are eight. These
        cases are read as whole child sequences - "Header#h Cue#a Cue#new" - and
        eight random characters six times over would hide the shape that is the
        entire subject. */
    juce::ValueTree child (const juce::String& element, const juce::String& id)
    {
        juce::ValueTree node { juce::Identifier (element) };
        node.setProperty ("id", id, nullptr);
        return node;
    }

    /*  And a child carrying no identifier. Today those are the containers -
        <Lists>, <Mounts>, <Rack> - which hold what a collection holds and
        nothing of their own. */
    juce::ValueTree unnamed (const juce::String& element)
    {
        return juce::ValueTree { juce::Identifier (element) };
    }

    juce::ValueTree container (const juce::String& element,
                               const std::vector<juce::ValueTree>& children)
    {
        juce::ValueTree parent { juce::Identifier (element) };

        for (const auto& c : children)
            parent.appendChild (c, nullptr);

        return parent;
    }

    /*  THE TEST'S OWN IDEA OF WHAT A MEMBER IS, written out in full rather than
        asked of `isSequenceChild`. It is the same sentence the rule under test
        contains, and that is the point: a case that reads its answer back
        through the function it is checking cannot tell a correct answer from a
        consistent one. The first case below pins this sentence against the
        function; everything after it uses the sentence. */
    bool memberByHand (const juce::ValueTree& node)
    {
        const auto element = node.getType().toString();

        return node.hasProperty ("id")
                 && element != "Header" && element != "Footer" && element != "Persistent";
    }

    std::vector<std::string> memberIds (const juce::ValueTree& parent)
    {
        std::vector<std::string> out;

        for (const auto& c : parent)
            if (memberByHand (c))
                out.push_back (c["id"].toString().toStdString());

        return out;
    }

    /*  A member sequence as one line, so that a failure prints what the sequence
        became and not `{?}`. The same spelling `order` uses. */
    std::string joined (const std::vector<std::string>& ids)
    {
        std::string out;

        for (const auto& id : ids)
        {
            if (! out.empty())
                out += ' ';

            out += id;
        }

        return out;
    }

    /*  A child as its element AND its identifier. The element is half of it
        because the subject of several cases below is where a <Header> ended up,
        and a line of identifiers alone would not say - which is exactly the
        blindness that let the measured defect publish a correct-looking order
        out of a file whose header had moved. */
    std::string spellOne (const juce::ValueTree& node)
    {
        return (node.getType().toString() + "#"
                  + (node.hasProperty ("id") ? node["id"].toString()
                                             : juce::String ("-"))).toStdString();
    }

    /** Every child of a container, in file order. */
    std::string spell (const juce::ValueTree& parent)
    {
        std::vector<std::string> out;

        for (const auto& c : parent)
            out.push_back (spellOne (c));

        return joined (out);
    }

    /** The same, of only the children that are NOT members. */
    std::string spellSections (const juce::ValueTree& parent)
    {
        std::vector<std::string> out;

        for (const auto& c : parent)
            if (! memberByHand (c))
                out.push_back (spellOne (c));

        return joined (out);
    }

    /*  How many children a container carries before its first member: the
        <Header> of a group that was given one first, and the <Header> and
        <Footer> of a group that was given both. Nothing a member position can
        name may ever land inside that run. */
    int leadingSections (const juce::ValueTree& parent)
    {
        int n = 0;

        while (n < parent.getNumChildren() && ! memberByHand (parent.getChild (n)))
            ++n;

        return n;
    }

    /** The cue these cases insert, named so a spelled sequence says where it went. */
    const juce::String newcomer { "new" };

    /*  What the rule is FOR, in one function: ask it where a new member goes,
        put one there, and hand back what the container became. A copy each
        time, so one shape can be asked a dozen questions. */
    juce::ValueTree afterInserting (const juce::ValueTree& parent, int position)
    {
        auto copy = parent.createCopy();
        const auto raw = rawIndexForPosition (copy, position);

        copy.addChild (child ("Cue", newcomer), raw, nullptr);
        return copy;
    }
}

//==============================================================================
TEST_CASE ("sequence: a member is an identified child that is not one of the three sections")
{
    /*  EVERY KIND A SHOW IS MADE OF IS A MEMBER OF ITS PARENT. One element per
        cue kind (author, 2026-09-05), so the predicate cannot be "is it a
        <Cue>" - a rule written that way would publish an `order` holding only
        the memos, and every fade, stop and media cue in the show would lose its
        row and its position. */
    CHECK (isSequenceChild (child ("Cue", "a")));
    CHECK (isSequenceChild (child ("Group", "a")));
    CHECK (isSequenceChild (child ("Media", "a")));
    CHECK (isSequenceChild (child ("Fade", "a")));
    CHECK (isSequenceChild (child ("Transport", "a")));
    CHECK (isSequenceChild (child ("Osc", "a")));
    CHECK (isSequenceChild (child ("Midi", "a")));
    CHECK (isSequenceChild (child ("List", "a")));

    /*  AND THE THREE THAT ARE NOT, each named for itself. A rule that forgot
        any ONE of them still answers correctly about the other two, and about
        every group that does not happen to hold the forgotten one - which is
        how a missing name survives a suite. <Persistent> is the likeliest to be
        forgotten, being the youngest (§3.29, decision S) and a list's rather
        than a group's. */
    CHECK_FALSE (isSequenceChild (child ("Header", "h")));
    CHECK_FALSE (isSequenceChild (child ("Footer", "f")));
    CHECK_FALSE (isSequenceChild (child ("Persistent", "p")));

    /*  WHATEVER THEY CARRY. A header is what runs before the members (§3.6) and
        it is an ordinary cue list, so it holds cues and carries attributes like
        anything else. A predicate that asked what a child CONTAINS rather than
        what it IS would count this one the moment somebody put a cue in it. */
    auto loaded = child ("Header", "h");
    loaded.appendChild (child ("Media", "h1"), nullptr);
    loaded.appendChild (child ("Osc", "h2"), nullptr);
    loaded.setProperty ("name", "Pre-arm the rig", nullptr);

    CHECK_FALSE (isSequenceChild (loaded));

    /*  AND NO IDENTIFIER IS NO MEMBER, WHATEVER IT IS CALLED. An unidentified
        child is nothing a client can address, so there is no position it could
        hold and no entry it could be in `order`; the containers are all of them
        today. The last two say the two halves are independent - a <Cue> without
        an id is still not a member, and an element named for a section without
        one is refused twice over rather than by accident. */
    CHECK_FALSE (isSequenceChild (unnamed ("Lists")));
    CHECK_FALSE (isSequenceChild (unnamed ("Mounts")));
    CHECK_FALSE (isSequenceChild (unnamed ("Rack")));
    CHECK_FALSE (isSequenceChild (unnamed ("Cue")));
    CHECK_FALSE (isSequenceChild (unnamed ("Header")));

    /*  A child that is not there at all answers no rather than reaching into
        nothing. `findById` hands back an invalid tree for an identifier that is
        not in the show, and a rule asked about one on the way to a refusal must
        answer rather than fall over. */
    CHECK_FALSE (isSequenceChild (juce::ValueTree {}));

    /*  A <TRIGGER> IS COUNTED, and this case says so out loud because it is the
        answer that surprises people. A trigger carries an identifier, may be a
        child of a group, and is not one of the three named elements - so
        `orderOf` lists it, and so must this, or the door and the publisher
        disagree about what a show says.

        Whether it SHOULD is open, and Sequence.h states the terms: the walk
        behind `/godot/cue/<id>/index` skips triggers, so the two published
        numbers already disagree about a group holding one, and changing it
        means changing `order`, this rule and that walk together. This line is
        here to fail when somebody changes one of the three on its own. */
    CHECK (isSequenceChild (child ("Trigger", "t")));

    /*  Last, the sentence the rest of this file reads members back with, put
        against the rule once so that the duplicate is checked rather than
        merely believed. */
    for (const auto& c : { child ("Cue", "a"),       child ("Group", "g"),
                           child ("Trigger", "t"),   child ("Header", "h"),
                           child ("Footer", "f"),    child ("Persistent", "p"),
                           unnamed ("Lists"),        unnamed ("Cue") })
    {
        INFO ("child " << spellOne (c));
        CHECK (isSequenceChild (c) == memberByHand (c));
    }
}

//==============================================================================
TEST_CASE ("sequence: a container with no members appends, whatever it is asked")
{
    const auto empty = container ("Group", {});

    /*  A group that holds its two sections and not one cue: created, given a
        header and a footer, and not yet filled. Every position there is asks
        the same question of it - where does the first member go - and there is
        only one answer, which is after everything that is already there. */
    const auto sections = container ("Group", { child ("Header", "h"), child ("Footer", "f") });

    /*  `endOfSequence` IS IN THE LIST BECAUSE IT IS INT_MAX. Every create that
        appends rather than places passes it, so it goes through this arithmetic
        each time a route, a range, a trigger or a header is made, and it has to
        be a position like any other. An implementation that added the sections it
        had skipped to the position it was handed would overflow a signed int on
        this line - undefined behaviour, and on the compilers this project uses
        a large negative number, which lands a new child at index nought, in
        front of the header. */
    for (const int position : { -99, -1, 0, 1, 2, 99, endOfSequence })
    {
        INFO ("position " << position);

        CHECK (rawIndexForPosition (empty, position) == 0);
        CHECK (rawIndexForPosition (sections, position) == 2);
    }

    /*  And what the arithmetic is for, on the shape with nothing to count: the
        first member of a sectioned group lands after both sections. Where it
        sits relative to the <Footer> is the choice Sequence.cpp argues for -
        past the last member is past the last CHILD - and the case below about a
        footer says why it is pinned rather than left to the insertion. */
    CHECK (spell (afterInserting (empty, 0)) == "Cue#new");
    CHECK (spell (afterInserting (sections, 0)) == "Header#h Footer#f Cue#new");

    /*  A parent that is not there at all, for the reason the invalid child
        above is here: a door that reached a refusal through this rule must get
        an answer and not a crash. */
    CHECK (rawIndexForPosition (juce::ValueTree {}, 0) == 0);
    CHECK (rawIndexForPosition (juce::ValueTree {}, endOfSequence) == 0);
}

//==============================================================================
TEST_CASE ("sequence: a header in front shifts every answer, and nothing may be put above it")
{
    /*  The measured shape (Sequence.h): a group whose header was created before
        its cues, so the three members sit at child positions 1, 2 and 3 and
        every index there is gets a different answer from the two counts. */
    const auto staged = container ("Group", { child ("Header", "h"), child ("Cue", "a"),
                                              child ("Cue", "b"),    child ("Cue", "c") });

    REQUIRE (joined (memberIds (staged)) == "a b c");

    CHECK (rawIndexForPosition (staged, 0) == 1);
    CHECK (rawIndexForPosition (staged, 1) == 2);
    CHECK (rawIndexForPosition (staged, 2) == 3);

    // The last position, one past the end, and far past it: the child count.
    CHECK (rawIndexForPosition (staged, 3) == 4);
    CHECK (rawIndexForPosition (staged, 4) == 4);
    CHECK (rawIndexForPosition (staged, 99) == 4);
    CHECK (rawIndexForPosition (staged, endOfSequence) == 4);

    /*  A NEGATIVE POSITION ANSWERS LIKE NOUGHT rather than refusing, and that
        is a decision Sequence.h states: the doors refuse one before they get
        here, and a rule that answers a question should not also be a second
        place that judges it. DocumentTests checks the refusal at the command;
        what this pins is that the rule itself stays an answer - and that it
        does not answer with a negative index, which juce::Array would read as
        an append and quietly put the cue at the far end. */
    CHECK (rawIndexForPosition (staged, -1) == 1);
    CHECK (rawIndexForPosition (staged, -99) == 1);

    /*  AND THE HALF A MEMBER SEQUENCE CANNOT SEE. Answer 0 to position 0 and
        the published order is exactly what the client asked for, because
        `orderOf` skips the header whichever side of it the new cue is on - and
        the file has quietly acquired a group whose header runs second. Only the
        raw spelling says so, which is why this case claims raw indices where
        the property cases below claim member positions. */
    CHECK (spell (afterInserting (staged, 0)) == "Header#h Cue#new Cue#a Cue#b Cue#c");
    CHECK (spell (afterInserting (staged, 2)) == "Header#h Cue#a Cue#b Cue#new Cue#c");
    CHECK (spell (afterInserting (staged, 99)) == "Header#h Cue#a Cue#b Cue#c Cue#new");
}

//==============================================================================
TEST_CASE ("sequence: a section at the back, and the choice about where the end is")
{
    /*  The other file a real show carries, and probably the commoner of the
        two: a group that was filled and THEN given its header and its footer,
        both of which `createRole` appends. The members are at child positions 0
        and 1, so every answer below agrees with a raw index until the end. */
    const auto staged = container ("Group", { child ("Cue", "a"),      child ("Cue", "b"),
                                              child ("Header", "h"),   child ("Footer", "f") });

    REQUIRE (joined (memberIds (staged)) == "a b");

    CHECK (rawIndexForPosition (staged, 0) == 0);
    CHECK (rawIndexForPosition (staged, 1) == 1);

    /*  PAST THE LAST MEMBER IS PAST THE LAST CHILD, and that is a decision
        rather than an accident, which is why it is pinned here and only here.
        The two candidates - 2, just after the last member, and 4, after
        everything - publish the same `order`, so nothing a client can read
        tells them apart and DocumentTests deliberately declines to assert it
        through the command, where it would be pinning a detail of the
        insertion. The rule's own file is where the choice lives: Sequence.cpp
        argues for the simple one because a footer is a footer by its element
        name and not by where it sits, and a group that was given its footer
        early has carried it last since long before any of this existed. Change
        that paragraph and this line, together, or not at all. */
    CHECK (rawIndexForPosition (staged, 2) == 4);
    CHECK (rawIndexForPosition (staged, 99) == 4);
    CHECK (rawIndexForPosition (staged, endOfSequence) == 4);

    CHECK (spell (afterInserting (staged, 2)) == "Cue#a Cue#b Header#h Footer#f Cue#new");

    // And a member placed among the members leaves both sections where they are.
    CHECK (spell (afterInserting (staged, 0)) == "Cue#new Cue#a Cue#b Header#h Footer#f");
    CHECK (spell (afterInserting (staged, 1)) == "Cue#a Cue#new Cue#b Header#h Footer#f");
}

//==============================================================================
TEST_CASE ("sequence: a section in the middle, several in a row, and a child with no name")
{
    /*  A HEADER IN THE MIDDLE IS A FILE THAT EXISTS. It is what a group holding
        one cue becomes when it is given a header and a footer - both appended -
        and then a second cue, which appends past them. It is also the shape the
        measured defect PRODUCED, so the rule has to be right about files the
        bug it fixes left behind. */
    const auto middle = container ("Group", { child ("Cue", "a"),    child ("Header", "h"),
                                              child ("Footer", "f"), child ("Cue", "b") });

    REQUIRE (joined (memberIds (middle)) == "a b");

    CHECK (rawIndexForPosition (middle, 0) == 0);
    CHECK (rawIndexForPosition (middle, 1) == 3);      // two sections skipped in one step
    CHECK (rawIndexForPosition (middle, 2) == 4);

    CHECK (spell (afterInserting (middle, 1)) == "Cue#a Header#h Footer#f Cue#new Cue#b");

    /*  TWO IN A ROW IN FRONT: a group given both sections before any cue. The
        run is what a rule that skipped ONE non-member and then stopped looking
        would get wrong, and one section in front is not enough to catch that -
        the loop that walks every child is the difference. */
    const auto both = container ("Group", { child ("Header", "h"), child ("Footer", "f"),
                                            child ("Cue", "a"),    child ("Cue", "b") });

    CHECK (rawIndexForPosition (both, 0) == 2);
    CHECK (rawIndexForPosition (both, 1) == 3);
    CHECK (rawIndexForPosition (both, 2) == 4);
    CHECK (spell (afterInserting (both, 0)) == "Header#h Footer#f Cue#new Cue#a Cue#b");

    /*  A LIST'S SECTION IS ITS <PERSISTENT> ONE (§3.29), which no group ever
        holds and which `createPersistent` appends like the others. Its own
        shape, because a rule that knew about headers and footers and not about
        this one answers every case above correctly. */
    const auto list = container ("List", { child ("Persistent", "p"), child ("Cue", "a"),
                                           child ("Cue", "b") });

    CHECK (rawIndexForPosition (list, 0) == 1);
    CHECK (rawIndexForPosition (list, 1) == 2);
    CHECK (rawIndexForPosition (list, 2) == 3);
    CHECK (spell (afterInserting (list, 0)) == "Persistent#p Cue#new Cue#a Cue#b");

    /*  AND THE OTHER HALF OF THE PREDICATE, PUT IN FRONT OF THE ARITHMETIC. No
        document has this shape today - the unidentified children are containers
        and none of them sits beside a cue - so the only place the arithmetic
        meets a child skipped for having no identifier is here. It is the half
        that would be silently untested otherwise, and the half a future element
        will arrive through. */
    const auto nameless = container ("Group", { unnamed ("Lists"), child ("Cue", "a"),
                                                child ("Cue", "b") });

    CHECK (rawIndexForPosition (nameless, 0) == 1);
    CHECK (rawIndexForPosition (nameless, 1) == 2);
    CHECK (rawIndexForPosition (nameless, 2) == 3);
    CHECK (spell (afterInserting (nameless, 0)) == "Lists#- Cue#new Cue#a Cue#b");
}

//==============================================================================
TEST_CASE ("sequence: inserting at the answer is inserting into the member list, at every position")
{
    /*  THE WHOLE CONTRACT IN ONE LOOP, and the case that would catch a real
        regression rather than a changed number. Every shape above and one or
        two more, asked at every position there is, with the answer checked
        against the same insertion done to a plain list of the identifiers
        `order` publishes. The expected value is computed rather than written
        down, so a shape can be added to the table without anybody working out a
        column of indices by hand and getting one wrong.

        THREE CLAIMS, because the member sequence alone is not the whole rule
        and the measured defect is the proof: a cue moved above a header
        published a perfectly correct order. So each pass also asks that the
        children which are NOT members are where they were, and that a container
        which opened with a run of sections still does. */
    const std::vector<std::pair<std::string, juce::ValueTree>> shapes {
        { "an empty group",
          container ("Group", {}) },
        { "a group that is nothing but its sections",
          container ("Group", { child ("Header", "h"), child ("Footer", "f") }) },
        { "three cues and no section at all",
          container ("Group", { child ("Cue", "a"), child ("Cue", "b"), child ("Cue", "c") }) },
        { "one member and nothing else",
          container ("Group", { child ("Cue", "a") }) },
        { "a header in front",
          container ("Group", { child ("Header", "h"), child ("Cue", "a"),
                                child ("Cue", "b"), child ("Cue", "c") }) },
        { "both sections in front",
          container ("Group", { child ("Header", "h"), child ("Footer", "f"),
                                child ("Cue", "a"), child ("Cue", "b") }) },
        { "both sections in the middle",
          container ("Group", { child ("Cue", "a"), child ("Header", "h"),
                                child ("Footer", "f"), child ("Cue", "b") }) },
        { "both sections at the back",
          container ("Group", { child ("Cue", "a"), child ("Cue", "b"),
                                child ("Header", "h"), child ("Footer", "f") }) },
        { "a section between every pair of cues",
          container ("Group", { child ("Header", "h"), child ("Cue", "a"),
                                child ("Footer", "f"), child ("Cue", "b") }) },
        { "a list with its persistent section",
          container ("List", { child ("Persistent", "p"), child ("Cue", "a"),
                               child ("Cue", "b") }) },
        { "a child with no identifier among them",
          container ("Group", { unnamed ("Lists"), child ("Cue", "a"), child ("Cue", "b") }) },
    };

    for (const auto& shape : shapes)
    {
        const auto& description = shape.first;
        const auto& parent = shape.second;

        const auto members = memberIds (parent);
        const auto childrenBefore = spell (parent);
        const auto sectionsBefore = spellSections (parent);
        const auto lead = leadingSections (parent);

        /*  Every position a member of this container could be given, the two
            past the end, the word the appending creates use, and the two before
            the beginning that only a broken client sends. */
        std::vector<int> positions { -99, -1, endOfSequence };

        for (int p = 0; p <= static_cast<int> (members.size()) + 2; ++p)
            positions.push_back (p);

        for (const int position : positions)
        {
            INFO (description << ", position " << position);

            /*  THE SAME INSERTION DONE TO A PLAIN LIST. A position past the end
                appends and a negative one is read as nought, which is what the
                clamp says; between those two it is the position itself, and
                nothing here knows or cares how many children the container
                has. */
            auto expected = members;
            expected.insert (expected.begin()
                               + std::clamp (position, 0, static_cast<int> (members.size())),
                             newcomer.toStdString());

            const auto raw = rawIndexForPosition (parent, position);

            // A legal insertion point, first of all: juce::Array reads anything
            // outside this range as "put it at the end", which would turn a
            // wrong answer into a plausible-looking one.
            CHECK (raw >= 0);
            CHECK (raw <= parent.getNumChildren());

            auto after = parent.createCopy();
            after.addChild (child ("Cue", newcomer), raw, nullptr);

            CHECK (joined (memberIds (after)) == joined (expected));

            /*  AND THE TWO CLAIMS THE MEMBER SEQUENCE CANNOT MAKE. The sections
                are the same sections in the same order - a rule that displaced
                one would publish the right cue list out of a file whose header
                had moved - and a run of them at the front is still at the
                front, which is the one thing position nought must never be
                allowed to break into. */
            CHECK (spellSections (after) == sectionsBefore);
            CHECK (leadingSections (after) == lead);

            /*  The rule was ASKED, not told. juce::ValueTree is a handle onto
                shared data, so a const reference is not a promise that nothing
                was written: a rule that tidied the parent on its way past would
                compile perfectly and edit the operator's show from inside a
                question. */
            CHECK (spell (parent) == childrenBefore);
        }
    }
}
