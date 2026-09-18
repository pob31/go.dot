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

        what it is      number, name, colour, notes
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

#include <string>
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
    enum class Control { text, toggle, choice, loopCount, file };

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

        bool hasMinimum = false, hasMaximum = false;
        double minimum = 0.0, maximum = 0.0;
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

        bool empty() const noexcept { return blocks.empty() && details.empty(); }
    };

    /** Everything published under one cue, sorted into blocks. Empty for no cue. */
    Inspection inspect (const tree::TreeSnapshot& snapshot, const std::string& cueId);
}
