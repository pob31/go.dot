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
    namespace
    {
        /*  Whether a cue is a group the pointer goes INSIDE: a manual sequence.

            Read from the document with its defaults applied - `mode` defaults
            to `sequence` and `advance` to `manual`, so a group somebody made
            and did not configure is one the operator drives, which is the
            gentler default of the two. An attribute absent from a ValueTree is
            the default and not an empty string, and reading it directly gets
            that wrong; here the elements are checked against the same words the
            table uses. */
        bool isManualSequence (const juce::ValueTree& cue)
        {
            if (! cue.isValid() || cue.getType().toString() != "Group")
                return false;

            const auto mode = cue[juce::Identifier ("mode")].toString();
            const auto advance = cue[juce::Identifier ("advance")].toString();

            return (mode.isEmpty() || mode == "sequence")
                     && (advance.isEmpty() || advance == "manual");
        }

        /*  ABSENT MEANS THE DEFAULT, AND PRESENT MEANS ITS OWN TYPE - which
            is two ways to get this wrong and the reason it is written out.

            The canonical writer omits an attribute holding its default, so a
            cue nobody has disabled has no `enabled` property at all and the
            answer is `true`. And a cue somebody HAS disabled holds a BOOLEAN
            false, not the text "false": every value goes into the tree through
            the schema, typed. Comparing the text would have answered "0" and
            matched nothing, which is exactly what the first version did. */
        bool isEnabled (const juce::ValueTree& cue)
        {
            const juce::Identifier enabled { "enabled" };

            return ! cue.hasProperty (enabled) || static_cast<bool> (cue[enabled]);
        }

        /** Whether this element is a cue at all - not a header, footer or route. */
        bool isCueElement (const juce::ValueTree& node)
        {
            const auto element = node.getType().toString();

            return element == "Cue" || element == "Group" || element == "Media"
                     || element == "Fade" || element == "Stop" || element == "Osc";
        }

        /** The cues the pointer may stand on among a container's children. */
        std::vector<juce::ValueTree> stops (const juce::ValueTree& container)
        {
            std::vector<juce::ValueTree> out;

            for (const auto& child : container)
                if (child.hasProperty (idProperty) && isCueElement (child) && isEnabled (child))
                    out.push_back (child);

            return out;
        }

        /*  The first place the pointer can stand at or below this cue: itself,
            unless it is a manual group, in which case its first member - and so
            on down, because a manual group's first member may be one too.

            An EMPTY manual group has nowhere inside it, so the pointer stands
            on the group row itself: GO there completes it, which is the honest
            thing for a container somebody has not filled in yet. */
        juce::ValueTree descendTo (const juce::ValueTree& cue)
        {
            if (! isManualSequence (cue))
                return cue;

            const auto inside = stops (cue);

            return inside.empty() ? cue : descendTo (inside.front());
        }

        /** The last place the pointer can stand at or below this cue. */
        juce::ValueTree descendToLast (const juce::ValueTree& cue)
        {
            if (! isManualSequence (cue))
                return cue;

            const auto inside = stops (cue);

            return inside.empty() ? cue : descendToLast (inside.back());
        }

        /*  The cue with this identifier, searched only where the pointer may
            go: down through manual groups, never into a header or a footer. */
        juce::ValueTree findOnPath (const juce::ValueTree& container, const std::string& cueId)
        {
            for (const auto& child : stops (container))
            {
                if (child[idProperty].toString().toStdString() == cueId)
                    return child;

                if (isManualSequence (child))
                    if (const auto found = findOnPath (child, cueId); found.isValid())
                        return found;
            }

            return {};
        }

        /*  The step after `from` within its own container, descending into what
            it finds; or an invalid tree when there is nothing after it, which is
            what tells the caller to climb.

            `list` is the top, so climbing stops there rather than walking out of
            the show. */
        juce::ValueTree stepFrom (const juce::ValueTree& list, const juce::ValueTree& from,
                                  bool forwards)
        {
            auto cue = from;

            while (cue.isValid() && cue != list)
            {
                const auto parent = cue.getParent();
                const auto siblings = stops (parent);

                const auto at = std::find (siblings.begin(), siblings.end(), cue);

                if (at != siblings.end())
                {
                    if (forwards)
                    {
                        if (at + 1 != siblings.end())
                            return descendTo (*(at + 1));
                    }
                    else if (at != siblings.begin())
                    {
                        return descendToLast (*(at - 1));
                    }
                }

                /*  Exhausted where it was, so it climbs: the next place after
                    the last member of a group is whatever follows the GROUP,
                    and the place before its first member is whatever precedes
                    it.

                    THE GROUP ROW ITSELF IS NEVER A STOP, which is decision M
                    (2026-09-06) seen from the inside: GO at a manual group's row
                    fires its first member, so the row and the first member are
                    one position rather than two, and `descendTo` never leaves
                    the pointer on the row going forwards. Stopping there going
                    backwards would have made the path asymmetric - a press of
                    `previous` followed by `next` would not have come back to
                    where it started. */
                cue = parent;
            }

            return {};
        }
    }

    bool isInList (const juce::ValueTree& list, const std::string& cueId)
    {
        if (cueId.empty() || ! list.isValid())
            return false;

        for (const auto& child : list)
        {
            if (child.hasProperty (idProperty)
                  && child[idProperty].toString().toStdString() == cueId)
                return true;

            if (isInList (child, cueId))
                return true;
        }

        return false;
    }

    std::string nextStandby (const juce::ValueTree& list, const std::string& current)
    {
        if (! list.isValid())
            return current;

        const auto from = findOnPath (list, current);

        /*  Nowhere, or somewhere the pointer cannot be: it stays put. Only
            `standby.set` arms a list, so there is nothing here that turns
            "nowhere" into "the first cue" - and no wrap at either end, which is
            what the end-of-list rule is for. */
        if (! from.isValid())
            return current;

        const auto next = stepFrom (list, from, true);
        return next.isValid() ? next[idProperty].toString().toStdString() : current;
    }

    std::string previousStandby (const juce::ValueTree& list, const std::string& current)
    {
        if (! list.isValid())
            return current;

        const auto from = findOnPath (list, current);

        if (! from.isValid())
            return current;

        const auto previous = stepFrom (list, from, false);
        return previous.isValid() ? previous[idProperty].toString().toStdString() : current;
    }

    bool isOnManualPath (const juce::ValueTree& list, const std::string& cueId)
    {
        return cueId.empty() || findOnPath (list, cueId).isValid();
    }

    //==============================================================================
    /*  The parameter is `wanted` rather than the obvious `listId` because this
        class has a listId() accessor, and GCC's -Wshadow objects to a parameter
        that shadows a member - including a member FUNCTION. MSVC says nothing,
        so it would have been a Linux-only build failure. The same trap already
        cost this project two CI round trips. */
    std::string focusAddress()
    {
        return "/godot/list/focus";
    }

    bool Focus::request (doc::ShowDocument& document, const std::string& wanted)
    {
        const auto node = document.findById (wanted);

        /*  Nothing is written until the request is known good. A rejected
            list.focus must leave the previous focus exactly where it was, and
            assigning first would quietly clear it - a failure the fallback to
            the first list would then hide. */
        if (! node.isValid() || node.getType().toString() != "List")
            return false;

        /*  THROUGH THE DOCUMENT'S ONE WRITE DOOR, which is what changed in PR
            3.2. Focus was a string on this object: engine state, unpublished,
            forgotten on every close. Now it is `/godot/list/focus`, so a client
            can read which list GO acts on, a surface can move it, and a show
            reopens on the list the operator was working in - the same argument
            that persisted the standby, applied to the pointer that says which
            standby is being pointed at. */
        return document.setAttribute (focusAddress(), wanted).ok;
    }

    void Focus::clear (doc::ShowDocument& document)
    {
        document.setAttribute (focusAddress(), {});
    }

    juce::ValueTree Focus::list (const doc::ShowDocument& document) const
    {
        const auto requested = document.getAttribute (focusAddress()).value_or (std::string {});

        if (! requested.empty())
        {
            const auto node = document.findById (requested);

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

    std::string Focus::requested (const doc::ShowDocument& document) const
    {
        return document.getAttribute (focusAddress()).value_or (std::string {});
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
