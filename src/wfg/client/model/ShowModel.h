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
#include <unordered_set>
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

    /*  A ROW IS A CUE OR IT IS A SECTION'S HEAD, and the list holds both in one
        flat sequence - the page's own answer, and for the page's own reason:
        there is no element around a section to put a border on, so the frame is
        drawn by the rows themselves and a head is just another row.

        A HEAD IS WHAT FOLDS (author, 2026-09-18: "I think we need a special
        container for the persistent cues that can be folded or expanded"). It
        carries the word, the count and the twist; §4.8 wants all three, because
        a shape, a word and a number are three tellings and none of them is a
        colour. */
    enum class RowKind { cue, band };

    struct Row
    {
        RowKind rowKind = RowKind::cue;

        /*  A BAND'S OWN KEY, stable across rebuilds so a fold survives an edit:
            the container's identifier and the section's word. A cue row leaves
            it empty. */
        std::string bandKey;
        std::size_t count = 0;      ///< for a band: how many rows it heads
        bool shut = false;          ///< for a band: whether those rows are hidden

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

        /*  Where this row stands among its parent's MEMBERS, which is the
            index `cue.create` and `object.move` speak in - not its position
            in the drawn list, which counts bands and nested rows too. A drop
            that means "after this one" needs the first and would land
            anywhere with the second. */
        int indexInParent = 0;

        /*  WHETHER THE POINTER MAY STAND HERE, which is decision X's rule and
            not a guess: *any cue of this list that is not in a header, a
            footer or a persistent section*. A header and a footer run with
            their group and a persistent cue runs from the moment the show
            starts; none of the three is a place an operator waits, and the
            engine refuses `standby.set` on them with `not-a-stop`.

            IT IS HERE SO THAT A CLIENT NEED NOT FIND OUT BY BEING REFUSED. The
            window drew those rows - correctly, they are part of the show - and
            offered a click on them, so the first thing the author did with the
            cue list was press two rows that could never take the pointer and
            get a log record where an answer should have been. */
        bool mayPark() const noexcept
        {
            return rowKind == RowKind::cue && section == Section::member;
        }
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

        /*  OPENS OR SHUTS A SECTION, by the key its head carries. The fold is
            the CLIENT's and never the engine's (§14.1): what somebody has
            collapsed on their screen is not something the show decided, and
            writing it to the document would put one operator's screen into
            everybody else's. It outlives a rebuild - an edit does not reopen
            what you folded - which is why the set is keyed on the container
            rather than on a row's position. */
        void toggle (const std::string& bandKey);

        /** Whether that section is currently shut. */
        bool isShut (const std::string& bandKey) const;

    private:
        void walk (const tree::TreeSnapshot& snapshot, const std::string& container,
                   bool isList, int depth);
        void section (const tree::TreeSnapshot& snapshot, const std::string& container,
                      const std::vector<std::string>& ids, Section which,
                      const char* word, int depth);
        void append (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                     Section section, int depth, const std::string& parent,
                     int indexInParent = 0);

        std::vector<Row> drawn;
        std::unordered_map<std::string, int> indexOfCue;
        std::unordered_set<std::string> folded;
        std::string listId;
        std::uint64_t revision = 0;
        std::size_t walks = 0;
        bool built = false;
        bool foldsMoved = false;
    };
}
