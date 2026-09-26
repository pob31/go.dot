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

    THE SCAN (2026-09-26, the author's decision: a Scan button in the app).
    `plugin.scan [format:s] [folder:s]` begins one - every format, or `vst3` /
    `au` / `lv2`, and a folder to search beside the format's own (an LV2 folder
    that is not a default one, say) - and `plugin.scanRetry <file:s>` scans one
    skipped file again,
    alone. Both are refused `locked` while the show is locked and
    `scan-running` while a scan is under way; that a scan is under way is
    the commands' own state (ScanTable.h), set by the one and cleared by
    `plugin.scanned <found:i> <skipped:i> <problem:s>`, which the engine
    submits when the scan's child has gone - so a replay, which launches
    nothing, refuses exactly where the session did. The launch is a hook, as
    the restart is; a replay has none.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/ScanTable.h>

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

        /** Whether the show is locked, for the scan's refusal - read off the
            document, which a replay has as the session had it. Absent, never. */
        std::function<bool()> locked;

        /** `plugin.scan` / `plugin.scanRetry` applied: launch the scan (the
            format word, or empty; one file to retry, or empty; a folder to
            search too, or empty). Called on the tick thread and expected to
            hand the work to the message thread. Absent in a replay. */
        std::function<void (const std::string& formatWord, const std::string& retryFile,
                            const std::string& folder)> scan;

        /** Where the scan's state is kept. Absent, the commands keep one of
            their own - a replay's or a listing's, which nobody reads. */
        ScanTable* scans = nullptr;
    };

    void registerPluginCommands (CommandRegistry& registry, PluginTable& table, PluginCommandHooks hooks);

    /*  The `locked` hook every session should use, read off the document; the
        document must outlive the registry. */
    std::function<bool()> showLockedBy (const doc::ShowDocument& document);

    /*  The `knows` hook every session should use: whether the id names an
        entry of the show's set, read off the document - which a replay has
        as the live session had it, while a table it never filled says
        nothing. The document must outlive the registry. */
    std::function<bool (const std::string& pluginId)> pluginKnownBy (const doc::ShowDocument& document);
}
