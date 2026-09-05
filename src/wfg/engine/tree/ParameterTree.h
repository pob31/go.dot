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
    The `/godot` namespace, built from the parameter table and published once
    per tick.

    IT IS A PROJECTION. Nothing here owns a value. A node under `/godot/cue`
    reads an attribute of show.xml, a node under `/godot/engine` reads a counter
    the tick thread keeps, and a node under `/godot/cmd` describes a command the
    registry already holds. Writing to one is the `node.set` command, which goes
    through the document's single write path like every other mutation.

    OBJECTS ARE ADDRESSED BY IDENTITY, never by position: a cue lives at
    `/godot/cue/<id>` whatever list contains it and wherever it sits in the
    order. So a client's subscription survives a reorder, and the address an
    operator reads off the screen is the address a Choufleur pointer resolves
    (PRD §3.23). Order is a separate read-only node on the container.

    DERIVED VALUES ARE COMPUTED HERE, not stored. A cue's `kind`, `parent`,
    `index` and a container's `order` are `persist=none` in the table: the
    structure already says them, so storing them would be a second copy that
    eventually disagrees with the first. The document drops those rows
    entirely; this is what puts them back on the wire.

    WHAT IS REBUILT AND WHEN. The document side is the big one and changes only
    when someone edits the show, so it is built once, shared between snapshots
    by pointer, and thrown away when markStale() says the show moved. The
    engine's own counters are a dozen nodes that change every tick and are
    rebuilt every tick. Phase 1 rebuilds the document side on ANY applied
    mutation rather than working out which subtree was affected - the fixtures
    are small, and the property that has to be right now is that a published
    snapshot never changes afterwards, not that publishing is cheap. When there
    is a show big enough to measure, measure it.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace wfg::tree
{
    /*  The engine's own numbers, handed to the tree each tick because the tree
        has no way to ask for them: Engine.h is deliberately vendor-free and
        knows nothing about a parameter tree, and the clock lives on a thread of
        its own. Whoever wires the three together fills this in. */
    struct EngineState
    {
        std::string product = "Go.dot";
        std::string version;

        std::int64_t tick = 0;
        int sampleRate = 0;
        int blockSize = 0;
        int samplesPerTick = 0;
        std::int64_t lateness = 0;
        std::int64_t latenessMax = 0;

        /** `dummy` in Phase 1, `device` from Phase 2. */
        std::string clock = "dummy";

        std::uint64_t errorCount = 0;
        std::string lastError;

        //======================================================================
        // Which bundle is open. Runtime, not document: PRD §4.10.
        std::string documentPath;
        std::string documentName;
        bool documentDirty = false;
    };

    class ParameterTree
    {
    public:
        /** Neither reference may outlive the tree. */
        ParameterTree (const doc::ShowDocument& documentToProject,
                       const CommandRegistry& commandsToDescribe);

        /*  Tick thread only. Rebuilds the document side if it has been marked
            stale, then publishes an immutable snapshot and returns it. */
        std::shared_ptr<const TreeSnapshot> publish (std::int64_t tick, const EngineState& state);

        /** The show changed; rebuild before the next publish. */
        void markStale() noexcept { stale = true; }

        /*  Any thread. The most recently published snapshot, or an empty one
            before the first publish - never nullptr, so a caller never has to
            check. */
        std::shared_ptr<const TreeSnapshot> snapshot() const;

    private:
        void rebuildDocumentPart();

        const doc::ShowDocument& document;
        const CommandRegistry& commands;

        std::shared_ptr<const std::vector<Node>> documentPart;
        bool stale = true;

        /*  A plain mutex rather than the RtSnapshot spin lock, and the
            difference is which threads are involved. Neither side here is the
            audio thread: the tick thread publishes and the server threads read,
            and both are allowed to block for the few nanoseconds a pointer swap
            takes. When Phase 2 puts a READER on the audio callback, that reader
            gets the spin-lock treatment - the lipogram (PRD §4.2) is about that
            thread, not about this one. */
        mutable std::mutex publishMutex;
        std::shared_ptr<const TreeSnapshot> published;
    };
}
