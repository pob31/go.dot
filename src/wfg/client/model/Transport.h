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
    /*  WHERE THE DOH! WINDOW STANDS (the author, 2026-10-02: "I would display
        the button in a distinctive colour and fade out when the Doh! timer is
        over"). `open` while the last GO can still be taken back and more than
        the fade is left, `fading` over the window's last ticks, `over` once a
        press would be refused - or when there is nothing to take back at all. */
    enum class DohPhase { over, open, fading };

    struct DohLook
    {
        DohPhase phase = DohPhase::over;

        /*  How much of the button's own colour is left: 1 the colour whole,
            0 the idle look, and in between while it fades. */
        double strength = 0.0;

        /** Whole seconds left in the window, rounded up; 0 once it is over. */
        int secondsLeft = 0;
    };

    struct TransportReading
    {
        std::string show;             ///< `/godot/document/name`
        Flag dirty = Flag::unsaid;    ///< `/godot/document/dirty` - the dot
        Flag locked = Flag::unsaid;   ///< `/godot/document/locked` - show mode
        Flag recovery = Flag::unsaid; ///< `/godot/document/recovery` - work found beside the show
        Flag recording = Flag::unsaid; ///< `/godot/document/recording` - the live recorder is on

        std::string tick;             ///< `/godot/engine/tick`, as digits
        std::string clock;            ///< `/godot/engine/clock`: dummy or device
        std::string rate;             ///< "48000 / 256"; empty before the clock is known

        std::string listId;           ///< `/godot/list/focus`, or the first of `/godot/list/order`
        std::string listName;
        std::string standbyId;        ///< `/godot/list/<listId>/standby`; empty when the standby is clear
        std::string standbyName;
        std::string standbyKind;

        /*  THE STANDBY CUE'S NOTES, beside its name (author, 2026-09-18:
            "notes should appear next to the cue name at the standby - this
            field can be long since it's usually where you put what to look
            out for to launch the cue"). The line an operator reads before GO. */
        std::string standbyNotes;

        Flag canUndo = Flag::unsaid;  ///< `/godot/document/canUndo`
        Flag canRedo = Flag::unsaid;
        std::string undoName;         ///< the command that opened the transaction: `node.set`, `cue.create`
        std::string redoName;

        std::string status;           ///< `/godot/audio/status`
        std::string lastError;        ///< `/godot/engine/lastError`; empty when nothing was refused

        /*  `/godot/audio/rateMoved` and its tick, as digits: the last time the
            show followed the interface onto another clock (PRD §6.2), in
            words - said until something newer is refused (`errorLine`). */
        std::string rateMoved;
        std::string rateMovedTick;
        std::string dial;             ///< what the master dial turns, in words (`dialLine`); empty while free
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

        /*  DOH! (PRD §3.32, 2026-10-01): `/godot/list/doh` as the engine
            spells it - "<list> <cue> <tick>" of the GO it would take back, or
            empty - the number (or the name) of that cue, and the show's
            `/godot/list/dohWindow` in seconds. */
        std::string doh;
        std::string dohCue;
        std::string dohWindow;

        /*  WHAT THE NEXT GO CARRIES ON (2026-10-02, Doh! D2): the focused
            list's `/godot/list/<id>/resume` as the engine spells it - "<cue>
            <seconds>", the cue a Doh paused and the second it carries on from,
            "<cue>" alone for a mic, which has no position - or empty. */
        std::string resume;

        /*  WHAT A PRESS OF DOH! WOULD FORGET NOW (D2's review, MY):
            `/godot/list/dohForget` - "<list> <cue>" of the resume the last Doh
            left, when no GO a press could take back stands before it, or empty
            - and that cue in the words the list shows it by. The engine's
            answer, whichever list has the focus: a press acts on the list of the
            last Doh. */
        std::string dohForget;
        std::string dohForgetCue;

        /*  WHAT THE LAST DOH! PUT BACK AND WHAT IT LEFT (D4, namespace draft
            §24.14): `/godot/list/dohReport` as the engine spells it - "<list>
            <tick> <sentence>", the runner's, whichever list the Doh was on - or
            empty; and the name of that list, which the notice opens with when
            it is not the focused one. */
        std::string dohReport;
        std::string dohReportList;

        /*  THE DOH NOTICE: the report in front of the operator, whatever list
            has the focus - "Doh!: ..." on the focused list, "Doh! on Act 2: ..."
            on another - what was left to an operator first, since that is what
            the operator must tell the other department. Empty when there is no
            report, or a refusal newer than it has taken the line. */
        std::string dohNotice() const;

        /*  WHAT A PRESS OF DOH! SAYS AT ONCE: nothing, unless the audio is out -
            then the pointer goes back now and the rest waits for the clock
            (L18), and the report with it. */
        std::string dohPressLine() const;

        /*  Whether a click of the button would do something: take back the GO
            the caption names, or forget the resume `dohForget` names. */
        bool dohClickable() const;

        /*  "  resumes at 0:08" when the next GO carries the standby on rather
            than starting it, "  resumes" for a mic; empty otherwise. In words,
            beside the standby's name, never a colour alone (§4.8). */
        std::string resumeWords() const;

        /*  WHAT THE DOH! BUTTON SAYS: "Doh! 12" while the last GO is still
            inside the window - read off the engine's own tick - and "Doh!"
            otherwise, so the operator sees which GO a press would take back. */
        std::string dohCaption() const;

        /*  THE BUTTON'S LOOK, from the same three things the caption reads -
            the engine's tick, the GO's and the show's window - and never from a
            clock of this window's own: open in its colour, fading over the
            window's last two seconds (half the window when it is shorter than
            four), over - the idle look, and no click - from the tick a press
            would start being refused. */
        DohLook dohLook() const;

        /*  THE BUTTON'S TOOLTIP: what a press does and the key, and while the
            window is open, which cue and how many seconds are left - the time
            said in words as well as in the fading colour (§4.8). */
        std::string dohTip() const;

        /*  The window, in seconds: the snapshot's, the schema's ten when it has
            not said, never below nought - which is Doh! switched off. */
        double dohWindowSeconds() const;

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

        /*  GO WEARS THE AUDIO (author, 2026-09-25: "Can the Go button be grey
            when the audio interface is not running be bright yellow with black
            letters when it's running?"). Bright only when a device is open and
            producing: the one glance before a press is also the one that says
            whether the sound will leave. GO still fires when grey - a MIDI or
            a network cue needs no interface, and GO never blocks (§4.1).

            AND IN A WORD UNDER IT (§4.8), since grey alone would be colour
            carrying the news: "no audio", "no clock", or the dash for an
            engine that has not said. Empty while running - the yellow is the
            ordinary state and needs no caption. */
        bool audioRunning() const noexcept { return status == "running"; }
        std::string goLine() const;

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
