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
    The commands that move the standby, and the one that says which list they
    move it on.

    ARGUMENT-LESS BY DESIGN. `standby.next`, `standby.previous` and
    `standby.clear` take nothing and act on the focused list, because that is
    the gesture an operator makes - a key, a footswitch, a button - and a
    command that required the list would make every surface carry a piece of
    state it has no business knowing. `standby.set` takes the cue because
    naming one is the gesture.

    A DIRECT WRITE IS THE SAME THING WITH A DIFFERENT NAME. `node.set` on
    `/godot/list/<id>/standby` goes through the same door and is checked by the
    same invariant, and it works on ANY list rather than only the focused one -
    which is what makes the load path work, since restoring a saved show writes
    every list's standby with no focus involved. The log records `node.set`
    when that is what the client sent: the record says what happened, and
    renaming it would make it disagree with the origin beside it.

    WHAT COUNTS AS A REJECTION, and it is a narrower set than it looks:

      * No list at all → `not-in-list`. The command names an operation on a
        list and there is no list; that is a refusal, not a silent success.
      * A pointer with nowhere to go → APPLIED. At the end of a list, or from
        empty, `next` and `previous` leave the standby where it is and are
        logged applied. There is a list and the command did what it does; it
        simply had nowhere to move. The approved plan fixes this for the end of
        a list, and the author fixed it for empty on 2026-09-06.
      * Setting the standby it already holds, or clearing an empty one →
        APPLIED. An empty standby is a resting state (PRD §3.5), not a failure,
        and a surface that sends the same gesture twice is doing something
        reasonable.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/document/ShowDocument.h>

namespace wfg::cue
{
    /*  Adds standby.set, standby.clear, standby.next, standby.previous and
        list.focus, bound to the document and to a Focus the caller owns.

        `focus` is runtime state and must outlive the registry. It is not
        published and not persisted - see the note in CueList.h. */
    void registerCueCommands (CommandRegistry& registry, doc::ShowDocument& document,
                              Focus& focus);
}
