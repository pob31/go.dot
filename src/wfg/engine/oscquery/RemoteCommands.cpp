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

#include <wfg/engine/oscquery/RemoteCommands.h>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/oscquery/OscQueryServer.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::oscquery
{
    namespace
    {
        constexpr std::int64_t defaultSeconds = 30;
        constexpr std::int64_t longestSeconds = 300;
    }

    void registerRemoteCommands (CommandRegistry& registry, OscQueryServer* server)
    {
        registry.add (machineLocal ({ "remote.find",
                        "Asks a remote - a tablet running Go.dot's app - to show itself: its screen flashes, it"
                        " sounds and vibrates until touched or for the seconds given (thirty when none, five"
                        " minutes at most). The remote by the identifier it said when it connected, as"
                        " /godot/network/remotes lists it. Tonight's, never saved.",
                        { { "remote", 's', false }, { "seconds", 'i', true } },
                        false,
                        [server] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto& remote = args[0].getString();

                            if (remote.empty())
                                return Outcome::rejected (reason::badValue);

                            std::int64_t seconds = defaultSeconds;

                            if (args.size() > 1 && args[1].isNumber())
                                seconds = std::clamp<std::int64_t> (static_cast<std::int64_t> (args[1].asDouble()),
                                                                    1, longestSeconds);

                            /*  A remote not connected now is told nothing, and the
                                command still applies: it asked for a tablet that is
                                not there, which is a fact about tonight rather than
                                a fault in the asking - the Network tab says who is. */
                            if (server != nullptr)
                                server->find (remote, static_cast<int> (seconds));

                            return Outcome::ok ({ osc::Value::string (remote),
                                                  osc::Value::int32 (static_cast<std::int32_t> (seconds)) });
                        } }));
    }
}
