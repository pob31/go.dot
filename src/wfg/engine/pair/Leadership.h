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
    WHO SENDS TO THE DEVICES (namespace draft §58, RD.7; decisions AFQ, AFR,
    AFV, AGA, AGJ, AGK): the one place that decides what this machine is in
    its pair, from what it knows - its role, whether the other machine is
    there and what that one says it is, whether this one has run anything
    since it started, whether the stream it follows still comes - and from
    what an operator chose, here or on the other machine.

    FIVE SEATS (`pair/state`):

      alone        sends; not paired, or a main with no backup linked
      leading      sends, as the pair's leader: a follower is linked, or this
                   is a backup that took over (a follower may come back)
      following    holds; applies the leader's stream a fixed delay later,
                   and passes its own clients' commands on to it (RD.6)
      waiting      holds; a backup that has never found its main since it
                   started (AGK): its own clients' commands are applied here,
                   held, and it offers to take over
      bothSending  sends; two machines that had each been sending found each
                   other (AFQ): each sends, and each applies the other's
                   operators' commands, so a GO on either reaches the devices
                   from both - until an operator on either picks one

    PURE: functions of the seat the log last recorded and of the situation
    this tick, so a test lays every seat against every event as a table. Serve
    asks them once a tick, at the head of the tick, and a move they answer is
    RECORDED - `pair.lead` when the new seat sends, `pair.hold` when it holds,
    with the role, the seat and the reason - and takes effect when that record
    is applied: the seat is what the log says it is (`Seated`, set by the
    records' own handlers in PairSettings.cpp), so a replay knows it as the
    session did, and refuses an operator's `pair.takeOver` exactly where the
    session refused it.
*/

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::pair
{
    enum class Seat { alone, leading, following, waiting, bothSending };
    enum class Role { none, main, backup };

    /** `pair/state`'s words, and back. */
    const char* wordOf (Seat seat) noexcept;
    std::optional<Seat> seatNamed (std::string_view word) noexcept;

    /** `pair/role`'s words, and back; anything else is `none`. */
    const char* wordOf (Role role) noexcept;
    Role roleNamed (std::string_view word) noexcept;

    /** Whether a seat sends to the devices. */
    bool sends (Seat seat) noexcept;

    /*  Whether a seat applies the other machine's stream: following, and both
        sending, where each applies the other's operators' commands. */
    bool follows (Seat seat) noexcept;

    /*  Whether a seat closes the engine's gate (RD.6): following alone - its
        clients' commands passed on, its devices' ignored. A waiting backup and
        two machines both sending apply their own. */
    bool gated (Seat seat) noexcept;

    //==========================================================================
    /*  AN OPERATOR'S CHOICE ON THE OTHER MACHINE, told to this one (§58.4):
        `hold` - it took over, or kept the show: hold and follow it; `lead` -
        it handed the show over: send. Carried by `/godot/pair/order`. */
    enum class Order { hold, lead };

    struct Ordered
    {
        Order order = Order::hold;
        std::string reason;         // the other machine's own: take-over, keep, hand-over
    };

    /*  WHAT THE LOG LAST SAID THIS MACHINE IS: the arguments of the last
        `pair.lead` or `pair.hold` applied. Before any, a machine not paired,
        alone, which is what a machine whose role is `none` stays. */
    struct Seated
    {
        Role role = Role::none;
        Seat seat = Seat::alone;
        std::string reason;
    };

    /*  WHAT THIS MACHINE KNOWS THIS TICK, built by serve and by the tests. */
    struct Situation
    {
        Role role = Role::none;                 // in force (pairing.xml and the options)

        //  The other machine.
        bool linked = false;                    // its engine heard within the timeout (pair/Presence.h)
        std::optional<Seat> peerSeat;           // what its last hello said it is; none before one
        bool peerFresh = false;                 // it has applied no GO and run no cue since it started
        Role peerRole = Role::none;
        std::string peerStream;                 // its own stream's identity

        //  This machine.
        bool fresh = true;                      // AGA: no GO applied, no cue run, since it started
        std::string stream;                     // its own stream's identity, for a tie between two fresh mains

        /*  While following: whether the leader's stream still comes - a beat
            within the timeout, or the follow began less than a timeout ago -
            and whether the leader says a stream other than the one followed,
            which is a leader started again (a crash and a quick restart). */
        bool streamLive = true;
        bool leaderChanged = false;
    };

    /*  A MOVE: the seat this machine takes, and why - the record's arguments
        - and what the other machine is told, if anything. */
    struct Move
    {
        Role role = Role::none;
        Seat to = Seat::alone;
        std::string reason;
        std::optional<Order> tell;

        /** `pair.lead` when the new seat sends, `pair.hold` when it holds. */
        const char* command() const noexcept { return sends (to) ? "pair.lead" : "pair.hold"; }
    };

    /*  WHAT THE SITUATION CALLS FOR, with no operator in it: a role that
        changed, the other machine found or lost, a stream that stopped (AFV:
        the follower takes over), a leader started again, two machines that
        each send (AFQ), a fresh machine finding one that sends (AFR), a
        waiting backup finding its main (AGK). Nothing when the seat stands. */
    std::optional<Move> decide (const Seated& now, const Situation& situation);

    /*  AN OPERATOR'S COMMAND, already accepted (`refusal` empty): `pair.takeOver`,
        `pair.handOver`, `pair.keep`. Nothing when the moment has passed - the
        other machine went between the command and this tick. */
    enum class Ask { takeOver, handOver, keep };

    std::optional<Move> asked (const Seated& now, Ask ask, const Situation& situation);

    /** What the other machine's operator chose, told to this one. */
    std::optional<Move> ordered (const Seated& now, const Ordered& order, const Situation& situation);

    /*  WHY AN OPERATOR'S COMMAND IS REFUSED, from the seat the log last said
        and the other machine's seat as the command was applied (`peerSeat`,
        empty when none was linked - the command's argument as applied, so a
        replay decides alike): `not-paired`, `not-holding` (take over what
        already sends), `not-sending` (hand over what holds), `not-linked`
        (hand over to nobody), `not-both-sending` (keep outside AFQ). Empty
        when it is accepted. */
    std::string refusal (const Seated& now, Ask ask, std::string_view peerSeat);

    //==========================================================================
    /*  WHAT A FOLLOWER IS NOT IN STEP WITH: cues that were already running on
        the leader when it started following, and what its own clients changed
        while it waited for its main - until it is seated at the leader's
        position (RD.8), which `seating` says is on its way; and once seated,
        what the leader's summaries say differs (`differs`, pair/Summary.h),
        empty while the two are in step. */
    struct NotInStep
    {
        std::int64_t leaderRuns = 0;
        bool changedWhileWaiting = false;
        bool seating = false;
        std::string differs = {};

        /*  AND WHAT DIFFERS BETWEEN THE TWO SHOWS (RD.10, pair/ShowCheck.h):
            the sentence, and whether the show itself differs - which taking
            the leader's mends - or only its media, which nothing sends. */
        std::string showDiffers = {};
        bool showTextDiffers = false;
    };

    //==========================================================================
    /*  WHAT A FOLLOWER SAYS OF ITSELF, UP THE LINK (namespace draft §58,
        RD.12): only the follower compares (RD.8's summaries, RD.10's
        fingerprints), so the machine that leads - the one an operator is
        running the show on - would otherwise never know that its follower
        would not be a safe changeover until somebody walked to the follower's
        screen. The follower says its two verdicts and their sentences, as its
        own rows read them, and its seat; the leader publishes them as its
        `pair/peer*` rows and puts the bad news in its alarm.

        Each verdict is yes, no or not known: in step once a summary matched,
        not in step once one said what differs, not known before either (a
        follow just begun, a seat on its way); the same show once the
        fingerprints and the files agree, not once something was named, not
        known before - and while the follower's own fingerprint is not of the
        show it has now, a moment after every edit. The sentences are the
        follower's own words - "cue Bed is running on the main, not here", "the
        show differs: cue 4 Rain changed" - so "here" in them is the follower. */
    struct FollowerVerdict
    {
        std::optional<bool> inStep = std::nullopt;
        std::string differs = {};
        std::optional<bool> showMatch = std::nullopt;
        std::string showDiffers = {};
        std::string seat = {};                  // the follower's `pair/state` word

        bool operator== (const FollowerVerdict& other) const = default;
    };

    /*  A VERDICT AS THE `pair/peer*` ROWS SAY IT: `yes`, `no`, or `unknown`
        for one not known - which is also what a leader with no follower, or
        whose follower has not spoken within the link's timeout, reads. */
    const char* verdictWord (std::optional<bool> said) noexcept;

    /*  `pair/alarm`: the sentence the window and the page put in front of the
        operator when there is something to act on - both machines sending,
        waiting for the main, the other machine lost and this one sending, a
        follower not in step - and empty otherwise. On a machine that sends,
        what its follower said of itself (RD.12) after its own: the follower's
        show not this one's, or the follower not in step, in the follower's
        words, with what mends it - on the follower. */
    std::string alarm (const Seated& now, const Situation& situation, const NotInStep& behind,
                       const FollowerVerdict& follower = {});
}
