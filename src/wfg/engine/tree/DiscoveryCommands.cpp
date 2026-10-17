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

#include <wfg/engine/tree/DiscoveryCommands.h>

#include <wfg/engine/command/Command.h>

#include <utility>
#include <vector>

namespace wfg::tree
{
    void registerDiscoveryCommands (CommandRegistry& registry, doc::ShowDocument&, MountTable&,
                                    const juce::File&, DiscoveryHooks hooks)
    {
        /*  THE NETWORK ASKED NOW (namespace draft §76, ARW): what answers is
            `network/found`, filled by the discovery thread; nothing here
            waits for it. */
        registry.add (machineLocal ({ "network.browse",
                        "Asks the network now for the OSCQuery servers announced on it (namespace draft 76):"
                        " what answers is listed in network/found. Taken and ignored where nothing asks.",
                        {},
                        false,
                        [ask = std::move (hooks.browse)] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (ask)
                                ask();

                            return Outcome::ok (args);
                        } }));
    }
}
