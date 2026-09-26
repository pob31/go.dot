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

#include <wfg/engine/plugin/PluginCommands.h>

#include <memory>

namespace wfg::plugin
{
    std::function<bool (const std::string& pluginId)> pluginKnownBy (const doc::ShowDocument& document)
    {
        return [&document] (const std::string& id)
        {
            const auto plugins = document.root().getChildWithName ("Audio").getChildWithName ("Plugins");

            for (const auto& entry : plugins)
                if (entry.hasType ("Plugin") && entry.getProperty ("id").toString().toStdString() == id)
                    return true;

            return false;
        };
    }

    std::function<bool()> showLockedBy (const doc::ShowDocument& document)
    {
        return [&document] { return document.isLocked(); };
    }

    void registerPluginCommands (CommandRegistry& registry, PluginTable& table, PluginCommandHooks hooks)
    {
        const auto known = [&table, knows = hooks.knows] (const std::string& id)
        {
            return knows ? knows (id) : table.holds (id);
        };

        registry.add ({ "plugin.failed",
                        "The plugin's child died or stopped answering; every voice plays dry"
                        " through it until it is restarted.",
                        { { "id", 's', false }, { "problem", 's', false } },
                        true,
                        [&table, known] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            if (! known (id))
                                return Outcome::rejected (reason::unknownId);

                            /*  What the table already holds is kept - the
                                latency and the count are facts about the
                                plugin, not about tonight's failure. */
                            auto status = table.statusOf (id);
                            status.state = "failed";
                            status.problem = args[1].getString();
                            table.set (id, status);

                            return Outcome::ok (args);
                        } });

        registry.add ({ "plugin.restart",
                        "A fresh child for the plugin, from any state.",
                        { { "id", 's', false } },
                        true,
                        [known, restart = hooks.restart] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            if (! known (id))
                                return Outcome::rejected (reason::unknownId);

                            if (restart)
                            {
                                std::string problem;

                                if (! restart (id, problem))
                                    return Outcome::rejected (problem.empty() ? "not-restartable" : problem);
                            }

                            return Outcome::ok (args);
                        } });

        /*  THE SCAN'S STATE, the caller's or one of the commands' own - held
            by a shared pointer so the lambdas below can share it whichever. */
        std::shared_ptr<ScanTable> ownScans;
        auto* scans = hooks.scans;

        if (scans == nullptr)
        {
            ownScans = std::make_shared<ScanTable>();
            scans = ownScans.get();
        }

        const auto refusal = [locked = hooks.locked, scans] () -> std::string
        {
            if (locked && locked())
                return reason::locked;

            if (scans->scanning())
                return "scan-running";

            return {};
        };

        registry.add ({ "plugin.scan",
                        "Scans this machine for plugins, out of process: every format, or vst3, au or lv2,"
                        " and a folder to search too. Refused while the show is locked and while a scan runs.",
                        { { "format", 's', true }, { "folder", 's', true } },
                        true,
                        [refusal, scans, ownScans, scan = hooks.scan] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            (void) ownScans;   // held, so the table outlives the registration
                            const auto word = args.empty() ? std::string {} : args[0].getString();

                            if (! word.empty() && word != "vst3" && word != "au" && word != "lv2")
                                return Outcome::rejected (reason::badValue);

                            if (const auto why = refusal(); ! why.empty())
                                return Outcome::rejected (why);

                            scans->begin (word);

                            if (scan)
                                scan (word, {}, args.size() > 1 ? args[1].getString() : std::string {});

                            return Outcome::ok (args);
                        } });

        registry.add ({ "plugin.scanRetry",
                        "Scans one file an earlier scan gave up on, alone, taking it off the skip list."
                        " Refused while the show is locked and while a scan runs.",
                        { { "file", 's', false } },
                        true,
                        [refusal, scans, ownScans, scan = hooks.scan] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            (void) ownScans;   // held, so the table outlives the registration
                            const auto file = args[0].getString();

                            if (file.empty())
                                return Outcome::rejected (reason::badValue);

                            if (const auto why = refusal(); ! why.empty())
                                return Outcome::rejected (why);

                            scans->begin ({});

                            if (scan)
                                scan ({}, file, {});

                            return Outcome::ok (args);
                        } });

        registry.add ({ "plugin.scanned",
                        "The scan's child has gone: what the machine knows, how many files it gave up on,"
                        " and why it failed, empty when it did not.",
                        { { "found", 'i', false }, { "skipped", 'i', false }, { "problem", 's', false } },
                        true,
                        [scans, ownScans] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            (void) ownScans;
                            scans->end (args[0].getInt32(), args[1].getInt32(), args[2].getString());
                            return Outcome::ok (args);
                        } });
    }
}
