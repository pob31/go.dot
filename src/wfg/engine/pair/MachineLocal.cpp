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

#include <wfg/engine/pair/MachineLocal.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/Schema.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::pair
{
    bool isMachineLocal (const Command& command, const std::vector<osc::Value>& args)
    {
        if (command.machineLocal)
            return true;

        if (command.name == "node.set")
            return ! args.empty() && args.front().isString()
                     && doc::Schema::isMachineAddress (args.front().getString());

        /*  EVERY ADDRESS, or it is the show's. Pairs, as the command takes them;
            an odd count is a message cut short, which the command refuses, and
            an empty set names no machine row. */
        if (command.name == "node.setMany")
        {
            if (args.empty() || args.size() % 2 != 0)
                return false;

            for (std::size_t at = 0; at < args.size(); at += 2)
                if (! args[at].isString() || ! doc::Schema::isMachineAddress (args[at].getString()))
                    return false;

            return true;
        }

        return false;
    }
}
