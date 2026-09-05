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

#include <wfg/engine/cue/CueList.h>

#include <algorithm>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  The `Lists` container, or an invalid tree. The show always has one -
            ShowDocument's constructor makes it - but a document adopted from a
            file could in principle not, and asking is cheaper than assuming. */
        juce::ValueTree listsContainer (const doc::ShowDocument& document)
        {
            for (const auto& child : document.root())
                if (child.getType().toString() == "Lists")
                    return child;

            return {};
        }
    }

    //==============================================================================
    std::vector<std::string> childrenOf (const juce::ValueTree& container)
    {
        std::vector<std::string> ids;

        if (! container.isValid())
            return ids;

        for (const auto& child : container)
            if (child.hasProperty (idProperty))
                ids.push_back (child[idProperty].toString().toStdString());

        return ids;
    }

    bool isTopLevelChild (const juce::ValueTree& list, const std::string& cueId)
    {
        if (cueId.empty())
            return false;

        const auto ids = childrenOf (list);
        return std::find (ids.begin(), ids.end(), cueId) != ids.end();
    }

    //==============================================================================
    std::string nextOf (const juce::ValueTree& list, const std::string& current)
    {
        const auto ids = childrenOf (list);
        const auto at = std::find (ids.begin(), ids.end(), current);

        /*  Empty, or naming something that is not a top-level child: stays
            where it is. Only standby.set arms a list, so there is nothing here
            that turns "nowhere" into "the first cue". */
        if (at == ids.end())
            return current;

        const auto after = at + 1;
        return after == ids.end() ? current : *after;
    }

    std::string previousOf (const juce::ValueTree& list, const std::string& current)
    {
        const auto ids = childrenOf (list);
        const auto at = std::find (ids.begin(), ids.end(), current);

        if (at == ids.end() || at == ids.begin())
            return current;

        return *(at - 1);
    }

    //==============================================================================
    /*  The parameter is `wanted` rather than the obvious `listId` because this
        class has a listId() accessor, and GCC's -Wshadow objects to a parameter
        that shadows a member - including a member FUNCTION. MSVC says nothing,
        so it would have been a Linux-only build failure. The same trap already
        cost this project two CI round trips. */
    bool Focus::request (const doc::ShowDocument& document, const std::string& wanted)
    {
        const auto node = document.findById (wanted);

        /*  Nothing is written until the request is known good. A rejected
            list.focus must leave the previous focus exactly where it was, and
            assigning first would quietly clear it - a failure the fallback to
            the first list would then hide. */
        if (! node.isValid() || node.getType().toString() != "List")
            return false;

        requestedId = wanted;
        return true;
    }

    juce::ValueTree Focus::list (const doc::ShowDocument& document) const
    {
        if (! requestedId.empty())
        {
            const auto node = document.findById (requestedId);

            if (node.isValid() && node.getType().toString() == "List")
                return node;
        }

        /*  The first list in the show. This is what makes "exactly one list is
            focused whenever a list exists" true without anything having to
            maintain it: a request that no longer resolves - the list was
            deleted, or the show was replaced under it - falls back here rather
            than leaving the engine pointed at nothing. */
        const auto lists = listsContainer (document);

        for (const auto& child : lists)
            if (child.getType().toString() == "List" && child.hasProperty (idProperty))
                return child;

        return {};
    }

    std::string Focus::listId (const doc::ShowDocument& document) const
    {
        const auto node = list (document);
        return node.isValid() ? node[idProperty].toString().toStdString() : std::string {};
    }

    //==============================================================================
    std::string standbyAddressOf (const std::string& listId)
    {
        return "/godot/list/" + listId + "/standby";
    }
}
