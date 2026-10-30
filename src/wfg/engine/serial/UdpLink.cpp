// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/serial/UdpLink.h>

#include <wfg/engine/serial/TcpLink.h>

#include <juce_core/juce_core.h>

#include <vector>

namespace wfg::serial
{
    namespace
    {
        /*  The largest datagram IPv4 carries; SSC sends a whole message in
            one, and a reply about many properties can be long. */
        constexpr int largestDatagram = 65507;

        class UdpLink final : public Link
        {
        public:
            UdpLink (std::unique_ptr<juce::DatagramSocket> bound, std::string hostToUse, int portToUse)
                : socket (std::move (bound)), host (std::move (hostToUse)), port (portToUse),
                  buffer (static_cast<std::size_t> (largestDatagram))
            {
            }

            std::optional<std::string> read (std::chrono::milliseconds wait) override
            {
                if (socket == nullptr)
                    return std::nullopt;

                const auto ready = socket->waitUntilReady (true, static_cast<int> (wait.count()));

                if (ready < 0)
                    return fail ("the socket went away");

                if (ready == 0)
                    return std::string {};

                juce::String from;
                int fromPort = 0;
                const auto got = socket->read (buffer.data(), largestDatagram, false, from, fromPort);

                if (got < 0)
                    return fail ("the socket went away");

                /*  ONLY THE DEVICE'S OWN: a datagram from another port, or -
                    where the host is a number - from another address, is not
                    its report, and is let fall. A host given by name is
                    matched on the port alone: the socket reports a sender by
                    number, and resolving the name again for every datagram
                    would put a lookup on the link's thread. */
                if (got == 0 || fromPort != port || (! numeric.empty() && from.toStdString() != numeric))
                    return std::string {};

                return std::string (buffer.data(), static_cast<std::size_t> (got));
            }

            bool write (const std::string& bytes) override
            {
                if (socket == nullptr)
                    return false;

                const auto sent = socket->write (juce::String (host), port, bytes.data(), static_cast<int> (bytes.size()));

                if (sent != static_cast<int> (bytes.size()))
                {
                    fail ("a datagram could not be sent");
                    return false;
                }

                return true;
            }

            std::string problem() const override { return why; }

            /*  The host when it is a number, which is how the socket reports
                a sender; empty for a name. */
            std::string numeric;

        private:
            std::optional<std::string> fail (const char* reason)
            {
                why = reason;
                socket.reset();
                return std::nullopt;
            }

            std::unique_ptr<juce::DatagramSocket> socket;
            std::string host;
            int port = 0;
            std::vector<char> buffer;
            std::string why;
        };
    }

    std::unique_ptr<Link> openUdpLink (const std::string& hostPort, int, std::string& problem)
    {
        std::string host;
        int port = 0;

        if (! splitHostPort (hostPort, host, port))
        {
            problem = hostPort + ": not a host and a port";
            return nullptr;
        }

        auto socket = std::make_unique<juce::DatagramSocket> (false);

        if (! socket->bindToPort (0))
        {
            problem = hostPort + ": no port to send from";
            return nullptr;
        }

        auto link = std::make_unique<UdpLink> (std::move (socket), host, port);
        const auto address = juce::IPAddress (juce::String (host));
        link->numeric = address.isNull() ? std::string {} : address.toString().toStdString();
        return link;
    }
}
