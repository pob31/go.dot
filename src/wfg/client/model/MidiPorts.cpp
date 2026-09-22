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

#include <wfg/client/model/MidiPorts.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view portPrefix = "/godot/port/";

        /*  One name per line, which is how the engine publishes a list whose
            members contain spaces. An empty reading is no devices, not one
            device with an empty name. */
        std::vector<std::string> lines (const std::string& text)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at <= text.size())
            {
                const auto end = text.find('\n', at);
                const auto piece = text.substr (at, end == std::string::npos
                                                      ? std::string::npos : end - at);

                if (! piece.empty())
                    out.push_back (piece);

                if (end == std::string::npos)
                    break;

                at = end + 1;
            }

            return out;
        }
    }

    std::string PortRow::label() const
    {
        return name.empty() ? id : name;
    }

    std::string PortRow::stateWord() const
    {
        /*  IN WORDS, NEVER COLOUR ALONE (§4.8), and the three states are
            genuinely different: bound is a cable in a socket, a problem is a
            cable somebody expected and this machine has not got, and neither
            is a port nobody has finished writing. */
        if (bound)
            return "bound";

        if (! problem.empty())
            return "not found";

        return outputDevice.empty() && inputDevice.empty() ? "no device chosen" : "unbound";
    }

    std::vector<PortRow> readPorts (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS, gathering by identifier, as `readDevices` and
            `readOutputs` do: `childrenOf` walks the whole tree per call and is
            banned for it. */
        std::map<std::string, PortRow> found;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (portPrefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (portPrefix.size());
            const auto slash = rest.find ('/');

            /*  `/godot/port/inputs` and `/godot/port/outputs` are the
                CONTAINER's own rows - what this machine has - and have no
                identifier in the middle. They are read by the two functions
                below, not here. */
            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);

            auto& row = found[id];
            row.id = id;

            if (name == "name")               row.name = text (node);
            else if (name == "outputDevice")  row.outputDevice = text (node);
            else if (name == "inputDevice")   row.inputDevice = text (node);
            else if (name == "rx")            row.rx = text (node) == "true";
            else if (name == "tx")            row.tx = text (node) == "true";
            else if (name == "bound")         row.bound = text (node) == "true";
            else if (name == "problem")       row.problem = text (node);
        }

        std::vector<PortRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
            rows.push_back (std::move (row));

        return rows;
    }

    std::vector<std::string> readMidiInputs (const tree::TreeSnapshot& snapshot)
    {
        return lines (text (snapshot, "/godot/port/inputs"));
    }

    std::vector<std::string> readMidiOutputs (const tree::TreeSnapshot& snapshot)
    {
        return lines (text (snapshot, "/godot/port/outputs"));
    }

    std::vector<std::pair<std::string, std::string>> portChoices (const std::vector<PortRow>& rows)
    {
        std::vector<std::pair<std::string, std::string>> choices;
        choices.reserve (rows.size() + 1);

        /*  EMPTY IS A CHOICE AND NOT AN ABSENCE, as it is for an output and
            for a network device, and first because that is where a hand goes
            looking for it. */
        choices.push_back ({ "", "(none)" });

        for (const auto& row : rows)
            choices.push_back ({ row.id, row.label() });

        return choices;
    }
}
