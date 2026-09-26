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

#include <wfg/client/model/Rack.h>

#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        /*  A number off the tree's own text, never a locale question - and
            never a throw on a malformed reading. `reading` rather than `text`
            for OutputList's reason: GCC's -Wshadow. */
        double numberOf (const std::string& reading, double fallback)
        {
            return osc::parseDouble (reading).value_or (fallback);
        }
    }

    std::string RackChannelRow::classWord() const
    {
        if (channelClass == "mono")         return "Mono";
        if (channelClass == "monoToStereo") return "Mono to stereo";
        if (channelClass == "stereo")       return "Stereo";

        return channelClass;
    }

    std::string RackChannelRow::chainWord() const
    {
        if (chain.empty())
            return "No plugins";

        return std::to_string (chain.size()) + (chain.size() == 1 ? " plugin" : " plugins");
    }

    RackReading readRack (const tree::TreeSnapshot& snapshot)
    {
        RackReading out;
        out.budgetMs = numberOf (text (snapshot, "/godot/audio/rackBudget"), 5.0);
        out.sampleRate = static_cast<int> (numberOf (text (snapshot, "/godot/engine/sampleRate"), 0.0));

        /*  THE SLOTS' OWN ORDER, which is the document's, kept to the rack's:
            the rack channels are slots of the second kind (§3.9e), published
            beside the processor inputs and the strips and told apart by the
            word the engine derives from the element. */
        for (const auto& id : words (text (snapshot, "/godot/slot/order")))
        {
            const auto base = "/godot/slot/" + id + "/";

            if (text (snapshot, base + "kind") != "rackChannel")
                continue;

            RackChannelRow row;
            row.id = id;
            row.name = text (snapshot, base + "name");
            row.channelClass = text (snapshot, base + "class");
            row.latencySamples = static_cast<int> (numberOf (text (snapshot, base + "latencySamples"), 0.0));

            if (row.name.empty())
                row.name = "Channel " + std::to_string (out.channels.size() + 1);

            if (row.channelClass.empty())
                row.channelClass = "mono";

            for (const auto& pluginId : words (text (snapshot, base + "plugins")))
                row.chain.push_back (readPluginEntry (snapshot, pluginId));

            out.channels.push_back (std::move (row));
        }

        return out;
    }

    bool overBudget (const RackChannelRow& channel, const RackReading& rack)
    {
        if (rack.sampleRate <= 0 || channel.latencySamples <= 0)
            return false;

        return 1000.0 * channel.latencySamples / rack.sampleRate > rack.budgetMs + 1.0e-9;
    }

    std::string millisecondWords (double milliseconds)
    {
        const auto tenths = std::llround (std::max (0.0, milliseconds) * 10.0);
        const auto whole = std::to_string (tenths / 10);

        return (tenths % 10 == 0 ? whole : whole + "." + std::to_string (tenths % 10)) + " ms";
    }

    std::string busyWords (const tree::TreeSnapshot& snapshot)
    {
        std::vector<std::string> names;

        for (const auto& row : readRuns (snapshot))
        {
            if (row.state == "done" || row.state == "failed" || row.state == "preparing")
                continue;

            /*  A CUE ONLY GOT READY is not a sound: the horizon's arm, which
                the engine lets go of when the graph is rebuilt. */
            if (row.state == "armed")
                if (const auto prepared = text (snapshot, "/godot/cue/" + row.cueId + "/prepare");
                    ! prepared.empty() && prepared != "idle")
                    continue;

            const auto called = row.cueName.empty() ? row.cueId : row.cueName;

            if (std::find (names.begin(), names.end(), called) == names.end())
                names.push_back (called);
        }

        if (names.empty())
            return {};

        std::string said;
        const auto shown = std::min<std::size_t> (names.size(), 3);

        for (std::size_t at = 0; at < shown; ++at)
            said += (at == 0 ? "" : at + 1 == shown && names.size() == shown ? " and " : ", ") + names[at];

        if (names.size() > shown)
            said += " and " + std::to_string (names.size() - shown) + " more";

        return said + (names.size() == 1 ? " is sounding" : " are sounding");
    }

    std::string budgetWords (const RackChannelRow& channel, const RackReading& rack)
    {
        if (channel.chain.empty())
            return "No plugins: the channel adds no delay.";

        std::vector<std::string> notLoaded;

        for (const auto& entry : channel.chain)
            if (entry.state != "loaded")
                notLoaded.push_back (entry.name.empty() ? entry.identifier : entry.name);

        /*  NOTHING LOADED IS NOTHING KNOWN: a plugin declares its delay when it
            loads, which is when the audio is open and the graph is built. */
        if (notLoaded.size() == channel.chain.size())
            return rack.sampleRate <= 0
                     ? "The plugins load when the audio opens, and say then how late they make the channel."
                     : "No plugin has loaded yet, so none has said how late it makes the channel.";

        std::string said;

        if (channel.latencySamples <= 0)
            said = "Adds no delay, with every plugin in.";
        else if (rack.sampleRate <= 0)
            said = std::to_string (channel.latencySamples) + " samples at worst, with every plugin in.";
        else
        {
            const auto late = millisecondWords (1000.0 * channel.latencySamples / rack.sampleRate);

            said = late + " at worst, with every plugin in - "
                     + (overBudget (channel, rack)
                          ? "over the " + millisecondWords (rack.budgetMs) + " budget. A mic cue that switches"
                            " them all in says so, and plays."
                          : "within the " + millisecondWords (rack.budgetMs) + " budget.");
        }

        if (! notLoaded.empty())
        {
            said += " Not counted, not loaded: ";

            for (std::size_t at = 0; at < notLoaded.size(); ++at)
                said += (at == 0 ? "" : ", ") + notLoaded[at];

            said += ".";
        }

        return said;
    }
}
