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
    }
}
