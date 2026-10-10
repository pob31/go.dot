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
    One node of the parameter tree, as a client sees it.

    A PROJECTION, NOT A SECOND COPY. Every value here comes from somewhere that
    already owns it - an attribute in show.xml, a counter in the engine, a
    command in the registry - and the tree holds it only for as long as one
    snapshot lives. Nothing writes back through a Node; a write is the
    `node.set` command, which goes to the document's own write path like every
    other mutation (PRD §4.11).

    PLAIN DATA ON PURPOSE. No juce::var, no ValueTree handle, no pointer into
    the document. Snapshots are read from server threads while the tick thread
    is already building the next one, so a Node that referred to anything the
    tick thread owns would be a data race dressed as a struct.

    THE FOUR THINGS PRD §3.3 SAYS A NODE DECLARES are `kind`, `rateCap`,
    `anticipatable` and `panic`. They travel to a client in one vendor key,
    `GODOT`, because the OSCQuery proposal makes custom attributes
    "intentionally trivial" and a client that does not know the key ignores it.
    Every one of them comes from a column of the parameter table.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree
{
    /*  PRD §3.3's distinction, and it is not cosmetic: settable state has a
        value at any given time and can therefore be solved for, saved and
        recalled, while an event is one-shot and has none. A container holds
        other nodes and nothing else. */
    enum class Kind { container, state, event };

    /*  OSCQuery's ACCESS numbers, spelled as the wire spells them so that
        nothing has to translate at the boundary. */
    enum class Access
    {
        none      = 0,      ///< a container
        read      = 1,
        write     = 2,      ///< a command: invoke it, never read it
        readWrite = 3
    };

    struct Node
    {
        /** The full OSC address. Unique, and the tree's only key. */
        std::string address;

        Kind kind = Kind::container;
        Access access = Access::none;

        /*  OSC type tags, one per argument: "s", "d", "T", or a command's whole
            signature like "sis". Empty for a container. */
        std::string typeTags;

        /** One sentence, from the parameter table. What a client shows at 2 a.m. */
        std::string description;
        /*  WHAT THE NODE IS FOR, in the vocabulary every preset shares
            (namespace draft §57, AFM): `scene.recall`, `strip.level`,
            `object.position`... from the description's `GODOT.ROLE`; empty for
            a node that claims none, which is most of them. Published back as
            `ROLE`, so a client names it and a later round retargets by it. */
        std::string role;

        /*  HOW THE NODE IS SPELLED ON THE RCP WIRE (namespace draft §57, AFH;
            DP.7), from `GODOT.RCP`: the verb - `set`, or a scene recall's
            own - and how many of the address's trailing segments are the
            console's X and Y; -1 where the file said nothing and the wire
            infers them from the segments that are whole numbers. */
        std::string rcpVerb;
        int rcpIndexes = -1;

        //======================================================================
        // The declared range, when the table gives one.
        bool hasMinimum = false;
        double minimum = 0.0;
        bool hasMaximum = false;
        double maximum = 0.0;

        /** A closed set of legal values, when the table declares one. */
        std::vector<std::string> enumValues;

        /*  THE RANGE OF EACH ARGUMENT AFTER THE FIRST, for a node that takes
            several - `/adm/obj/1/xyz` has three, each with its own bounds
            (namespace draft §45). The first stays in `hasMinimum` and its
            neighbours above, which every node of Go.dot's own is read by;
            this is empty for a node of one argument, and holds one entry per
            later type tag where a description gave them. `rangeOf` reads
            either. */
        struct ArgumentRange
        {
            bool hasMinimum = false;
            double minimum = 0.0;
            bool hasMaximum = false;
            double maximum = 0.0;
        };

        std::vector<ArgumentRange> laterRanges;

        /** The bounds of one argument, from nought: unbounded where none were given. */
        ArgumentRange rangeOf (std::size_t argument) const
        {
            if (argument == 0)
                return { hasMinimum, minimum, hasMaximum, maximum };

            if (argument - 1 < laterRanges.size())
                return laterRanges[argument - 1];

            return {};
        }

        /** `s`, `Hz`, `dB`, `samples`, … Empty when the value is not a quantity. */
        std::string unit;

        //======================================================================
        // The GODOT key: PRD §3.3's four declarations.
        double rateCap = 50.0;
        bool anticipatable = false;

        /*  WHAT THE NODE RESTS AT (PRD §4.6), as one of three words: "park"
            (it stays where it is), "snap", or "value" - it rests at the value
            in `panicValues`. A policy is a word and a value is a value, so the
            two are kept apart rather than one string that might be either: a
            string node resting at the text "park" would otherwise read as the
            policy. Nothing APPLIES any of them yet (devplan Phase 10); they are
            declared, checked where they are read, and published. */
        std::string panic = "park";

        /*  THE DECLARED SAFE VALUE, when `panic` is "value", and empty
            otherwise: one element per type tag, as `values` is, so it is
            written out exactly as VALUE is - an array in OSCQuery's PANIC
            (namespace draft §3). */
        std::vector<osc::Value> panicValues;

        //======================================================================
        /*  The value, when the node has one. EMPTY for a container, and empty
            for an event - which is the whole point of that distinction: asking
            an event for its value is a question with no answer, and a nil or a
            zero would be an answer.

            A VECTOR RATHER THAN AN OPTIONAL, and the reason is the protocol
            rather than Go.dot. OSCQuery's VALUE is an array and always has
            been - the JSON here has emitted `"VALUE": [x]` since Phase 1 - and
            OSC carries as many arguments as the type tag string declares. A
            node holding one value is the common case, not the only one:
            `Route/@gains` is C_in x width numbers under a single address, and
            modelling that as a string would have put a parser in every client.

            So `typeTags` and this stay the same length, and a node with one
            value has one of each. */
        std::vector<osc::Value> values;

        bool isContainer() const noexcept { return kind == Kind::container; }

        /*  The value, for a node that holds exactly one - which is almost all
            of them. Empty for a container, for an event, and for a LIST, and
            that last one is deliberate: a caller that meant "the value" and
            meets four gains has asked a question with no single answer, and
            handing back the first would be a wrong answer rather than none. */
        std::optional<osc::Value> soleValue() const
        {
            if (values.size() != 1)
                return std::nullopt;

            return values.front();
        }
    };
}
