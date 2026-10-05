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

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/send/";
    }

    std::size_t SendStrip::having() const noexcept
    {
        return static_cast<std::size_t> (std::count_if (each.begin(), each.end(),
                                                        [] (const SendShare& share) { return share.present(); }));
    }

    std::size_t SendStrip::onCount() const noexcept
    {
        return static_cast<std::size_t> (std::count_if (each.begin(), each.end(),
                                                        [] (const SendShare& share) { return share.present() && share.on; }));
    }

    bool SendStrip::mixed() const noexcept
    {
        /*  A HUNDREDTH APART IS APART: every level here is written to the
            tenth, so two that differ by less were written as one. */
        return loudestDb - lowestDb > 0.005;
    }

    std::vector<SendStrip> readSends (const tree::TreeSnapshot& snapshot,
                                      const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        auto strips = readSendsMany (snapshot, { cueId });

        //  One cue is no spread: `each` is for several.
        for (auto& strip : strips)
            strip.each.clear();

        return strips;
    }

    std::vector<SendStrip> readSendsMany (const tree::TreeSnapshot& snapshot,
                                          const std::vector<std::string>& cueIds)
    {
        if (cueIds.empty() || cueIds.front().empty())
            return {};

        /*  WHAT EVERY SEND IS, gathered by the same route a range is: sends
            are published flat under their own owner with a derived `cue`, so
            the way back to the cue is that row and not containment. Once for
            the tree, whichever cues are asked about. */
        struct Held { std::string id, cue, bus; double level = 0.0; bool on = true; bool live = false; };

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

            if (name == "cue")        one.cue = reading;
            else if (name == "bus")   one.bus = reading;
            else if (name == "level") one.level = osc::parseDouble (reading).value_or (0.0);
            else if (name == "on")    one.on = reading != "false";
            else if (name == "live")  one.live = reading == "true";
        }

        //  Each asked-about cue's sends, by the mix they go into.
        std::map<std::pair<std::string, std::string>, Held> byCueAndBus;

        for (auto& [id, one] : held)
            if (! one.bus.empty() && std::find (cueIds.begin(), cueIds.end(), one.cue) != cueIds.end())
                byCueAndBus[{ one.cue, one.bus }] = one;

        /*  AND A STRIP FOR EVERY MIX CHANNEL THE SHOW HAS, whether this cue
            feeds it or not: the desk is what the rig declares, and what is up
            is what somebody pushed. In output-list order, so the mixer reads
            left to right the way the Outputs tab reads top to bottom. */
        std::vector<SendStrip> out;
        const auto& lead = cueIds.front();

        for (const auto& row : readOutputs (snapshot))
        {
            if (row.kind != "mix")
                continue;

            SendStrip strip;
            strip.busId = row.id;
            strip.name = row.name.empty() ? row.id : row.name;
            strip.widthWord = row.widthWord();
            strip.channelWord = row.channelWord();

            if (const auto found = byCueAndBus.find ({ lead, row.id }); found != byCueAndBus.end())
            {
                strip.sendId = found->second.id;
                strip.levelDb = found->second.level;
                strip.on = found->second.on;
                strip.live = found->second.live;
            }
            else
            {
                /*  NO SEND IS DRAWN AT SILENCE, which is what it sounds like.
                    The strip is still there to be raised, and raising it is
                    what makes the object. */
                strip.levelDb = silenceDb;
            }

            /*  EVERY CUE'S PART OF IT, lead first, and the spread of the ones
                that send here (namespace draft §30.11). A cue that rides it
                live makes the strip say "live", as the lead's does. */
            auto any = false;

            for (const auto& cueId : cueIds)
            {
                SendShare share;
                share.cueId = cueId;

                if (const auto found = byCueAndBus.find ({ cueId, row.id }); found != byCueAndBus.end())
                {
                    share.sendId = found->second.id;
                    share.levelDb = found->second.level;
                    share.on = found->second.on;
                    strip.live = strip.live || found->second.live;

                    strip.lowestDb = any ? std::min (strip.lowestDb, share.levelDb) : share.levelDb;
                    strip.loudestDb = any ? std::max (strip.loudestDb, share.levelDb) : share.levelDb;
                    any = true;
                }

                strip.each.push_back (std::move (share));
            }

            if (! any)
            {
                strip.lowestDb = strip.levelDb;
                strip.loudestDb = strip.levelDb;
            }

            out.push_back (std::move (strip));
        }

        return out;
    }

    std::string levelText (double decibels)
    {
        return osc::formatDouble (std::round (std::clamp (decibels, silenceDb, loudestDb) * 10.0) / 10.0);
    }

    std::vector<std::pair<std::string, std::string>> levelsMovedBy (const std::vector<std::pair<std::string, double>>& held,
                                                                    double decibels)
    {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve (held.size());

        for (const auto& [address, from] : held)
            out.emplace_back (address, levelText (from + decibels));

        return out;
    }

    std::vector<std::pair<std::string, std::string>> levelsSetTo (const std::vector<std::string>& addresses, double decibels)
    {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve (addresses.size());

        for (const auto& address : addresses)
            out.emplace_back (address, levelText (decibels));

        return out;
    }
}
