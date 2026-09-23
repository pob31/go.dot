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
    THE PARAMETER DOOR IN FRONT OF A CUE'S INSERT (Phase 9a, §17.4, PR 9a.8).

    A cue's values for one plugin of the set are ONE sparse row, `fx/values`,
    written whole by a script; a hand writes ONE parameter through the `p<n>`
    nodes beneath, one node a parameter, so a rotary's turn is one address and
    one undo step (PRD §4.10). This is the door those nodes go through: the
    shape of LiveRows' `liveWriteFor`, composed into `node.set`'s one live slot
    beside it - but where that door answers in front of the document because
    the document cannot hold a fader's trim, this one REWRITES THE DOCUMENT:
    it reads the row, replaces entry `n`, and writes the row back through the
    ordinary door, so the lock, the undo transaction and the coalescing all
    apply. `isLiveWrite` stays false for it: an FX write IS undoable.

    WHAT IT REFUSES. `unknown-id` for an `Fx` nobody made; `type-mismatch` for
    a value that is not a number in 0..1; and, when the catalogue knows the
    plugin, `unknown-id` for a parameter past its count - a node that does
    not exist. When the catalogue does not know it (a replay on a machine
    without the plugin) any index is accepted, so a log replays whole (plan
    decision 8).

    THE ROW'S SPELLING: `index:value` pairs, space-separated, sorted by
    index, each value through the canonical number formatter, so the row
    round-trips under every locale and a diff of two shows reads.
*/

#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/plugin/Catalogue.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wfg::cue
{
    /** `/godot/fx/<id>/p<n>`, and nothing else. */
    bool isFxParameterAddress (const std::string& address);

    /*  The sparse row, parsed: index to normalised value, sorted. Doubles,
        as the document holds numbers, so what was written is what is spelled
        back; the audio side takes a float at its own boundary. */
    std::map<int, double> parseFxValues (const std::string& text);

    /** The row's spelling for a map, canonical. */
    std::string formatFxValues (const std::map<int, double>& values);

    /** The door. `catalogues` may be null: then any index is accepted. */
    doc::LiveWrite fxWriteFor (doc::ShowDocument& document, const plugin::CatalogueStore* catalogues);

    /*  Two doors as one: the first that answers, answers. What `node.set`'s
        one live slot is handed when a session has a fader door and an FX
        door - a live row is answered in front of the document, an FX
        parameter through it, and everything else falls to the document. */
    doc::LiveWrite eitherOf (doc::LiveWrite first, doc::LiveWrite second);
}
