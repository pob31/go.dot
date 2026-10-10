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

#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>

#include <algorithm>
#include <cmath>

namespace wfg::tree
{
    std::uint64_t MountSender::queue (const std::string& mountId, const Destination& destination,
                                      const std::string& address, const osc::Values& values,
                                      const std::string& owner)
    {
        const auto ticket = nextTicket++;

        /*  LAST WRITE WINS, IN THE FIRST WRITE'S PLACE. Two things are being
            decided here and they pull in opposite directions, so both are
            stated: the VALUE is the newest, because sending a value somebody
            has already changed their mind about is sending a wrong number; the
            ORDER is the oldest, because a cue that sets a mode and then a
            parameter of that mode has to arrive in that order, and re-queueing
            a re-written address at the back would silently reverse it. */
        const auto existing = queuedAt.find (address);

        if (existing != queuedAt.end())
        {
            auto& message = queued[existing->second];

            /*  The superseded message is answered now rather than left pending.
                Nothing will ever flush it, and a cue waiting on a ticket that
                cannot arrive is a cue that hangs - the exact failure a
                three-valued wait exists to make visible. It reports SENT and
                not failed: what the caller asked for was that this address
                reach the target this tick, and it will. */
            outcomes.push_back ({ message.ticket, Outcome::sent, false });

            message.ticket = ticket;
            message.mountId = mountId;
            message.destination = destination;
            message.values = values;
            message.owner = owner;
            return ticket;
        }

        queuedAt[address] = queued.size();
        queued.push_back (Message { ticket, mountId, destination, address, values, owner });
        return ticket;
    }

    //==============================================================================
    namespace
    {
        /*  How many flushes must pass between two sends of one address.

            A flush is a tick and a tick is a fiftieth of a second, so a cap of
            fifty hertz is one flush - which every address already obeys, since
            the queue holds one message per address. Below fifty it waits; above
            it, and at nought, it does not. */
        std::uint64_t intervalFor (double rateCap) noexcept
        {
            if (! (rateCap > 0.0) || rateCap >= 50.0)
                return 1;

            return static_cast<std::uint64_t> (std::llround (50.0 / rateCap));
        }
    }

    void MountSender::flush()
    {
        ++flushes;

        /*  WHAT COULD NOT GO YET, kept in its place in the order.

            A capped address holds the newest value it was given and waits; the
            order it waits in is the order it was first queued in, which is the
            same rule the coalescing above follows and for the same reason - a
            cue that sets a mode and then a parameter of that mode has to arrive
            in that order. */
        std::vector<Message> waiting;

        /*  WHAT GOES TO A DEVICE THAT TAKES BUNDLES (namespace draft 45, YQ),
            gathered by device in the order its first message was queued, and
            sent once the rest have gone. Pointers into `queued`, which stands
            until the end of this flush. */
        std::vector<std::pair<std::string, std::vector<const Message*>>> bundled;

        for (const auto& message : queued)
        {
            const auto interval = intervalFor (message.destination.rateCap);
            const auto last = lastSentAt.find (message.address);

            /*  A MESSAGE THE RATE CAP HOLDS STAYS OUT OF THE BUNDLE, in its
                place in the queue: it goes in a later flush's. */
            if (interval > 1 && last != lastSentAt.end()
                 && flushes - last->second < interval)
            {
                waiting.push_back (message);
                continue;
            }

            lastSentAt[message.address] = flushes;

            if (message.destination.bundles)
            {
                auto device = std::find_if (bundled.begin(), bundled.end(),
                                            [&message] (const auto& entry) { return entry.first == message.mountId; });

                if (device == bundled.end())
                {
                    bundled.push_back ({ message.mountId, {} });
                    device = std::prev (bundled.end());
                }

                device->second.push_back (&message);
                continue;
            }

            sendAlone (message);
        }

        for (const auto& device : bundled)
            sendBundled (device.second);

        queued = std::move (waiting);
        queuedAt.clear();

        for (std::size_t index = 0; index < queued.size(); ++index)
            queuedAt[queued[index].address] = index;

        forgetOldAnswers();
    }

    bool MountSender::deliver (const Destination& to, const std::vector<std::uint8_t>& bytes)
    {
        if (! to.serial.empty())
            return serialSink ? serialSink (to.serial, bytes) : false;

        if (! to.link.empty())
            return linkSink ? linkSink (to.link, bytes) : false;

        return udp != nullptr && to.port > 0 && udp->send (to.host, to.port, bytes);
    }

    void MountSender::sendAlone (const Message& message)
    {
        auto ok = false;
        std::string error;

        /*  Encoded here rather than at queue time, because a coalesced address
            is encoded once however many times it was written - which is the
            point of coalescing, and would be lost if the bytes were built where
            the value arrived. */
        if (const auto bytes = osc::encode (osc::Packet::message (message.address, message.values), error))
            ok = deliver (message.destination, *bytes);

        if (ok)
            ++sent[message.mountId];

        outcomes.push_back ({ message.ticket, ok ? Outcome::sent : Outcome::failed, false });
    }

    void MountSender::sendBundled (const std::vector<const Message*>& messages)
    {
        /*  ONE DEVICE'S MESSAGES OF ONE FLUSH, in their order, as few bundles as
            fit (namespace draft 45, ZB): each closed before it would pass
            `bundleBytes`, time-tagged immediately - Go.dot's own processors
            would be the ones to read a later time, and spatcore's parser drops
            it (PRD 3.12). A message too big to share a datagram goes alone, as
            a plain message, and a bundle of one is still a bundle: the device
            said it reads them. Each message is answered and counted as itself. */
        constexpr std::size_t header = 16;      // "#bundle\0" and the time tag
        constexpr std::size_t lengthField = 4;  // each element's size, before it

        std::vector<const Message*> batch;
        std::vector<osc::Packet> elements;
        std::size_t size = header;

        const auto close = [this, &batch, &elements, &size]
        {
            if (batch.empty())
                return;

            auto ok = false;
            std::string error;
            const auto& to = batch.front()->destination;

            if (const auto bytes = osc::encode (osc::Packet::bundle (osc::TimeTag {}, elements), error))
                ok = deliver (to, *bytes);

            for (const auto* message : batch)
            {
                if (ok)
                    ++sent[message->mountId];

                outcomes.push_back ({ message->ticket, ok ? Outcome::sent : Outcome::failed, false });
            }

            batch.clear();
            elements.clear();
            size = header;
        };

        for (const auto* message : messages)
        {
            auto element = osc::Packet::message (message->address, message->values);
            std::string error;
            const auto bytes = osc::encode (element, error);
            const auto length = bytes.has_value() ? bytes->size() : 0u;

            if (! bytes.has_value() || header + lengthField + length > bundleBytes)
            {
                close();
                sendAlone (*message);
                continue;
            }

            if (size + lengthField + length > bundleBytes)
                close();

            elements.push_back (std::move (element));
            batch.push_back (message);
            size += lengthField + length;
        }

        close();
    }

    std::size_t MountSender::dropQueued (const std::function<bool (const std::string& owner)>& keep)
    {
        /*  WHAT IS KEPT KEEPS ITS PLACE, for the reason the coalescing keeps
            the first write's: a pre-send that sets a mode and then a parameter
            of that mode still arrives in that order. */
        std::vector<Message> kept;
        std::size_t dropped = 0;

        for (auto& message : queued)
        {
            if (keep && keep (message.owner))
            {
                kept.push_back (std::move (message));
                continue;
            }

            /*  ANSWERED NOW, as a superseded message is: nothing will ever
                flush it, and a cue waiting on a ticket that cannot arrive is a
                cue that hangs. `failed`, because it did not reach the target;
                marked dropped, because nothing went wrong on the wire. */
            outcomes.push_back ({ message.ticket, Outcome::failed, true });
            ++dropped;
        }

        queued = std::move (kept);
        queuedAt.clear();

        for (std::size_t index = 0; index < queued.size(); ++index)
            queuedAt[queued[index].address] = index;

        forgetOldAnswers();
        return dropped;
    }

    void MountSender::forgetOldAnswers()
    {
        while (outcomes.size() > outcomesKept)
            outcomes.pop_front();
    }

    bool MountSender::stillQueued (std::uint64_t ticket) const
    {
        return std::any_of (queued.begin(), queued.end(),
                            [ticket] (const Message& message)
                            {
                                return message.ticket == ticket;
                            });
    }

    //==============================================================================
    MountSender::Outcome MountSender::outcomeOf (std::uint64_t ticket) const
    {
        /*  From the back, because the answer a caller wants is almost always
            the most recent flush's - a cue asks one tick after it queued. */
        for (auto entry = outcomes.rbegin(); entry != outcomes.rend(); ++entry)
            if (entry->ticket == ticket)
                return entry->outcome;

        /*  Not flushed yet, or flushed so long ago that the answer has been
            dropped. `pending` is the honest word for both: this object no
            longer knows, and a caller still asking about a ticket five hundred
            messages old has a bug of its own. */
        return Outcome::pending;
    }

    bool MountSender::wasDropped (std::uint64_t ticket) const
    {
        for (auto entry = outcomes.rbegin(); entry != outcomes.rend(); ++entry)
            if (entry->ticket == ticket)
                return entry->dropped;

        return false;
    }

    std::size_t MountSender::sentFor (const std::string& mountId) const
    {
        const auto found = sent.find (mountId);
        return found == sent.end() ? 0u : found->second;
    }

    //==============================================================================
    MountSender::Destination MountSender::destinationFor (const MountDeclaration& declaration)
    {
        Destination destination;
        destination.host = declaration.host;
        destination.port = declaration.port;
        destination.rateCap = declaration.rateCap;
        destination.bundles = declaration.bundles;
        destination.serial = declaration.transport == "serial" ? declaration.serial : std::string {};
        destination.link = declaration.transport == "tcp" ? declaration.id : std::string {};
        return destination;
    }

    MountTable::WriteResult writeToDevice (MountTable& mounts, MountSender& sender,
                                           const std::string& address, const osc::Values& values)
    {
        const auto written = mounts.write (address, values);

        if (! written.ok)
            return written;

        if (const auto* declaration = mounts.declarationOf (written.mountId); declaration != nullptr && declaration->tx)
            sender.queue (written.mountId, MountSender::destinationFor (*declaration), address, written.values);

        return written;
    }
}
