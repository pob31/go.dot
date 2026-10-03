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
    ONE ENTRY OF A FADE'S MOVES, BY HAND (namespace draft §26, PF).

    `/godot/cue/<fade>/moves/send/<bus>`, `.../moves/eq/<row>` and
    `.../moves/fx/<plugin>/<n>`: a number sets that entry of the fade's
    `sends`, `eq` or `fx` list, and an empty text takes it out - the tick box.
    FxRows' shape, for its reason: the door reads the list, changes one entry
    and writes the list back through the ordinary door, so the lock, the undo
    step and the coalescing of a drag are the document's - and two entries
    moved in one pass cannot overwrite each other, as a client merging lists
    from a snapshot a pass old could.

    WHAT IT REFUSES: `unknown-id` for a cue that is not a fade, a bus or a
    plugin entry the show has not got; `bad-address` for an EQ row a fade
    cannot move (a switch, a shape) and, when the catalogue knows the plugin,
    an index past its count; `type-mismatch` for a value that is not a number
    in the row's range.
*/

#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/plugin/Catalogue.h>

#include <string>

namespace wfg::cue
{
    /** `/godot/cue/<id>/moves/...`, and nothing else. */
    bool isFadeMoveAddress (const std::string& address);

    /** The door. `catalogues` may be null: then any index is accepted. */
    doc::LiveWrite fadeMoveWriteFor (doc::ShowDocument& document, const plugin::CatalogueStore* catalogues);
}
