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

#include <wfg/client/model/Dual.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        std::string kindOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            return text (snapshot, "/godot/cue/" + cueId + "/kind");
        }

        /*  THE CUE STRAIGHT AFTER `cueId` in whichever of its container's
            sections holds it - the members, a group's header or footer, a
            list's persistent band - or empty when it is the last. The list
            draws a pair only within one section, so a movie at the end of a
            header and a sound at the top of the members are not one. */
        std::string nextSibling (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto parent = text (snapshot, "/godot/cue/" + cueId + "/parent");

            if (parent.empty())
                return {};

            const auto isList = snapshot.find ("/godot/list/" + parent + "/order") != nullptr;
            const auto base = (isList ? "/godot/list/" : "/godot/cue/") + parent + "/";

            for (const auto* section : { "order", "headerOrder", "footerOrder", "persistentOrder" })
            {
                const auto ids = words (text (snapshot, base + section));
                const auto found = std::find (ids.begin(), ids.end(), cueId);

                if (found != ids.end())
                    return found + 1 != ids.end() ? *(found + 1) : std::string {};
            }

            return {};
        }
    }

    Dual dualOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        const auto kind = kindOf (snapshot, cueId);

        if (kind == "video")
        {
            const auto next = nextSibling (snapshot, cueId);

            if (! next.empty() && kindOf (snapshot, next) == "media"
                  && text (snapshot, "/godot/cue/" + next + "/lockedTo") == cueId)
                return { cueId, next };

            return {};
        }

        if (kind == "media")
        {
            const auto movie = text (snapshot, "/godot/cue/" + cueId + "/lockedTo");

            if (! movie.empty() && kindOf (snapshot, movie) == "video" && nextSibling (snapshot, movie) == cueId)
                return { movie, cueId };
        }

        return {};
    }

    std::string aimForPick (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        const auto kind = kindOf (snapshot, cueId);

        if (kind == "media" || kind == "mic")
            return cueId;

        if (kind == "video")
            return dualOf (snapshot, cueId).sound;

        return {};
    }
}
