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
    Who is listening to what.

    OSCQuery's LISTEN and IGNORE, kept per connection. A client sends
    `{"COMMAND": "LISTEN", "DATA": "/godot/cue/B3N8R5TW/name"}` over the
    WebSocket and expects that node's value from then on, without polling.

    THIS TABLE IS TOUCHED FROM TWO THREADS and says so out loud, because it is
    the only structure in the engine that is. LISTEN and IGNORE arrive on
    juce_simpleweb's server thread; the per-tick flush that reads the table runs
    on the tick thread. Everything else in Go.dot avoids sharing by queueing,
    and that is not available here: a subscription must take effect for the very
    next tick, and a client that unsubscribed must not receive one more push.
    So: one mutex, held only for the length of a set lookup, never across a
    send. The lock is never taken while the model is being read, so it cannot
    order against the tick thread's own work.

    A CONNECTION THAT GOES AWAY TAKES ITS SUBSCRIPTIONS WITH IT. That is not
    housekeeping - juce_simpleweb reuses connection ids (they are
    `<ip>:<port>`, and a port is reused within seconds on a busy loopback), so a
    table that outlived its connection would deliver a previous client's
    subscriptions to a new one. `drop()` is called from the connectionClosed
    listener for exactly that reason, and it also releases everything that
    origin was touching (PRD 3.16: a disconnect releases what it held).

    WHAT IS NOT HERE: pattern subscriptions. A client may LISTEN to one address
    at a time. The OSCQuery proposal does not define a wildcard LISTEN, and
    Go.dot's decoder refuses address patterns outright (osc/OscAddress.h), so
    accepting one here would be the only place in the engine where a pattern
    meant something.
*/

#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wfg::oscquery
{
    /*  A connection's identity, which is also its ORIGIN in the event log:
        `ws:<ip>:<port>`. The port is part of it because two clients behind one
        NAT share an address, and echo suppression keyed on the address alone
        would silence a message for a surface that never sent it. */
    using ConnectionId = std::string;

    class Subscriptions
    {
    public:
        Subscriptions() = default;

        Subscriptions (const Subscriptions&) = delete;
        Subscriptions& operator= (const Subscriptions&) = delete;

        /** LISTEN. False when that connection was already listening to it. */
        bool listen (const ConnectionId& connection, const std::string& address);

        /** IGNORE. False when it was not listening to it in the first place. */
        bool ignore (const ConnectionId& connection, const std::string& address);

        /*  Everything that must be told about `address`, in a stable order so a
            test can assert on it and two runs push in the same sequence.

            The ORDER of pushes across connections is not otherwise defined by
            anything, and leaving it to a hash map's iteration order would make
            a replay's byte-for-byte claim depend on the allocator. */
        std::vector<ConnectionId> listenersOf (const std::string& address) const;

        /** What this connection is listening to, sorted. */
        std::vector<std::string> heldBy (const ConnectionId& connection) const;

        /*  Forgets a connection entirely. Returns what it had been listening
            to, so the caller can release the touches that went with it.

            Called on disconnect, and it is not optional: connection ids are
            `<ip>:<port>` and a loopback port is reused within seconds, so a
            stale entry would deliver one client's subscriptions to the next
            client that happened to get the same port. */
        std::vector<std::string> drop (const ConnectionId& connection);

        std::size_t connectionCount() const;
        std::size_t totalSubscriptions() const;

        bool isListening (const ConnectionId& connection, const std::string& address) const;

    private:
        mutable std::mutex mutex;

        /*  Ordered containers, not hash maps, and for a reason that outlives
            performance: `listenersOf` has to return the same order on every run
            and on every platform, because the pushes it drives are recorded in
            the event log and the log has to replay byte for byte. */
        std::map<ConnectionId, std::set<std::string>> byConnection;
    };
}
