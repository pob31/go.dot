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

#include <wfg/client/model/Devices.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view devicePrefix = "/godot/mount/";

        /*  `reading` and not `text`, which is the free function this file also
            calls: GCC's -Wshadow reports a parameter shadowing a global
            declaration, and the repo's convention is to rename the parameter
            rather than the thing it hid. */
        int number (const std::string& reading, int fallback)
        {
            try
            {
                std::size_t used = 0;
                const auto value = std::stoi (reading, &used);
                return used == 0 ? fallback : value;
            }
            catch (...)
            {
                return fallback;
            }
        }

        /*  Whether an address falls under a prefix, with the boundary the
            engine uses: "/desk/fader" is under "/desk" and "/desktop/fader" is
            not. Written out rather than a `rfind (prefix, 0) == 0`, which is
            the mistake this guards against. */
        bool under (const std::string& address, const std::string& prefix)
        {
            return prefix.size() < address.size()
                     && address.compare (0, prefix.size(), prefix) == 0
                     && address[prefix.size()] == '/';
        }
    }

    std::string DeviceRow::label() const
    {
        return name.empty() ? prefix : name;
    }

    std::vector<DeviceRow> readDevices (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS over the tree, gathering by identifier, the way
            `readOutputs` and `inspect` do: `childrenOf` walks the whole tree
            per call and is banned for it. */
        std::map<std::string, DeviceRow> found;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (devicePrefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (devicePrefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);

            /*  A mount's SLOTS are published under their own owner, not under
                this one, so anything with a second slash here is not a row of
                the device itself and is passed over. */
            if (name.find ('/') != std::string::npos)
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")            row.name = text (node);
            else if (name == "prefix")     row.prefix = text (node);
            else if (name == "host")       row.host = text (node);
            else if (name == "port")       row.port = number (text (node), 0);
            else if (name == "rx")         row.rx = text (node) == "true";
            else if (name == "tx")         row.tx = text (node) == "true";
            else if (name == "namespace")  row.namespaceFile = text (node);
            else if (name == "sent")       row.sent = number (text (node), 0);
            else if (name == "problem")    row.problem = text (node);
        }

        std::vector<DeviceRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
            rows.push_back (std::move (row));

        return rows;
    }

    std::string deviceOf (const std::string& address, const std::vector<DeviceRow>& rows)
    {
        std::string bestId;
        std::size_t bestLength = 0;

        for (const auto& row : rows)
        {
            if (row.prefix.empty() || ! under (address, row.prefix))
                continue;

            /*  THE LONGEST WINS. Two devices may nest - "/desk" and
                "/desk/aux" are both legal prefixes - and the cue belongs to the
                more specific one, which is the same rule a filesystem uses for
                a mount point and the one `MountTable::mountOf` follows. */
            if (row.prefix.size() > bestLength)
            {
                bestLength = row.prefix.size();
                bestId = row.id;
            }
        }

        return bestId;
    }

    std::string retarget (const std::string& address, const std::vector<DeviceRow>& rows,
                          const std::string& deviceId)
    {
        /*  What is under the current device's prefix, which is what the new
            one will carry. An address aimed at nothing is already that. */
        const auto currentId = deviceOf (address, rows);
        std::string tail = address;

        for (const auto& row : rows)
            if (row.id == currentId)
                tail = address.substr (row.prefix.size());

        /*  AIMED AT NOTHING, deliberately. It leaves an address the engine
            will refuse when the cue fires, and that is the honest inverse of
            putting a prefix on: somebody taking a cue off a device has to be
            able to say so, and `wfg validate` is what says it out loud
            afterwards. Silently keeping the old prefix would be a menu that
            lies about what it did. */
        if (deviceId.empty())
            return tail;

        for (const auto& row : rows)
            if (row.id == deviceId)
                return row.prefix + tail;

        /*  A device that is not there any more. The address is left exactly as
            it was rather than half rewritten: a menu that cannot find what it
            was asked for should change nothing. */
        return address;
    }

    std::vector<std::pair<std::string, std::string>>
        targetChoices (const std::string& address, const std::vector<DeviceRow>& rows)
    {
        std::vector<std::pair<std::string, std::string>> choices;
        choices.reserve (rows.size() + 1);

        /*  EMPTY IS A CHOICE AND NOT AN ABSENCE, the same rule the output menu
            follows, and first so it is where a hand goes looking for it. */
        choices.push_back ({ retarget (address, rows, {}), "(none)" });

        for (const auto& row : rows)
        {
            if (row.prefix.empty())
                continue;

            choices.push_back ({ retarget (address, rows, row.id), row.label() });
        }

        return choices;
    }
}
