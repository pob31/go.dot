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
    /*  A STEP ROW IS A READING UNDER A CUE (2026-09-19): while the window is
        loading to time, the steps the list took after the aimed cue fired are
        drawn under its row, spaced as rows, each at its offset into the cue -
        and the aim itself is one of them, the pointer. Neither is a cue: no
        drop lands on one, no pick takes one, and the model never builds one;
        the list inserts them (`CueListComponent::setSteps`). */
    enum class RowKind { cue, band, step };

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

        /*  WHAT THE ROW IS CALLED, and what somebody called it (namespace draft
            §53): `ownName` is the cue's `name`, as typed - what an edit starts
            from and writes back - and `name` is that, or the cue's `autoName`
            while it has none, which is what every label reads. */
        std::string name;
        std::string ownName;
        std::string kind;        ///< memo, media, fade, stop, group, osc, midi, …
        std::string number;      ///< the operator's own numbering, a string on purpose

        /*  Already text, and already the page's rule: a read-only nought is
            blank, so one column does not spell nothing in two ways - an empty
            box where a fade waits for none, and "0.0" where a media cue's file
            could not be read and its length is unknown. */
        std::string preWait;
        std::string duration;    ///< only some kinds have one; empty otherwise
        std::string postWait;

        std::string mode;

        /*  A TRANSPORT CUE'S VERB, as the tree spells it (hard, afterMember,
            record...), so the kind column can say what the cue does the way a
            group's says its mode (2026-09-27). Empty on anything else. */
        std::string verb;
        bool andGo = false;      ///< a jump cue's "and Go" (namespace draft §27)

        /*  A GROUP'S BEHAVIOUR, as the tree spells it: `sequential` or
            `shuffle`, and how many rounds it plays - where nought is for ever
            and one is once. Empty on anything that is not a group. */
        std::string selection;
        std::string loops;        ///< a group's sequence: manual, automatic, timeline; empty otherwise
        std::string preset;      ///< the ancestor group this cue is a preset of, when it is one

        /*  WHAT THE ROW'S MARKS ARE READ FROM (author, 2026-09-30: icons for
            "type of cue or group and their important settings"), each as the
            tree spells it and each empty where the kind has no such row, so
            `model::marksFor` says nothing about a setting a cue cannot have.

            `colour` is the cue's own decoration, `#rrggbb` - somebody's
            decision, drawn as a small tab and never as the only telling.
            `advance` and `play` are a group's: on GO or after the one before,
            and how many members a round plays. `members` counts its `order`.
            `rate` and `rateMode` are a media cue's speed, or the speed a fade
            takes its target to when `rateOn`. `dca` is the NAME of the DCA
            the cue answers to, for a mark that reads as the desk does. */
        std::string colour;
        std::string advance;
        std::string play;
        std::size_t members = 0;
        std::string rate;
        std::string rateMode;
        std::string dca;
        bool lane = false;           ///< a media cue with a level lane drawn over its file

        /*  A DUAL CUE (namespace draft 37.5, WM, the author's): a movie and the
            sound locked to it, drawn as one cue of two lines. `lockedTo` is the
            sound's movie, by identifier; `soundOfAbove` says the row above is
            that movie, so this row is its second line; `soundBelow` says the
            row below is this movie's sound. A sound locked to a movie that is
            there is never a place to park - its movie's GO fires it. */
        std::string lockedTo;
        bool followsMovie = false;
        bool soundOfAbove = false;
        bool soundBelow = false;
        bool rateOn = false;         ///< a fade that moves its target's speed
        bool memberOfSampler = false;  ///< a member (not header or footer) of a sampler group (§39)
        bool stopWhenDone = false;   ///< a fade that stops what it faded, once it arrives

        int depth = 0;           ///< 0 at the top of the list; a group's members are one deeper
        bool isGroup = false;

        /*  A STEP ROW'S OWN FIELDS: seconds into the aimed cue, whether it is
            the aim's pointer rather than a step, and whether the step lies
            past the instant - one a load would undo. `id` names the step's
            cue and `name` its name; `number` carries the origin's word. */
        double offset = 0.0;
        bool pointer = false;
        bool undone = false;

        /*  A HEADER LINE THAT IS A READING, NOT A MEMBER (§13.7): a cue whose
            `preset` names this group, drawn in the header band so the header
            reads as what it will prepare - the page draws these in italics.
            The cue's own row stands where the cue is; this one is not in the
            index and is not walked into. */
        bool derived = false;
        bool enabled = true;

        /*  WHAT AN ENABLE OR DISABLE CUE HAS SWITCHED FOR THIS RUN (namespace draft
            §27): `file`, `on` or `off`, the engine's `cue/override`. `enabled`
            stays what the file says, which the inspector edits; whether the
            row is drawn as running is `runsNow`. */
        std::string overridden = "file";

        bool runsNow() const noexcept
        {
            return overridden == "on" || (overridden != "off" && enabled);
        }

        Section section = Section::member;

        /** The group this row sits in, or empty at the top of the list. */
        std::string parent;

        /*  THE SECTION'S OWN IDENTIFIER for a band and for the rows inside a
            header, a footer or a persistent section: the container a move
            into that section names (`group/header`, `group/footer`,
            `list/persistent`). Empty for a member row, whose container is
            its parent. */
        std::string sectionId;

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
            return rowKind == RowKind::cue && section == Section::member && ! followsMovie;
        }

        /*  WHERE A CLICK IN THIS ROW'S GUTTER SENDS THE POINTER, which since
            2026-09-26 is not always the row: a header's or a footer's line -
            its band too - parks on the GROUP it belongs to (author: "move the
            pointer to the group instead of showing an error"). Empty for a
            persistent bed, which has no group to stand for it, and for a step
            row, which is a reading and not a cue. A sampler member is a member
            row and sends itself: the engine knows it is one and lands it on
            its bank (`cue::nearestStop`). */
        std::string parksOn() const
        {
            if (mayPark())
                return id;

            if (rowKind != RowKind::step
                  && (section == Section::header || section == Section::footer))
                return parent;

            return {};
        }
    };

    /*  WHAT AN EMPTY PERSISTENT BAND SAYS IN PLACE OF ITS COUNT (namespace
        draft §30, S5). The band is drawn for every list since 2026-10-05, with
        nothing under it in most shows, and a heading over nothing reads as a
        stray line unless it says what it is for. Empty for any other row: a
        band with cues under it says how many, as it always has. */
    std::string emptyBandWords (const Row& band);

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

        /*  OPENS OR SHUTS A SECTION, by the key its head carries. The set is
            what is DRAWN, and it outlives a rebuild - an edit does not reopen
            what you folded - which is why it is keyed on the container rather
            than on a row's position. Since 2026-09-18 the fold is also the
            SHOW's, as a state row the engine keeps in state.xml (author:
            "fold state should be recorded in project file"): the window
            writes the flag when a fold is toggled (`foldAddress`), and the
            walk seeds this set from the flags when it rebuilds, so a show
            opens folded the way it was left. State, not show: it does not
            mark the show unsaved. */
        void toggle (const std::string& bandKey);

        /*  The tree address of the flag a band key stands for: a group's own
            `folded`, or a container's `<word>Folded`. */
        std::string foldAddress (const std::string& bandKey) const;

        /** Whether that section is currently shut. */
        bool isShut (const std::string& bandKey) const;

    private:
        void walk (const tree::TreeSnapshot& snapshot, const std::string& container,
                   bool isList, int depth);
        void section (const tree::TreeSnapshot& snapshot, const std::string& container,
                      const std::string& sectionId,
                      const std::vector<std::string>& ids, Section which,
                      const char* word, int depth,
                      const std::vector<std::string>& derivedIds = {},
                      bool evenEmpty = false);
        void append (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                     Section section, int depth, const std::string& parent,
                     int indexInParent = 0, bool derived = false,
                     const std::string& sectionId = {});

        std::vector<Row> drawn;
        std::unordered_map<std::string, int> indexOfCue;
        std::unordered_set<std::string> folded;

        /*  THE FLAG AS THE TREE LAST SAID IT, per key, so a rebuild applies the
            show's record only when that record MOVED - the first build, or
            another client's fold - and never over a toggle made here that the
            engine has not published back yet. Without this a toggle was undone
            by the very rebuild it caused. */
        std::unordered_map<std::string, bool> flagSeen;
        void seedFold (const std::string& bandKey, bool flag);

        std::string listId;
        std::uint64_t revision = 0;
        std::size_t walks = 0;
        bool built = false;
        bool foldsMoved = false;
    };
}
