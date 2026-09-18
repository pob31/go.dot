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

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

        /*  `/godot/document/warnings` IS NOT CARRIED WHOLE, and that is a
            fix rather than a saving. It is one line per thing wrong with the
            show that did not stop it opening, and on a generated 300-cue show
            it measured 233,471 characters over 1,824 lines. A reading is
            compared field by field twenty-five times a second, and its text
            reaches a label: this window hung - spinning, not crashing - the
            first time it was handed the whole of it, because laying 233 kB of
            text into a one-row box is work without end. So the reading carries
            the COUNT and the FIRST ONE, clipped, and a view with room to show
            them all can read the node itself. */
        std::size_t warningCount = 0;
        std::string warningFirst;

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
        /*  WHAT UNDO AND REDO WOULD TAKE BACK, for the buttons' own
            tooltips rather than for a line of its own. The line went on the
            author's word (2026-09-18: "to gain a bit of headroom, the undo/redo
            messages can be removed. If we need we can have a history toggle to
            display the list of edits we can undo and redo") - so the sentence is
            one hover away instead of always on screen, and the history it
            describes is a panel somebody will build once.

            THE ENGINE'S OWN WORD FOR IT, and no table here: `undoName` already
            reads `node.set`, `cue.create`, `object.delete` - the names §4.11
            makes every action carry. A lookup table in this file would go stale
            the day a command is added by somebody who never opened it. */
        std::string undoTip() const;
        std::string redoTip() const;

        /*  "undo: node.set · redo: nothing to redo" - said, not only greyed
            (§4.8): a disabled button reports that something is unavailable and
            never which something. */


        /** "audio running · locked", the lock said in a word beside its colour. */
        /*  THE LOCK, SAID IN A WORD and not only drawn in a colour (§4.8),
            and empty when the show is not locked or has not said.

            IT USED TO CARRY THE AUDIO TOO - "audio running" - and no longer
            does (author, 2026-09-18: "the 'Audio running' can go. We will add a
            configuration panel for the various settings"). Whether a device is
            open is something somebody sets up once and then stops reading; the
            lock is something that changes what the next press will do. */
        std::string lockLine() const;

        /** "3 warnings · <the first>", or empty. Bounded, whatever the show says. */
        std::string warningLine() const;

        /*  THE LAST REFUSAL, AS A SENTENCE RATHER THAN A LOG LINE.
            `/godot/engine/lastError` is five fields - tick, sequence, origin,
            reason and command - and it is written for `grep` at four in the
            morning, which is the right shape for a log and the wrong one for a
            strip. An operator who has just pressed something needs to know
            WHAT was refused and WHY, and the tick it happened on is noise: they
            were there.

            So: "standby.set refused: not-a-stop". The reason word is the
            engine's own and is not translated here - a table of friendly
            sentences in this file would go stale the day a reason is added by
            somebody who never opened it, which is the argument `undoLine`
            makes too. Anything that does not parse into five fields is shown
            whole, so a format change is visible rather than swallowed. */
        std::string errorLine() const;

        /** Whether this client should OFFER a save: not while the show is locked (§9, decision W). */
        bool mayOfferSave() const noexcept { return locked != Flag::yes; }

        /*  AND WHETHER THERE IS ANYTHING TO SAVE, which is the other half of
            the same button and is now the ONLY place the window says so
            (author, 2026-09-18: "'Saved' can also go and be replaced by a
            dimmed Save button when there are no changes to save").

            A STATE A CONTROL CAN BE IN beats a state a control is described by:
            a dimmed Save says "nothing to save" where somebody is already
            looking when they wonder, and it costs no line. `unsaid` counts as
            something to save, because a dot the engine has not published yet is
            not a show with nothing in it - offering a save that turns out to be
            unnecessary costs a write, and withholding one can cost an
            afternoon. */
        bool hasSomethingToSave() const noexcept { return dirty != Flag::no; }

        bool operator== (const TransportReading& other) const noexcept;
        bool operator!= (const TransportReading& other) const noexcept { return ! (*this == other); }
    };

    /** One pass over the snapshot. Any thread that holds a snapshot may call it. */
    TransportReading readTransport (const tree::TreeSnapshot& snapshot);

    /*  How many warnings a `/godot/document/warnings` node holds, and the
        first of them, clipped. Declared rather than kept private because they
        are where a window once hung: a test that hands them a quarter of a
        megabyte is the one that says it cannot happen again. */
    std::size_t countWarnings (std::string_view all);
    std::string firstWarning (std::string_view all);
}
