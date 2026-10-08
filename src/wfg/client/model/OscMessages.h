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
    AN OSC CUE'S MESSAGES, as the foot panel's table shows them (namespace draft
    45, O.5): the cue's own address and values first, then each further message
    in its order, every value of each with its type, and the curve on it if one
    moves it.

    A VALUE IS AN ATOM OF A LIST, `i:3 f:0.5 s:"left"`, written whole: changing
    one value, its type, or how many there are rewrites the message's `value`
    row with one `node.set`, which the document judges as it judges any. The
    functions below answer what that row becomes; they never write.

    std only, and pure: the engine's OSC value type is shared (the boundary
    forbids the document's types, not the wire's), so a list read here and a
    list the engine sends are read by one grammar.
*/

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  One value of a message. `payload` is what the value box shows - the
        number, the words - and is empty for the four types that carry none. */
    struct OscArgument
    {
        char tag = 'f';
        std::string payload;

        /** A number, which a curve may move. */
        bool number = false;

        /** The curve moving it, empty for none. */
        std::string curveId;
    };

    struct OscMessageRow
    {
        /** The message's identifier, empty for the cue's own - its first. */
        std::string id;

        /** Where its rows are: /godot/cue/<cue>/ or /godot/message/<id>/. */
        std::string base;

        std::string address;

        /** The list as written, and whether it reads as one. */
        std::string value;
        bool spells = true;

        std::vector<OscArgument> args;

        std::string addressRow() const { return base + "address"; }
        std::string valueRow() const   { return base + "value"; }

        /** What a curve on one of its values is created under: the cue, or the message. */
        std::string parentId (const std::string& cueId) const { return id.empty() ? cueId : id; }
    };

    struct OscMessagesReading
    {
        std::string cueId;

        /** The cue's own message first, then each further one in its order. */
        std::vector<OscMessageRow> rows;

        bool locked = false;
    };

    /*  The cue's messages, in one pass over the snapshot. Empty rows for a cue
        that is not an OSC cue, or that is not there. */
    OscMessagesReading readOscMessages (const tree::TreeSnapshot&, const std::string& cueId);

    //==============================================================================
    /*  THE TYPES A VALUE MAY BE, in the order the menu offers them, each with
        its word: the OSC tag first, the word after. */
    const std::vector<std::pair<char, std::string>>& argumentTypes();

    /*  What a value box's text makes, as an atom of `tag`: "0.5" as a float is
        `f:0.5`. nullopt when the text cannot be that type - "loud" is not a
        float. The four types with no payload ignore the text. */
    std::optional<std::string> atomFor (char tag, const std::string& typed);

    /*  THE LIST WITH ONE VALUE REPLACED, appended, or taken away, as the
        `value` row's text. nullopt where the list does not read, or the index
        is not one of its values. */
    std::optional<std::string> withArgument (const std::string& list, std::size_t index, const std::string& atom);
    std::optional<std::string> withArgumentAppended (const std::string& list, const std::string& atom);
    std::optional<std::string> withoutArgument (const std::string& list, std::size_t index);

    /*  ONE VALUE MADE ANOTHER TYPE, keeping what it says where the new type can
        say it - "0.5" stays 0.5 as a double, becomes the words "0.5" as text -
        and taking the type's resting value where it cannot: nought, empty
        words, nothing at all. */
    std::optional<std::string> retyped (const std::string& list, std::size_t index, char tag);
}
