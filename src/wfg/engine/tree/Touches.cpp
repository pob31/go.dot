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

#include <wfg/engine/tree/Touches.h>

namespace wfg::tree
{
    bool TouchTable::touch (const std::string& origin, const std::string& address)
    {
        if (origin.empty() || address.empty())
            return false;

        return byOrigin[origin].insert (address).second;
    }

    bool TouchTable::release (const std::string& origin, const std::string& address)
    {
        const auto it = byOrigin.find (origin);

        if (it == byOrigin.end())
            return false;

        const auto removed = it->second.erase (address) > 0;

        /*  An origin holding nothing is removed rather than left as an empty
            set. Otherwise a long-running server would accumulate one entry per
            connection it ever had, and holdersOf would walk them all. */
        if (it->second.empty())
            byOrigin.erase (it);

        return removed;
    }

    std::vector<std::string> TouchTable::releaseAll (const std::string& origin)
    {
        const auto it = byOrigin.find (origin);

        if (it == byOrigin.end())
            return {};

        std::vector<std::string> released { it->second.begin(), it->second.end() };
        byOrigin.erase (it);

        return released;
    }

    bool TouchTable::isHeld (const std::string& origin, const std::string& address) const
    {
        const auto it = byOrigin.find (origin);
        return it != byOrigin.end() && it->second.count (address) > 0;
    }

    std::vector<std::string> TouchTable::holdersOf (const std::string& address) const
    {
        std::vector<std::string> origins;

        for (const auto& [origin, held] : byOrigin)
            if (held.count (address) > 0)
                origins.push_back (origin);

        return origins;
    }

    std::vector<std::string> TouchTable::heldBy (const std::string& origin) const
    {
        const auto it = byOrigin.find (origin);

        if (it == byOrigin.end())
            return {};

        return { it->second.begin(), it->second.end() };
    }

    std::size_t TouchTable::size() const noexcept
    {
        std::size_t total = 0;

        for (const auto& entry : byOrigin)
            total += entry.second.size();

        return total;
    }

    //==============================================================================
    bool shouldPush (const TouchTable& touches,
                     const std::string& toOrigin,
                     const std::string& address,
                     const std::string& causedBy)
    {
        /*  Echo suppression first, because it applies whether or not anything
            is touched: an origin is never told about a change it made itself.
            An empty causedBy means the engine changed it on its own - a cue
            fired, a curve moved - and there is nobody to suppress. */
        if (! causedBy.empty() && toOrigin == causedBy)
            return false;

        return ! touches.isHeld (toOrigin, address);
    }
}
