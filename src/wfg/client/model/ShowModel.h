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
    The cue list as rows, in the order they are drawn.

    WHY THIS IS A CACHE AND NOT A READ PER PAINT. `TreeSnapshot::childrenOf()`
    calls `all()`, which allocates a vector of the whole tree; asking it once
    per row is quadratic, and on a five-hundred-cue show at twenty-five passes
    a second it is the client's whole budget. So the rows are built once by a
    linear walk and kept, and the walk reads by address - `find()` is a binary
    search over three sorted vectors - exactly as the page does.

    WHAT IT IS KEYED ON, and this is the part that needed an engine change.
    The obvious key is the snapshot's document half, and it is the wrong one:
    `markStale()` fires on ANY applied command (`Console.cpp:2987`), GO
    included, so a chain of runs starting and ending mints a fresh document
    half several times a second and a model keyed on it would rebuild five
    hundred rows at run rate for a show nobody edited. `/godot/document/revision`
    (M0) counts what `show.xml` would record and nothing else: an edit, an
    undo, a revert, a load move it; a GO, a standby move, a focus change and
    every run leave it alone. The model rebuilds when that number moves, and
    when the focused list changes - which the number cannot say, because
    changing focus is not a change to the show.

    WHAT IS NOT IN A ROW: the standby, whether a cue is running, its position,
    the aim. Those move at tick rate and belong to the overlay the view reads
    per pass, not to the structure that moves when somebody edits. Keeping the
    two apart is what lets `updateContent()` run at show-change rate while
    `repaintRow()` runs at tick rate.

    std only, like the rest of model/: no JUCE type is named here, so the rows
    can be asserted with no window in the room.
*/

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  WHERE A ROW SITS IN ITS CONTAINER, which is not the same question as
        what kind of cue it is. The page draws these as frames with a rule and
        a word; §14.3 argues why they are sections rather than cues in a list:
        a header runs before its group and is not a member of it, and a client
        reading `order` is reading the cue list. */
    enum class Section { persistent, header, member, footer };

    struct Row
    {
        std::string id;
        std::string name;
        std::string kind;        ///< memo, media, fade, stop, group, osc, midi, …
        std::string number;      ///< the operator's own numbering, a string on purpose

        /*  Already text, and already the page's rule: a read-only nought is
            blank, so one column does not spell nothing in two ways - an empty
            box where a fade waits for none, and "0.0" where a media cue's file
            could not be read and its length is unknown. */
        std::string preWait;
        std::string duration;    ///< only some kinds have one; empty otherwise
        std::string postWait;

        std::string mode;        ///< a group's sequence: manual, automatic, timeline; empty otherwise
        std::string preset;      ///< the ancestor group this cue is a preset of, when it is one

        int depth = 0;           ///< 0 at the top of the list; a group's members are one deeper
        bool isGroup = false;
        bool enabled = true;
        Section section = Section::member;

        /** The group this row sits in, or empty at the top of the list. */
        std::string parent;
    };

    class ShowModel
    {
    public:
        /*  Rebuilds only when the show or the focused list has moved. True when
            it rebuilt, which is what the counting test reads - and what a view
            uses to decide between `updateContent()` and doing nothing at all. */
        bool refresh (const tree::TreeSnapshot& snapshot, std::string_view listId);

        const std::vector<Row>& rows() const noexcept { return drawn; }

        /** The row's position, or -1. */
        int indexOf (std::string_view cueId) const;

        /** `/godot/document/revision` as it stood when these rows were built; 0 if never. */
        std::uint64_t builtAt() const noexcept { return revision; }

        /** The list these rows belong to. */
        const std::string& list() const noexcept { return listId; }

        /** How many times a walk has actually run, for the counting test. */
        std::size_t rebuilds() const noexcept { return walks; }

    private:
        void walk (const tree::TreeSnapshot& snapshot, const std::string& container,
                   bool isList, int depth);
        void append (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                     Section section, int depth, const std::string& parent);

        std::vector<Row> drawn;
        std::unordered_map<std::string, int> indexOfCue;
        std::string listId;
        std::uint64_t revision = 0;
        std::size_t walks = 0;
        bool built = false;
    };
}
