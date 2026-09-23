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
    What the sandbox reports and what an operator asks of it, as commands like
    any other (Phase 9a, §17.3) - AudioCommands.h's rule: a state transition is
    an event, logged and replayable.

    `plugin.failed <id:s> <problem:s>` - the child died or stopped answering,
    and every voice plays dry through it. Submitted ONCE per failure by the
    proxy host (never per miss - the queue is finite), applied on the tick it
    was observed; a replay re-injects it and the table reads `failed` with the
    same sentence on a machine with no plugin and no child. Idempotent.

    `plugin.restart <id:s>` - a fresh child for the entry, from any state. The
    hook is the proxy host's; a replay has none and applies it as a no-op,
    which is the right answer for a log of a performance.

    `unknown-id` for an entry no proxy host holds and no table knows.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/plugin/PluginTable.h>

#include <functional>
#include <string>

namespace wfg::plugin
{
    struct PluginCommandHooks
    {
        /** `plugin.restart`: false with a sentence when it could not. Absent
            in a replay. */
        std::function<bool (const std::string& pluginId, std::string& problem)> restart;

        /** Whether an id names an entry of the show's set, for `unknown-id`.
            Absent, the table alone decides. */
        std::function<bool (const std::string& pluginId)> knows;
    };

    void registerPluginCommands (CommandRegistry& registry, PluginTable& table, PluginCommandHooks hooks);
}
