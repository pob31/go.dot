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

namespace wfg::client::gesture
{
    /** GO: the button, and Space. */
    Event go();

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

    /** Writes the show into its bundle: the button, and ctrl/⌘-S. */
    Event save();

    /** Reloads the show from disk, throwing away everything since the last save. Asks first. */
    Event revert();

    /** Answers the recovery offer: adopt that work, or delete it for good. */
    Event recover();
    Event discardRecovery();

    /** Show mode, written as the node it is. */
    Event setLocked (bool locked);

    /*  Stops one run and everything under it. The running pane's cross, and
        only the cross: a cue stopped by a click that landed anywhere on a row
        is a cue nobody meant to stop. */
    Event kill (const std::string& runId);
}
