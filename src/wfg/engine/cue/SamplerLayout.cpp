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

#include <wfg/engine/cue/SamplerLayout.h>

#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>
#include <functional>
#include <set>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        std::string idOf (const juce::ValueTree& node)
        {
            return node[idProperty].toString().toStdString();
        }

        std::string cueRow (const doc::ShowDocument& document, const std::string& id, const char* row)
        {
            return document.getAttribute ("/godot/cue/" + id + "/" + row).value_or (std::string {});
        }

        bool isSamplerGroup (const doc::ShowDocument& document, const juce::ValueTree& node)
        {
            return node.getType().toString() == "Group"
                && cueRow (document, idOf (node), "mode") == "sampler";
        }

        /*  THE CUES THAT HOLD NO OTHER CUES, which a walk for groups need not
            enter: their children are sends and inserts. */
        bool isLeafCue (const juce::ValueTree& node)
        {
            const auto element = node.getType().toString();

            return element == "Media" || element == "Cue" || element == "Fade"
                || element == "Transport" || element == "Osc" || element == "Midi"
                || element == "Start";
        }
    }

    //==========================================================================
    std::vector<std::string> samplerStripsOf (const doc::ShowDocument& document)
    {
        std::vector<std::string> roster;

        for (const auto& box : document.root().getChildWithName ("Surfaces"))
        {
            if (box.getType().toString() != "Surface")
                continue;

            for (const auto& strip : box)
            {
                if (strip.getType().toString() != "Strip")
                    continue;

                const auto stripId = idOf (strip);

                //  Addressed as a slot, answering `strip` by kind: ShowDocument::addressOwnerFor.
                if (document.getAttribute ("/godot/slot/" + stripId + "/role").value_or (std::string {})
                      == "sampler")
                    roster.push_back (stripId);
            }
        }

        return roster;
    }

    std::vector<Placement> placeMembers (const doc::ShowDocument& document,
                                         const juce::ValueTree& group,
                                         const std::vector<std::string>& roster)
    {
        std::vector<Placement> out;

        /*  THE MEMBERS A SAMPLER PLAYS: media cues, enabled, in member order
            - the Runner's `membersOf` narrowed to what a strip can play (a
            memo among them would take a strip nothing could be played from). */
        for (const auto& child : group)
        {
            if (child.getType().toString() != "Media")
                continue;

            const auto id = idOf (child);

            if (id.empty() || cueRow (document, id, "enabled") == "false")
                continue;

            out.push_back ({ id, {}, false });
        }

        std::set<std::string> taken;

        //  1. The pins, first come first served in member order.
        for (auto& member : out)
        {
            const auto pin = cueRow (document, member.cue, "strip");

            if (pin.empty() || taken.count (pin) != 0
                 || std::find (roster.begin(), roster.end(), pin) == roster.end())
                continue;

            member.strip = pin;
            member.pinned = true;
            taken.insert (pin);
        }

        //  2. Everybody else, on what is left, in roster order.
        auto next = roster.begin();

        for (auto& member : out)
        {
            if (! member.strip.empty())
                continue;

            while (next != roster.end() && taken.count (*next) != 0)
                ++next;

            if (next == roster.end())
                break;

            member.strip = *next;
            taken.insert (*next);
            ++next;
        }

        return out;
    }

    //==========================================================================
    void SamplerLayout::ensureBuilt (const doc::ShowDocument& document)
    {
        if (built && builtRevision == document.showRevision())
            return;

        built = true;
        builtRevision = document.showRevision();
        placed.clear();
        before.clear();

        const auto roster = samplerStripsOf (document);

        /*  ONE LIST AT A TIME, IN ROW ORDER: what an earlier sampler group put
            on a strip is carried forward until a later one puts something else
            there. A list is a show's running order, and two lists are two
            running orders - what one put on a strip says nothing about the
            other. */
        for (const auto& list : document.root().getChildWithName ("Lists"))
        {
            std::map<std::string, std::string> onStrip;

            const std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                {
                    if (isLeafCue (child))
                        continue;

                    if (isSamplerGroup (document, child))
                    {
                        const auto members = placeMembers (document, child, roster);

                        //  What was there BEFORE this group, in roster order.
                        std::string pairs;

                        for (const auto& strip : roster)
                            if (const auto found = onStrip.find (strip); found != onStrip.end())
                                pairs += (pairs.empty() ? "" : " ") + strip + " " + found->second;

                        for (const auto& member : members)
                        {
                            placed[member.cue] = member.strip;
                            before[member.cue] = pairs;
                        }

                        for (const auto& member : members)
                            if (! member.strip.empty())
                                onStrip[member.strip] = member.cue;
                    }

                    walk (child);
                }
            };

            walk (list);
        }
    }

    std::string SamplerLayout::stripOf (const std::string& cueId) const
    {
        const auto found = placed.find (cueId);
        return found == placed.end() ? std::string {} : found->second;
    }

    std::string SamplerLayout::stripsBeforeOf (const std::string& cueId) const
    {
        const auto found = before.find (cueId);
        return found == before.end() ? std::string {} : found->second;
    }
}
