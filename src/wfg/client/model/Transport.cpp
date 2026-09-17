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

#include <wfg/client/model/Transport.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstdint>
#include <string>
#include <tuple>

namespace wfg::client::model
{
    std::string TransportReading::standbyLine() const
    {
        if (standbyId.empty())
            return "no standby";

        if (standbyKind.empty())
            return standbyName;

        return standbyName + "  " + standbyKind;
    }

    bool TransportReading::operator== (const TransportReading& other) const noexcept
    {
        const auto tie = [] (const TransportReading& r)
        {
            return std::tie (r.show, r.dirty, r.locked, r.tick, r.clock, r.rate,
                             r.listId, r.listName, r.standbyId, r.standbyName, r.standbyKind,
                             r.status, r.lastError, r.revision);
        };

        return tie (*this) == tie (other);
    }

    TransportReading readTransport (const tree::TreeSnapshot& snapshot)
    {
        TransportReading reading;

        reading.show = text (snapshot, "/godot/document/name");
        reading.dirty = flag (snapshot, "/godot/document/dirty");
        reading.locked = flag (snapshot, "/godot/document/locked");

        reading.tick = text (snapshot, "/godot/engine/tick");
        reading.clock = text (snapshot, "/godot/engine/clock");

        const auto rate = text (snapshot, "/godot/engine/sampleRate");
        const auto block = text (snapshot, "/godot/engine/blockSize");
        reading.rate = (rate.empty() || rate == "0") ? std::string() : rate + " / " + block;

        /*  THE FOCUSED LIST, WITH THE PAGE'S FALLBACK (strip.js:223): the
            engine's `focus` when it names one, else the first list there is,
            else nothing - a show with no list has no standby to show. */
        reading.listId = text (snapshot, "/godot/list/focus");

        if (reading.listId.empty())
            if (const auto lists = words (text (snapshot, "/godot/list/order")); ! lists.empty())
                reading.listId = lists.front();

        if (! reading.listId.empty())
        {
            reading.listName = text (snapshot, "/godot/list/" + reading.listId + "/name");
            reading.standbyId = text (snapshot, "/godot/list/" + reading.listId + "/standby");
        }

        if (! reading.standbyId.empty())
        {
            reading.standbyName = text (snapshot, "/godot/cue/" + reading.standbyId + "/name");
            reading.standbyKind = text (snapshot, "/godot/cue/" + reading.standbyId + "/kind");
        }

        reading.status = text (snapshot, "/godot/audio/status");
        reading.lastError = text (snapshot, "/godot/engine/lastError");

        if (const auto* node = snapshot.find ("/godot/document/revision"))
            if (const auto sole = node->soleValue(); sole.has_value() && sole->isInt64())
                reading.revision = static_cast<std::uint64_t> (sole->getInt64());

        return reading;
    }
}
