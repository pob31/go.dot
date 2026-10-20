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

        /*  WHAT THE AUDIO WAS BUILT WITH, which an edit since has gone past
            (PRD §3.25): the file is there and nothing is wrong with it, so
            "missing" would send somebody looking for the wrong thing. Each
            says where it is put right. */
        if (prepare == "partial" && error == "no-slot")
            return { Icon::missing, "too many slices",
                     "Too many slices: this cue has more than the audio was built with room for on one cue - GO will"
                     " play nothing until Apply while stopped, in Show settings > Audio interface" };

        if (prepare == "partial" && error == "bad-route")
            return { Icon::missing, "no route",
                     "Cannot be routed: its sound is wider than the audio was built to carry on one voice, or it is"
                     " sent where the show has no output - GO will play nothing" };

        if (prepare == "partial" && error == "not-built")
            return { Icon::missing, "not loaded",
                     "Not loaded: its rack channel was added since the audio was built - GO will play nothing until"
                     " Load now, in Show settings > Rack" };

        //  A mic cue's own (Phase 9b), put right in its inspector or the rack.
        if (prepare == "partial" && error == "no-input")
            return { Icon::missing, "no input",
                     "No input: this mic cue takes no named input - GO will play nothing" };

        if (prepare == "partial" && error == "bad-channel")
            return { Icon::missing, "no channel",
                     "No rack channel: it plays through none, or through one another mic cue uses - GO will play nothing" };

        if (prepare == "partial" && error == "bad-width")
            return { Icon::missing, "wrong width",
                     "Wrong width: its input is not as wide as its rack channel takes - GO will play nothing" };

        /*  THE FILE IS THERE, AND SAID SO (namespace draft §99, AXU): until 0.1.9
            both of these read "missing", and sent somebody looking for a file
            that sat in the show's media folder. Words mine, owed the author's. */
        if (prepare == "partial" && error == "not-audio")
            return { Icon::missing, "not audio",
                     "Not audio: the file is in the show's media, and Go.dot cannot read it as sound - GO will play nothing" };

        if (prepare == "partial" && error == "range-outside")
            return { Icon::missing, "outside the file",
                     "Outside the file: a range, or the start offset, begins at or past the end of the file - GO will"
                     " play nothing until it is moved back, in the cue's ranges" };

        if (prepare == "partial" && error == "media-missing")
            return { Icon::missing, "missing",
                     std::string ("Missing: the file is not in the show's media, or Go.dot cannot read it - GO will ")
                         + (picture (kind) ? "show nothing" : "play nothing") };

        //  Any other refusal: said as not ready, never as a file that is missing.
        if (prepare == "partial" && ! error.empty())
            return { Icon::missing, "not ready",
                     "Not ready: Go.dot could not get it ready (" + error + ") - GO will "
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
