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

#include <wfg/client/model/Readiness.h>
#include <wfg/client/model/Text.h>

namespace wfg::client::model
{
    namespace
    {
        bool picture (const std::string& kind)
        {
            return kind == "video";
        }

        /*  WHAT IS GOT READY, in the row's own terms: a picture is read, a
            sound has its voice and its file, a scene its first cues. */
        std::string whatIsReady (const std::string& kind)
        {
            if (picture (kind))
                return "read ahead - GO shows it at once";

            if (kind == "group")
                return "what it starts first is got ready - GO starts it at once";

            return "its voice is held and its file opened - GO plays it at once";
        }
    }

    Mark readinessMark (const std::string& prepare, const std::string& error, const std::string& kind)
    {
        if (prepare.empty() || prepare == "idle")
            return {};

        if (prepare == "preparing")
            return { Icon::loading, {},
                     picture (kind) ? "Getting ready: Go.dot is reading the file ahead of GO"
                                    : "Getting ready: Go.dot is making it ready ahead of GO" };

        if (prepare == "pending")
            return { Icon::loading, {}, "Waiting: a voice or a slot another cue holds - it is got ready when that one ends" };

        if (prepare == "armed" || prepare == "verified")
            return { Icon::ready, {}, "Ready: " + whatIsReady (kind) };

        if (prepare == "partial" && error == "no-track")
            return { Icon::missing, "no voice", "No voice: every track was busy when it was got ready - GO will not play it" };

        /*  A SOUND OR A MOVIE WHOSE EDIT HAS NO RENDER YET (namespace draft §55,
            ADM; §55.5): the
            renderer is making it; the cue is got ready by itself once it has. */
        if (prepare == "partial" && error == "rendering")
            return { Icon::loading, "rendering",
                     "Rendering the edit: Go.dot is making what the cue plays or shows, and gets it ready once that is there" };

        if (prepare == "partial" && ! error.empty())
            return { Icon::missing, "missing",
                     std::string ("Missing: the file is not in the show's media, or Go.dot cannot read it - GO will ")
                         + (picture (kind) ? "show nothing" : "play nothing") };

        if (prepare == "partial")
            return { Icon::partly, {}, "Partly ready: some of it could not be got ready ahead and will happen at GO" };

        return {};
    }

    std::map<std::string, Mark> readinessOf (const tree::TreeSnapshot& snapshot, const std::vector<Row>& rows)
    {
        std::map<std::string, Mark> out;

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.derived || row.id.empty())
                continue;

            if (row.kind != "media" && row.kind != "mic" && row.kind != "video" && row.kind != "group")
                continue;

            const auto base = "/godot/cue/" + row.id + "/";
            auto mark = readinessMark (text (snapshot, base + "prepare"), text (snapshot, base + "prepareError"), row.kind);

            if (mark.icon != Icon::none)
                out.emplace (row.id, std::move (mark));
        }

        return out;
    }
}
