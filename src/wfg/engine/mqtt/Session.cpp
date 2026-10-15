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

#include <wfg/engine/mqtt/Session.h>

#include <wfg/engine/mqtt/Words.h>
#include <wfg/engine/tree/Mount.h>

#include <algorithm>

namespace wfg::mqtt
{
    namespace
    {
        constexpr std::int64_t connectWaitTicks = 10 * Session::ticksPerSecond;
        constexpr std::int64_t pingEveryTicks = (Session::keepAliveSeconds / 2) * Session::ticksPerSecond;
        constexpr std::int64_t silentAfterTicks = Session::keepAliveSeconds * Session::ticksPerSecond;

        /*  `<user> <password>` off the login row: the first word and the
            rest. A login with no space is a user name alone. */
        void splitLogin (const std::string& login, std::optional<std::string>& user, std::optional<std::string>& password)
        {
            if (login.empty())
                return;

            const auto space = login.find (' ');
            user = login.substr (0, space);

            if (space != std::string::npos && space + 1 < login.size())
                password = login.substr (space + 1);
        }
    }

    std::string connackProblem (std::uint8_t code)
    {
        switch (code)
        {
            case connack::accepted:       return {};
            case connack::badProtocol:    return "refused: protocol - the broker does not take MQTT 3.1.1";
            case connack::badIdentifier:  return "refused: identifier - the broker would not take this client id";
            case connack::unavailable:    return "refused: unavailable - the broker is not taking clients";
            case connack::badLogin:       return "refused: bad user name or password";
            case connack::notAuthorised:  return "refused: not authorised";
            default:                      return "refused: code " + std::to_string (code);
        }
    }

    //==========================================================================
    Session::Session (SessionWanted wanted) : what (std::move (wanted)) {}

    void Session::want (const SessionWanted& now)
    {
        const bool topicsChanged = now.topics != what.topics;
        what = now;

        if (topicsChanged && isConnected)
            queueSubscribe();
    }

    std::uint16_t Session::takePacketId() noexcept
    {
        const auto id = nextPacketId;
        nextPacketId = static_cast<std::uint16_t> (nextPacketId == 65535 ? 1 : nextPacketId + 1);
        return id;
    }

    void Session::reset() noexcept
    {
        awaitingConnack = false;
        isConnected = false;
        pingSentAt = -1;
        subscribedTopics.clear();
        outgoing.clear();
    }

    void Session::queueSubscribe()
    {
        /*  What the broker holds for us that the show no longer names is
            let go first; then the list as it is now, whole - a SUBSCRIBE to
            a filter already held is replaced in place (§3.8.4), so the whole
            list is simplest and right. */
        std::vector<std::string> gone;
        const auto now = tree::prefixesOf (what.topics);

        for (const auto& old : tree::prefixesOf (subscribedTopics))
            if (std::find (now.begin(), now.end(), old) == now.end())
                gone.push_back (old);

        if (! gone.empty())
            outgoing.push_back (Packet::unsubscribeFrom (takePacketId(), std::move (gone)));

        std::vector<std::pair<std::string, std::uint8_t>> filters;
        for (const auto& filter : now)
            if (validFilter (filter))
                filters.emplace_back (filter, std::uint8_t { 0 });

        if (! filters.empty())
            outgoing.push_back (Packet::subscribeTo (takePacketId(), std::move (filters)));

        subscribedTopics = what.topics;
    }

    std::vector<Packet> Session::tick (const serial::PortState& link, std::int64_t tick)
    {
        std::vector<Packet> out;

        const bool open = link.state == "open";

        if (! open)
        {
            if (linkOpen)
                reset();
            linkOpen = false;
            return out;
        }

        /*  A NEW OPENING: the table counted it, whether or not we saw the
            closing in between. Everything the broker held of us is gone with
            the old connection; start again. */
        if (link.openings != seenOpenings)
        {
            seenOpenings = link.openings;
            reset();
            linkOpen = true;
            said.clear();

            auto connect = Packet::connectWith (what.clientId, keepAliveSeconds);
            connect.cleanSession = true;
            splitLogin (what.login, connect.user, connect.password);
            out.push_back (std::move (connect));

            awaitingConnack = true;
            connectSentAt = tick;
            lastSentAt = tick;
            lastHeardAt = tick;
            return out;
        }

        linkOpen = true;

        if (awaitingConnack)
        {
            if (tick - connectSentAt > connectWaitTicks && said.empty())
            {
                said = "the broker answered nothing to CONNECT";
                wantsReopen = true;
            }
            return out;
        }

        if (! isConnected)
            return out;

        /*  THE KEEPALIVE: a PINGREQ when nothing has gone out for half of it;
            a broker that has said nothing for the whole of it is gone, and
            the link is asked to open again - a half-open socket never says. */
        if (tick - lastHeardAt > silentAfterTicks)
        {
            said = "the broker has gone silent";
            wantsReopen = true;
            isConnected = false;
            return out;
        }

        if (tick - lastSentAt >= pingEveryTicks && pingSentAt < 0)
        {
            out.push_back (Packet::plain (Type::pingreq));
            pingSentAt = tick;
            lastSentAt = tick;
        }

        for (auto& packet : outgoing)
            out.push_back (std::move (packet));
        outgoing.clear();

        if (! out.empty())
            lastSentAt = tick;

        return out;
    }

    std::optional<Heard> Session::feed (const Packet& packet, std::int64_t tick)
    {
        lastHeardAt = tick;

        switch (packet.type)
        {
            case Type::connack:
                awaitingConnack = false;

                if (packet.returnCode != connack::accepted)
                {
                    said = connackProblem (packet.returnCode);
                    isConnected = false;
                    return std::nullopt;
                }

                isConnected = true;
                said.clear();
                subscribedTopics.clear();
                queueSubscribe();
                return std::nullopt;

            case Type::suback:
            {
                /*  A filter the broker would not take is said; the rest hold. */
                const auto filters = tree::prefixesOf (subscribedTopics);
                for (std::size_t at = 0; at < packet.returnCodes.size() && at < filters.size(); ++at)
                    if (packet.returnCodes[at] == subscribeFailed)
                        said = "the broker refused the subscription to " + filters[at];
                return std::nullopt;
            }

            case Type::pingresp:
                pingSentAt = -1;
                return std::nullopt;

            case Type::publish:
            {
                if (packet.qos == 1)
                    outgoing.push_back (Packet::acknowledgement (Type::puback, packet.packetId));
                else if (packet.qos == 2)
                    outgoing.push_back (Packet::acknowledgement (Type::pubrec, packet.packetId));

                const auto address = addressOfTopic (what.prefix, packet.topic);
                if (! address.has_value())
                {
                    ++droppedCount;
                    return std::nullopt;
                }

                return Heard { *address, valuesOfPayload (packet.payload) };
            }

            case Type::pubrel:
                /*  The second half of a QoS 2 publish we were handed: done. */
                outgoing.push_back (Packet::acknowledgement (Type::pubcomp, packet.packetId));
                return std::nullopt;

            case Type::puback:
            case Type::pubrec:
            case Type::pubcomp:
            case Type::unsuback:
                /*  Nothing we send asks for these but an UNSUBSCRIBE; taken
                    and forgotten. */
                return std::nullopt;

            case Type::connect:
            case Type::subscribe:
            case Type::unsubscribe:
            case Type::pingreq:
            case Type::disconnect:
                /*  A broker does not send these; a stream that does is not
                    one worth listening to. */
                said = std::string ("the broker sent a ") + nameOf (packet.type) + ", which a broker does not";
                wantsReopen = true;
                return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<Packet> Session::publish (const std::string& address, const osc::Values& values) const
    {
        const auto topic = topicOfAddress (what.prefix, address);
        if (! topic.has_value())
            return std::nullopt;

        return Packet::publishOf (*topic, payloadOfValues (values), 0, false);
    }

    bool Session::takeReopenWish() noexcept
    {
        const auto wish = wantsReopen;
        wantsReopen = false;
        return wish;
    }
}
