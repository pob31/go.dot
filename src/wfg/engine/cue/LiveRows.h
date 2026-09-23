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
    THE ROWS A HAND RIDES, and the door `node.set` goes through to reach them.

    Two of them in Phase 6: `/godot/run/<id>/trim`, the fader or pad on the
    strip a run holds, and `/godot/dca/<id>/trim`, a DCA's fader. Both are
    `persist=none` - what a fader is doing tonight is not a decision (PRD
    §4.10) - and a `none` row is derived by construction: the document has
    nowhere to put one and its write door refuses it `read-only`. So these are
    answered IN FRONT OF the document, by this.

    WHY `node.set` AND NOT A COMMAND OF THEIR OWN. `list.aim` is the precedent
    that goes the other way - a runtime row written by a named command - and it
    would have worked here too. But a fader is exactly what the rest of the
    engine already speaks `node.set` to: the touch table that stops the engine
    fighting a hand is kept per ADDRESS (`tree/Touches.h`), the tablet's
    generic inspector writes whatever node it is shown, and a surface bridge
    that rides "the node this strip is on" wants one verb whatever that node
    is. A command per row would give each of them a second vocabulary to learn.

    NOT UNDOABLE, AND NOT A TRANSACTION. A ride is hundreds of writes; folded
    into the show's history they would bury the three edits somebody actually
    made, and Undo after a mistyped cue name would move a fader instead
    (namespace draft §14.9's reserved second domain, still not built). The
    transaction hook asks `isLiveAddress` and opens nothing.

    LOGGED AND REPLAYED like any `node.set`: the record is the address and the
    value as submitted, and a replay applies it through this same door.
*/

#include <wfg/engine/document/DocumentCommands.h>

#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    class DcaTable;
    class RunTable;

    /** Whether `address` is one of the rows answered here rather than by the
        document: `/godot/run/<id>/trim` or `/godot/dca/<id>/trim`. */
    bool isLiveAddress (std::string_view address);

    /*  Whether an applied command was a ride on a live row - `node.set` on
        one of the addresses above - which is the one thing the transaction
        hook must not open a transaction for. Asked by every installation of
        that hook (serve, replay, the tests), so the rule is written once. */
    bool isLiveWrite (const std::string& commandName, const std::vector<osc::Value>& args);

    /*  The door, bound to the tables it writes. Answers nothing for an address
        that is not live, so `node.set` goes on to the document as it always
        did; for one that is, the outcome:

        - a value that does not parse as the row's type, or is outside its
          range, is `type-mismatch` - the answer the document's own door gives
          for the same mistake on a stored row;
        - a DCA the show does not declare is `unknown-id`;
        - a run that does not exist, or has finished, is APPLIED AND IGNORED:
          a surface a tick behind a clip that just ended is not making a
          mistake, and fifty refusals a second while a hand lets go would be
          noise. */
    doc::LiveWrite liveWriteFor (RunTable& runs, DcaTable& dcas,
                                 const doc::ShowDocument& document);
}
