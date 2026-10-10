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
    THE ANSWERS GO.DOT SENDS A PROCESSOR, and nothing else (namespace draft §56,
    decision AEI: "Go.dot answers a capture with one message to the device").

    Go.dot has no reply channel, on purpose (EngineNamespace.cpp: a rejection is
    an `R` record and /godot/engine/lastError). Two verbs are the exception,
    because a processor writing a cue into the show has to know whether it
    landed: `mount.declare` answers /godot/declared, `cue.capture` answers
    /godot/captured, and a description fetched answers /godot/described.

    A SIBLING OF MountSender, NOT A METHOD ON IT. Everything MountSender queues
    is keyed by address and coalesced, rate-capped and bundled per device: a
    value written forty times leaves once. An answer is none of that - two
    captures in one tick are two answers - and it writes no node, so it must not
    reach the tree, a ticket or a device's `sent` count either.

    SAME SOCKET, SAME MOMENT. Answers leave from the one endpoint the process
    has, so a processor sees Go.dot's port as the source, and at the END of the
    tick, after MountSender's flush, so an answer never overtakes the cue's
    first send nor a syscall sits inside a command handler.

    WITH NO SOCKET IT ANSWERS NOBODY, and that is complete: `wfg replay` and
    `wfg tree` apply the same records and must not reach the network.
*/

#include <wfg/engine/osc/OscCodec.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::osc
{
    class UdpEndpoint;
}

namespace wfg::tree
{
    class RawSender
    {
    public:
        explicit RawSender (osc::UdpEndpoint& socket) noexcept : udp (&socket) {}

        /** A sender with nowhere to send: replay, tree, validate and the rigs. */
        RawSender() = default;

        /*  The socket, when it comes to exist after this object does - serve
            builds its commands before it opens a port (MountSender::setSocket). */
        void setSocket (osc::UdpEndpoint& socket) noexcept { udp = &socket; }

        /*  Queues one packet for `host:port`. Tick thread. A port outside
            1..65535 or an empty host is dropped at once and counted: an answer
            to nowhere is not an error worth more than that. */
        void queue (const std::string& host, int port, osc::Packet packet);

        /*  Sends everything queued, in order, and empties the queue. Tick
            thread, once per tick, after MountSender::flush. */
        void flush();

        std::size_t pending() const noexcept { return queued.size(); }
        std::size_t sentCount() const noexcept { return sent; }
        std::size_t droppedCount() const noexcept { return dropped; }

        /** What was queued and not yet flushed, for tests. */
        struct Raw
        {
            std::string host {};
            int port = 0;
            osc::Packet packet {};
        };

        const std::vector<Raw>& waiting() const noexcept { return queued; }

    private:
        osc::UdpEndpoint* udp = nullptr;
        std::vector<Raw> queued;
        std::size_t sent = 0;
        std::size_t dropped = 0;
    };
}
