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
    WHAT THE MACHINE FOUND ABOUT EACH PLUGIN THE SHOW DECLARES (Phase 9a,
    decision AE) - whether its child is up, why not, the delay it declares,
    how many parameters it has.

    The `SurfaceTable` shape exactly, and for its reason: the show says "this
    processor, by this identifier" (a decision, PRD §4.10), and whether tonight's
    machine has it, whether its child answered, and what it turned out to hold
    are facts about this building. So the decision is in the document, this is
    beside it, and the parameter tree publishes both at /godot/plugin/<id>
    without either being a copy of the other.

    NAMES NO JUCE TYPE, so the tree, the proxy host and a test with no plugin on
    the machine can all hold one.

    THREADING: none of its own. The message thread fills it (the proxy host,
    from its timer) and the tick thread reads it (the tree, when it rebuilds its
    document half); a change that should reach a client asks the tree to
    rebuild, because this is read from the cached half. Until the sandbox
    exists (9a.6) nothing fills it, and every entry reads `unloaded` - which is
    the truth.
*/

#include <map>
#include <string>

namespace wfg::plugin
{
    class PluginTable
    {
    public:
        struct Status
        {
            /** `unloaded | loading | loaded | missing | failed`, the row's words. */
            std::string state = "unloaded";

            /** Why it is not loaded, in one sentence; empty when it is. */
            std::string problem;

            /** The delay the plugin itself declares, uncompensated. */
            int latencySamples = 0;

            /** How many parameters the catalogue knows for it. */
            int paramCount = 0;
        };

        /** Replaces what is known about one entry. Answers whether anything a
            reader could see changed, so the caller knows whether to ask the
            tree for a rebuild. */
        bool set (const std::string& pluginId, const Status& status)
        {
            auto& held = table[pluginId];

            const auto changed = held.state != status.state
                                   || held.problem != status.problem
                                   || held.latencySamples != status.latencySamples
                                   || held.paramCount != status.paramCount;
            held = status;
            return changed;
        }

        void forget (const std::string& pluginId) { table.erase (pluginId); }

        /** What is known, or a default Status - unloaded, no sentence - for an
            entry nobody has looked at. */
        Status statusOf (const std::string& pluginId) const
        {
            const auto found = table.find (pluginId);
            return found != table.end() ? found->second : Status {};
        }

    private:
        std::map<std::string, Status> table;
    };
}
