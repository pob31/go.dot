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
    WHAT THE MACHINE FOUND ABOUT EACH SURFACE THE SHOW DECLARES - whether it is
    being talked to, why not, and the serial it gave when it was asked.

    The `PortTable` shape exactly, and for its reason: the show says "the D700
    on these two ports" (a decision, PRD §4.10), and whether anything answers
    tonight is a fact about this building. So the decision is in the document,
    this is beside it, and the parameter tree publishes both at
    /godot/surface/<id> without either being a copy of the other.

    NAMES NO JUCE TYPE, so the tree, the bridge and a test with no MIDI in the
    room can all hold one.

    THREADING: none of its own. The tick thread fills it (the bridge, in the
    after-tick) and the tick thread reads it (the tree, when it rebuilds its
    document half) - the model's thread, like the port table. A change that
    should reach a client asks the tree to rebuild, because this is read from
    the cached half.
*/

#include <map>
#include <string>

namespace wfg::surface
{
    class SurfaceTable
    {
    public:
        struct Status
        {
            bool connected = false;

            /** Why not, in one sentence; empty when it is, and empty when
                nobody has looked yet. */
            std::string problem;

            /** What the hardware said when it was asked who it is. */
            std::string serial;
        };

        /** Replaces what is known about one surface. Answers whether anything
            a reader could see changed, so the caller knows whether to ask the
            tree for a rebuild. */
        bool set (const std::string& surfaceId, const Status& status)
        {
            auto& held = table[surfaceId];

            const auto changed = held.connected != status.connected
                                   || held.problem != status.problem
                                   || held.serial != status.serial;
            held = status;
            return changed;
        }

        void forget (const std::string& surfaceId) { table.erase (surfaceId); }

        /** What is known, or a default Status - not connected, no sentence -
            for a surface nobody has looked at. */
        Status statusOf (const std::string& surfaceId) const
        {
            const auto found = table.find (surfaceId);
            return found != table.end() ? found->second : Status {};
        }

    private:
        std::map<std::string, Status> table;
    };
}
