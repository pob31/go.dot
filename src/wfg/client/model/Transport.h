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
    What the transport shows, read out of one snapshot.

    The transport is the strip along the top of the window: which show, whether
    it has unsaved changes, where the clock is, which list is focused and which
    cue GO would fire, whether the audio is running, and the last thing the
    engine refused. It reads the same addresses the page's views/strip.js reads
    (:26-230) so that the two clients cannot disagree about what the engine
    said, and it reads them into plain strings so that a test can assert the
    words with no window in the room and under either locale.

    A reading is a value, taken once per timer pass from one snapshot, and the
    window compares it with the last one to decide what to redraw. Nothing here
    keeps a pointer into the snapshot: TreeSnapshot is immutable and shared,
    but a reading that outlived its snapshot would be a lifetime question, and
    a struct of strings has none.
*/

#include <cstdint>
#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct TransportReading
    {
        std::string show;             ///< `/godot/document/name`
        bool dirty = false;           ///< `/godot/document/dirty` - the dot
        bool locked = false;          ///< `/godot/document/locked` - the close button asks this first

        std::string tick;             ///< `/godot/engine/tick`, as digits
        std::string clock;            ///< `/godot/engine/clock`: dummy, hosted, device
        std::string rate;             ///< "48000 / 256", as strip.js:186 draws it; empty before the clock is known

        std::string listId;           ///< `/godot/list/focus`, or the first of `/godot/list/order`
        std::string listName;
        std::string standbyId;        ///< `/godot/list/<listId>/standby`; empty when the standby is clear
        std::string standbyName;
        std::string standbyKind;

        std::string status;           ///< `/godot/audio/status`
        std::string lastError;        ///< `/godot/engine/lastError`; empty when nothing was refused

        /** `/godot/document/revision`: 0 before the first publish, never 0 after it. */
        std::uint64_t revision = 0;

        /** The standby as one line: "name  kind", or the words for none. */
        std::string standbyLine() const;

        bool operator== (const TransportReading& other) const noexcept;
        bool operator!= (const TransportReading& other) const noexcept { return ! (*this == other); }
    };

    /** One pass over the snapshot. Any thread that holds a snapshot may call it. */
    TransportReading readTransport (const tree::TreeSnapshot& snapshot);
}
