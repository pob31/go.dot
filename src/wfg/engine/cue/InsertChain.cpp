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

#include <wfg/engine/cue/InsertChain.h>

#include <wfg/engine/cue/ShowWalk.h>

#include <algorithm>

namespace wfg::cue
{
    InsertChain chainOf (int fileChannels, int trackChannels,
                         const std::vector<bool>& switchedIn, const std::vector<InsertShape>& shapes)
    {
        InsertChain out;
        const auto voice = std::max (1, trackChannels);
        auto width = std::clamp (fileChannels, 1, voice);

        for (std::size_t slot = 0; slot < switchedIn.size(); ++slot)
        {
            InsertStep step;
            step.switchedIn = switchedIn[slot];

            if (step.switchedIn)
            {
                const auto shape = slot < shapes.size() ? shapes[slot] : InsertShape {};

                if (! shape.known)
                {
                    /*  Nothing said yet: counted as taking the cue at its width,
                        which is how wide the voice stays - silent, the lane not
                        called (CU), until the plugin is up and says. */
                    step.feed = width;
                    step.back = width;
                }
                else if (width > shape.inputs)
                {
                    step.dryWhy = "this cue is " + std::to_string (width) + " channels wide here and the plugin takes "
                                  + std::to_string (shape.inputs) + ": it plays dry";
                }
                else if (shape.outputs < width)
                {
                    step.dryWhy = "the plugin gives back " + std::to_string (shape.outputs) + " channels and this cue is "
                                  + std::to_string (width) + " wide here: it plays dry rather than narrower";
                }
                else
                {
                    /*  WIDER ONLY WHEN THE CUE FILLS THE PLUGIN'S INPUTS - all of
                        them, or a mono cue fed into every one. A stereo cue in
                        the first two inputs of a plugin as wide as an eight-
                        channel voice comes back stereo: its other outputs are
                        what silence made, and taking them would fold the cue
                        down at a quarter wherever it is sent. */
                    step.feed = width;
                    step.back = width == shape.inputs || width == 1 ? std::min (shape.outputs, voice) : width;
                    width = step.back;
                    out.latencySamples += shape.latencySamples;
                }
            }

            out.steps.push_back (std::move (step));
        }

        out.channels = width;
        return out;
    }

    std::vector<std::string> slotIdsOf (const juce::ValueTree& plugins, const plugin::PluginTable* table)
    {
        if (table != nullptr && table->hasGraph())
            return table->built();

        std::vector<std::string> out;

        for (const auto entry : plugins)
            if (entry.hasType ("Plugin"))
                out.push_back (entry.getProperty ("id").toString().toStdString());

        return out;
    }

    std::vector<InsertShape> shapesOf (const std::vector<std::string>& slotIds, const plugin::PluginTable* table)
    {
        std::vector<InsertShape> out (slotIds.size());

        if (table == nullptr)
            return out;

        for (std::size_t slot = 0; slot < slotIds.size(); ++slot)
        {
            const auto status = table->statusOf (slotIds[slot]);

            if (status.state == "loaded" && status.inputs > 0 && status.outputs > 0)
                out[slot] = { true, status.inputs, status.outputs, status.latencySamples };
        }

        return out;
    }

    namespace
    {
        /*  A child of `Audio` by its container's element and its own id. */
        juce::ValueTree audioChild (const juce::ValueTree& anyNode, const char* container,
                                    const char* element, const juce::String& id)
        {
            if (id.isEmpty())
                return {};

            for (const auto holder : anyNode.getRoot().getChildWithName ("Audio"))
                if (holder.hasType (container))
                    for (const auto child : holder)
                        if (child.hasType (element) && child.getProperty ("id").toString() == id)
                            return child;

            return {};
        }
    }

    int sourceChannelsOf (const juce::ValueTree& cue)
    {
        static const Reader schema;

        if (cue.hasType ("Mic"))
        {
            const auto input = audioChild (cue, "Inputs", "Input", cue.getProperty ("input").toString());
            return input.isValid() ? std::max (1, static_cast<int> (schema.integer (input, "input", "width"))) : 1;
        }

        return static_cast<int> (schema.integer (cue, "media", "channels"));
    }

    InsertChain chainOfCue (const juce::ValueTree& cue, const plugin::PluginTable* table, int trackChannels)
    {
        static const Reader schema;

        /*  A MIC CUE'S CHAIN IS ITS CHANNEL'S (Phase 9b): the plugins as the
            graph built that channel - as declared, with no graph - its own
            inserts switched in or not, its input's width at the head, on a
            track two channels wide whatever the class. */
        if (cue.hasType ("Mic"))
        {
            const auto channel = audioChild (cue, "Rack", "Channel", cue.getProperty ("channel").toString());
            const auto channelId = channel.getProperty ("id").toString().toStdString();

            std::vector<std::string> declared;

            for (const auto entry : channel)
                if (entry.hasType ("Plugin"))
                    declared.push_back (entry.getProperty ("id").toString().toStdString());

            const auto slots = table != nullptr && table->rackBuilt (channelId) ? table->builtRackOf (channelId)
                                                                                : declared;
            std::vector<bool> switchedIn (slots.size(), false);

            for (std::size_t slot = 0; slot < slots.size(); ++slot)
            {
                if (std::find (declared.begin(), declared.end(), slots[slot]) == declared.end())
                    continue;

                for (const auto child : cue)
                    if (child.hasType ("Fx") && child.getProperty ("plugin").toString().toStdString() == slots[slot])
                    {
                        switchedIn[slot] = schema.flag (child, "fx", "enabled");
                        break;
                    }
            }

            return chainOf (sourceChannelsOf (cue), rackTrackChannels, switchedIn, shapesOf (slots, table));
        }

        const auto plugins = cue.getRoot().getChildWithName ("Audio").getChildWithName ("Plugins");
        const auto slots = slotIdsOf (plugins, table);

        std::vector<std::string> inSet;

        for (const auto entry : plugins)
            if (entry.hasType ("Plugin"))
                inSet.push_back (entry.getProperty ("id").toString().toStdString());

        std::vector<bool> switchedIn (slots.size(), false);

        for (std::size_t slot = 0; slot < slots.size(); ++slot)
        {
            if (std::find (inSet.begin(), inSet.end(), slots[slot]) == inSet.end())
                continue;

            for (const auto child : cue)
                if (child.hasType ("Fx") && child.getProperty ("plugin").toString().toStdString() == slots[slot])
                {
                    switchedIn[slot] = schema.flag (child, "fx", "enabled");
                    break;
                }
        }

        return chainOf (static_cast<int> (schema.integer (cue, "media", "channels")), trackChannels,
                        switchedIn, shapesOf (slots, table));
    }
}
