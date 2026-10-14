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

#include <wfg/engine/pair/Summary.h>

#include <wfg/engine/pair/PeerLink.h>

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

namespace wfg::pair
{
    namespace
    {
        std::string labelOf (const std::function<std::string (const std::string&)>& say, const std::string& id)
        {
            if (say)
                if (auto said = say (id); ! said.empty())
                    return said;

            return id;
        }

        std::string spelledMs (std::int64_t ticks)
        {
            //  A tick is twenty milliseconds at the engine's fifty a second (TickClock::rateHz).
            return std::to_string (ticks * 20) + " ms";
        }
    }

    //==========================================================================
    std::vector<std::uint8_t> summaryBytes (const Summary& summary)
    {
        std::vector<osc::Value> args;
        args.reserve (5 + 2 * summary.standby.size() + 6 * summary.runs.size());

        args.push_back (osc::Value::int64 (summary.tick));
        args.push_back (osc::Value::int64 (static_cast<std::int64_t> (summary.counter)));
        args.push_back (osc::Value::int32 (static_cast<std::int32_t> (summary.standby.size())));

        for (const auto& standby : summary.standby)
        {
            args.push_back (osc::Value::string (standby.list));
            args.push_back (osc::Value::string (standby.cue));
        }

        for (const auto& run : summary.runs)
        {
            args.push_back (osc::Value::string (run.id));
            args.push_back (osc::Value::string (run.cue));
            args.push_back (osc::Value::string (run.state));
            args.push_back (osc::Value::int64 (run.playhead));
            args.push_back (osc::Value::int32 (run.range));
            args.push_back (osc::Value::int64 (run.loop));
        }

        //  RD.10's two, after the runs: two strings can never be read as a run, which is six.
        args.push_back (osc::Value::string (summary.showFingerprint));
        args.push_back (osc::Value::string (summary.mediaDigest));

        return wire::encode (wire::summary, std::move (args));
    }

    std::optional<Summary> readSummary (const osc::Packet& message)
    {
        const auto& a = message.args;

        if (message.isBundle() || message.address != wire::summary || a.size() < 3
              || ! a[0].isInt64() || ! a[1].isInt64() || a[1].getInt64() < 0 || ! a[2].isInt32() || a[2].getInt32() < 0)
            return std::nullopt;

        const auto lists = static_cast<std::size_t> (a[2].getInt32());
        const auto rest = a.size() - 3;

        if (rest < 2 * lists)
            return std::nullopt;

        //  The runs, six each, and RD.10's show after them - or, from an RD.8 machine, nothing after.
        const auto tail = (rest - 2 * lists) % 6;

        if (tail != 0 && tail != 2)
            return std::nullopt;

        Summary out;
        out.tick = a[0].getInt64();
        out.counter = static_cast<std::uint64_t> (a[1].getInt64());

        std::size_t at = 3;

        for (std::size_t n = 0; n < lists; ++n, at += 2)
        {
            if (! a[at].isString() || ! a[at + 1].isString())
                return std::nullopt;

            out.standby.push_back ({ a[at].getString(), a[at + 1].getString() });
        }

        for (; at + tail < a.size(); at += 6)
        {
            if (! a[at].isString() || ! a[at + 1].isString() || ! a[at + 2].isString()
                  || ! a[at + 3].isInt64() || ! a[at + 4].isInt32() || ! a[at + 5].isInt64())
                return std::nullopt;

            out.runs.push_back ({ a[at].getString(), a[at + 1].getString(), a[at + 2].getString(),
                                  a[at + 3].getInt64(), a[at + 4].getInt32(), a[at + 5].getInt64() });
        }

        if (tail == 2)
        {
            if (! a[at].isString() || ! a[at + 1].isString())
                return std::nullopt;

            out.showFingerprint = a[at].getString();
            out.mediaDigest = a[at + 1].getString();
        }

        return out;
    }

    //==========================================================================
    std::string differenceOf (const Summary& leader, const Summary& here, const Labels& labels,
                              std::int64_t toleranceTicks)
    {
        const auto cueOf = [&labels] (const std::string& cueId) { return "cue " + labelOf (labels.cue, cueId); };
        const auto& there = labels.leader;

        /*  THE STANDBY FIRST: it is what the next GO acts on, and two machines
            whose next GO would fire different cues are apart however their
            sounds agree now. */
        for (const auto& theirs : leader.standby)
        {
            std::string mine;

            for (const auto& own : here.standby)
                if (own.list == theirs.list)
                    mine = own.cue;

            if (mine != theirs.cue)
                return "standby differs on list " + labelOf (labels.list, theirs.list);
        }

        //  Then the runs: the leader's in its order, then any of this machine's it does not have.
        std::map<std::string_view, const Summary::Run*> ours;

        for (const auto& own : here.runs)
            ours.emplace (own.id, &own);

        for (const auto& theirs : leader.runs)
        {
            const auto found = ours.find (theirs.id);

            if (found == ours.end())
                return cueOf (theirs.cue) + " is running on " + there + ", not here";

            const auto& mine = *found->second;

            if (mine.state != theirs.state)
                return cueOf (theirs.cue) + " is " + theirs.state + " on " + there + " and " + mine.state + " here";

            if (mine.range != theirs.range)
                return cueOf (theirs.cue) + " plays range " + std::to_string (theirs.range + 1) + " on " + there
                     + " and range " + std::to_string (mine.range + 1) + " here";

            auto apart = mine.playhead > theirs.playhead ? mine.playhead - theirs.playhead
                                                          : theirs.playhead - mine.playhead;

            //  Round the loop, for a slice that loops: the same point of it is the same sound.
            if (const auto loop = std::max (mine.loop, theirs.loop); loop > 0)
            {
                apart %= loop;
                apart = std::min (apart, loop - apart);
            }

            if (apart > toleranceTicks)
                return cueOf (theirs.cue) + "'s playhead is " + spelledMs (apart) + " apart";
        }

        if (here.runs.size() != leader.runs.size())
        {
            std::map<std::string_view, bool> theirs;

            for (const auto& run : leader.runs)
                theirs.emplace (run.id, true);

            for (const auto& own : here.runs)
                if (theirs.count (own.id) == 0)
                    return cueOf (own.cue) + " is running here, not on " + there;
        }

        return {};
    }

    //==========================================================================
    void InStep::heard (Summary summary)
    {
        /*  A few seconds of them at most: more is a follower whose ticks have
            stopped reading them, and the oldest is the least use. */
        if (held.size() >= 256)
            held.pop_front();

        held.push_back (std::move (summary));
    }

    void InStep::seated (std::int64_t settledFrom)
    {
        settleFrom = settledFrom;
        missesInRow = 0;
    }

    void InStep::recordDiffered (std::string said)
    {
        matched = false;
        missesInRow = 0;
        sentence = std::move (said);
    }

    void InStep::afterTick (std::int64_t localTick, std::int64_t offset, bool comparable,
                            const std::function<Summary()>& own, const Labels& labels)
    {
        /*  NOTHING TO LAY THEM AGAINST - no delay fixed yet, a seat on its way:
            every one held is of a moment that will not be compared, and is let
            go now rather than waiting for a slot read on a clock not yet fixed. */
        if (! comparable)
        {
            held.clear();
            return;
        }

        while (! held.empty())
        {
            const auto slot = held.front().tick + offset;

            if (slot > localTick)
                return;

            /*  ITS SLOT PASSED, OR IN A SEAT'S SETTLING: a summary that arrived
                late reads a moment this machine has already left, and one of
                the ticks a seat's sounds take to start would say only that they
                have not started yet. */
            if (slot < localTick || slot < settleFrom || ! own)
            {
                held.pop_front();
                continue;
            }

            const auto leader = std::move (held.front());
            held.pop_front();

            auto difference = differenceOf (leader, own(), labels);
            ++comparedCount;

            if (difference.empty())
            {
                matched = true;
                missesInRow = 0;
                sentence.clear();
                continue;
            }

            ++mismatchCount;

            /*  TWICE IN A ROW BEFORE IT IS SAID: once can be a launch a tick
                apart on the two machines, or a loop point read either side. */
            if (++missesInRow >= 2)
            {
                matched = false;
                sentence = std::move (difference);
            }
        }
    }

    void InStep::reset()
    {
        held.clear();
        settleFrom = 0;
        matched = false;
        missesInRow = 0;
        sentence.clear();
        comparedCount = 0;
        mismatchCount = 0;
    }
}
