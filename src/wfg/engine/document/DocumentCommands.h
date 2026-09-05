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
    The document's commands, which is the only way its contents ever change.

    Registered against a document the caller owns, so that a test can drive one
    without an engine and the engine can hold one without the document layer
    knowing what an engine is. The handlers run on whatever thread calls
    processTick, which for the engine means the tick thread and only that one.

    Every command that creates something takes its identifier as an optional
    last argument and returns the identifier it used. That convention is what
    makes replay work: the log records the call as APPLIED, so a replay supplies
    the identifier that was drawn rather than drawing a different one.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>

#include <functional>
#include <string>

namespace wfg::doc
{
    /** Adds list.create, cue.create, mount.create, object.delete, object.move
        and node.set, all bound to `document`. */
    /*  Where a write goes when the address is not the document's.

        `node.set` is the one value-write command (PRD §4.11 - a client reaches
        the model through named commands and nothing else), and from Phase 2 not
        every address it can be given belongs to the show: `/wfs/input/1/gain`
        is a node on somebody else's box, mounted into the tree. This is how
        that write leaves the document layer without the document layer learning
        what a mount is.

        A callback rather than a MountTable& because the layering is the point:
        this file knows about a show and a schema, and `wfg tree`, `wfg canon`
        and every document test register these commands with no mounts at all.
        Absent, a foreign address is refused exactly as it was in Phase 1. */
    using ForeignWrite = std::function<Outcome (const std::string& address, const osc::Value&)>;

    void registerDocumentCommands (CommandRegistry& registry, ShowDocument& document,
                                   ForeignWrite foreign = {});
}
