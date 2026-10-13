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
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::cue
{
    RunTable::Current::Current (const RunTable& owner)
        : table (&owner)
    {
        /*  THE ORDER MENDED BEFORE A WALK BEGINS, and only when no other walk
            is under way: a run put back during the last one went on the end,
            and a walk in progress must not have its places moved under it. */
        if (table->viewsOut == 0)
            table->reorder();

        ++table->viewsOut;
    }

    RunTable::Current::Current (const Current& other)
        : table (other.table)
    {
        ++table->viewsOut;
    }

    RunTable::Current::~Current()
    {
        --table->viewsOut;
    }

    void RunTable::reorder() const
    {
        if (! unsorted)
            return;

        std::sort (open.begin(), open.end());
        unsorted = false;
    }

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

        /*  THE NEWEST OF ALL, so its place is the end of `current` as it is of
            the vector; and the first made under its name keeps the name, as
            the walk `find` replaced answered with the first. */
        const auto place = runs.size();
        placeOf.emplace (run.id, place);
        newestPlaceOf[run.cue] = place;
        settled.push_back (0);
        open.push_back (place);

        runs.push_back (std::move (run));
    }

    void RunTable::clear()
    {
        runs.clear();
        placeOf.clear();
        newestPlaceOf.clear();
        open.clear();
        unsorted = false;
        settled.clear();
        settledChildren.clear();
    }

    void RunTable::remove (const std::string& id)
    {
        remove (std::set<std::string> { id });
    }

    void RunTable::remove (const std::set<std::string>& ids)
    {
        std::vector<char> going (runs.size(), 0);

        for (const auto& id : ids)
            if (const auto found = placeOf.find (id); found != placeOf.end())
                going[found->second] = 1;

        std::vector<std::pair<std::string, std::string>> taken;
        std::vector<char> settledBefore;
        std::size_t to = 0;

        for (std::size_t from = 0; from < runs.size(); ++from)
        {
            if (going[from] != 0)
            {
                taken.emplace_back (runs[from].id, runs[from].parent);
                continue;
            }

            settledBefore.push_back (settled[from]);

            if (to != from)
                runs[to] = std::move (runs[from]);

            ++to;
        }

        runs.erase (runs.begin() + static_cast<std::ptrdiff_t> (to), runs.end());
        reindex (settledBefore);

        for (const auto& [id, parentRun] : taken)
            if (auto* parent = find (parentRun))
                parent->children.erase (std::remove (parent->children.begin(), parent->children.end(), id),
                                        parent->children.end());
    }

    void RunTable::reindex (const std::vector<char>& settledBefore)
    {
        placeOf.clear();
        newestPlaceOf.clear();
        open.clear();
        unsorted = false;
        settled = settledBefore;
        settledChildren.clear();

        for (std::size_t place = 0; place < runs.size(); ++place)
        {
            const auto& run = runs[place];

            placeOf.emplace (run.id, place);
            newestPlaceOf[run.cue] = place;

            if (settled[place] != 0)
                settledChildren[run.parent].push_back (place);
            else
                open.push_back (place);
        }
    }

    void RunTable::settle (std::int64_t tick)
    {
        //  Never under a walk: it would take places out from under it.
        if (viewsOut > 0)
            return;

        reorder();

        std::size_t to = 0;

        for (const auto place : open)
        {
            const auto& run = runs[place];

            /*  THE TREE'S OWN RULE, and a finished run's only: a run that is not
                over is the tick's whatever its ending tick says - one brought
                back without that tick cleared, or given one in the past to hide
                it from the tree. */
            if (run.isFinished() && run.endedAtTick >= 0 && tick - run.endedAtTick > retentionTicks)
            {
                settled[place] = 1;

                auto& under = settledChildren[run.parent];
                under.insert (std::upper_bound (under.begin(), under.end(), place), place);
                continue;
            }

            open[to++] = place;
        }

        open.resize (to);
    }

    void RunTable::thaw (std::size_t place)
    {
        settled[place] = 0;

        if (const auto under = settledChildren.find (runs[place].parent); under != settledChildren.end())
        {
            auto& places = under->second;
            places.erase (std::remove (places.begin(), places.end(), place), places.end());

            if (places.empty())
                settledChildren.erase (under);
        }

        /*  ON THE END WHILE A WALK IS UNDER WAY, which then still reaches it;
            in its place otherwise. */
        if (viewsOut > 0)
        {
            open.push_back (place);
            unsorted = true;
            return;
        }

        reorder();
        open.insert (std::lower_bound (open.begin(), open.end(), place), place);
    }

    std::vector<const Run*> RunTable::childrenOf (const std::string& parentRun) const
    {
        /*  THE ONES THE TICK STILL READS, by looking - a parent can be moved
            on a run that has not settled (an adoption, a Doh! putting a run
            back where it was) - and the settled ones from where they were
            filed when they settled, which nothing can have moved since.
            Creation order across both. */
        std::vector<std::size_t> places;

        for (const auto place : open)
            if (runs[place].parent == parentRun)
                places.push_back (place);

        std::sort (places.begin(), places.end());

        if (const auto under = settledChildren.find (parentRun); under != settledChildren.end())
        {
            std::vector<std::size_t> merged;
            merged.reserve (places.size() + under->second.size());
            std::merge (places.begin(), places.end(), under->second.begin(), under->second.end(),
                        std::back_inserter (merged));
            places = std::move (merged);
        }

        std::vector<const Run*> out;
        out.reserve (places.size());

        for (const auto place : places)
            out.push_back (&runs[place]);

        return out;
    }

    bool RunTable::askedForUnder (const std::string& runId) const
    {
        const auto under = descendantsOf (runId);

        return std::any_of (under.begin(), under.end(),
                            [] (const Run* run) { return ! run->isFinished() && run->prepare.empty(); });
    }

    bool RunTable::allChildrenFinished (const std::string& parentRun) const
    {
        //  A settled run is finished, so only the ones the tick still reads can say no.
        return std::all_of (open.begin(), open.end(),
                            [this, &parentRun] (std::size_t place)
                            {
                                const auto& run = runs[place];
                                return run.parent != parentRun || run.isFinished();
                            });
    }

    Run* RunTable::find (const std::string& id)
    {
        const auto found = placeOf.find (id);

        if (found == placeOf.end())
            return nullptr;

        if (settled[found->second] != 0)
            thaw (found->second);

        return &runs[found->second];
    }

    const Run* RunTable::find (const std::string& id) const
    {
        const auto found = placeOf.find (id);

        return found == placeOf.end() ? nullptr : &runs[found->second];
    }

    const Run* RunTable::newestOf (const std::string& cueId) const
    {
        const auto found = newestPlaceOf.find (cueId);

        return found == newestPlaceOf.end() ? nullptr : &runs[found->second];
    }

    const Run* RunTable::liveRunOf (const std::string& cueId) const
    {
        /*  The LAST unfinished one, not the first. They are the same thing in
            Phase 2 - a cue has at most one live run - but when Phase 3 makes
            them plural the newest is the one a stop or a fade means, and
            answering with the oldest would be the wrong answer arrived at
            quietly.

            THE NEWEST BY ITS PLACE, not by where it is met: `open` is out of
            order for as long as a walk that put a run back is under way. */
        const Run* newest = nullptr;
        std::size_t newestPlace = 0;

        for (const auto place : open)
        {
            const auto& run = runs[place];

            if (run.cue == cueId && ! run.isFinished() && run.state != runState::preparing
                  && (newest == nullptr || place > newestPlace))
            {
                newest = &run;
                newestPlace = place;
            }
        }

        return newest;
    }

    const Run* RunTable::preparedRunOf (const std::string& cueId) const
    {
        /*  Newest first, for the reason above: a second prepare of the same cue
            supersedes the first, and answering with the older one would adopt
            a run the horizon had already moved past. */
        const Run* newest = nullptr;
        std::size_t newestPlace = 0;

        for (const auto place : open)
        {
            const auto& run = runs[place];

            if (run.cue == cueId && run.state == runState::preparing
                  && (newest == nullptr || place > newestPlace))
            {
                newest = &run;
                newestPlace = place;
            }
        }

        return newest;
    }

    std::vector<const Run*> RunTable::descendantsOf (const std::string& parentRun) const
    {
        std::vector<const Run*> out;

        for (const auto* child : childrenOf (parentRun))
        {
            out.push_back (child);

            for (const auto* deeper : descendantsOf (child->id))
                out.push_back (deeper);
        }

        return out;
    }

    bool RunTable::hasChildFor (const std::string& parentRun, const std::string& cueId) const
    {
        const auto children = childrenOf (parentRun);

        return std::any_of (children.begin(), children.end(),
                            [&cueId] (const Run* run) { return run->cue == cueId; });
    }

    bool RunTable::isTrackBusy (int track) const
    {
        return std::any_of (open.begin(), open.end(),
                            [this, track] (std::size_t place)
                            {
                                const auto& run = runs[place];
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

        //  The first in creation order, as the walk of every run met it.
        const Run* first = nullptr;
        std::size_t firstPlace = 0;

        for (const auto place : open)
        {
            const auto& run = runs[place];

            if (! run.isFinished() && holds (run, slotId) && (first == nullptr || place < firstPlace))
            {
                first = &run;
                firstPlace = place;
            }
        }

        return first;
    }

    std::vector<const Run*> RunTable::waitersFor (const std::string& slotId) const
    {
        std::vector<const Run*> waiting;

        if (slotId.empty())
            return waiting;

        std::vector<std::size_t> places;

        for (const auto place : open)
        {
            const auto& run = runs[place];

            if (! run.isFinished()
                  && std::find (run.pending.begin(), run.pending.end(), slotId) != run.pending.end())
                places.push_back (place);
        }

        //  Creation order is the queue, whatever order `open` is in.
        std::sort (places.begin(), places.end());

        for (const auto place : places)
            waiting.push_back (&runs[place]);

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
            std::string nextId;

            if (! waiting.empty())
                if (auto* next = find (waiting.front()->id))
                {
                    next->pending.erase (std::remove (next->pending.begin(), next->pending.end(), slotId),
                                         next->pending.end());
                    next->claims.push_back (slotId);
                    nextId = next->id;
                }

            if (onRelease)
                onRelease (slotId, runId, nextId);
        }
    }
}
