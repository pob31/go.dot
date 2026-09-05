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

        //======================================================================
        // The declared range, when the table gives one.
        bool hasMinimum = false;
        double minimum = 0.0;
        bool hasMaximum = false;
        double maximum = 0.0;

        /** A closed set of legal values, when the table declares one. */
        std::vector<std::string> enumValues;

        /** `s`, `Hz`, `dB`, `samples`, … Empty when the value is not a quantity. */
        std::string unit;

        //======================================================================
        // The GODOT key: PRD §3.3's four declarations.
        double rateCap = 50.0;
        bool anticipatable = false;
        std::string panic = "park";

        //======================================================================
        /*  The value, when the node has one. Absent for a container, and absent
            for an event - which is the whole point of the distinction: asking
            an event for its value is a question with no answer, and a nil or a
            zero would be an answer. */
        std::optional<osc::Value> value;

        bool isContainer() const noexcept { return kind == Kind::container; }
    };
}
