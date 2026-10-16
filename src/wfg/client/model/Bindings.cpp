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

#include <wfg/client/model/Bindings.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

namespace wfg::client::model
{
    namespace
    {
        const std::string dot { "\xc2\xb7" };

        int integerAt (const tree::TreeSnapshot& snapshot, const std::string& address, int otherwise)
        {
            const auto number = osc::parseDouble (text (snapshot, address));
            return number.has_value() ? static_cast<int> (*number) : otherwise;
        }

        //  A cue as a row names it: its number, then what it is called.
        std::string cueWords (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto number = text (snapshot, "/godot/cue/" + cueId + "/number");
            const auto name = shownCueName (snapshot, cueId);
            const auto called = name.empty() ? cueId : name;
            return number.empty() ? called : number + " " + called;
        }
    }

    std::vector<BindingRow> readBindings (const tree::TreeSnapshot& snapshot)
    {
        std::vector<BindingRow> rows;

        for (const auto& id : words (text (snapshot, "/godot/binding/order")))
        {
            const auto base = "/godot/binding/" + id + "/";
            const auto row = [&snapshot, &base] (const char* name) { return text (snapshot, base + name); };

            BindingRow binding;
            binding.id = id;
            binding.name = row ("name");
            binding.cue = row ("cue");
            binding.enabled = row ("enabled") != "false";
            binding.source = row ("source") == "osc" ? "osc" : "midi";
            binding.port = row ("port");
            binding.channel = integerAt (snapshot, base + "channel", 0);
            binding.type = row ("type").empty() ? std::string ("controlChange") : row ("type");
            binding.number = integerAt (snapshot, base + "number", 0);
            binding.address = row ("address");
            binding.argument = integerAt (snapshot, base + "argument", 0);
            binding.control = row ("control").empty() ? std::string ("absolute") : row ("control");
            binding.target = row ("target");
            binding.pickup = row ("pickup") != "false";
            binding.feedback = row ("feedback") == "true";
            rows.push_back (std::move (binding));
        }

        return rows;
    }

    std::string movedByWords (const BindingRow& row, const std::vector<PortRow>& ports)
    {
        if (row.source == "osc")
        {
            if (row.address.empty())
                return "nothing yet";

            return "OSC " + row.address + (row.argument > 0 ? " " + dot + " argument " + std::to_string (row.argument + 1)
                                                            : std::string {});
        }

        auto port = std::string ("any port");

        if (! row.port.empty())
        {
            port = row.port;

            for (const auto& each : ports)
                if (each.id == row.port && ! each.name.empty())
                    port = each.name;
        }

        std::string what;

        if (row.type == "controlChange14")
            what = "CC " + std::to_string (row.number) + "+" + std::to_string (row.number + 32) + " (14 bits)";
        else if (row.type == "noteVelocity")
            what = "note " + std::to_string (row.number);
        else if (row.type == "pitchBend")
            what = "pitch bend";
        else if (row.type == "channelPressure")
            what = "pressure";
        else
            what = "CC " + std::to_string (row.number);

        return port + " " + dot + " " + (row.channel > 0 ? "ch " + std::to_string (row.channel) + " " + dot + " " : std::string {})
                 + what;
    }

    std::string howWords (const std::string& control)
    {
        if (control == "relative") return "Relative";
        if (control == "rate")     return "Rate";
        return "Absolute";
    }

    std::string nextControl (const std::string& control)
    {
        if (control == "absolute") return "relative";
        if (control == "relative") return "rate";
        return "absolute";
    }

    std::string targetAddressOf (const BindingRow& row)
    {
        if (row.target.empty())
            return {};

        if (row.target.front() == '/')
            return row.target;

        if (row.cue.empty() || row.target.find ('/') != std::string::npos)
            return {};

        return "/godot/cue/" + row.cue + "/" + row.target;
    }

    std::string movesWords (const BindingRow& row, const tree::TreeSnapshot& snapshot)
    {
        if (row.target.empty())
            return "nothing yet";

        //  A row of its own cue, by name: what copies with the cue.
        if (row.target.front() != '/')
            return row.cue.empty() ? row.target : "its " + row.target;

        const std::string cuePrefix { "/godot/cue/" };

        if (row.target.rfind (cuePrefix, 0) == 0)
        {
            const auto rest = row.target.substr (cuePrefix.size());
            const auto slash = rest.find ('/');

            if (slash != std::string::npos)
                return cueWords (snapshot, rest.substr (0, slash)) + " " + dot + " " + rest.substr (slash + 1);
        }

        return row.target;
    }

    std::string liveWords (const BindingRow& row, const tree::TreeSnapshot& snapshot)
    {
        return row.cue.empty() ? std::string ("all show") : "while " + cueWords (snapshot, row.cue) + " runs";
    }

    std::vector<BindingRow> bindingsOn (const std::vector<BindingRow>& rows, const std::string& address)
    {
        std::vector<BindingRow> on;

        for (const auto& row : rows)
            if (! address.empty() && targetAddressOf (row) == address)
                on.push_back (row);

        return on;
    }

    std::string bindingLearning (const tree::TreeSnapshot& snapshot)
    {
        return text (snapshot, "/godot/engine/bindingLearn");
    }
}
