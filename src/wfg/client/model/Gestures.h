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
    What each gesture of the desktop client sends - the Event, whole.

    PRD §4.11: every gesture-reachable action exists as a named command. The
    page keeps that as gestures/commands.json, a table it reads at start; the
    desktop keeps it here for now, as functions returning the Event a gesture
    submits, so that a test can assert what a click sends with no engine and
    no window (§14.16's "client-side test with no engine"). M5 grows this into
    the one table both clients read - and until then every name here is one
    `wfg commands` lists, which ClientTests pins.

    Every Event carries origin::window, which is what a log reader needs to
    tell a click from a datagram - and what makes §14.16's first rule
    checkable rather than trusted: a change that reached the show any other
    way would write no record, and `wfg replay` of that session would not
    reproduce it.

    THE LOCK IS A NODE, NOT A COMMAND (§14.7), so `lock` is a `node.set` like
    any other write rather than a verb of its own. The page does the same, and
    for the same reason: a `document.lock` would be a second way to say what
    `node.set /godot/document/locked` already says.
*/

#include <wfg/engine/command/Event.h>

#include <string>
#include <vector>

namespace wfg::client::gesture
{
    /** GO: the button, and Space. */
    Event go();

    /*  THE FIRST TWO LEVELS OF STOP (PRD §4.4). Esc, or the PANIC button:
        every run comes down gracefully and the footers run. Esc again within
        the double-press window (model/Panic.h), or PANIC again: everything is
        dropped and no footer runs. The timing is the client's to read - a key
        pressed twice is a fact about a hand - and each press is still one
        named command, so the log says which level was reached and when. */
    Event stopAll();
    Event killAll();

    /*  Where GO would act next, and where it would act before: the cue
        list's arrows. Not a selection - this list has none yet - but the
        pointer itself, which is what the page's arrows move too. */
    Event standbyNext();
    Event standbyPrevious();

    /*  PARK: put the pointer on this cue. The arrows cannot reach a list whose
        standby is clear - `standby.next` stays put "from nowhere", which is
        the engine's own wording and its own decision - so without this a show
        that opens with no standby has no keyboard route into it at all. The
        page answers that with a click on a row and so does this. */
    Event park (const std::string& cueId);

    /** Undo and redo: the buttons, and ctrl/⌘-Z and ctrl/⌘-shift-Z. */
    Event undo();
    Event redo();

    /** Writes the show into its bundle: the menu, and ctrl/⌘-S. */
    Event save();

    /*  Writes a COPY of the show into another folder - manifest, show, state
        and namespaces, not media - and keeps working on this one: the menu's
        Save as, and ctrl/⌘-shift-S. The engine's `document.saveAs`. */
    Event saveAs (const std::string& folder);

    /** Reloads the show from disk, throwing away everything since the last save. Asks first. */
    Event revert();

    /** Answers the recovery offer: adopt that work, or delete it for good. */
    Event recover();
    Event discardRecovery();

    /** Show mode, written as the node it is. */
    Event setLocked (bool locked);

    /*  Makes a cue. The identifier is NOT supplied: `cue.create` takes one as
        an optional last argument and that argument is for replay - the engine
        draws it, the log records the call with it, and a replay supplies it
        rather than drawing again. There is one entropy consumer in this
        project and a window is not going to be the second, so a create is
        followed by finding what it made (model/Media.h). */
    Event createCue (const std::string& parent, int index,
                     const std::string& kind, const std::string& name);

    /*  Moves a cue or a group: a row dragged in the cue list (model/Reorder.h).
        `index` is a member position in `parent`, or -1 for the end. */
    Event moveObject (const std::string& id, const std::string& parent, int index);

    /** Deletes a cue or a group: ctrl/⌘-Backspace on the picked one. Undo brings it back with its ids. */
    Event deleteObject (const std::string& id);

    /*  Gives a group its header or its footer, so a cue can be moved into it:
        the engine answers with the one that exists, so asking twice is safe. */
    Event groupRole (const std::string& group, const std::string& role);

    /*  COPY AND PASTE. Copy asks the engine for a fragment of these cues,
        which the tree then publishes and the window carries to the
        operating system's clipboard; paste hands a fragment back, to land in
        `parent` at member position `index`. The engine draws the new names
        and records them, so a replay draws none (§14.16). */
    Event copyCues (const std::vector<std::string>& ids);
    Event pasteCues (const std::string& parent, int index, const std::string& fragment);

    /*  ONE FIELD, COMMITTED. The address is the NODE's own, never one this
        client assembled: a generic inspector writes back to what it read,
        which is the whole reason it needs no table of field names. */
    Event setNode (const std::string& address, const std::string& text);

    /*  Stops one run and everything under it. The running pane's cross, and
        only the cross: a cue stopped by a click that landed anywhere on a row
        is a cue nobody meant to stop. */
    Event kill (const std::string& runId);
}
