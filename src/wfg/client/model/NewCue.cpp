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

#include <wfg/client/model/NewCue.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/process/PatchText.h>

namespace wfg::client::model
{
    const std::vector<std::string>& cueKinds()
    {
        /*  NO "start" (the author, 2026-09-27): a start cue is made from the
            "+ transport" list, among the other cues that act on a cue. The
            kind is unchanged; only its button went. */
        static const std::vector<std::string> kinds {
            "memo", "media", "mic", "video", "fade", "transport", "osc", "midi", "process", "group"
        };

        return kinds;
    }

    std::vector<std::pair<std::string, std::string>> bornWith (const std::string& kind)
    {
        /*  A PROCESS CUE OPENS ON ONE COMMENT (namespace draft §51) that says how
            a patch reaches the world, rather than on an empty canvas. */
        if (kind == "process")
            return { { "patch", process::starterPatch() } };

        return {};
    }

    std::string kindWord (const std::string& kind)
    {
        return kind == "media" ? std::string ("audio") : kind;
    }

    int positionAfter (const std::string& orderText, const std::string& cueId)
    {
        if (cueId.empty())
            return -1;

        const auto members = words (orderText);

        for (std::size_t at = 0; at < members.size(); ++at)
            if (members[at] == cueId)
                return static_cast<int> (at) + 1;

        return -1;
    }

    bool madeByCreate (const Creation& job, const std::string& kind, const std::string& name)
    {
        return kind == job.kind && name.empty();
    }
}
