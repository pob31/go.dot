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
    What leaves Go.dot for somebody else's box, and when.

    ONE FLUSH PER TICK, AT THE END OF IT, and that is the whole design. A write
    to a mounted node does not reach a socket where it happens: it is queued,
    and everything queued during a tick leaves together once the tick's commands
    have all been applied. Three things follow, and each is a reason rather than
    a consequence.

    EVERY MESSAGE BELONGING TO ONE GO LEAVES IN THE SAME FRAME (PRD §3.4). A cue
    that moves twelve parameters is twelve datagrams that a receiving box sees
    as one gesture, not a dribble spread across whatever the tick thread was
    doing. Sending at the point of the write would have made the spread depend
    on the order commands happened to arrive in.

    A NODE WRITTEN FORTY TIMES IN A TICK SENDS ONCE. The queue is keyed by
    address and the last value wins, which is the same coalescing the OSCQuery
    push side already gets from reading a published snapshot. A fade running at
    fifty a second and a client dragging a fader at four hundred both come out
    at the tick rate, and neither can flood a console.

    THE SYSCALL IS BOUNDED AND IN ONE PLACE. `sendto` blocks; PRD §4.2's
    lipogram is about the audio thread and says nothing about this one, but an
    unbounded loop of syscalls anywhere near the tick is still how a 50 Hz clock
    stops being 50 Hz. One flush, one pass over a queue whose length is the
    number of distinct addresses written, and nothing hidden inside a command
    handler.

    WHAT IT DOES NOT DO. It does not retry, because UDP has no notion of a
    delivery to retry and a resend of a stale value is worse than a gap. It does
    not rate-cap: `mount/<id>/rateCap` is declared and Phase 4's prepare/commit
    is what will read it, and capping before there is anything to cap would be
    inventing behaviour nobody has measured. It does not bundle: one message per
    datagram, because bundle support is uneven in the field and Phase 4's
    timetagged bundles to Go.dot's OWN processors are a different feature with a
    different reason.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace wfg::osc
{
    class UdpEndpoint;
}

namespace wfg::tree
{
    class MountSender
    {
    public:
        /** Where one target is. Copied from the mount's declaration at queue
            time, so a reload cannot move a message that is already in flight. */
        struct Destination
        {
            std::string host = "127.0.0.1";
            int port = 0;
        };

        /*  The socket is a reference and is not owned. In `wfg serve` it is the
            one endpoint the process has, already bound, so every outbound
            datagram carries the same source port and a receiving box sees one
            correspondent rather than a new one per message. */
        explicit MountSender (osc::UdpEndpoint& socket) noexcept : udp (&socket) {}

        /*  A sender with nowhere to send, which is a complete configuration and
            not a degraded one: `wfg replay` and `wfg tree` have no socket and
            must still queue, coalesce and report exactly as a live session did,
            because only the sound is missing. */
        MountSender() = default;

        /*  The socket, when it comes to exist after this object does. `wfg
            serve` builds its command set before it opens a port, because the
            OSCQuery namespace needs the port number and the port needs the
            handler that the namespace provides - so the endpoint is the LAST
            thing constructed, and a sender captured into a command handler
            before it cannot have been given one yet. */
        void setSocket (osc::UdpEndpoint& socket) noexcept { udp = &socket; }

        //======================================================================
        /*  Queues one message. Tick thread.

            Returns a ticket that names this message for the rest of its short
            life. `outcomeOf` answers with it after the flush, which is what
            lets a cue whose wait is `sent` report what actually happened rather
            than what was asked for. */
        std::uint64_t queue (const std::string& mountId, const Destination&,
                             const std::string& address, const osc::Value&);

        /*  Sends everything queued and empties the queue. Tick thread, once per
            tick, AFTER the tick's commands have been applied - anything else
            sends a tick's writes in the middle of the tick that made them. */
        void flush();

        //======================================================================
        enum class Outcome { pending, sent, failed };

        /** What became of one queued message. */
        Outcome outcomeOf (std::uint64_t ticket) const;

        /** How many messages have left for a mount since the show opened. */
        std::size_t sentFor (const std::string& mountId) const;

        /** How many are waiting for the next flush. */
        std::size_t pending() const noexcept { return queued.size(); }

    private:
        struct Message
        {
            std::uint64_t ticket = 0;
            std::string mountId;
            Destination destination;
            std::string address;
            osc::Value value;
        };

        osc::UdpEndpoint* udp = nullptr;

        std::vector<Message> queued;          // in first-queued order
        std::map<std::string, std::size_t> queuedAt;   // address -> index in queued
        std::map<std::string, std::size_t> sent;       // mount id -> count

        /*  What the last few flushes did, newest last. Bounded because it is a
            diagnostic and a handful of cues' worth of answers, never a log: the
            log is the log, and §3.15 keeps per-message readouts out of it. */
        std::deque<std::pair<std::uint64_t, bool>> outcomes;

        std::uint64_t nextTicket = 1;

        static constexpr std::size_t outcomesKept = 512;
    };
}
