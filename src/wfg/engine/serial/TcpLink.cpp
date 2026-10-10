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

#include <wfg/engine/serial/TcpLink.h>

#include <juce_core/juce_core.h>

#include <cstdlib>

namespace wfg::serial
{
    namespace
    {
        /*  Connecting waits this long and no longer: the worker is on its own
            thread, but a device that is off should be reported as retrying
            within the tick thread's patience, not after a TCP handshake has
            timed out on its own terms. */
        constexpr int connectTimeoutMs = 2000;

        class TcpLink final : public Link
        {
        public:
            explicit TcpLink (std::unique_ptr<juce::StreamingSocket> connected) : socket (std::move (connected)) {}

            std::optional<std::string> read (std::chrono::milliseconds wait) override
            {
                if (socket == nullptr)
                    return std::nullopt;

                const auto ready = socket->waitUntilReady (true, static_cast<int> (wait.count()));

                if (ready < 0)
                    return fail ("the connection went away");

                if (ready == 0)
                    return std::string {};

                char buffer[4096];
                const auto got = socket->read (buffer, static_cast<int> (sizeof (buffer)), false);

                /*  A READ OF NOTHING ON A READY SOCKET IS THE OTHER END CLOSING:
                    that is how a stream says goodbye, and it is the failure the
                    worker retries on rather than an empty read to wait through. */
                if (got <= 0)
                    return fail (got < 0 ? "the connection went away" : "the device closed the connection");

                return std::string (buffer, static_cast<std::size_t> (got));
            }

            bool write (const std::string& bytes) override
            {
                if (socket == nullptr)
                    return false;

                const auto written = socket->write (bytes.data(), static_cast<int> (bytes.size()));

                if (written != static_cast<int> (bytes.size()))
                {
                    fail ("the connection went away while writing");
                    return false;
                }

                return true;
            }

            std::string problem() const override { return why; }

        private:
            std::optional<std::string> fail (const char* reason)
            {
                why = reason;
                socket.reset();
                return std::nullopt;
            }

            std::unique_ptr<juce::StreamingSocket> socket;
            std::string why;
        };
    }

    std::string hostPortOf (const std::string& host, int port)
    {
        return host + ":" + std::to_string (port);
    }

    bool splitHostPort (const std::string& hostPort, std::string& host, int& port)
    {
        const auto colon = hostPort.rfind (':');

        if (colon == std::string::npos || colon == 0 || colon + 1 >= hostPort.size())
            return false;

        host = hostPort.substr (0, colon);
        port = std::atoi (hostPort.c_str() + colon + 1);
        return port > 0 && port <= 65535;
    }

    std::unique_ptr<Link> openTcpLink (const std::string& hostPort, int, std::string& problem)
    {
        std::string host;
        int port = 0;

        if (! splitHostPort (hostPort, host, port))
        {
            problem = hostPort + ": not a host and a port";
            return nullptr;
        }

        auto socket = std::make_unique<juce::StreamingSocket>();

        if (! socket->connect (juce::String (host), port, connectTimeoutMs))
        {
            problem = hostPort + ": nothing answered";
            return nullptr;
        }

        return std::make_unique<TcpLink> (std::move (socket));
    }
}
