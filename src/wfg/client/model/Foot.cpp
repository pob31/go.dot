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

#include <wfg/client/model/Foot.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    bool followsPick (Subject::Kind kind)
    {
        switch (kind)
        {
            /*  A WAVEFORM FOLLOWS. Clicking the next cue while looking at one
                file's shape means "show me that one", every time - it is the
                same question asked of a different cue, and making somebody
                shut and reopen the panel to ask it would be the panel getting
                in the way of the work.

                What will NOT follow, when they exist, are the subjects that
                are not about the picked cue at all: a rack channel's plugins
                belong to the rack. Each says so here rather than in its own
                editor, so the rule is one list rather than a habit. */
            /*  AND SO DO THE SENDS, for the same reason: "how much of this
                cue goes to the reverb" is a question about whichever cue is
                in hand, and a mixer that stayed pointed at the last one would
                be a mixer that lies. */
            case Subject::Kind::waveform:  return true;
            case Subject::Kind::sends:     return true;

            /*  AND THE TIMELINE FOLLOWS TOO, but only as far as a group: a
                cue picked inside the group being arranged is not a new
                subject, and re-pointing at it would shut the very thing
                somebody is dragging in. `Client` holds it on the group. */
            case Subject::Kind::timeline:  return true;
            case Subject::Kind::none:      break;
        }

        return false;
    }

    FootReading readFoot (const tree::TreeSnapshot& snapshot, const Subject& subject)
    {
        FootReading out;
        out.subject = subject;

        if (! subject.isOpen() || subject.objectId.empty())
            return out;

        const auto at = [&snapshot] (const std::string& address)
        {
            return text (snapshot, address);
        };

        const auto cue = "/godot/cue/" + subject.objectId + "/";

        out.cueName = at (cue + "name");
        out.cueKind = at (cue + "kind");

        if (subject.kind == Subject::Kind::waveform)
        {
            out.file = at (cue + "file");
            out.startOffset = osc::parseDouble (at (cue + "startOffset")).value_or (0.0);
            out.fileLength = osc::parseDouble (at (cue + "duration")).value_or (0.0);
            out.ranges = readRanges (snapshot, subject.objectId);

            /*  WHY THERE IS NOTHING TO DRAW, when there is nothing to draw, in
                the words that say what to do about it. A panel that just sat
                blank would leave somebody wondering whether the file is silent,
                missing, or still being looked at - three different situations
                with three different answers. */
            if (out.cueKind != "media")
                out.notice = "Only a media cue has a waveform.";
            else if (out.file.empty())
                out.notice = "This cue names no file yet.";
            else if (! (out.fileLength > 0.0))
                out.notice = "The length of this file is not known yet - it is read when the show "
                             "opens, so a file imported in this session has none until the show is "
                             "reopened.";
        }

        if (subject.kind == Subject::Kind::timeline)
            out.timeline = readTimeline (snapshot, subject.objectId);

        if (subject.kind == Subject::Kind::sends)
        {
            out.cueLevel = osc::parseDouble (at (cue + "level")).value_or (0.0);
            out.sends = readSends (snapshot, subject.objectId);

            if (out.cueKind != "media")
                out.notice = "Only a media cue has send levels.";
            else if (out.sends.empty())
                out.notice = "This show declares no mix channels yet - Show, Audio settings, "
                             "Outputs, add a mix channel.";
        }

        /*  AND THE PLAYHEAD, from whichever run is sounding this cue. Read from
            the run half rather than from the document, because where a cue has
            got to is a fact about a performance and not about a show. */
        for (const auto* node : snapshot.all())
        {
            constexpr std::string_view runs = "/godot/run/";

            if (node->address.rfind (runs, 0) != 0)
                continue;

            const auto rest = node->address.substr (runs.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos || rest.substr (slash + 1) != "cue")
                continue;

            if (text (node) != subject.objectId)
                continue;

            const auto run = "/godot/run/" + rest.substr (0, slash) + "/";
            const auto state = at (run + "state");

            if (state == "playing" || state == "stopping")
            {
                out.running = true;
                out.runId = rest.substr (0, slash);
                out.position = osc::parseDouble (at (run + "position")).value_or (0.0);
                break;
            }
        }

        return out;
    }
}
