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
    A DEVICE'S SESSION WITH ITS BROKER (namespace draft §67, ANE, ANI; MQ.3):
    the client side of MQTT, on the tick thread, over a link the serial
    table keeps open for it.

    The link is the table's: it opens the connection, reads it into packets
    cut by `mqtt` framing, writes what it is handed, and opens it again with
    its backoff when the far end goes. What the session adds is the protocol
    on top: CONNECT at each opening the table counts, SUBSCRIBE to the
    device's filters once the broker says yes, PINGREQ by the keepalive, the
    acknowledgement a QoS 1 or 2 publish asks for, and a publish taken apart
    into the address and the values the show hears. Time is ticks, never the
    wall clock, so a test drives it and a replay reads the same log.

    What it says about itself - a CONNACK that refused, a subscription the
    broker would not grant, a broker gone silent - is a sentence for the
    device's `linkProblem` row, beside the table's own.
*/

#include <wfg/engine/mqtt/Packet.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/serial/SerialTable.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wfg::mqtt
{
    /*  What the show says about one device on a broker: read off its mount
        rows by whoever builds the session. */
    struct SessionWanted
    {
        std::string id;          ///< the mount's identifier
        std::string prefix;      ///< the `prefix` row; topics land under its first root
        std::string topics;      ///< the `topics` row, space-separated filters
        std::string login;       ///< the `login` row: `<user> <password>`, or empty
        std::string clientId;    ///< `godot-<id>-<machine>`
    };

    /*  A publish as the show hears it. */
    struct Heard
    {
        std::string address;
        osc::Values values;
    };

    class Session
    {
    public:
        /*  The keepalive the CONNECT asks, in seconds, and the tick rate the
            session counts by. A PINGREQ goes at half the keepalive; a broker
            silent for the whole of it is given up on, and the link asked to
            open again. */
        static constexpr std::uint16_t keepAliveSeconds = 30;
        static constexpr std::int64_t ticksPerSecond = 50;

        explicit Session (SessionWanted wanted);

        const SessionWanted& wanted() const noexcept { return what; }

        /*  The show's rows as they are now. A changed filter list is carried
            to the broker in place (UNSUBSCRIBE, SUBSCRIBE); a changed login
            or client id is the link's to carry, by opening again - the
            caller puts those in the link's `Wanted::session`. */
        void want (const SessionWanted& now);

        /*  ONCE A TICK: the link's state and this tick, the packets to hand
            the link in return. A new opening - the table counts them - sends
            CONNECT; the rest follows what came in. */
        std::vector<Packet> tick (const serial::PortState& link, std::int64_t tick);

        /*  A PACKET THE LINK READ: a publish comes back as what the show
            hears, or nothing for one whose topic cannot be an address (ANF,
            counted in `dropped`); everything else is the session's own. */
        std::optional<Heard> feed (const Packet& packet, std::int64_t tick);

        /*  A PUBLISH FROM THE SHOW, as a packet for the link: QoS 0, not
            retained (ANF). nullopt for an address not under the prefix. */
        std::optional<Packet> publish (const std::string& address, const osc::Values& values) const;

        bool connected() const noexcept { return isConnected; }

        /*  The session's own sentence for `linkProblem`, empty when all is well. */
        const std::string& problem() const noexcept { return said; }

        /*  Set once the broker has gone silent for the keepalive: the caller
            asks the table to open the link again, and takes the flag. */
        bool takeReopenWish() noexcept;

        std::uint64_t dropped() const noexcept { return droppedCount; }

    private:
        SessionWanted what;
        std::uint64_t seenOpenings = 0;
        bool linkOpen = false;
        bool awaitingConnack = false;
        bool isConnected = false;
        bool wantsReopen = false;
        std::int64_t connectSentAt = 0;
        std::int64_t lastSentAt = 0;
        std::int64_t lastHeardAt = 0;
        std::int64_t pingSentAt = -1;
        std::uint16_t nextPacketId = 1;
        std::string subscribedTopics;      // what the broker holds for us now
        std::string said;
        std::uint64_t droppedCount = 0;
        std::vector<Packet> outgoing;

        std::uint16_t takePacketId() noexcept;
        void queueSubscribe();
        void reset() noexcept;
    };

    /*  The sentence for a CONNACK return code that refused. */
    std::string connackProblem (std::uint8_t code);
}
