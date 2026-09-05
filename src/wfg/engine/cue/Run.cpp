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

#include <wfg/engine/cue/Run.h>

#include <algorithm>

namespace wfg::cue
{
    void RunTable::create (std::string id, std::string cueId, std::string kind)
    {
        Run run;
        run.id = std::move (id);
        run.cue = std::move (cueId);
        run.kind = std::move (kind);

        runs.push_back (std::move (run));
    }

    Run* RunTable::find (const std::string& id)
    {
        const auto found = std::find_if (runs.begin(), runs.end(),
                                         [&id] (const Run& run) { return run.id == id; });

        return found == runs.end() ? nullptr : &*found;
    }

    const Run* RunTable::find (const std::string& id) const
    {
        return const_cast<RunTable*> (this)->find (id);
    }

    const Run* RunTable::liveRunOf (const std::string& cueId) const
    {
        /*  The LAST unfinished one, not the first. They are the same thing in
            Phase 2 - a cue has at most one live run - but when Phase 3 makes
            them plural the newest is the one a stop or a fade means, and
            answering with the oldest would be the wrong answer arrived at
            quietly. */
        for (auto run = runs.rbegin(); run != runs.rend(); ++run)
            if (run->cue == cueId && ! run->isFinished())
                return &*run;

        return nullptr;
    }

    bool RunTable::isTrackBusy (int track) const
    {
        return std::any_of (runs.begin(), runs.end(),
                            [track] (const Run& run)
                            {
                                return run.holdsTrack() && run.track == track;
                            });
    }

    int RunTable::lowestFreeTrack (int trackCount) const
    {
        for (int track = 0; track < trackCount; ++track)
            if (! isTrackBusy (track))
                return track;

        return -1;
    }
}
