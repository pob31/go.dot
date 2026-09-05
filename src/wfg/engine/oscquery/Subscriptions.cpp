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

#include <wfg/engine/oscquery/Subscriptions.h>

namespace wfg::oscquery
{
    bool Subscriptions::listen (const ConnectionId& connection, const std::string& address)
    {
        const std::lock_guard<std::mutex> lock { mutex };
        return byConnection[connection].insert (address).second;
    }

    bool Subscriptions::ignore (const ConnectionId& connection, const std::string& address)
    {
        const std::lock_guard<std::mutex> lock { mutex };

        const auto entry = byConnection.find (connection);

        if (entry == byConnection.end())
            return false;

        const auto erased = entry->second.erase (address) > 0;

        /*  A connection listening to nothing is removed rather than left as an
            empty set. connectionCount() is a diagnostic an operator reads, and
            "4 connections" when three of them are listening to nothing is a
            number that invites the wrong conclusion. */
        if (entry->second.empty())
            byConnection.erase (entry);

        return erased;
    }

    std::vector<ConnectionId> Subscriptions::listenersOf (const std::string& address) const
    {
        const std::lock_guard<std::mutex> lock { mutex };

        std::vector<ConnectionId> out;

        /*  Walked rather than looked up in a second address-keyed index. The
            table is one entry per open connection - a handful, not thousands -
            and a second index would be a second thing to keep in step for a
            saving nobody would measure. Phase 6, with real surfaces on it, is
            where that becomes worth revisiting. */
        for (const auto& [connection, addresses] : byConnection)
            if (addresses.find (address) != addresses.end())
                out.push_back (connection);

        return out;
    }

    std::vector<std::string> Subscriptions::heldBy (const ConnectionId& connection) const
    {
        const std::lock_guard<std::mutex> lock { mutex };

        const auto entry = byConnection.find (connection);

        if (entry == byConnection.end())
            return {};

        return { entry->second.begin(), entry->second.end() };
    }

    std::vector<std::string> Subscriptions::drop (const ConnectionId& connection)
    {
        const std::lock_guard<std::mutex> lock { mutex };

        const auto entry = byConnection.find (connection);

        if (entry == byConnection.end())
            return {};

        std::vector<std::string> had { entry->second.begin(), entry->second.end() };
        byConnection.erase (entry);
        return had;
    }

    std::size_t Subscriptions::connectionCount() const
    {
        const std::lock_guard<std::mutex> lock { mutex };
        return byConnection.size();
    }

    std::size_t Subscriptions::totalSubscriptions() const
    {
        const std::lock_guard<std::mutex> lock { mutex };

        std::size_t total = 0;

        for (const auto& entry : byConnection)
            total += entry.second.size();

        return total;
    }

    bool Subscriptions::isListening (const ConnectionId& connection,
                                     const std::string& address) const
    {
        const std::lock_guard<std::mutex> lock { mutex };

        const auto entry = byConnection.find (connection);
        return entry != byConnection.end()
                 && entry->second.find (address) != entry->second.end();
    }
}
