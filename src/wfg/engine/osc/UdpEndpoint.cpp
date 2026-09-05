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

#include <wfg/engine/osc/UdpEndpoint.h>

#include <juce_core/juce_core.h>

namespace wfg::osc
{
    namespace
    {
        /*  How long a blocked read waits before looking at the stop flag.

            The only reason it is bounded at all: a socket with nothing arriving
            would otherwise sit in the kernel until something did, and stop()
            would wait with it. A tenth of a second is imperceptible on the way
            out and costs ten wake-ups a second on an idle port. */
        constexpr int readTimeoutMs = 100;
    }

    //==============================================================================
    std::string Datagram::origin() const
    {
        return "udp:" + senderIp + ":" + std::to_string (senderPort);
    }

    //==============================================================================
    UdpEndpoint::UdpEndpoint() = default;

    UdpEndpoint::~UdpEndpoint()
    {
        stop();
    }

    //==============================================================================
    bool UdpEndpoint::start (int portToBind, Handler handler)
    {
        if (worker.joinable() || handler == nullptr)
            return false;

        auto bound = std::make_unique<juce::DatagramSocket>();

        if (! bound->bindToPort (portToBind))
            return false;

        socket = std::move (bound);
        port.store (socket->getBoundPort(), std::memory_order_relaxed);

        /*  Written before the thread exists, and never written again while it
            runs. That is the whole of the synchronisation story for the
            handler: there is no setter, so there is nothing to race with. */
        onDatagram = std::move (handler);

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);
        worker = std::thread ([this] { run(); });

        return true;
    }

    void UdpEndpoint::stop()
    {
        if (! worker.joinable())
        {
            socket.reset();
            return;
        }

        stopping.store (true, std::memory_order_relaxed);

        /*  shutdown() before the join, or the thread could be inside a read
            that nothing is going to satisfy and the join would wait out the
            timeout for no reason. */
        if (socket != nullptr)
            socket->shutdown();

        worker.join();
        running.store (false, std::memory_order_relaxed);

        socket.reset();
        onDatagram = nullptr;
    }

    //==============================================================================
    void UdpEndpoint::run()
    {
        std::vector<std::uint8_t> buffer (static_cast<std::size_t> (maxDatagramSize));

        while (! stopping.load (std::memory_order_relaxed))
        {
            const auto ready = socket->waitUntilReady (true, readTimeoutMs);

            if (ready < 0)
                break;                  // the socket is gone

            if (ready == 0)
                continue;               // nothing arrived; look at the flag again

            juce::String senderIp;
            int senderPort = 0;

            const auto bytesRead = socket->read (buffer.data(), maxDatagramSize, false,
                                                 senderIp, senderPort);

            if (bytesRead < 0)
            {
                errors.fetch_add (1, std::memory_order_relaxed);
                continue;
            }

            if (bytesRead == 0)
                continue;

            /*  A zero-length datagram is legal UDP and is not an OSC packet;
                it is counted as received and handed on, because deciding what
                bytes mean is the codec's job and not this one's. */
            Datagram datagram;
            datagram.bytes.assign (buffer.begin(),
                                   buffer.begin() + static_cast<std::ptrdiff_t> (bytesRead));
            datagram.senderIp = senderIp.toStdString();
            datagram.senderPort = senderPort;

            received.fetch_add (1, std::memory_order_relaxed);
            onDatagram (std::move (datagram));
        }
    }

    //==============================================================================
    bool UdpEndpoint::send (const std::string& host, int destinationPort,
                            const std::uint8_t* data, std::size_t size)
    {
        if (data == nullptr || size == 0 || destinationPort <= 0)
            return false;

        /*  A sender of its own when this endpoint is not receiving, so a caller
            can send without binding anything - which is what a test driving
            another endpoint needs, and what an outbound-only client is. */
        juce::DatagramSocket sender;
        auto* target = socket != nullptr ? socket.get() : &sender;

        const auto written = target->write (juce::String (host), destinationPort,
                                            data, static_cast<int> (size));

        return written == static_cast<int> (size);
    }

    bool UdpEndpoint::send (const std::string& host, int destinationPort,
                            const std::vector<std::uint8_t>& bytes)
    {
        return send (host, destinationPort, bytes.data(), bytes.size());
    }
}
