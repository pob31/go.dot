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
    One cue's parameters, in the order somebody works in.

    BUILT FROM THE TREE AND NEVER FROM A COPY OF THE PARAMETER TABLE. Every
    field here comes from the node itself - its type tags, its access, its
    range, its description - which is what §14.2 means by a generic inspector
    and what makes a row added to the CSV appear in this window with no line
    written anywhere. The page proved the shape; this is the same shape in C++.

    THE ORDER IS THE PAGE'S, TRANSCRIBED AS DATA, because the author settled it
    there with the page open (2026-09-16) and a window that re-argued it would
    be two clients disagreeing about where `preWait` goes. The tree is
    alphabetical inside each owner, which puts `postWait` above `preWait` and
    `duration` under another owner word entirely - the one ordering nobody
    works in. So four blocks:

        what it is      number, name, shortName, colour, notes
        when            preWait, duration, postWait
        what it does    the kind's own rows, in that kind's working order
        in the list     enabled, preset

    A row nobody has named falls to the end of its block, alphabetically, so a
    new row in the parameter table turns up somewhere predictable rather than
    vanishing.

    WHAT IS A DECISION AND WHAT IS THE MACHINE ANSWERING BACK is decided by the
    node and never by a list of names: a WRITABLE row is something somebody
    chose, and a read-only one is the engine reporting - `kind`, `parent`,
    `index`, `prepare`, a hash. The second kind goes behind a fold (author,
    2026-09-16: "we could hide the internal stuff like the various UIDs, hash
    and other things that are not really necessary for the user"). A row that
    becomes writable, or arrives writable, moves by itself.

    std only, like the rest of model/.
*/

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  HOW A FIELD IS BEST ASKED FOR, which is not always a box.

        `loops` is the case that earned this. It is one integer carrying three
        different questions - does this repeat at all, for ever or a set number
        of times, and how many - and a box showing `0` answers none of them:
        the author set the field beside it twice, ran the show, and watched an
        ambience bed loop for ever both times (2026-09-18). Nothing was broken;
        the panel simply never said what it was asking.

        So a control may be composed. The VALUE is still one integer and the
        commit is still one `node.set`: what changes is how somebody is asked. */
    /*  WHICH CONTROL ASKS A FIELD BEST. Decided from the node wherever the
        node can say so - a `T` row is a switch, a closed set of values is a
        choice - and NAMED only where no property of the row could have said
        it: `loops`, where one integer carries three questions, and `file`,
        where the value is a name on a disk that a machine can be asked to
        find. Both stay ordinary text underneath and send the same `node.set`,
        so the generic route is never the only casualty of the specific one. */
    /*  `cueRef` is a field that names another cue - a fade's or a stop's
        target - and takes the cue's number or name as well as its identifier
        (author, 2026-09-18); the window resolves what was typed to the
        identifier before it is written (model/Reorder.h). */
    /*  `longText` is a field written at length - a cue's notes, what to look
        out for before GO - and gets a box of several lines rather than one
        (author, 2026-09-18: "the edit field in the Inspector is too limited
        for this"). */
    enum class Control
    {
        text, toggle, choice, loopCount, file, cueRef, longText,

        /*  A MENU OF THE SHOW'S OUTPUTS, which is a choice whose options the
            parameter table cannot declare: a bus is an object somebody made,
            so the legal values are a fact about THIS show and change as the
            rig is written. `choices` carries them, id and label apart, because
            the row stores an identifier and a designer reads a name - and a
            menu that wrote the name would break the moment somebody renamed
            an output. */
        busRef,

        /*  A MENU OF THE SHOW'S DEVICES, and the one control here that does
            not write the row it is drawn on.

            A network cue carries the whole address it writes, prefix and all,
            so which box it is aimed at is not a field: it is the front of that
            address. Rather than adding a second place for the answer to live -
            which could then disagree with the address - this line is DERIVED
            from the address and commits back to it. Picking a device rewrites
            the cue's `address`, which is an ordinary `node.set` and needs no
            new command, no new attribute and no new refusal.

            Its `choices` therefore hold whole addresses as keys, not
            identifiers. See `model::targetChoices`. */
        deviceRef,

        /*  A MENU OF THE SHOW'S MIDI PORTS. The same kind of thing as an
            output's menu and unlike the device one above: it writes an
            IDENTIFIER, because a cue names the port the show declares and a
            port's name is what a person reads and may rename. The row already
            held that identifier; what was missing was anything to pick it
            from, so it was a box you typed eight characters into. */
        portRef,

        /*  A MENU OF THE SHOW'S DCAS (PRD §3.28), and the port menu's twin: it
            writes an IDENTIFIER - the mark a media cue, a group or a fade
            carries - and a person reads the DCA's name, which may change while
            every cue marked with it stays marked. The mark is on the member
            and never a list on the DCA (§4.12), so putting eight cues on one
            DCA is eight writes of the row this menu is drawn on: the `node.set`
            a selection already sends, and nothing new. */
        dcaRef,

        /*  A MENU OF THE SHOW'S SAMPLER STRIPS, faders and pads (author,
            2026-09-25), the DCA menu's twin: it writes a strip's IDENTIFIER
            to a sampler member's `strip` row, and each item says what is on
            that strip - another member of the group, what an earlier group in
            the list put there, or free. "automatic" is the empty id. */
        stripRef,

        /*  A BUTTON THAT OPENS THE PANEL AT THE FOOT on this cue (author,
            2026-09-21: "it would be great if the controls to show the
            waveform, the send levels, the EQ, the group timeline were in the
            inspector. No hunting in the menus").

            It is not an attribute and writes nothing: `value` carries the
            SUBJECT the button opens, in the words `model::Subject` uses, and
            the window turns that into a `setFoot`. It sits among the fields
            because that is where somebody is already looking when they want
            one - the inspector is the place a cue is worked on, and a panel
            about this cue is a thing to ask for from there rather than from a
            menu three levels up. */
        opener
    };

    struct Field
    {
        std::string address;        ///< the full address `node.set` would be given
        std::string name;           ///< the attribute, as the tree spells it

        /*  What to call it on screen, when its own name is not what somebody
            reading it would call it (author: "Play could read 'Items to play'
            to make it more obvious what it does"). Falls back to `name`, so a
            row nobody has renamed still appears with the name the tree gives
            it rather than vanishing. */
        std::string label;

        Control control = Control::text;
        std::string value;          ///< as text, through the same formatter every cell uses
        std::string description;    ///< one sentence from the parameter table
        std::string unit;           ///< Hz, dB, samples, s - empty when it is not a quantity
        std::string typeTags;

        bool writable = false;
        bool boolean = false;       ///< a `T` row: a switch rather than a box

        /** A closed set of legal values, when the row declares one: a choice, not typing. */
        std::vector<std::string> options;

        /*  The same idea for a set the SHOW declares rather than the table:
            what is stored first, what is read second. Used by `busRef`, and by
            whatever else comes to point at an object by identifier. */
        std::vector<std::pair<std::string, std::string>> choices;

        /*  WHETHER THIS ROW MEANS ANYTHING FOR THIS CUE, as opposed to being
            legal for its kind. A fold is a statement about a two-channel file
            and says nothing about a mono one, so it is drawn greyed rather
            than hidden: hiding it would leave a designer hunting for a control
            they have seen before, and greying says "not this cue" where an
            absence says "not this program". */
        bool applies = true;

        bool hasMinimum = false, hasMaximum = false;
        double minimum = 0.0, maximum = 0.0;

        /*  SEVERAL CUES AT ONCE (author, 2026-09-18: "multiple selection and
            batch editing of parameters"). `addresses` is every picked cue's
            own address for this row, and a commit writes each one - N
            `node.set`s, which is what the page does too and what §4.11 makes
            a batch edit: N decisions, not one command that means "all of
            them". Empty for one cue, whose `address` is the whole story.
            `mixed` says the cues do not agree, and `value` is then empty. */
        std::vector<std::string> addresses;
        bool mixed = false;
    };

    struct Block
    {
        std::string heading;
        std::vector<Field> fields;
    };

    struct Inspection
    {
        std::string cueId;
        std::string cueName;
        std::string kind;

        /** What somebody decided, in the order they would fill it in. */
        std::vector<Block> blocks;

        /** What the engine says back, behind the fold. */
        std::vector<Field> details;

        /** How many cues this is about: 0, 1, or several. */
        std::size_t count = 0;

        bool empty() const noexcept { return blocks.empty() && details.empty(); }
    };

    /*  WHICH PANELS THIS KIND OF CUE HAS, in the order they are offered.

        ONLY WHAT IS BUILT IS LISTED. The author named four - the waveform, the
        send levels, the EQ and the group timeline - and three of them have no
        editor yet; a button that opened nothing would be worse than the menu
        it replaces, because a dead control teaches somebody the feature is
        broken rather than absent. Each is added here on the day its editor
        lands, and that one line is the whole of the change. */
    std::vector<Field> openersFor (const std::string& kind, const std::string& cueId);

    /** Everything published under one cue, sorted into blocks. Empty for no cue. */
    Inspection inspect (const tree::TreeSnapshot& snapshot, const std::string& cueId);

    /*  WHETHER A CLICK ON THIS FIELD PUTS IT ON THE MASTER DIAL (author,
        2026-09-26): a number somebody decides and may write, one value, not a
        switch or a menu - the rows `surface.dial` takes. A field over several
        cues gives the dial its first cue's row. */
    bool mayDial (const Field& field);

    /*  WHAT THE MASTER DIAL IS ON, in words: "Kick: level -6 dB", "Kick to
        Reverb: level -12 dB" for a send. Empty while it is free. The row's
        name is the one the inspector gives it. */
    std::string dialLine (const tree::TreeSnapshot& snapshot);

    /*  WHAT SEVERAL CUES HAVE IN COMMON: the writable rows every one of them
        has, by name, with the value they agree on or `mixed`; the reported
        rows are left out, since a run position is one cue's. One cue is
        `inspect`; none is empty. The blocks are ordered by the first cue's
        kind, and the heading says how many and which kinds. */
    Inspection inspectMany (const tree::TreeSnapshot& snapshot, const std::vector<std::string>& cueIds);
}
