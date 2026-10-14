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

/*
    WHO SENDS TO THE DEVICES (namespace draft §58, RD.7; AFQ, AFR, AFV, AGA,
    AGJ, AGK).

    What is pinned here: the state machine as a TABLE - every seat, for a
    main and a backup, against every event: the other machine found
    following, found sending while this one has run nothing, two machines
    that each sent finding each other, a fresh machine that sends, a waiting
    one, the other machine gone, the stream silent, a leader started again,
    the operator's three commands and the other machine's two orders - and
    what each seat does (sends or holds, applies the other's stream, closes
    the gate, the record it is logged by), the refusals, the alarms, a role
    that changes, the tie between two machines that have both run nothing;
    the commands as records, replayed alike; AGJ - what a follower passed on
    and never saw answered, settled at a takeover: Esc applied once, a GO
    dropped and counted, a relay the leader answered never applied again;
    the link symmetric - a backup that leads streams to a main that follows,
    which applies and holds, and a device hears every GO once; and what the
    pair costs a tick.

    Every case runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/command/Recovery.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/Follow.h>
#include <wfg/engine/pair/Hold.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/pair/MachineLocal.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/pair/PeerLink.h>
#include <wfg/engine/pair/Presence.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using Clock = pair::LinkClock;
    using namespace std::chrono_literals;
    using pair::Order;
    using pair::Role;
    using pair::Seat;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }

    const Clock::time_point t0 = Clock::time_point {} + std::chrono::hours { 1000 };

    //==========================================================================
    /*  THE EVENTS OF THE TABLE: what this machine finds this tick, or what an
        operator - here, or on the other machine - chose. */
    enum class Ev
    {
        followerLinked,     // the other machine is there and follows this one
        leaderFoundFresh,   // it leads and has run cues; this one has run nothing (AFR)
        twoLeaders,         // it leads, and both have run cues (AFQ)
        freshPeerSends,     // it sends but has run nothing: it will follow this one
        peerWaiting,        // it is a backup waiting for its main
        peerGone,           // not heard within the timeout; a stream followed still in its grace
        streamLost,         // not heard, and the stream followed silent for the timeout (AFV)
        leaderRestarted,    // the leader says another stream: started again since
        takeOver, handOver, keep,
        orderHold,          // the other machine took over: hold and follow it
        orderLead           // the other machine handed the show over: send
    };

    const char* nameOf (Ev ev)
    {
        switch (ev)
        {
            case Ev::followerLinked:   return "follower linked";
            case Ev::leaderFoundFresh: return "a leader found, this one fresh";
            case Ev::twoLeaders:       return "two leaders meet";
            case Ev::freshPeerSends:   return "a fresh machine sends";
            case Ev::peerWaiting:      return "the other waits";
            case Ev::peerGone:         return "the other gone, the stream in its grace";
            case Ev::streamLost:       return "the link lost";
            case Ev::leaderRestarted:  return "the leader started again";
            case Ev::takeOver:         return "pair.takeOver";
            case Ev::handOver:         return "pair.handOver";
            case Ev::keep:             return "pair.keep";
            case Ev::orderHold:        return "told to hold";
            case Ev::orderLead:        return "told to lead";
        }

        return "?";
    }

    pair::Situation situationFor (Role role, Ev ev)
    {
        pair::Situation situation;
        situation.role = role;
        situation.fresh = false;
        situation.stream = "S-this";
        situation.peerStream = "S-other";
        situation.peerRole = role == Role::main ? Role::backup : Role::main;
        situation.linked = true;
        situation.peerSeat = Seat::following;

        switch (ev)
        {
            case Ev::followerLinked:
                break;

            case Ev::leaderFoundFresh:
                situation.peerSeat = Seat::leading;
                situation.fresh = true;
                break;

            case Ev::twoLeaders:
                situation.peerSeat = Seat::leading;
                break;

            case Ev::freshPeerSends:
                situation.peerSeat = Seat::alone;
                situation.peerFresh = true;
                break;

            case Ev::peerWaiting:
                situation.peerSeat = Seat::waiting;
                break;

            case Ev::peerGone:
                situation.linked = false;
                break;

            case Ev::streamLost:
                situation.linked = false;
                situation.streamLive = false;
                break;

            case Ev::leaderRestarted:
                situation.peerSeat = Seat::alone;
                situation.peerFresh = true;
                situation.leaderChanged = true;
                break;

            case Ev::takeOver: case Ev::handOver: case Ev::keep: case Ev::orderHold: case Ev::orderLead:
                break;
        }

        return situation;
    }

    std::optional<pair::Move> happen (const pair::Seated& now, Ev ev)
    {
        const auto situation = situationFor (now.role, ev);

        switch (ev)
        {
            case Ev::takeOver:  return pair::asked (now, pair::Ask::takeOver, situation);
            case Ev::handOver:  return pair::asked (now, pair::Ask::handOver, situation);
            case Ev::keep:      return pair::asked (now, pair::Ask::keep, situation);
            case Ev::orderHold: return pair::ordered (now, { Order::hold, "take-over" }, situation);
            case Ev::orderLead: return pair::ordered (now, { Order::lead, "hand-over" }, situation);

            case Ev::followerLinked: case Ev::leaderFoundFresh: case Ev::twoLeaders: case Ev::freshPeerSends:
            case Ev::peerWaiting: case Ev::peerGone: case Ev::streamLost: case Ev::leaderRestarted:
                return pair::decide (now, situation);
        }

        return std::nullopt;
    }

    struct Row
    {
        Role role;
        Seat seat;
        Ev event;
        std::optional<Seat> to;
        const char* reason;
        std::optional<Order> tell;
    };

    constexpr auto none = std::nullopt;
}

//==============================================================================
TEST_CASE ("leadership: every seat against every event - the next seat, why, and what the other machine is told")
{
    const auto M = Role::main;
    const auto B = Role::backup;

    const std::vector<Row> table {
        //  ALONE: a main that sends with no backup following.
        { M, Seat::alone, Ev::followerLinked,   Seat::leading,     "follower-linked", none },
        { M, Seat::alone, Ev::leaderFoundFresh, Seat::following,   "leader-found",    none },       // AFR: a fresh main finds the backup that took over
        { M, Seat::alone, Ev::twoLeaders,       Seat::bothSending, "both-sending",    none },       // AFQ
        { M, Seat::alone, Ev::freshPeerSends,   none,              "",                none },       // it will follow this one
        { M, Seat::alone, Ev::peerWaiting,      none,              "",                none },
        { M, Seat::alone, Ev::peerGone,         none,              "",                none },
        { M, Seat::alone, Ev::streamLost,       none,              "",                none },
        { M, Seat::alone, Ev::leaderRestarted,  none,              "",                none },
        { M, Seat::alone, Ev::takeOver,         none,              "",                none },
        { M, Seat::alone, Ev::handOver,         none,              "",                none },
        { M, Seat::alone, Ev::keep,             none,              "",                none },
        { M, Seat::alone, Ev::orderHold,        Seat::following,   "peer-take-over",  none },
        { M, Seat::alone, Ev::orderLead,        none,              "",                none },

        //  LEADING, a main: its backup follows.
        { M, Seat::leading, Ev::followerLinked,   none,              "",              none },
        { M, Seat::leading, Ev::leaderFoundFresh, Seat::following,   "leader-found",  none },
        { M, Seat::leading, Ev::twoLeaders,       Seat::bothSending, "both-sending",  none },
        { M, Seat::leading, Ev::freshPeerSends,   none,              "",              none },
        { M, Seat::leading, Ev::peerWaiting,      none,              "",              none },
        { M, Seat::leading, Ev::peerGone,         Seat::alone,       "follower-lost", none },       // link lost while leading: it keeps sending
        { M, Seat::leading, Ev::streamLost,       Seat::alone,       "follower-lost", none },
        { M, Seat::leading, Ev::leaderRestarted,  none,              "",              none },
        { M, Seat::leading, Ev::takeOver,         none,              "",              none },
        { M, Seat::leading, Ev::handOver,         Seat::following,   "hand-over",     Order::lead },
        { M, Seat::leading, Ev::keep,             none,              "",              none },
        { M, Seat::leading, Ev::orderHold,        Seat::following,   "peer-take-over", none },
        { M, Seat::leading, Ev::orderLead,        none,              "",              none },

        //  LEADING, a backup that took over.
        { B, Seat::leading, Ev::followerLinked,   none,              "",              none },
        { B, Seat::leading, Ev::twoLeaders,       Seat::bothSending, "both-sending",  none },
        { B, Seat::leading, Ev::freshPeerSends,   none,              "",              none },       // the main came back: it follows (AFR)
        { B, Seat::leading, Ev::peerWaiting,      none,              "",              none },
        { B, Seat::leading, Ev::peerGone,         none,              "",              none },       // a backup that leads goes on leading
        { B, Seat::leading, Ev::streamLost,       none,              "",              none },
        { B, Seat::leading, Ev::takeOver,         none,              "",              none },
        { B, Seat::leading, Ev::handOver,         Seat::following,   "hand-over",     Order::lead },
        { B, Seat::leading, Ev::keep,             none,              "",              none },
        { B, Seat::leading, Ev::orderHold,        Seat::following,   "peer-take-over", none },
        { B, Seat::leading, Ev::orderLead,        none,              "",              none },

        //  FOLLOWING, a backup.
        { B, Seat::following, Ev::followerLinked,   none,           "",                 none },
        { B, Seat::following, Ev::leaderFoundFresh, none,           "",                 none },
        { B, Seat::following, Ev::twoLeaders,       none,           "",                 none },
        { B, Seat::following, Ev::freshPeerSends,   none,           "",                 none },
        { B, Seat::following, Ev::peerWaiting,      none,           "",                 none },
        { B, Seat::following, Ev::peerGone,         none,           "",                 none },     // the stream still comes, or is in its grace
        { B, Seat::following, Ev::streamLost,       Seat::leading,  "link-lost",        none },     // AFV: it takes over
        { B, Seat::following, Ev::leaderRestarted,  Seat::leading,  "leader-restarted", none },
        { B, Seat::following, Ev::takeOver,         Seat::leading,  "take-over",        Order::hold },
        { B, Seat::following, Ev::handOver,         none,           "",                 none },
        { B, Seat::following, Ev::keep,             none,           "",                 none },
        { B, Seat::following, Ev::orderHold,        none,           "",                 none },
        { B, Seat::following, Ev::orderLead,        Seat::leading,  "peer-hand-over",   none },

        //  FOLLOWING, a main that came back and follows the backup.
        { M, Seat::following, Ev::streamLost,       Seat::alone,    "link-lost",        none },
        { M, Seat::following, Ev::leaderRestarted,  Seat::leading,  "leader-restarted", none },
        { M, Seat::following, Ev::takeOver,         Seat::leading,  "take-over",        Order::hold },
        { M, Seat::following, Ev::orderLead,        Seat::leading,  "peer-hand-over",   none },

        //  WAITING, a backup that never found its main (AGK).
        { B, Seat::waiting, Ev::followerLinked,   none,             "",               none },
        { B, Seat::waiting, Ev::leaderFoundFresh, Seat::following,  "main-found",     none },
        { B, Seat::waiting, Ev::twoLeaders,       Seat::following,  "main-found",     none },       // whatever its clients did meanwhile
        { B, Seat::waiting, Ev::freshPeerSends,   Seat::following,  "main-found",     none },
        { B, Seat::waiting, Ev::peerWaiting,      none,             "",               none },
        { B, Seat::waiting, Ev::peerGone,         none,             "",               none },
        { B, Seat::waiting, Ev::streamLost,       none,             "",               none },
        { B, Seat::waiting, Ev::leaderRestarted,  Seat::following,  "main-found",     none },
        { B, Seat::waiting, Ev::takeOver,         Seat::leading,    "take-over",      Order::hold },
        { B, Seat::waiting, Ev::handOver,         none,             "",               none },
        { B, Seat::waiting, Ev::keep,             none,             "",               none },
        { B, Seat::waiting, Ev::orderHold,        Seat::following,  "peer-take-over", none },
        { B, Seat::waiting, Ev::orderLead,        Seat::leading,    "peer-hand-over", none },

        //  BOTH SENDING (AFQ): neither changes until an operator picks one.
        { M, Seat::bothSending, Ev::followerLinked,   Seat::leading,   "follower-linked", none },
        { M, Seat::bothSending, Ev::leaderFoundFresh, none,            "",                none },
        { M, Seat::bothSending, Ev::twoLeaders,       none,            "",                none },
        { M, Seat::bothSending, Ev::freshPeerSends,   none,            "",                none },
        { M, Seat::bothSending, Ev::peerWaiting,      none,            "",                none },
        { M, Seat::bothSending, Ev::peerGone,         Seat::alone,     "peer-lost",       none },
        { M, Seat::bothSending, Ev::streamLost,       Seat::alone,     "peer-lost",       none },
        { M, Seat::bothSending, Ev::takeOver,         none,            "",                none },
        { M, Seat::bothSending, Ev::handOver,         Seat::following, "hand-over",       Order::lead },
        { M, Seat::bothSending, Ev::keep,             Seat::leading,   "keep",            Order::hold },
        { M, Seat::bothSending, Ev::orderHold,        Seat::following, "peer-take-over",  none },
        { M, Seat::bothSending, Ev::orderLead,        Seat::leading,   "peer-hand-over",  none },
        { B, Seat::bothSending, Ev::peerGone,         Seat::leading,   "peer-lost",       none },
        { B, Seat::bothSending, Ev::keep,             Seat::leading,   "keep",            Order::hold },
        { B, Seat::bothSending, Ev::handOver,         Seat::following, "hand-over",       Order::lead },
    };

    for (const auto& row : table)
    {
        pair::Seated now;
        now.role = row.role;
        now.seat = row.seat;
        now.reason = "earlier";

        INFO (pair::wordOf (row.role) << " " << pair::wordOf (row.seat) << ", " << nameOf (row.event));

        const auto move = happen (now, row.event);
        REQUIRE (move.has_value() == row.to.has_value());

        if (! move.has_value())
            continue;

        CHECK (move->role == row.role);
        CHECK (move->to == *row.to);
        CHECK (move->reason == std::string (row.reason));
        CHECK (move->tell == row.tell);

        //  LOGGED: pair.lead when the new seat sends, pair.hold when it holds.
        CHECK (std::string (move->command()) == (pair::sends (*row.to) ? "pair.lead" : "pair.hold"));
    }
}

TEST_CASE ("leadership: what each seat does - sends or holds, applies the other's stream, closes the gate")
{
    struct Does
    {
        Seat seat;
        bool sends, follows, gated;
        const char* word;
    };

    for (const auto& does : { Does { Seat::alone,       true,  false, false, "alone" },
                              Does { Seat::leading,     true,  false, false, "leading" },
                              Does { Seat::following,   false, true,  true,  "following" },
                              Does { Seat::waiting,     false, false, false, "waiting" },
                              Does { Seat::bothSending, true,  true,  false, "bothSending" } })
    {
        INFO (does.word);
        CHECK (pair::sends (does.seat) == does.sends);           // the hold off, the stream kept
        CHECK (pair::follows (does.seat) == does.follows);       // the other's records applied
        CHECK (pair::gated (does.seat) == does.gated);           // clients passed on, devices ignored
        CHECK (std::string (pair::wordOf (does.seat)) == does.word);
        CHECK (pair::seatNamed (does.word) == std::optional<Seat> (does.seat));
    }

    CHECK_FALSE (pair::seatNamed ("sending").has_value());
    CHECK (pair::roleNamed ("main") == Role::main);
    CHECK (pair::roleNamed ("backup") == Role::backup);
    CHECK (pair::roleNamed ("boss") == Role::none);
}

TEST_CASE ("leadership: a role in force seats the machine as it starts; two fresh machines never both wait")
{
    pair::Situation situation;
    pair::Seated never;

    //  The first tick of a paired machine: a main sends alone, a backup waits (AGK).
    situation.role = Role::main;
    auto move = pair::decide (never, situation);
    REQUIRE (move.has_value());
    CHECK (move->to == Seat::alone);
    CHECK (move->reason == "started");
    CHECK (std::string (move->command()) == "pair.lead");

    situation.role = Role::backup;
    move = pair::decide (never, situation);
    REQUIRE (move.has_value());
    CHECK (move->to == Seat::waiting);
    CHECK (move->reason == "started");
    CHECK (std::string (move->command()) == "pair.hold");

    //  A machine not paired: nothing, ever.
    situation.role = Role::none;
    CHECK_FALSE (pair::decide (never, situation).has_value());

    //  A role changed later seats it afresh, whatever it was doing.
    situation.role = Role::backup;
    move = pair::decide ({ Role::main, Seat::leading, "follower-linked" }, situation);
    REQUIRE (move.has_value());
    CHECK (move->to == Seat::waiting);
    CHECK (move->reason == "role");

    situation.role = Role::none;
    move = pair::decide ({ Role::backup, Seat::following, "main-found" }, situation);
    REQUIRE (move.has_value());
    CHECK (move->role == Role::none);
    CHECK (move->to == Seat::alone);
    CHECK (move->reason == "role");

    /*  TWO MACHINES THAT HAVE BOTH RUN NOTHING: the backup follows the main,
        whichever was sending - and two mains, the later identity follows. */
    pair::Situation fresh;
    fresh.linked = true;
    fresh.fresh = true;
    fresh.peerFresh = true;
    fresh.peerSeat = Seat::alone;

    fresh.role = Role::main;
    fresh.peerRole = Role::backup;
    fresh.peerSeat = Seat::leading;
    CHECK_FALSE (pair::decide ({ Role::main, Seat::alone, "started" }, fresh).has_value());

    fresh.role = Role::backup;
    fresh.peerRole = Role::main;
    fresh.peerSeat = Seat::alone;
    move = pair::decide ({ Role::backup, Seat::leading, "link-lost" }, fresh);
    REQUIRE (move.has_value());
    CHECK (move->to == Seat::following);

    fresh.role = Role::main;
    fresh.peerRole = Role::main;
    fresh.stream = "B";
    fresh.peerStream = "A";
    CHECK (pair::decide ({ Role::main, Seat::alone, "started" }, fresh).has_value());
    fresh.stream = "A";
    fresh.peerStream = "B";
    CHECK_FALSE (pair::decide ({ Role::main, Seat::alone, "started" }, fresh).has_value());
}

TEST_CASE ("leadership: the operator's commands are refused where the seat makes them meaningless")
{
    using pair::Ask;

    struct Case
    {
        Role role;
        Seat seat;
        Ask ask;
        const char* peer;
        const char* refused;
    };

    for (const auto& c : { Case { Role::none,   Seat::alone,       Ask::takeOver, "",            "not-paired" },
                           Case { Role::none,   Seat::alone,       Ask::handOver, "following",   "not-paired" },
                           Case { Role::none,   Seat::alone,       Ask::keep,     "",            "not-paired" },
                           Case { Role::backup, Seat::following,   Ask::takeOver, "leading",     "" },
                           Case { Role::backup, Seat::following,   Ask::handOver, "leading",     "not-sending" },
                           Case { Role::backup, Seat::following,   Ask::keep,     "leading",     "not-both-sending" },
                           Case { Role::backup, Seat::waiting,     Ask::takeOver, "",            "" },
                           Case { Role::backup, Seat::waiting,     Ask::handOver, "",            "not-sending" },
                           Case { Role::backup, Seat::waiting,     Ask::keep,     "",            "not-both-sending" },
                           Case { Role::main,   Seat::alone,       Ask::takeOver, "",            "not-holding" },
                           Case { Role::main,   Seat::alone,       Ask::handOver, "",            "not-linked" },
                           Case { Role::main,   Seat::alone,       Ask::keep,     "",            "not-both-sending" },
                           Case { Role::main,   Seat::leading,     Ask::takeOver, "following",   "not-holding" },
                           Case { Role::main,   Seat::leading,     Ask::handOver, "following",   "" },
                           Case { Role::backup, Seat::leading,     Ask::handOver, "",            "not-linked" },
                           Case { Role::main,   Seat::leading,     Ask::keep,     "following",   "not-both-sending" },
                           Case { Role::main,   Seat::bothSending, Ask::takeOver, "bothSending", "not-holding" },
                           Case { Role::main,   Seat::bothSending, Ask::handOver, "bothSending", "" },
                           Case { Role::backup, Seat::bothSending, Ask::keep,     "bothSending", "" } })
    {
        INFO (pair::wordOf (c.role) << " " << pair::wordOf (c.seat) << ", ask " << static_cast<int> (c.ask)
                                    << ", the other " << c.peer);
        CHECK (pair::refusal ({ c.role, c.seat, "earlier" }, c.ask, c.peer) == std::string (c.refused));
    }
}

TEST_CASE ("leadership: the alarm in front of the operator, and empty when there is nothing to act on")
{
    pair::Situation linkedFollower;
    linkedFollower.linked = true;
    linkedFollower.peerSeat = Seat::following;

    const pair::Situation nobody;
    const pair::NotInStep inStep;

    CHECK (pair::alarm ({ Role::none, Seat::alone, "" }, nobody, inStep).empty());
    CHECK (pair::alarm ({ Role::main, Seat::alone, "started" }, nobody, inStep).empty());
    CHECK (pair::alarm ({ Role::main, Seat::leading, "follower-linked" }, linkedFollower, inStep).empty());
    CHECK (pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, inStep).empty());

    CHECK (pair::alarm ({ Role::backup, Seat::waiting, "started" }, nobody, inStep)
           == "Waiting for the main: this backup holds and sends nothing until it finds its main and follows it,"
              " or is told to take over.");

    CHECK (pair::alarm ({ Role::main, Seat::bothSending, "both-sending" }, nobody, inStep)
           == "Both machines are sending: every command reaches the devices from both. Make one of them the"
              " main; the other will hold and follow.");

    //  The main lost and this backup took over - until the main, back, follows it.
    CHECK (pair::alarm ({ Role::backup, Seat::leading, "link-lost" }, nobody, inStep)
           == "The main was lost and this machine took over: it sends now.");
    CHECK (pair::alarm ({ Role::backup, Seat::leading, "link-lost" }, linkedFollower, inStep).empty());
    CHECK (pair::alarm ({ Role::main, Seat::alone, "link-lost" }, nobody, inStep)
           == "The backup was lost and this machine took over: it sends now.");
    CHECK (pair::alarm ({ Role::backup, Seat::leading, "leader-restarted" }, nobody, inStep)
           == "The main started again and this machine took over: it sends now.");
    CHECK (pair::alarm ({ Role::backup, Seat::leading, "take-over" }, nobody, inStep)
           == "This machine took over: it sends now, and the main does not follow it.");

    //  A follower that joined while cues ran on its leader, or that was changed while it waited.
    CHECK (pair::alarm ({ Role::main, Seat::following, "leader-found" }, nobody, { 2, false })
           == "Following, but not yet in step: 2 cues were running on the other machine when this one began to"
              " follow, and they do not run here.");
    CHECK (pair::alarm ({ Role::main, Seat::following, "leader-found" }, nobody, { 1, false })
           == "Following, but not yet in step: 1 cue was running on the other machine when this one began to"
              " follow, and it does not run here.");
    CHECK (pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, { 0, true })
           == "Following, but not yet in step: this machine was changed while it waited for its main.");

    //  RD.8: a seat on its way, said with why it is needed; and once seated, what the summaries say differs.
    pair::NotInStep seating;
    seating.seating = true;
    CHECK (pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, seating)
           == "Following, and taking the other machine's position.");

    seating.leaderRuns = 2;
    CHECK (pair::alarm ({ Role::main, Seat::following, "leader-found" }, nobody, seating)
           == "Following, but not yet in step: 2 cues were running on the other machine when this one began to"
              " follow, and they do not run here. Taking the other machine's position.");

    pair::NotInStep apart;
    apart.differs = "cue 12 is running on the main, not here";
    CHECK (pair::alarm ({ Role::backup, Seat::following, "main-found" }, nobody, apart)
           == "Following, but not in step: cue 12 is running on the main, not here. Realign to take the other"
              " machine's position again.");
    CHECK (pair::alarm ({ Role::backup, Seat::leading, "take-over" }, linkedFollower, apart).empty());
}

//==============================================================================
namespace
{
    /*  AN ENGINE WITH THE PAIRING'S COMMANDS, the other machine's seat as this
        one would see it set by the test. */
    struct SeatRig
    {
        SeatRig()
        {
            engine.log().openInMemory ({});
            pair::registerPairCommands (engine.commands(), pairing);
            pairing.peerNow = [this] { return peer; };
        }

        LogRecord apply (std::int64_t tick, const std::string& command, std::vector<osc::Value> args = {},
                         const std::string& from = std::string (origin::cli))
        {
            REQUIRE (engine.submit (from, command, std::move (args)));
            engine.processTick (tick);

            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            return parsed.records.back();
        }

        Engine engine;
        pair::Pairing pairing;
        std::string peer;
    };

    std::vector<osc::Value> seatArgs (const char* role, const char* seat, const char* why)
    {
        return { s (role), s (seat), s (why) };
    }
}

TEST_CASE ("leadership: pair.lead and pair.hold are where the seat lives; the operator's three are applied with the other's seat, and replay alike")
{
    SeatRig rig;
    const auto engineOrigin = std::string (origin::engine);

    for (const auto* name : { "pair.lead", "pair.hold", "pair.takeOver", "pair.handOver", "pair.keep", "pair.realign" })
    {
        const auto* command = rig.engine.commands().find (name);
        REQUIRE_MESSAGE (command != nullptr, "not registered: " << name);
        CHECK_MESSAGE (pair::isMachineLocal (*command, {}), name);
    }

    //  Not paired yet: refused.
    auto record = rig.apply (1, "pair.takeOver");
    CHECK (record.kind == LogRecord::Kind::rejected);
    CHECK (record.reason == "not-paired");

    //  A backup starting: it waits; taking over is accepted, applied with the other's seat - nobody's.
    record = rig.apply (2, "pair.hold", seatArgs ("backup", "waiting", "started"), engineOrigin);
    REQUIRE (record.kind == LogRecord::Kind::applied);
    CHECK (rig.pairing.seated.role == Role::backup);
    CHECK (rig.pairing.seated.seat == Seat::waiting);
    CHECK (rig.pairing.seatCount == 1);

    record = rig.apply (3, "pair.takeOver");
    REQUIRE (record.kind == LogRecord::Kind::applied);
    CHECK (record.args == std::vector<osc::Value> { s ("") });
    REQUIRE (rig.pairing.asked.has_value());
    CHECK (*rig.pairing.asked == pair::Ask::takeOver);
    rig.pairing.asked.reset();

    //  The move it makes, recorded; and a second take-over refused.
    REQUIRE (rig.apply (4, "pair.lead", seatArgs ("backup", "leading", "take-over"), engineOrigin).kind
             == LogRecord::Kind::applied);
    CHECK (rig.apply (5, "pair.takeOver").reason == "not-holding");

    //  The main back, following: handing over is applied with what it is.
    rig.peer = "following";
    record = rig.apply (6, "pair.handOver");
    REQUIRE (record.kind == LogRecord::Kind::applied);
    CHECK (record.args == std::vector<osc::Value> { s ("following") });
    rig.pairing.asked.reset();

    REQUIRE (rig.apply (7, "pair.hold", seatArgs ("backup", "following", "hand-over"), engineOrigin).kind
             == LogRecord::Kind::applied);
    CHECK (rig.apply (8, "pair.keep").reason == "not-both-sending");
    CHECK (rig.apply (9, "pair.handOver").reason == "not-sending");

    //  Following: a realign is accepted, and waits for serve's next tick (RD.8).
    REQUIRE (rig.apply (9, "pair.realign").kind == LogRecord::Kind::applied);
    CHECK (rig.pairing.realignAsked);
    rig.pairing.realignAsked = false;

    //  Both sending: keep is the way out; handing over to nobody is not.
    REQUIRE (rig.apply (10, "pair.lead", seatArgs ("backup", "bothSending", "both-sending"), engineOrigin).kind
             == LogRecord::Kind::applied);
    rig.peer = "";
    CHECK (rig.apply (11, "pair.handOver").reason == "not-linked");
    CHECK (rig.apply (12, "pair.keep").kind == LogRecord::Kind::applied);

    //  RD.8's realign: only where this machine follows, and it is asked of serve's next tick.
    CHECK (rig.apply (12, "pair.realign").reason == "not-following");
    CHECK_FALSE (rig.pairing.realignAsked);

    //  A record that says what cannot be: a held seat by pair.lead, a role nobody knows, no reason.
    CHECK (rig.apply (13, "pair.lead", seatArgs ("backup", "following", "x"), engineOrigin).reason == "bad-value");
    CHECK (rig.apply (14, "pair.hold", seatArgs ("boss", "waiting", "x"), engineOrigin).reason == "bad-value");
    CHECK (rig.apply (15, "pair.lead", seatArgs ("main", "alone", ""), engineOrigin).reason == "bad-value");
    CHECK (rig.pairing.seated.seat == Seat::bothSending);

    /*  A REPLAY KNOWS THE SEAT FROM THE LOG and sees no other machine: every
        record comes out as the session wrote it, the refusals included. */
    Engine again;
    pair::Pairing replayed;
    pair::registerPairCommands (again.commands(), replayed);

    const auto result = wfg::replay (again, LogFile::parse (rig.engine.log().contents()));

    std::string mismatches;
    for (const auto& m : result.mismatches)
        mismatches += m + "\n";

    INFO (mismatches);
    CHECK (result.ok);
    CHECK (replayed.seated.role == Role::backup);
    CHECK (replayed.seated.seat == Seat::bothSending);
}

//==============================================================================
TEST_CASE ("AGJ: what a follower passed on and never saw answered - Esc and double Esc applied, the rest dropped")
{
    std::vector<Event> unconfirmed {
        { "ws:10.0.0.7:5555", "go", {} },
        { "ws:10.0.0.7:5555", "run.stopAll", {} },
        { "window", "go.doh", {} },
        { "surface:K4QW8YSC", "node.set", { s ("/godot/cue/B3N8R5TW/level"), osc::Value::float32 (-6.0f) } },
        { "window", "run.killAll", {} },
        { "cli", "node.set", { s ("/godot/list/7K2QM9X4/standby"), s ("B3N8R5TW") } },
        { "window", "run.stop", { s ("R8M2K4Q1") } },
    };

    const auto settled = pair::settle (unconfirmed);

    REQUIRE (settled.applied.size() == 3u);
    CHECK (settled.applied[0].command == "run.stopAll");
    CHECK (settled.applied[0].origin == "ws:10.0.0.7:5555");
    CHECK (settled.applied[1].command == "run.killAll");
    CHECK (settled.applied[2].command == "run.stop");
    CHECK (settled.dropped == 4);

    //  The one list, read by the audio's gate as well (PRD §6.2).
    for (const auto* stop : { "run.stopAll", "run.killAll", "run.stop", "run.kill" })
        CHECK (recovery::isStop (stop));

    for (const auto* other : { "go", "go.doh", "go.dohRelaunch", "node.set", "standby.next", "cue.fire" })
        CHECK_FALSE (recovery::isStop (other));
}

TEST_CASE ("AGJ: the ledger - a relay is kept until its answer comes, and one unanswered too long is forgotten and counted")
{
    Engine engine;
    engine.commands().add ({ "go", "", { { "run", 's', true, true } }, true,
                             [] (CommandContext&, const std::vector<osc::Value>& args) { return Outcome::ok (args); } });

    std::vector<std::vector<std::uint8_t>> sent;
    auto open = true;
    pair::FollowGate gate { engine.commands(), [&] (std::vector<std::uint8_t> bytes)
                            {
                                if (! open)
                                    return false;

                                sent.push_back (std::move (bytes));
                                return true;
                            },
                            0x500000001ull };

    Event first { "ws:10.0.0.7:5555", "go", {} };
    Event second { "window", "go", {} };
    REQUIRE (gate.take (first));
    REQUIRE (gate.take (second));
    CHECK (gate.unanswered() == 2u);
    REQUIRE (sent.size() == 2u);

    const auto ticketOf = [] (const std::vector<std::uint8_t>& bytes)
    {
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        REQUIRE (decoded.ok);
        REQUIRE (decoded.packet.args.size() == 2u);
        return static_cast<std::uint64_t> (decoded.packet.args[1].getInt64());
    };

    CHECK (ticketOf (sent[0]) == 0x500000001ull);
    CHECK (ticketOf (sent[1]) == 0x500000002ull);

    //  The leader's answer to the first came back: forgotten. Another session's ticket answers nothing.
    gate.confirm (ticketOf (sent[0]));
    gate.confirm (0x900000001ull);
    CHECK (gate.unanswered() == 1u);

    //  A relay with no path open is kept as well - the Esc of the second before a takeover.
    open = false;
    Event stop { "window", "run.stopAll", {} };
    REQUIRE (gate.take (stop));
    CHECK (gate.lost() == 1);
    CHECK (gate.unanswered() == 2u);

    /*  Taken at a takeover: what is left, oldest first; one older than the
        bound was the leader's to answer while the link was up, and is
        forgotten and counted instead. */
    const auto now = Clock::now();
    auto taken = gate.takeUnconfirmed (now + 10s, 60.0);
    REQUIRE (taken.size() == 2u);
    CHECK (taken[0].origin == "window");
    CHECK (taken[0].command == "go");
    CHECK (taken[1].command == "run.stopAll");
    CHECK (gate.unanswered() == 0u);
    CHECK (gate.unconfirmed() == 0);

    REQUIRE (gate.take (first));
    gate.expire (Clock::now(), 2.0);
    CHECK (gate.unanswered() == 1u);
    gate.expire (Clock::now() + 10s, 2.0);
    CHECK (gate.unanswered() == 0u);
    CHECK (gate.unconfirmed() == 1);
}

//==============================================================================
namespace
{
    Command counted (std::string name, std::function<void (const std::string&)> heard)
    {
        Command command;
        command.name = std::move (name);
        command.params = { { "anything", '*', true, true } };
        command.handler = [heard] (CommandContext& context, const std::vector<osc::Value>& args)
        {
            if (heard)
                heard (*context.origin);

            return Outcome::ok (args);
        };
        return command;
    }

    /*  TWO MACHINES IN ONE PROCESS, joined by functions: one leads - its
        stream - and one follows - its receiver, its follower and its gate -
        over a wire that delivers a tick's frame two milliseconds after it
        left, and a relay up it at once, while the leader is there. Each
        machine's GO and stops are "heard by the device" when the machine
        does not hold (pair/Hold.h). */
    struct Pairing2
    {
        Pairing2 (Seat leaderSeat, Seat followerSeat)
            : leaderHold (! pair::sends (leaderSeat)), followerHold (! pair::sends (followerSeat)),
              followsLeader (pair::follows (followerSeat))
        {
            for (auto* side : { &leader, &follower })
                side->log().openInMemory ({});

            for (const auto* name : { "go", "go.doh", "node.set", "run.stopAll", "run.killAll" })
            {
                leader.commands().add (counted (name, [this, n = std::string (name)] (const std::string& from)
                                                {
                                                    if (! leaderHold.held())
                                                        device.push_back ("leader " + n + " " + from);
                                                }));
                follower.commands().add (counted (name, [this, n = std::string (name)] (const std::string& from)
                                                  {
                                                      if (! followerHold.held())
                                                          device.push_back ("follower " + n + " " + from);
                                                  }));
            }

            //  The leader: keeps what travels, takes what is passed on as peer:<origin>, its ticket kept.
            stream.setLeading (true);
            leader.setAfterRecord ([this] (const LogRecord& record, const Command* command) { stream.keep (record, command); });
            stream.setRelayed ([this] (const LogRecord& passedOn)
            {
                Event event;
                event.origin = std::string (origin::fromPeer) + passedOn.origin;
                event.command = passedOn.command;
                event.args = passedOn.args;
                event.ticket = passedOn.ticket;
                leader.submit (std::move (event));
            });
            stream.setSend ([this] (const std::string&, const std::vector<std::uint8_t>& bytes)
            {
                const auto decoded = osc::decode (bytes.data(), bytes.size());

                if (! decoded.ok || ! leaderAlive)
                    return false;

                frames.push_back ({ sentAt + 2ms, decoded.packet });
                return true;
            });

            //  The follower: the gate passes its clients' commands up the wire, the leader's records come down.
            receiver.setOnRecord ([this] (std::uint64_t, const LogRecord& record)
                                  {
                                      gate.confirm (record.ticket);
                                      followerHalf.record (record);
                                  });
            receiver.setOnBeat ([this] (std::int64_t tick, Clock::time_point at)
                                {
                                    if (followerHalf.isFollowing())
                                        followerHalf.beat (tick, at);
                                });
            follower.setGate ([this] (Event& event) { return gate.take (event); });
            follower.setGated (pair::gated (followerSeat));
            follower.setAfterRecord ([this] (const LogRecord& record, const Command*) { followerHalf.applied (record); });

            //  It asks for the leader's stream from now, as a follow that starts does (RD.7).
            pair::wire::Hello hello;
            hello.machine = "follower";
            hello.role = "main";
            hello.lastCounter = pair::wire::fromNow;
            hello.stream = "S-leader";
            hello.state = pair::wordOf (followerSeat);
            hello.described = true;
            const auto bytes = pair::wire::helloBytes (hello);
            stream.received ("127.0.0.1:5010", osc::decode (bytes.data(), bytes.size()).packet, t0);
            receiver.startAfresh ("S-leader");
        }

        /*  One tick of each: the leader's, its frame on the wire; the
            follower's a little later, what arrived drained and applied at its
            slot. The relays it passed on reach the leader before the leader's
            next tick, while the leader is there. */
        void tick()
        {
            const auto now = t0 + 20ms * ticks;

            if (leaderAlive)
            {
                for (auto& bytes : upTheWire)
                {
                    const auto decoded = osc::decode (bytes.data(), bytes.size());
                    stream.received ("ws:127.0.0.1:50001", decoded.packet, now);
                }

                upTheWire.clear();
                leader.processTick (ticks);
                sentAt = now;
                stream.afterTick (ticks, 1.0);
            }

            const auto due = now + 7ms;

            while (! frames.empty() && frames.front().at <= due)
            {
                receiver.deliver ("127.0.0.1:5010", frames.front().packet, frames.front().at);
                frames.pop_front();
            }

            receiver.drain (due, 1.0);

            if (! followerHalf.isFollowing() && receiver.isLinked (due, 1.0) && followsLeader)
                followerHalf.start();

            if (followerHalf.isFollowing())
                followerHalf.beforeTick (ticks, due, 0.0005);

            follower.processTick (ticks);
            ++ticks;
        }

        /*  THE FOLLOWER TAKES OVER, as serve does it: what it holds of the
            leader applied at once, still holding; then its hold let go, its
            gate opened, and what it passed on and never saw answered settled. */
        void takeOver()
        {
            followerHalf.stop();
            follower.processTick (ticks++);

            followerHold.set (false);
            follower.setGated (false);

            //  The ledger stamps a relay on the real clock, as it is passed on.
            auto settled = pair::settle (gate.takeUnconfirmed (Clock::now(), 2.0));

            for (auto& stop : settled.applied)
                follower.submit (std::move (stop));

            gate.countDropped (settled.dropped);
            follower.processTick (ticks++);
        }

        std::vector<std::pair<std::string, std::string>> logged (Engine& engine) const
        {
            std::vector<std::pair<std::string, std::string>> out;

            for (const auto& record : LogFile::parse (engine.log().contents()).records)
                if (record.kind == LogRecord::Kind::applied)
                    out.emplace_back (record.origin, record.command);

            return out;
        }

        struct Frame
        {
            Clock::time_point at;
            osc::Packet packet;
        };

        Engine leader, follower;
        pair::Hold leaderHold, followerHold;
        bool followsLeader = true;
        pair::Stream stream { "S-leader" };
        pair::Receiver receiver;
        pair::Follower followerHalf { [this] (Event event) { return follower.submit (std::move (event)); } };
        pair::FollowGate gate { follower.commands(), [this] (std::vector<std::uint8_t> bytes)
                                {
                                    upTheWire.push_back (std::move (bytes));
                                    return true;
                                } };

        std::deque<Frame> frames;
        std::vector<std::vector<std::uint8_t>> upTheWire;
        Clock::time_point sentAt {};
        bool leaderAlive = true;
        std::int64_t ticks = 1;
        std::vector<std::string> device;
    };
}

TEST_CASE ("AGJ: at a takeover, an Esc never answered is applied once, a GO dropped and counted, and a relay the leader answered is never applied again")
{
    Pairing2 two { Seat::leading, Seat::following };

    for (int i = 0; i < 30; ++i)
        two.tick();

    REQUIRE (two.followerHalf.delayFixed());

    //  A GO and an Esc given at the follower, passed on, applied by the leader, answered.
    REQUIRE (two.follower.submit ("ws:10.0.0.7:5555", "go", { s ("answered") }));
    REQUIRE (two.follower.submit ("ws:10.0.0.7:5555", "run.stopAll", {}));

    for (int i = 0; i < 10; ++i)
        two.tick();

    CHECK (two.gate.unanswered() == 0u);

    //  THE LEADER GOES. What the follower's clients give now is passed on and never answered.
    two.leaderAlive = false;
    REQUIRE (two.follower.submit ("ws:10.0.0.7:5555", "run.stopAll", {}));
    REQUIRE (two.follower.submit ("window", "go", { s ("unanswered") }));
    REQUIRE (two.follower.submit ("surface:K4QW8YSC", "go.doh", {}));
    REQUIRE (two.follower.submit ("window", "run.killAll", {}));
    two.tick();
    CHECK (two.gate.unanswered() == 4u);

    two.takeOver();

    const auto logged = two.logged (two.follower);
    const auto count = [&logged] (const std::string& from, const std::string& command)
    {
        return std::count (logged.begin(), logged.end(), std::make_pair (from, command));
    };

    //  The answered ones once, as the leader's - never again at the takeover.
    CHECK (count ("main:peer:ws:10.0.0.7:5555", "go") == 1);
    CHECK (count ("main:peer:ws:10.0.0.7:5555", "run.stopAll") == 1);

    //  The unanswered: Esc and double Esc applied here, once each; the GO and Doh! dropped and counted.
    CHECK (count ("ws:10.0.0.7:5555", "run.stopAll") == 1);
    CHECK (count ("window", "run.killAll") == 1);
    CHECK (count ("window", "go") == 0);
    CHECK (count ("surface:K4QW8YSC", "go.doh") == 0);
    CHECK (two.gate.unconfirmed() == 2);
    CHECK (two.gate.unanswered() == 0u);

    /*  THE DEVICE: the answered GO and Esc from the leader, which sent; the
        follower held them. The two stops after the takeover from the follower,
        which sends now. */
    CHECK (two.device == std::vector<std::string> {
        "leader go peer:ws:10.0.0.7:5555", "leader run.stopAll peer:ws:10.0.0.7:5555",
        "follower run.stopAll ws:10.0.0.7:5555", "follower run.killAll window" });
}

TEST_CASE ("symmetric link: a backup that leads streams to a main that follows; the main applies and holds, and the device hears each GO once")
{
    /*  THE BACKUP TOOK OVER AND LEADS; the main came back and follows it -
        the stream from the machine that dials to the machine it dialled. */
    Pairing2 two { Seat::leading, Seat::following };

    for (int i = 0; i < 30; ++i)
        two.tick();

    REQUIRE (two.followerHalf.delayFixed());

    //  A GO at the backup's own client, and one at the main's, passed on up to the backup.
    REQUIRE (two.leader.submit ("ws:10.0.0.8:6000", "go", {}));

    for (int i = 0; i < 10; ++i)
        two.tick();

    REQUIRE (two.follower.submit ("window", "go", {}));

    for (int i = 0; i < 10; ++i)
        two.tick();

    //  Applied on both...
    const auto onLeader = two.logged (two.leader);
    const auto onFollower = two.logged (two.follower);
    CHECK (std::count (onLeader.begin(), onLeader.end(), std::make_pair (std::string ("ws:10.0.0.8:6000"), std::string ("go"))) == 1);
    CHECK (std::count (onLeader.begin(), onLeader.end(), std::make_pair (std::string ("peer:window"), std::string ("go"))) == 1);
    CHECK (std::count (onFollower.begin(), onFollower.end(), std::make_pair (std::string ("main:ws:10.0.0.8:6000"), std::string ("go"))) == 1);
    CHECK (std::count (onFollower.begin(), onFollower.end(), std::make_pair (std::string ("main:peer:window"), std::string ("go"))) == 1);
    CHECK (std::count (onFollower.begin(), onFollower.end(), std::make_pair (std::string ("window"), std::string ("go"))) == 0);

    //  ...and heard once each, from the machine that leads.
    CHECK (two.device == std::vector<std::string> { "leader go ws:10.0.0.8:6000", "leader go peer:window" });
    CHECK (two.followerHalf.differences() == 0);
    CHECK (two.followerHalf.late() == 0);
}

TEST_CASE ("both sending: each applies the other's operators' commands, and the device hears a GO at either from both")
{
    /*  AFQ: two machines that had each been sending, found again, both keep
        sending - and each applies what the other's operators do, so a GO at
        either reaches the devices from both, until somebody keeps the show on
        one. Here one half of it: a GO at the one machine, applied on the
        other, which does not hold. */
    Pairing2 two { Seat::bothSending, Seat::bothSending };

    for (int i = 0; i < 30; ++i)
        two.tick();

    REQUIRE (two.followerHalf.delayFixed());
    CHECK_FALSE (two.follower.isGated());

    REQUIRE (two.leader.submit ("ws:10.0.0.8:6000", "go", {}));

    for (int i = 0; i < 10; ++i)
        two.tick();

    CHECK (two.device == std::vector<std::string> { "leader go ws:10.0.0.8:6000", "follower go main:ws:10.0.0.8:6000" });

    //  And its own client's GO is applied where it was given - the gate is open - and heard from it.
    REQUIRE (two.follower.submit ("window", "go", {}));
    two.tick();
    CHECK (two.device.back() == "follower go window");
}

//==============================================================================
TEST_CASE ("leadership: what the pair costs a tick - the presence read, the seat decided, the alarm said")
{
    /*  WHAT SERVE'S HOOKS ADD TO A TICK FOR RD.7 when nothing moves: the
        presence's inbox swapped, the seat decided from the situation, the
        alarm said. Reported, and checked loosely for a busy runner. */
    pair::Presence presence;
    pair::Seated seated { Role::backup, Seat::following, "main-found" };
    pair::Situation situation = situationFor (Role::backup, Ev::peerGone);
    situation.linked = true;
    const pair::NotInStep behind;

    std::vector<double> took;

    for (int round = 0; round < 2000; ++round)
    {
        const auto a = std::chrono::steady_clock::now();
        presence.drain();
        situation.linked = presence.linked (t0, 1.0) || true;
        const auto move = pair::decide (seated, situation);
        const auto said = pair::alarm (seated, situation, behind);
        const auto b = std::chrono::steady_clock::now();

        CHECK_FALSE (move.has_value());
        CHECK (said.empty());
        took.push_back (std::chrono::duration<double, std::micro> (b - a).count());
    }

    std::sort (took.begin(), took.end());
    MESSAGE ("the pair's seat, on a tick where nothing moves: median " << took[took.size() / 2] << " us, best "
             << took.front() << " us");
    CHECK (took[took.size() / 2] < 500.0);

    /*  AND THE CUES RUNNING, counted once a tick for the beat and the hello
        (AGA, RD.7): one pass of the run table, here five hundred runs. */
    cue::RunTable runs;

    for (int i = 0; i < 500; ++i)
        runs.create ("R" + std::to_string (100000 + i), "C" + std::to_string (i), "memo");

    std::vector<double> counting;
    std::int64_t live = 0;

    for (int round = 0; round < 200; ++round)
    {
        const auto a = std::chrono::steady_clock::now();
        live = 0;

        for (const auto& run : runs.all())
            if (! run.isFinished() && run.state != cue::runState::preparing && run.prepare.empty())
                ++live;

        counting.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - a).count());
    }

    std::sort (counting.begin(), counting.end());
    MESSAGE ("the live runs of a table of 500 counted: median " << counting[counting.size() / 2] << " us");
    CHECK (live == 500);
}
