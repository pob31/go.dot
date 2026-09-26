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
#include <vector>

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

            /*  THE BUSES IT TOOK (2026-09-26): its main input and output
                widths, and the two in words - "stereo in, stereo out". A cue
                wider than `inputs`, or one it would make narrower, passes it
                dry. Nought before the child has said. */
            int inputs = 0;
            int outputs = 0;
            std::string layout;
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
                                   || held.inputs != status.inputs || held.outputs != status.outputs
                                   || held.layout != status.layout
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

        /*  THE ENTRIES THE AUDIO GRAPH WAS BUILT WITH, in slot order
            (2026-09-26). The graph is fixed when it is built (PRD §3.25) and
            the set can change after: an entry added since has no slot, and
            one taken out or moved ahead of another has moved every slot after
            it. A cue's inserts are sent by slot, so the slots are read from
            here - never counted off the document as it stands now, which is
            how a set edited mid-session came to send one plugin's settings to
            another. Written by the audio host when it builds and cleared when
            it stops; no graph at all is `hasGraph` false. */
        void setBuilt (std::vector<std::string> ids)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            builtIds = std::move (ids);
            graphBuilt = true;
            ++revisionCount;
        }

        void clearBuilt()
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (! graphBuilt && builtIds.empty())
                return;

            builtIds.clear();
            graphBuilt = false;
            ++revisionCount;
        }

        bool hasGraph() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return graphBuilt;
        }

        /** The entry's slot in the graph, or -1 for one the graph was built without. */
        int builtSlotOf (const std::string& pluginId) const
        {
            const std::lock_guard<std::mutex> lock { mutex };

            for (std::size_t slot = 0; slot < builtIds.size(); ++slot)
                if (builtIds[slot] == pluginId)
                    return static_cast<int> (slot);

            return -1;
        }

        std::vector<std::string> built() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return builtIds;
        }

    private:
        mutable std::mutex mutex;
        std::map<std::string, Status> table;
        std::uint64_t revisionCount = 0;
        std::vector<std::string> builtIds;
        bool graphBuilt = false;
    };
}
