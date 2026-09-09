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
    void RunTable::create (std::string id, std::string cueId, std::string kind,
                           std::string parentRun)
    {
        Run run;
        run.id = std::move (id);
        run.cue = std::move (cueId);
        run.kind = std::move (kind);
        run.parent = std::move (parentRun);

        /*  The parent is told about the child, and the child about the parent.
            Both directions, because both questions get asked: a scheduler walks
            down to find what it is waiting for, and a report walks up to find
            who is waiting. */
        if (! run.parent.empty())
            if (auto* parent = find (run.parent))
                parent->children.push_back (run.id);

        runs.push_back (std::move (run));
    }

    std::vector<const Run*> RunTable::childrenOf (const std::string& parentRun) const
    {
        std::vector<const Run*> out;

        for (const auto& run : runs)
            if (run.parent == parentRun)
                out.push_back (&run);

        return out;
    }

    bool RunTable::allChildrenFinished (const std::string& parentRun) const
    {
        return std::all_of (runs.begin(), runs.end(),
                            [&parentRun] (const Run& run)
                            {
                                return run.parent != parentRun || run.isFinished();
                            });
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
            if (run->cue == cueId && ! run->isFinished()
                 && run->state != runState::preparing)
                return &*run;

        return nullptr;
    }

    const Run* RunTable::preparedRunOf (const std::string& cueId) const
    {
        /*  Newest first, for the reason above: a second prepare of the same cue
            supersedes the first, and answering with the older one would adopt
            a run the horizon had already moved past. */
        for (auto run = runs.rbegin(); run != runs.rend(); ++run)
            if (run->cue == cueId && run->state == runState::preparing)
                return &*run;

        return nullptr;
    }

    bool RunTable::hasChildFor (const std::string& parentRun, const std::string& cueId) const
    {
        return std::any_of (runs.begin(), runs.end(),
                            [&parentRun, &cueId] (const Run& run)
                            {
                                return run.parent == parentRun && run.cue == cueId;
                            });
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

    namespace
    {
        bool holds (const Run& run, const std::string& slotId)
        {
            return std::find (run.claims.begin(), run.claims.end(), slotId) != run.claims.end();
        }
    }

    const Run* RunTable::holderOf (const std::string& slotId) const
    {
        if (slotId.empty())
            return nullptr;

        for (const auto& run : runs)
            if (! run.isFinished() && holds (run, slotId))
                return &run;

        return nullptr;
    }

    std::vector<const Run*> RunTable::waitersFor (const std::string& slotId) const
    {
        std::vector<const Run*> waiting;

        if (slotId.empty())
            return waiting;

        for (const auto& run : runs)
            if (! run.isFinished()
                  && std::find (run.pending.begin(), run.pending.end(), slotId)
                       != run.pending.end())
                waiting.push_back (&run);

        return waiting;
    }

    void RunTable::releaseSlotsOf (const std::string& runId)
    {
        auto* run = find (runId);

        if (run == nullptr)
            return;

        const auto released = run->claims;

        run->claims.clear();
        run->pending.clear();

        for (const auto& slotId : released)
        {
            /*  THE HEAD OF THE QUEUE, and creation order is the queue: a run is
                made when the log said so, so two runs that claimed one slot are
                in the order the log made them and a replay hands it to the same
                one. */
            const auto waiting = waitersFor (slotId);

            if (waiting.empty())
                continue;

            auto* next = find (waiting.front()->id);

            if (next == nullptr)
                continue;

            next->pending.erase (std::remove (next->pending.begin(), next->pending.end(), slotId),
                                 next->pending.end());
            next->claims.push_back (slotId);
        }
    }
}
