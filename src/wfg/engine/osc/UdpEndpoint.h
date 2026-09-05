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
    A UDP socket that says who sent each datagram.

    NOT juce::OSCReceiver, for two reasons. It throws on the type tags Go.dot
    needs - `h`, `d`, `N`, `I` and a time tag are all fatal to it - and it never
    exposes the sender. Go.dot needs the sender for every datagram, because the
    origin is what echo suppression and touch gating are keyed on (PRD §3.16),
    and because a rejection in the log that cannot say who caused it is a
    rejection nobody can act on.

    So: a raw juce::DatagramSocket on a thread of its own, and the five-argument
    read() overload, which is the only one that fills in the sender's address
    and port. That shape is spatcore's OSCReceiverWithSenderIP, which exists for
    exactly the same reason and says so in its own header.

    THE HANDLER IS FIXED AT start() AND NEVER CHANGES while the thread runs.
    spatcore's equivalent has a setRawDataCallback that writes a std::function
    with no synchronisation while the receive thread may be reading it; that is
    a data race with a very quiet failure mode. Here there is no setter: the
    handler goes in with start() and comes out with stop(), and the thread only
    ever reads a value written before it existed.

    THE HANDLER RUNS ON THE RECEIVE THREAD, deliberately, and it must be quick.
    Its job is to turn bytes into an event and hand it to Engine::submit, which
    is lock-bounded and does not touch the model. Anything slower belongs behind
    that queue: a handler that blocks here drops datagrams, because the socket
    buffer is the only thing catching them.

    NOTHING IS PARSED HERE. The endpoint moves bytes and names the sender; what
    those bytes mean is OscCodec's question. Keeping them apart is what lets the
    codec be tested against a byte fixture with no socket in sight.
*/

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace juce { class DatagramSocket; }

namespace wfg::osc
{
    /** One datagram, and who sent it. */
    struct Datagram
    {
        std::vector<std::uint8_t> bytes;
        std::string senderIp;
        int senderPort = 0;

        /*  The origin string the event log records: `udp:<ip>:<port>`.

            Port included, not just the address. Two clients behind one NAT
            share an address and must not share an origin, or echo suppression
            would silence a message for a surface that never sent it. */
        std::string origin() const;
    };

    class UdpEndpoint
    {
    public:
        using Handler = std::function<void (Datagram)>;

        UdpEndpoint();
        ~UdpEndpoint();

        UdpEndpoint (const UdpEndpoint&) = delete;
        UdpEndpoint& operator= (const UdpEndpoint&) = delete;

        /*  Binds and starts receiving. `port` 0 binds an ephemeral one, which
            is what every test uses - a fixed port makes a suite that cannot run
            twice at once, and this project runs ctest in parallel.

            False when the port could not be bound. Calling it twice without a
            stop() in between does nothing and returns false. */
        bool start (int port, Handler handler);

        /** Stops receiving and joins. The destructor calls it. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        /** The port actually bound, which is what a caller passing 0 needs. */
        int boundPort() const noexcept { return port.load (std::memory_order_relaxed); }

        //======================================================================
        /*  Sends one datagram. Independent of the receive side: an endpoint
            that was never started can still send, which is what a test driving
            another endpoint needs. */
        bool send (const std::string& host, int destinationPort,
                   const std::uint8_t* data, std::size_t size);

        bool send (const std::string& host, int destinationPort,
                   const std::vector<std::uint8_t>& bytes);

        //======================================================================
        std::int64_t datagramsReceived() const noexcept
        {
            return received.load (std::memory_order_relaxed);
        }

        /** Datagrams that arrived but could not be read. Diagnostics. */
        std::int64_t readErrors() const noexcept
        {
            return errors.load (std::memory_order_relaxed);
        }

        /** The largest datagram a single read will take. */
        static constexpr int maxDatagramSize = 65536;

    private:
        void run();

        std::unique_ptr<juce::DatagramSocket> socket;
        Handler onDatagram;

        std::thread worker;
        std::atomic<bool> stopping { false };
        std::atomic<bool> running { false };
        std::atomic<int> port { 0 };
        std::atomic<std::int64_t> received { 0 };
        std::atomic<std::int64_t> errors { 0 };
    };
}
