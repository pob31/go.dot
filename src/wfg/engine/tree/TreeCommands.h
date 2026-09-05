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
    The tree's own commands: taking a node and giving it back.

    `node.set` is not here. A value write goes to the document's single write
    path and is registered with the rest of the document's commands, which is
    where it belongs: the tree is a projection and owns no value.

    WHY TOUCHING IS A COMMAND like any other, rather than a flag on a
    connection. Because PRD §3.15 admits no second path: everything that
    changes engine state arrives as an event, is applied by the tick thread in
    arrival order, and is written to the log with the tick it landed on. A
    touch changes what the engine will send and to whom, so a replay that did
    not re-apply it would produce a different set of outbound messages from the
    session it claims to reproduce.

    THE ORIGIN COMES FROM THE EVENT, not from an argument. A client cannot
    touch a node on somebody else's behalf, because the only origin a handler
    can see is the one the transport stamped on the way in. That is not a
    permission check - Phase 1 has no permissions - it is that the question
    "who is holding this" has exactly one truthful answer and it is not the
    caller's to supply.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/tree/Touches.h>

namespace wfg::tree
{
    /** Adds node.touch and node.release, both bound to `touches`. */
    void registerTreeCommands (CommandRegistry& registry, TouchTable& touches);
}
