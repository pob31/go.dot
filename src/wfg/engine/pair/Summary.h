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

#pragma once

/*
    WHETHER TWO MACHINES ARE IN STEP (namespace draft §58, RD.8).

    A follower applies its leader's records a fixed delay later (RD.6), and
    from RD.8 it is seated at the leader's position when it starts following.
    Neither says it IS where the leader is: a sound whose file is missing
    here, a record refused here for a reason of this machine's own (AFZ), a
    hand on this machine's own page while it waited (AGK) - each leaves the two
    apart, and nothing in the stream would say so. So every five ticks the
    leader sends what an operator would look at - each list's standby and
    each cue running, with its state and its playhead - and the follower lays
    it against its own at the same moment of the record clock, and says in one
    sentence the first thing that differs.

    THE SAME MOMENT. A summary of the leader's tick T is held until the
    follower's tick T + offset - the slot every record of T was applied in -
    and compared after that tick has run, against what this machine has then.
    So everything on the record clock compares exactly: a GO the leader took at
    T made its runs here at T + offset, under the same names (AGE), and their
    playheads have run for as long.

    THE PLAYHEAD'S TOLERANCE IS TWO TICKS. Each machine reads a playhead once a
    tick off its own sample clock, and the two machines' ticks fall wherever
    they fall against their clocks - up to a tick apart; and each places a
    launch on a tick of its own - up to another. Forty milliseconds covers
    both, and is still well inside what an operator hears as one cue. Rounded
    to a tick on the wire, so the summary is integers.

    TWO SUMMARIES IN A ROW. A GO launches its sound on each machine on a tick
    of its own, so for a tick or two one reads `armed` where the other reads
    `playing`, and a slice crossing its loop point may be read on either side
    of it. A difference is said only when the next summary - a tenth of a
    second later - shows one too; a summary that matches says in step at once.

    ONE SENTENCE FOR `pair/differs`, THE MOST RECENT CAUSE. RD.6 counted the
    leader's records this machine applied otherwise and said the first; RD.8's
    summaries say whether the two are where each other is. They are merged: a
    record applied otherwise says so at once (the sentence RD.6 wrote, with its
    count), a summary that does not match says what differs, and a summary
    that matches clears both - two machines whose run tables, playheads and
    standbys agree are in step, whatever a record on the way did.
*/

#include <wfg/engine/osc/OscCodec.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wfg::pair
{
    /** How often a leader sends one, in ticks: a tenth of a second at 50 Hz. */
    inline constexpr std::int64_t summaryEveryTicks = 5;

    /** How far two playheads may be apart and still be in step, in ticks. */
    inline constexpr std::int64_t playheadToleranceTicks = 2;

    //==========================================================================
    /*  WHAT A MACHINE IS DOING, AS AN OPERATOR WOULD LOOK AT IT: each list's
        standby, and every cue running - neither finished nor only made ready
        by the horizon, which each machine does for itself - with its state and
        its playhead in ticks: seconds into its own material for a sound (into
        the slice, passes and all, for one playing a slice), into its scene for
        a group, since its GO for anything else; for a run in a wait, the ticks
        still to run, negative. */
    struct Summary
    {
        struct Standby
        {
            std::string list, cue;
        };

        struct Run
        {
            std::string id, cue, state;
            std::int64_t playhead = 0;
            int range = -1;

            /*  ONE PASS OF THE SLICE IT LOOPS, in ticks, nought for none: its
                playhead is compared round the loop. A follower seated in a
                slice the leader has looped for longer than the follower's own
                clock has run counts its passes again from the one it is in
                (Runner::launchIfDue, "a session younger than that"), and its
                playhead is the same point of the loop - the same sound. */
            std::int64_t loop = 0;
        };

        std::int64_t tick = 0;          // the machine's own tick it describes
        std::uint64_t counter = 0;      // the last record of its stream applied by then
        std::vector<Standby> standby;
        std::vector<Run> runs;

        /*  ITS SHOW (namespace draft §58, RD.10; pair/ShowCheck.h): its
            fingerprint and its media list's digest, as of this tick - each
            empty while the machine has not worked them out for the show it
            has now. A follower lays them against its own at the same slot as
            the rest, so a show both machines changed alike compares alike. */
        std::string showFingerprint;
        std::string mediaDigest;
    };

    /*  `/godot/pair/summary h:tick h:counter i:lists [s:list s:cue]...
        [s:run s:cue s:state h:playhead i:range h:loop]...
        s:showFingerprint s:mediaDigest`: one message, a few dozen bytes a run.
        A summary of RD.8's, with no show after its runs, still reads. */
    std::vector<std::uint8_t> summaryBytes (const Summary& summary);
    std::optional<Summary> readSummary (const osc::Packet& message);

    /*  HOW A SENTENCE NAMES WHAT IT IS ABOUT: a cue as the operator reads it
        (`cue 12`), a list by its name, the other machine by its role (`the
        main`). Any may be left empty, and the identifier is said instead. */
    struct Labels
    {
        std::function<std::string (const std::string& cueId)> cue;
        std::function<std::string (const std::string& listId)> list;
        std::string leader = "the main";
    };

    /*  THE FIRST THING THAT DIFFERS between the leader's summary and this
        machine's own, in one sentence - "cue 12 is running on the main, not
        here", "standby differs on list Main", "cue 7's playhead is 60 ms
        apart" - or empty when the two are in step. */
    std::string differenceOf (const Summary& leader, const Summary& here, const Labels& labels,
                              std::int64_t toleranceTicks = playheadToleranceTicks);

    //==========================================================================
    /*  THE FOLLOWER'S HALF, the tick thread's: the leader's summaries held to
        their slot and compared there, and what that says. */
    class InStep
    {
    public:
        /** A summary of the leader's, as it arrived. */
        void heard (Summary summary);

        /*  A SEAT WAS APPLIED (pair.seat): the summaries whose slot is before
            `settledFrom` - the ticks its sounds take to start - are passed over
            rather than compared, and nothing known before the seat counts. */
        void seated (std::int64_t settledFrom);

        /*  ONE OF THE LEADER'S RECORDS CAME OUT OTHERWISE HERE (RD.6): said at
            once, the most recent cause; the next summary that matches clears it. */
        void recordDiffered (std::string sentence);

        /*  AFTER LOCAL TICK `localTick` HAS RUN: the summary whose slot it is -
            the leader's tick plus `offset` - is compared with this machine's
            own, made by `own`; one whose slot has passed is dropped. With
            `comparable` false (no delay fixed yet, a seat on its way) every one
            held is dropped unread. */
        void afterTick (std::int64_t localTick, std::int64_t offset, bool comparable,
                        const std::function<Summary()>& own, const Labels& labels);

        /** A follow that starts or stops: nothing known, not in step. */
        void reset();

        bool inStep() const noexcept                { return matched; }
        const std::string& differs() const noexcept { return sentence; }

        /** Summaries compared since the last reset, and of them, those that did not match. */
        std::int64_t compared() const noexcept      { return comparedCount; }
        std::int64_t mismatched() const noexcept    { return mismatchCount; }

        /** Summaries held, waiting for their slot. */
        std::size_t waiting() const noexcept        { return held.size(); }

    private:
        std::deque<Summary> held;
        std::int64_t settleFrom = 0;
        bool matched = false;
        int missesInRow = 0;
        std::string sentence;
        std::int64_t comparedCount = 0, mismatchCount = 0;
    };
}
