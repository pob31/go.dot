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
    What the transport shows, read out of one snapshot.

    The transport is the strip the operator's eye rests on between cues: which
    show, whether the file holds it, where the clock is, which list is focused
    and which cue GO would fire, what undo would take back, whether the audio
    is running, and the last thing the engine refused. It reads the same
    addresses the page's views/strip.js reads so that the two clients cannot
    disagree about what the engine said, and it reads them into plain strings
    so that a test can assert the words with no window in the room and under
    either locale.

    THREE ANSWERS, NOT TWO, wherever the page gives three. A node the engine
    has not published is not the same thing as a node reading false: a show
    with no answer yet about its lock is not an unlocked show, and drawing it
    as one would be a client inventing a reading. So the flags that matter are
    `Flag::yes`, `Flag::no` and `Flag::unsaid`, and every sentence below has a
    word for the third.

    A reading is a value, taken once per timer pass from one snapshot, and the
    window compares it with the last one to decide what to redraw. Nothing
    here keeps a pointer into the snapshot: TreeSnapshot is immutable and
    shared, but a reading that outlived its snapshot would be a lifetime
    question, and a struct of strings has none.
*/

#include <wfg/client/model/Text.h>

#include <cstdint>
#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct TransportReading
    {
        std::string show;             ///< `/godot/document/name`
        Flag dirty = Flag::unsaid;    ///< `/godot/document/dirty` - the dot
        Flag locked = Flag::unsaid;   ///< `/godot/document/locked` - show mode
        Flag recovery = Flag::unsaid; ///< `/godot/document/recovery` - work found beside the show

        std::string tick;             ///< `/godot/engine/tick`, as digits
        std::string clock;            ///< `/godot/engine/clock`: dummy or device
        std::string rate;             ///< "48000 / 256"; empty before the clock is known

        std::string listId;           ///< `/godot/list/focus`, or the first of `/godot/list/order`
        std::string listName;
        std::string standbyId;        ///< `/godot/list/<listId>/standby`; empty when the standby is clear
        std::string standbyName;
        std::string standbyKind;

        Flag canUndo = Flag::unsaid;  ///< `/godot/document/canUndo`
        Flag canRedo = Flag::unsaid;
        std::string undoName;         ///< the command that opened the transaction: `node.set`, `cue.create`
        std::string redoName;

        std::string status;           ///< `/godot/audio/status`
        std::string lastError;        ///< `/godot/engine/lastError`; empty when nothing was refused
        std::string writeError;       ///< `/godot/document/writeError`; empty when no write is outstanding
        std::string warnings;         ///< `/godot/document/warnings`, one per line

        /** `/godot/document/revision`: 0 before the first publish, never 0 after it. */
        std::uint64_t revision = 0;

        //======================================================================
        /*  THE SENTENCES, made here rather than beside the labels that show
            them, because what a reading MEANS is the model's answer and where
            it sits on screen is the window's. They are also what a test can
            assert without a window. */

        /** "House to half  memo", or the words for none. */
        std::string standbyLine() const;

        /** "unsaved changes · recovery waiting", "saved", or the word for no answer. */
        std::string fileLine() const;

        /*  "undo: node.set · redo: nothing to redo" - said, not only greyed
            (§4.8): a disabled button reports that something is unavailable and
            never which something. */
        std::string undoLine() const;

        /** "audio running · locked", the lock said in a word beside its colour. */
        std::string statusLine() const;

        /** Whether this client should OFFER a save: not while the show is locked (§9, decision W). */
        bool mayOfferSave() const noexcept { return locked != Flag::yes; }

        bool operator== (const TransportReading& other) const noexcept;
        bool operator!= (const TransportReading& other) const noexcept { return ! (*this == other); }
    };

    /** One pass over the snapshot. Any thread that holds a snapshot may call it. */
    TransportReading readTransport (const tree::TreeSnapshot& snapshot);
}
