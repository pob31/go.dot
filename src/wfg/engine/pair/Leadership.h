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
    /*  WHAT A FOLLOWER IS NOT IN STEP WITH, until RD.8 seats it at the leader's
        position: cues that were already running on the leader when it started
        following - which never run here - and what its own clients changed
        while it waited for its main. */
    struct NotInStep
    {
        std::int64_t leaderRuns = 0;
        bool changedWhileWaiting = false;
    };

    /*  `pair/alarm`: the sentence the window and the page put in front of the
        operator when there is something to act on - both machines sending,
        waiting for the main, the other machine lost and this one sending, a
        follower not in step - and empty otherwise. */
    std::string alarm (const Seated& now, const Situation& situation, const NotInStep& behind);
}
