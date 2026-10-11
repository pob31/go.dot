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

#include <wfg/engine/pair/Presence.h>

#include <chrono>
#include <cmath>
#include <utility>

namespace wfg::pair
{
    namespace
    {
        bool within (LinkClock::time_point since, LinkClock::time_point at, double timeoutSeconds)
        {
            return std::chrono::duration<double> (at - since).count() <= timeoutSeconds;
        }
    }

    std::vector<std::uint8_t> orderBytes (Order order, const std::string& reason)
    {
        return wire::encode (wire::order, { osc::Value::string (order == Order::hold ? "hold" : "lead"),
                                            osc::Value::string (reason) });
    }

    //==========================================================================
    void Presence::push (Item item)
    {
        const std::lock_guard<std::mutex> held { lock };
        inbox.push_back (std::move (item));
    }

    void Presence::heard (const std::string& channel, const osc::Packet& message, LinkClock::time_point arrival)
    {
        Item item;
        item.channel = channel;
        item.at = arrival;

        /*  A TICK'S STREAM: its beat, the last thing in it, says the leader's
            tick. The rest of the bundle is the follower's half's. */
        if (message.isBundle())
        {
            for (auto at = message.elements.rbegin(); at != message.elements.rend(); ++at)
            {
                if (! at->isBundle() && at->address == wire::beat && ! at->args.empty() && at->args[0].isInt64())
                {
                    item.kind = Item::Kind::tick;
                    item.tick = at->args[0].getInt64();
                    push (std::move (item));
                    return;
                }
            }

            return;
        }

        const auto& a = message.args;

        if (const auto said = wire::readHello (message))
        {
            item.kind = Item::Kind::hello;
            item.hello = *said;
            push (std::move (item));
            return;
        }

        if (message.address == wire::order && a.size() == 2 && a[0].isString() && a[1].isString()
              && (a[0].getString() == "hold" || a[0].getString() == "lead"))
        {
            item.kind = Item::Kind::order;
            item.ordered.order = a[0].getString() == "hold" ? Order::hold : Order::lead;
            item.ordered.reason = a[1].getString();
            push (std::move (item));
            return;
        }

        /*  A PING carries the pinging machine's tick third (RD.7) and the round
            trip it measured second; RD.5's, without the tick, is heard as it
            comes. A pong carries the answering machine's tick second. */
        if (message.address == wire::ping && ! a.empty() && a.size() <= 3 && a[0].isInt64())
        {
            item.kind = Item::Kind::tick;

            if (a.size() >= 2 && a[1].isNumber() && std::isfinite (a[1].asDouble()) && a[1].asDouble() >= 0.0)
                item.roundTrip = a[1].asDouble();

            if (a.size() == 3 && a[2].isInt64())
                item.tick = a[2].getInt64();

            push (std::move (item));
            return;
        }

        if ((message.address == wire::pong || message.address == wire::beat) && a.size() >= 2
              && a[0].isInt64() && a[1].isInt64())
        {
            item.kind = Item::Kind::tick;
            item.tick = message.address == wire::pong ? a[1].getInt64() : a[0].getInt64();
            push (std::move (item));
        }
    }

    void Presence::closed (const std::string& channel)
    {
        Item item;
        item.kind = Item::Kind::closed;
        item.channel = channel;
        push (std::move (item));
    }

    std::string Presence::relayChannel() const
    {
        const std::lock_guard<std::mutex> held { lock };
        return relayVia;
    }

    //==========================================================================
    void Presence::drain()
    {
        openedNow.clear();

        {
            const std::lock_guard<std::mutex> held { lock };
            items.swap (inbox);
        }

        for (auto& item : items)
            take (item);

        items.clear();
    }

    void Presence::take (Item& item)
    {
        if (item.kind == Item::Kind::closed)
        {
            known.erase (item.channel);
            return;
        }

        auto found = known.find (item.channel);

        if (found == known.end())
        {
            found = known.emplace (item.channel, Channel {}).first;
            openedNow.push_back (item.channel);
        }

        auto& channel = found->second;
        auto alive = false;

        switch (item.kind)
        {
            case Item::Kind::hello:
                /*  WHAT IT IS, and a sign of life: a hello is said by its tick
                    thread or as a connection opens. Its ticks are counted
                    afresh from here - a machine started again counts from
                    nought, and its first tick must not wait for the old one's. */
                says = std::move (item.hello);
                ++helloCount;
                channel.lastTick = -1;
                alive = true;
                break;

            case Item::Kind::order:
                orders.push_back (std::move (item.ordered));
                alive = true;
                break;

            case Item::Kind::tick:
                if (item.roundTrip >= 0.0)
                {
                    channel.roundTrip = item.roundTrip;
                    channel.tripAt = item.at;
                    channel.tripSaid = true;
                }

                //  ALIVE ONLY WHEN ITS TICK HAS MOVED: a socket answering for a hung engine is not a machine there.
                if (item.tick < 0 || item.tick > channel.lastTick)
                {
                    if (item.tick >= 0)
                        channel.lastTick = item.tick;

                    alive = true;
                }

                break;

            case Item::Kind::closed:
                break;
        }

        if (! alive)
            return;

        if (! channel.heardAny || item.at > channel.lastHeard)
            channel.lastHeard = item.at;

        channel.heardAny = true;

        if (latestChannel.empty() || item.at >= latestAt)
        {
            if (latestChannel != item.channel)
            {
                latestChannel = item.channel;

                const std::lock_guard<std::mutex> held { lock };
                relayVia = latestChannel;
            }

            latestAt = item.at;
        }
    }

    bool Presence::linked (LinkClock::time_point now, double timeoutSeconds) const
    {
        for (const auto& entry : known)
            if (entry.second.heardAny && within (entry.second.lastHeard, now, timeoutSeconds))
                return true;

        return false;
    }

    std::vector<Ordered> Presence::takeOrders()
    {
        std::vector<Ordered> out;
        out.swap (orders);
        return out;
    }

    std::vector<std::string> Presence::channels() const
    {
        std::vector<std::string> out;
        out.reserve (known.size());

        for (const auto& entry : known)
            out.push_back (entry.first);

        return out;
    }

    double Presence::roundTrip (LinkClock::time_point now, double timeoutSeconds) const
    {
        const Channel* freshest = nullptr;

        for (const auto& entry : known)
            if (entry.second.tripSaid && within (entry.second.tripAt, now, timeoutSeconds)
                  && (freshest == nullptr || entry.second.tripAt > freshest->tripAt))
                freshest = &entry.second;

        return freshest != nullptr ? freshest->roundTrip : 0.0;
    }
}
