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

namespace wfg::tree
{
    std::uint64_t MountSender::queue (const std::string& mountId, const Destination& destination,
                                      const std::string& address, const osc::Value& value)
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
            outcomes.emplace_back (message.ticket, true);

            message.ticket = ticket;
            message.mountId = mountId;
            message.destination = destination;
            message.value = value;
            return ticket;
        }

        queuedAt[address] = queued.size();
        queued.push_back (Message { ticket, mountId, destination, address, value });
        return ticket;
    }

    //==============================================================================
    void MountSender::flush()
    {
        for (const auto& message : queued)
        {
            auto ok = false;
            std::string error;

            /*  Encoded here rather than at queue time, because a coalesced
                address is encoded once however many times it was written -
                which is the point of coalescing, and would be lost if the bytes
                were built where the value arrived. */
            if (const auto bytes = osc::encode (osc::Packet::message (message.address,
                                                                      { message.value }),
                                                error))
                if (udp != nullptr && message.destination.port > 0)
                    ok = udp->send (message.destination.host, message.destination.port, *bytes);

            if (ok)
                ++sent[message.mountId];

            outcomes.emplace_back (message.ticket, ok);
        }

        queued.clear();
        queuedAt.clear();

        while (outcomes.size() > outcomesKept)
            outcomes.pop_front();
    }

    //==============================================================================
    MountSender::Outcome MountSender::outcomeOf (std::uint64_t ticket) const
    {
        /*  From the back, because the answer a caller wants is almost always
            the most recent flush's - a cue asks one tick after it queued. */
        for (auto entry = outcomes.rbegin(); entry != outcomes.rend(); ++entry)
            if (entry->first == ticket)
                return entry->second ? Outcome::sent : Outcome::failed;

        /*  Not flushed yet, or flushed so long ago that the answer has been
            dropped. `pending` is the honest word for both: this object no
            longer knows, and a caller still asking about a ticket five hundred
            messages old has a bug of its own. */
        return Outcome::pending;
    }

    std::size_t MountSender::sentFor (const std::string& mountId) const
    {
        const auto found = sent.find (mountId);
        return found == sent.end() ? 0u : found->second;
    }
}
