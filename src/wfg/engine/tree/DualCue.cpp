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

#include <wfg/engine/tree/DualCue.h>

#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        std::string textOf (const TreeSnapshot& snapshot, const std::string& address)
        {
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isString())
                return {};

            return node->values.front().getString();
        }

        std::vector<std::string> wordsOf (const std::string& text)
        {
            std::vector<std::string> out;
            std::istringstream in (text);

            for (std::string word; in >> word;)
                out.push_back (word);

            return out;
        }

        std::string kindOf (const TreeSnapshot& snapshot, const std::string& cueId)
        {
            return textOf (snapshot, "/godot/cue/" + cueId + "/kind");
        }

        /*  THE CUE STRAIGHT AFTER `cueId` in whichever of its container's
            sections holds it - the members, a group's header or footer, a
            list's persistent band - or empty when it is the last. The list
            draws a pair only within one section, so a movie at the end of a
            header and a sound at the top of the members are not one. */
        std::string nextSibling (const TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto parent = textOf (snapshot, "/godot/cue/" + cueId + "/parent");

            if (parent.empty())
                return {};

            const auto isList = snapshot.find ("/godot/list/" + parent + "/order") != nullptr;
            const auto base = (isList ? "/godot/list/" : "/godot/cue/") + parent + "/";

            for (const auto* section : { "order", "headerOrder", "footerOrder", "persistentOrder" })
            {
                const auto ids = wordsOf (textOf (snapshot, base + section));
                const auto found = std::find (ids.begin(), ids.end(), cueId);

                if (found != ids.end())
                    return found + 1 != ids.end() ? *(found + 1) : std::string {};
            }

            return {};
        }
    }

    DualCue dualCueOf (const TreeSnapshot& snapshot, const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        const auto kind = kindOf (snapshot, cueId);

        if (kind == "video")
        {
            const auto next = nextSibling (snapshot, cueId);

            if (! next.empty() && kindOf (snapshot, next) == "media"
                  && textOf (snapshot, "/godot/cue/" + next + "/lockedTo") == cueId)
                return { cueId, next };

            return {};
        }

        if (kind == "media")
        {
            const auto movie = textOf (snapshot, "/godot/cue/" + cueId + "/lockedTo");

            if (! movie.empty() && kindOf (snapshot, movie) == "video" && nextSibling (snapshot, movie) == cueId)
                return { movie, cueId };
        }

        return {};
    }
}
