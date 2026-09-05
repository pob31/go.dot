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
    The /godot namespace, served over HTTP and WebSocket on one port.

    OSCQuery, as much of it as Phase 1 has anything to say. HTTP answers
    questions about the tree; the WebSocket carries subscriptions in text and
    OSC in binary, both ways. One port for both is not a convenience - the
    specification requires it, and it is the entire reason juce_simpleweb is a
    dependency rather than juce::StreamingSocket.

    IT NEVER TOUCHES THE MODEL, and the shape of this class is mostly that one
    rule made structural. juce_simpleweb calls in on its own threads, one per
    connection, at times nothing here chooses. So:

      * READS go through a published TreeSnapshot - immutable, complete, and
        already the answer to somebody's tick. A request that arrives between
        ticks is answered from the last one rather than from a model in the
        middle of being changed, which is also why two clients asking the same
        question in the same tick get identical bytes.
      * WRITES become events on Engine's queue, stamped with the connection's
        origin, and are applied by the tick thread in arrival order like every
        other mutation. There is no path from a socket to the document, and
        PRD 4.11 is what says there must not be.

    THE SEAM IS `Namespace`, below. The server knows how to speak OSCQuery and
    nothing about Go.dot: what a tree is, where snapshots come from, what
    happens to a write. That is deliberate - the plan has this shell moving into
    spatcore eventually, where WFS-DIY and XOA would use it against namespaces
    of their own - and it is also what lets the tests drive it with a two-node
    fake instead of standing up an engine.

    WHAT PHASE 1 DOES NOT DO, so nobody looks for it:

      * mDNS/Bonjour advertisement (`_oscjson._tcp`). Clients are pointed at a
        host and port. juce::NetworkServiceDiscovery is NOT mDNS and would
        advertise to nothing that speaks OSCQuery.
      * TLS. SIMPLEWEB_SECURE_SUPPORTED=0, no OpenSSL in the binary, and
        deps.no-openssl keeps it that way.
      * address patterns. A LISTEN or a write whose address carries a star
        where a cue identifier belongs is refused, in the one voice the codec
        already uses for it.
*/

#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/tree/OscQueryJson.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::oscquery
{
    /*  What the server needs from whatever it is serving.

        Four questions and one instruction. Everything Go.dot-shaped lives
        behind them, which is what keeps this file free of ShowDocument,
        ParameterTree, CommandRegistry and the engine.

        Implemented by `EngineNamespace` (OscQueryServer.cpp) for the real
        thing, and by a fake in the tests. */
    class Namespace
    {
    public:
        virtual ~Namespace() = default;

        /** The current published snapshot. Never null. Called from any thread. */
        virtual std::shared_ptr<const tree::TreeSnapshot> snapshot() const = 0;

        /*  A write arriving from a client: a node value or a command.

            Returns nothing. OSC has no reply channel, and neither does this:
            the outcome is a record in the event log and a reading at
            `/godot/engine/lastError`, which the black-box driver asserts on.
            Making this return a status would invent a synchronous answer that
            the queue hop means the server cannot actually have. */
        virtual void write (const std::string& origin, const osc::Packet& packet) = 0;

        /*  A connection went away: release whatever it was holding.

            Not optional. PRD 3.16 has a disconnect release every touch that
            origin held, or a surface that crashed mid-gesture would leave a
            node gated against everybody for the rest of the show. */
        virtual void forget (const std::string& origin) = 0;

        /*  May this value go to that connection, at this tick?

            Asked rather than decided here, so that ALL the policy lives in one
            place behind the seam - PRD 3.16's touch gating and echo suppression
            are the same question from two directions, and splitting them across
            two files is how they come to disagree.

            Two rules today, both of which say no:
              * `toOrigin` caused this change. Its fader is already there, and
                pushing it back is what makes a slider fight the hand on it.
              * `toOrigin` is touching this node. It is mid-gesture and has
                asked not to be corrected until it lets go. */
        virtual bool shouldPush (const std::string& toOrigin,
                                 const std::string& address,
                                 const std::string& causedBy) const = 0;

        /** The UDP port to advertise in HOST_INFO. Zero when OSC is not bound. */
        virtual int oscPort() const = 0;
    };

    //==========================================================================
    class OscQueryServer
    {
    public:
        OscQueryServer();
        ~OscQueryServer();

        OscQueryServer (const OscQueryServer&) = delete;
        OscQueryServer& operator= (const OscQueryServer&) = delete;

        /*  Binds and starts serving. `nameSpace` must outlive the server.

            `port` 0 binds an ephemeral one, and boundPort() then says which.
            That is what every test uses and what `wfg serve --http-port=0`
            gives the black-box harness: a fixed port makes a suite that cannot
            run twice at once, and two Go.dot instances on one machine would
            collide.

            It did not always work. juce_simpleweb's start callback compared the
            port REQUESTED against the port GRANTED, so an ephemeral request
            bound successfully and then reported failure for ever, discarding
            the number. Fixed upstream in the fork at b72ec94 and pinned here;
            OscQueryTests has a case that binds 0 and reaches the port it is
            told, so a regression in the submodule surfaces here rather than in
            PR 1.10's harness. */
        bool start (int port, Namespace& nameSpace);

        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }
        int boundPort() const noexcept { return port.load (std::memory_order_relaxed); }

        //======================================================================
        /*  Publishes what changed, to whoever asked for it. TICK THREAD ONLY,
            once per tick, after the snapshot for that tick is published.

            Coalesced by construction: it sends the value each subscribed node
            HAS at this tick, not the succession of values it passed through
            during it. A node written forty times in one tick produces one push,
            which is the difference between a control surface and a flood.

            `causedBy` is the origin whose write produced this tick's changes.
            It is not sent anything it caused - the client already knows, its
            fader is already there, and echoing it back is what makes a slider
            fight the hand holding it. */
        void publishChanges (const tree::TreeDiff& diff,
                             const tree::TreeSnapshot& current,
                             const std::string& causedBy);

        //======================================================================
        //  Diagnostics, for tests and for /godot/engine.
        std::size_t connectionCount() const;
        std::int64_t messagesIn() const noexcept
        {
            return inbound.load (std::memory_order_relaxed);
        }
        std::int64_t messagesOut() const noexcept
        {
            return outbound.load (std::memory_order_relaxed);
        }

    private:
        /** One text frame to every open connection, for PATH_* notifications. */
        void broadcastText (const std::string& message);

        struct Impl;
        std::unique_ptr<Impl> impl;

        std::atomic<bool> running { false };
        std::atomic<int> port { 0 };
        std::atomic<std::int64_t> inbound { 0 };
        std::atomic<std::int64_t> outbound { 0 };
    };
}
