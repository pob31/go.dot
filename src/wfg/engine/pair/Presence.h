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
    WHETHER THE OTHER MACHINE IS THERE, AND WHAT IT SAYS IT IS (namespace draft
    §58, RD.7; AFV, AFQ, AFR, AGK).

    RD.5's link told each end whether it was linked from its own half: a
    backup by its main's heartbeat, a main by its backup's pings. That was
    enough while the main always led. From RD.7 either may lead, and both ends
    must know the other is there whoever leads and whoever follows - two
    machines both sending have no follower between them, a main started again
    follows a backup that dialled it - so presence is read apart from the
    stream, from every message the other end sends, on every connection to it:
    the connections the server took from it, or the paths this machine dialled.

    ALIVE MEANS ITS ENGINE IS TICKING, not that its socket answers. A ping is
    answered by the server's own thread and a hello may be said by the
    dialler's, so a machine whose tick has hung would still look alive from
    those; every ping carries the pinging machine's tick and every pong and
    beat the answering one's, and a channel is heard from only when that tick
    has moved on - or when the other machine says hello or gives an order,
    which its tick thread does. Silent past the timeout on every channel, the
    other machine is not there (AFV).

    WHAT IT IS: the last hello it said, which every machine says on a
    connection's first message and again whenever what it is changes (PeerLink.h
    lists the nine arguments). And what its operator chose, as an order: hold
    (it took over, or kept the show) or lead (it handed the show over).

    Fed on the threads the messages arrive on, read on the tick thread once a
    tick, as the receiver is.
*/

#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/pair/PeerLink.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::pair
{
    /** `/godot/pair/order s:order s:reason`. */
    std::vector<std::uint8_t> orderBytes (Order order, const std::string& reason);

    class Presence
    {
    public:
        Presence() = default;

        Presence (const Presence&) = delete;
        Presence& operator= (const Presence&) = delete;

        //======================================================================
        //  Any thread: the server's, a dialled path's.

        /*  A message of the link from the other machine, by the channel that
            brought it - a server connection's origin, or a dialled address. A
            hello, an order, a ping, a pong, a beat (inside a tick's bundle) are
            read; anything else is not presence's. */
        void heard (const std::string& channel, const osc::Packet& message, LinkClock::time_point arrival);

        /** That channel closed. */
        void closed (const std::string& channel);

        /*  THE CHANNEL A FOLLOWER AT THE SERVER'S END PASSES ITS CLIENTS'
            COMMANDS DOWN (RD.7): a main following the backup that dialled it
            has no dialler, and relays down the connection it last heard the
            backup on. Empty when there is none. Any thread. */
        std::string relayChannel() const;

        //======================================================================
        //  The tick thread's.

        /** Reads what arrived. */
        void drain();

        /** Whether the other machine's engine was heard within the timeout, on any channel. */
        bool linked (LinkClock::time_point now, double timeoutSeconds) const;

        /*  What it last said it is; `described` false until an RD.7 hello has
            come. Kept after the link is lost, so a machine knows what it lost.
            `saidCount` moves with every hello, so a reader copies only then. */
        const wire::Hello& peer() const noexcept { return says; }
        std::uint64_t saidCount() const noexcept { return helloCount; }

        /** The orders since the last drain, taken: each is acted on once. */
        std::vector<Ordered> takeOrders();

        /*  Channels first heard at the last drain - a connection the other
            machine has just opened - which serve says this machine's hello
            down; and every channel still open, which a hello said again, or an
            order, goes down at the server's end. */
        const std::vector<std::string>& opened() const noexcept { return openedNow; }
        std::vector<std::string> channels() const;

        /** The channel heard from last, as `pair/peer` says it; kept after the link is lost. */
        const std::string& latest() const noexcept { return latestChannel; }

        /*  Seconds there and back as the other machine measured them and said
            in its ping - what a main reads, which never pings (RD.5); nought
            when none has said one within the timeout. */
        double roundTrip (LinkClock::time_point now, double timeoutSeconds) const;

    private:
        struct Item
        {
            enum class Kind { hello, tick, order, closed };

            Kind kind = Kind::tick;
            std::string channel;
            LinkClock::time_point at {};
            std::int64_t tick = -1;             // -1: a message that carries none (RD.5's ping)
            double roundTrip = -1.0;            // < 0: none said
            wire::Hello hello;
            Ordered ordered;
        };

        struct Channel
        {
            bool heardAny = false;
            LinkClock::time_point lastHeard {};
            std::int64_t lastTick = -1;
            double roundTrip = 0.0;
            LinkClock::time_point tripAt {};
            bool tripSaid = false;
        };

        void take (Item& item);
        void push (Item item);

        //  The tick thread's.
        std::map<std::string, Channel> known;
        std::vector<Item> items;                // swapped with the inbox, reused
        std::vector<std::string> openedNow;
        std::vector<Ordered> orders;
        wire::Hello says;
        std::uint64_t helloCount = 0;
        std::string latestChannel;
        LinkClock::time_point latestAt {};

        //  Both.
        mutable std::mutex lock;
        std::vector<Item> inbox;
        std::string relayVia;                   // under `lock`
    };
}
