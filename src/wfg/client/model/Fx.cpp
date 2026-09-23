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

#include <wfg/client/model/Fx.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>

#include <map>
#include <sstream>

namespace wfg::client::model
{
    namespace
    {
        constexpr const char* fxPrefix = "/godot/fx/";

        std::vector<std::string> words (const std::string& text)
        {
            std::vector<std::string> out;
            std::istringstream in (text);
            std::string word;

            while (in >> word)
                out.push_back (word);

            return out;
        }

        int integer (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return static_cast<int> (osc::parseDouble (text (snapshot, address)).value_or (0.0));
        }

        double number (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return osc::parseDouble (text (snapshot, address)).value_or (0.0);
        }

        /*  The cue's Fx children, keyed by the entry they name: flat under
            their own owner with a derived `cue`, as the sends are, so the way
            back to the cue is that row and not containment. */
        struct HeldFx { std::string id, plugin; bool enabled = true; bool mine = false; };

        std::map<std::string, HeldFx> fxOfCue (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            std::map<std::string, HeldFx> held;

            for (const auto* node : snapshot.all())
            {
                if (node->address.rfind (fxPrefix, 0) != 0)
                    continue;

                const auto rest = node->address.substr (std::string (fxPrefix).size());
                const auto slash = rest.find ('/');

                if (slash == std::string::npos)
                    continue;

                const auto id = rest.substr (0, slash);
                const auto name = rest.substr (slash + 1);

                if (name.find ('/') != std::string::npos)
                    continue;

                const auto reading = text (node);
                auto& one = held[id];
                one.id = id;

                if (name == "cue")          one.mine = reading == cueId;
                else if (name == "plugin")  one.plugin = reading;
                else if (name == "enabled") one.enabled = isYes (flag (snapshot, node->address));
            }

            std::map<std::string, HeldFx> byPlugin;

            for (auto& [id, one] : held)
                if (one.mine && ! one.plugin.empty())
                    byPlugin[one.plugin] = one;

            return byPlugin;
        }
    }

    //==============================================================================
    std::string fxAddress (const std::string& fxId, const std::string& leaf)
    {
        return std::string (fxPrefix) + fxId + "/" + leaf;
    }

    std::string fxParameterAddress (const std::string& fxId, int index)
    {
        return fxAddress (fxId, "p" + std::to_string (index));
    }

    FxReading readFx (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        FxReading out;

        if (cueId.empty())
        {
            out.notice = "Nothing is picked.";
            return out;
        }

        if (text (snapshot, "/godot/cue/" + cueId + "/kind") != "media")
        {
            out.notice = "Inserts belong to media cues; this cue plays no file.";
            return out;
        }

        out.present = true;
        const auto order = words (text (snapshot, "/godot/plugin/order"));

        if (order.empty())
        {
            out.notice = "The show declares no plugins yet: Show settings, Plugins.";
            return out;
        }

        const auto mine = fxOfCue (snapshot, cueId);
        auto index = 0;

        for (const auto& pluginId : order)
        {
            const auto base = "/godot/plugin/" + pluginId + "/";

            FxStrip strip;
            strip.pluginId = pluginId;
            strip.index = index++;
            strip.name = text (snapshot, base + "name");
            strip.state = text (snapshot, base + "state");
            strip.problem = text (snapshot, base + "problem");

            if (strip.name.empty())
                strip.name = pluginId;

            if (const auto found = mine.find (pluginId); found != mine.end())
            {
                strip.fxId = found->second.id;
                strip.enabled = found->second.enabled;
            }

            const auto count = integer (snapshot, base + "paramCount");

            for (int n = 0; n < count; ++n)
            {
                const auto param = base + "param/" + std::to_string (n) + "/";

                FxParameter p;
                p.index = n;
                p.name = text (snapshot, param + "name");
                p.shortName = text (snapshot, param + "shortName");
                p.unit = text (snapshot, param + "unit");
                p.defaultValue = number (snapshot, param + "default");
                p.discrete = integer (snapshot, param + "steps") > 0;
                p.bipolar = isYes (flag (snapshot, param + "bipolar"));

                /*  A stepped parameter's words are the p node's enum values,
                    published with it - read off the node when the cue has one. */
                if (strip.present())
                {
                    p.value = number (snapshot, fxParameterAddress (strip.fxId, n));
                    p.text = text (snapshot, fxAddress (strip.fxId, "t" + std::to_string (n)));

                    if (const auto* node = snapshot.find (fxParameterAddress (strip.fxId, n)))
                        p.steps = node->enumValues;
                }
                else
                {
                    p.value = p.defaultValue;
                }

                if (p.name.empty())
                    p.name = "Parameter " + std::to_string (n);

                strip.params.push_back (std::move (p));
            }

            out.strips.push_back (std::move (strip));
        }

        return out;
    }

    //==============================================================================
    std::vector<PluginRow> readPluginSet (const tree::TreeSnapshot& snapshot)
    {
        std::vector<PluginRow> out;

        for (const auto& id : words (text (snapshot, "/godot/plugin/order")))
        {
            const auto base = "/godot/plugin/" + id + "/";

            PluginRow row;
            row.id = id;
            row.name = text (snapshot, base + "name");
            row.identifier = text (snapshot, base + "identifier");
            row.format = text (snapshot, base + "format");
            row.path = text (snapshot, base + "path");
            row.preset = text (snapshot, base + "preset");
            row.state = text (snapshot, base + "state");
            row.problem = text (snapshot, base + "problem");
            row.latencySamples = integer (snapshot, base + "latencySamples");
            row.paramCount = integer (snapshot, base + "paramCount");
            out.push_back (std::move (row));
        }

        return out;
    }

    std::vector<KnownPluginRow> readKnownPlugins (const tree::TreeSnapshot& snapshot)
    {
        std::vector<KnownPluginRow> out;

        for (int n = 0;; ++n)
        {
            const auto base = "/godot/plugin/known/" + std::to_string (n) + "/";

            if (snapshot.find (base + "identifier") == nullptr)
                break;

            KnownPluginRow row;
            row.name = text (snapshot, base + "name");
            row.identifier = text (snapshot, base + "identifier");
            row.format = text (snapshot, base + "format");
            row.manufacturer = text (snapshot, base + "manufacturer");
            row.path = text (snapshot, base + "path");
            out.push_back (std::move (row));
        }

        return out;
    }
}
