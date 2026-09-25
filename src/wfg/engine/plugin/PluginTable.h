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

    THREADING: a mutex of its own, because two threads write it and a third
    reads. The message thread fills it (the proxy host, from its poll), the
    tick thread writes it too (a `plugin.failed` record, live or replayed) and
    reads it (the tree, when it rebuilds its document half). Nothing here is on
    the audio thread. A REVISION moves with every change a reader could see,
    and the tree compares it at every publish - the mount table's idiom - so
    a change made on the message thread reaches a client without anyone
    having to remember to mark the tree stale from a thread that must not.
*/

#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
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

            /*  How long the last cue's whole state took to load onto a voice,
                in milliseconds, and why the last one could not (author's
                decision of 2026-09-25: the whole state per cue). */
            double stateLoadMs = 0.0;
            std::string stateProblem;
        };

        /** Replaces what is known about one entry. Answers whether anything a
            reader could see changed, so the caller knows whether to ask the
            tree for a rebuild. */
        bool set (const std::string& pluginId, const Status& status)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            auto& held = table[pluginId];

            const auto changed = held.state != status.state
                                   || held.problem != status.problem
                                   || held.latencySamples != status.latencySamples
                                   || held.paramCount != status.paramCount
                                   || held.stateProblem != status.stateProblem
                                   || std::abs (held.stateLoadMs - status.stateLoadMs) > 1.0e-9;
            held = status;

            if (changed)
                ++revisionCount;

            return changed;
        }

        void forget (const std::string& pluginId)
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (table.erase (pluginId) > 0)
                ++revisionCount;
        }

        /** What is known, or a default Status - unloaded, no sentence - for an
            entry nobody has looked at. */
        Status statusOf (const std::string& pluginId) const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            const auto found = table.find (pluginId);
            return found != table.end() ? found->second : Status {};
        }

        /** Whether anything was ever written for the id. */
        bool holds (const std::string& pluginId) const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return table.count (pluginId) > 0;
        }

        /** Moves with every change a reader could see. */
        std::uint64_t revision() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return revisionCount;
        }

    private:
        mutable std::mutex mutex;
        std::map<std::string, Status> table;
        std::uint64_t revisionCount = 0;
    };
}
