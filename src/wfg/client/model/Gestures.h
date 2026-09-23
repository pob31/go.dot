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

    /*  FIRES ONE NAMED CUE, without touching standby - which is what a button
        on a surface does, and what the waveform panel's play button does when
        somebody wants to HEAR the region they are placing (author, 2026-09-21:
        *"we need to play/pause the clip for testing"*).

        IT IS THE REAL CUE AND NOT A PREVIEW. It plays through its own routing
        at its own level, its run appears in the running pane like any other,
        and PANIC stops it - which is the honest arrangement: an audition that
        went somewhere the show does not would be teaching somebody the wrong
        thing about what they are about to hear. */
    Event fireCue (const std::string& cueId);

    /*  Makes a range on a media cue: a named region of its file, and one entry
        in the playlist the cue plays instead of the whole thing (§3.24). As
        `createCue`, the identifier is the engine's to draw and the log's to
        record - a window is not the second consumer of entropy in this
        project - so the panel finds what it made on the next pass. */
    Event createRange (const std::string& cueId, double in, double out);

    /*  Gives a media cue a send into one mix channel. The level follows as an
        ordinary `node.set` once the object exists, which is why this carries
        none: there is one way values are written. */
    Event createSend (const std::string& cueId, const std::string& busId);

    /*  PHASE 9a, PR 9a.9: an insert on a media cue - one entry of the show's
        set switched in, made by the first switch on the FX panel; an entry
        declared in the set from the machine's known list, with the four words
        a replay needs; and a fresh child for an entry that failed. */
    Event createFx (const std::string& cueId, const std::string& pluginId);
    Event createPlugin (const std::string& name, const std::string& identifier,
                        const std::string& format, const std::string& path);
    Event restartPlugin (const std::string& pluginId);

    /** `eq.reset`: a media cue's EQ back to flat, one transaction (Phase 9a). */
    Event eqReset (const std::string& cueId);

    /*  Cuts one of a media cue's ranges in two where the playhead is. One
        command rather than a create, a shortening and a reorder, so it is one
        undo step and one record - see `ShowDocument::splitRange`. */
    Event splitRange (const std::string& cueId, double at);

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

    /*  A scrub settling on a second of a run's material: the head dragged
        along a running cue's strip, or along a running scene's row. One per
        position the hand settles on, and one when it lets go. */
    Event seek (const std::string& runId, double seconds);

    /*  Load to time (§3.13): where the operator is pointing - a cue and how
        far into it, -1 for before it fired - and the jump that makes the aim
        true. The aim is asked on every change, since asking is free and the
        answer is what the panel shows; the load is the one button. */
    Event aim (const std::string& listId, const std::string& cueId, double offset);
    Event loadToTime (const std::string& listId);

    /** The live recorder: keep every cue start from now, and write them into a take. */
    Event recordStart();
    Event recordStop();

    /*  THE OUTPUT LAYOUT (PRD §3.9b, §6.2). Four commands rather than writes,
        because `Bus/@firstChannel` is the running sum of the widths before it
        and the engine keeps it so: `document/OutputLayout.h` says why, and
        what each of these does to the interface patch.

        `kind` is "direct" or "mix"; `width` is 1 for mono and 2 for stereo;
        `index` is a position in the output list, -1 for the end, and
        `moveBus`'s is a position in the list AS IT STANDS - the same
        convention `moveObject` uses, so one drag rule serves both lists. */
    Event createBus (const std::string& kind, int width, int index);
    Event deleteBus (const std::string& busId);
    Event moveBus (const std::string& busId, int index);
    Event setBusWidth (const std::string& busId, int width);

    /*  A DEVICE THIS SHOW TALKS TO, declared at a prefix.

        WITH NO NAMESPACE FILE, which is what makes it an opaque device: the
        window has no way to write one and a desk almost never has one (PRD
        §3.22). The engine's command still takes the argument, because a
        described device is made by hand in the file and the command has to be
        able to say so - and because a replayed session holds the three-argument
        form. Everything else about the device is written afterwards, with
        `setNode`, like every other row a person edits.

        The identifier comes back on the applied record, which is what the
        window reads to know which row to put a name into. */
    Event createDevice (const std::string& prefix);

    /*  A MIDI port the show declares, by the name a person reads. Which cable
        it is on this machine is said afterwards with `setNode`, because the
        two are different kinds of fact (PRD 4.10). */
    Event createPort (const std::string& name);

    /*  A CONTROL SURFACE, AND THE STRIPS ITS PROFILE IMPLIES, in one command
        (PRD §3.16): eight for the virtual panel and a Mackie unit, sixteen for
        the D700 and for pads. `profile` is one of the four words
        `profileChoices` offers; with no name only the profile is sent, and
        the surface is called what its profile is until somebody names it.
        Every identifier the engine draws - the surface's and each strip's -
        rides on the applied record, so the window finds what it made on the
        next pass, as it does after `createCue`. */
    Event createSurface (const std::string& profile, const std::string& name = {});

    /*  ONE MORE STRIP at the end of a surface: the second eight of a Mackie
        unit with an extender, one more pad. Where it sits is its position and
        never a number somebody types (namespace draft §16.2). */
    Event createStrip (const std::string& surfaceId);

    /*  A DCA: a named trim the cues and groups marked with it follow (PRD
        §3.28). Which DCA it sits inside, and its short name, are written
        afterwards with `setNode`, like every other row a person edits. */
    Event createDca (const std::string& name);

    /*  A HAND ON A SAMPLER STRIP, AND THE HAND LIFTED (PRD §3.27): a pad of
        the virtual panel clicked, a number key held. `velocity` is 1 to 127,
        from where on the pad the click landed; below 1 the argument is left
        out, as a fader lifted from the bottom leaves it out, and the clip
        starts where its strip puts it. The origin is who owns a held clip,
        which is why a release from this window lets go only of what this
        window pressed. */
    Event pressStrip (const std::string& stripId, int velocity);
    Event releaseStrip (const std::string& stripId);

    /*  A HAND ON A FADER, AND OFF IT: the two halves of a ride, around the
        `setNode`s that move it. The address is the one the strip publishes as
        its `target` - a run's trim, a DCA's - so the touch table gates the
        panel exactly as it gates a motor fader, and a fader-start is the same
        rule from the mouse as from the hardware (§16.4). */
    Event touchNode (const std::string& address);
    Event releaseNode (const std::string& address);

    /*  THAT THE PATCH HAS STOPPED FOLLOWING THE LIST. Sent by the settings
        window before the first hand edit of the output matrix lands, and by
        its "follow the list" button with false to hand the outputs back to
        the order. The other way it becomes true is the engine's own: the
        first media run that launches with audio says so. */
    Event setPatchSettled (bool settled);
}
