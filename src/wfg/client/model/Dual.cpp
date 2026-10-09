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
#include <wfg/engine/tree/DualCue.h>
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
    }

    /*  THE ENGINE TREE'S RULE (tree/DualCue), the one the surfaces' SELECT
        asks too (namespace draft §49, ABG), so a pick and a press aim at the
        same sound. */
    Dual dualOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        const auto pair = tree::dualCueOf (snapshot, cueId);
        return { pair.movie, pair.sound };
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
