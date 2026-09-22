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

#include <wfg/client/model/Sends.h>

#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/send/";

        /** Silence, as this document spells it everywhere. */
        constexpr double silenceDb = -120.0;
    }

    std::vector<SendStrip> readSends (const tree::TreeSnapshot& snapshot,
                                      const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        /*  WHAT THIS CUE SENDS, gathered by the same route a range is: sends
            are published flat under their own owner with a derived `cue`, so
            the way back to the cue is that row and not containment. */
        struct Held { std::string id, bus; double level = 0.0; bool mine = false; };

        std::map<std::string, Held> held;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);
            const auto reading = text (node);

            auto& one = held[id];
            one.id = id;

            if (name == "cue")        one.mine = reading == cueId;
            else if (name == "bus")   one.bus = reading;
            else if (name == "level") one.level = osc::parseDouble (reading).value_or (0.0);
        }

        std::map<std::string, Held> byBus;

        for (auto& [id, one] : held)
            if (one.mine && ! one.bus.empty())
                byBus[one.bus] = one;

        /*  AND A STRIP FOR EVERY MIX CHANNEL THE SHOW HAS, whether this cue
            feeds it or not: the desk is what the rig declares, and what is up
            is what somebody pushed. In output-list order, so the mixer reads
            left to right the way the Outputs tab reads top to bottom. */
        std::vector<SendStrip> out;

        for (const auto& row : readOutputs (snapshot))
        {
            if (row.kind != "mix")
                continue;

            SendStrip strip;
            strip.busId = row.id;
            strip.name = row.name.empty() ? row.id : row.name;
            strip.widthWord = row.widthWord();
            strip.channelWord = row.channelWord();

            if (const auto found = byBus.find (row.id); found != byBus.end())
            {
                strip.sendId = found->second.id;
                strip.levelDb = found->second.level;
            }
            else
            {
                /*  NO SEND IS DRAWN AT SILENCE, which is what it sounds like.
                    The strip is still there to be raised, and raising it is
                    what makes the object. */
                strip.levelDb = silenceDb;
            }

            out.push_back (std::move (strip));
        }

        return out;
    }
}
