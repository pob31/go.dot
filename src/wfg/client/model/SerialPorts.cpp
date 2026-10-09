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

#include <wfg/client/model/SerialPorts.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <map>
#include <string_view>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view serialPrefix = "/godot/serial/";
    }

    std::string SerialRow::stateWords() const
    {
        if (path.empty())
            return "no port chosen";
        if (state == "open")
            return "open";
        if (state == "opening")
            return "opening";
        if (state == "retrying")
            return problem.empty() ? std::string ("trying again") : "trying again - " + problem;
        return "closed";
    }

    std::vector<SerialRow> readSerialPorts (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS, gathering by identifier, as readPorts does. */
        std::map<std::string, SerialRow> found;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (serialPrefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (serialPrefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);

            auto& row = found[id];
            row.id = id;

            if (name == "name")           row.name = text (node);
            else if (name == "path")      row.path = text (node);
            else if (name == "baud")      row.baud = static_cast<int> (osc::parseDouble (text (node)).value_or (115200.0));
            else if (name == "framing")   row.framing = text (node);
            else if (name == "rx")        row.rx = text (node) != "false";
            else if (name == "tx")        row.tx = text (node) != "false";
            else if (name == "state")     row.state = text (node);
            else if (name == "problem")   row.problem = text (node);
            else if (name == "lastLine")  row.lastLine = text (node);
        }

        std::vector<SerialRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
            rows.push_back (std::move (row));

        return rows;
    }

    std::vector<SystemSerialPort> readSystemSerialPorts (const tree::TreeSnapshot& snapshot)
    {
        std::vector<SystemSerialPort> out;
        const auto all = text (snapshot, "/godot/engine/serialPorts");
        std::size_t at = 0;

        while (at < all.size())
        {
            auto end = all.find ('\n', at);
            if (end == std::string::npos)
                end = all.size();

            const auto line = all.substr (at, end - at);
            if (! line.empty())
            {
                const auto tab = line.find ('\t');
                out.push_back ({ line.substr (0, tab), tab == std::string::npos ? std::string {} : line.substr (tab + 1) });
            }
            at = end + 1;
        }

        return out;
    }

    const std::vector<int>& serialSpeeds()
    {
        static const std::vector<int> speeds { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
                                               1200, 2400, 4800 };
        return speeds;
    }
}
