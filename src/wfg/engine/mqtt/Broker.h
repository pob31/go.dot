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
    GO.DOT'S OWN MQTT BROKER (namespace draft §67, AND, ANG; MQ.5): a service
    of this machine, like its OSC port, so a sensor, a controller or a hub
    with nothing but Go.dot's address on it has somewhere to publish.

    What it is: MQTT 3.1.1 and 3.1; level 5 refused at CONNECT. Clean
    sessions only - session present is always nought. Every subscription is
    granted QoS 0, which the specification allows and which keeps the broker
    from tracking acknowledgements it never needs; a client's QoS 1 publish
    is acknowledged, a QoS 2 one walked through PUBREC, PUBREL, PUBCOMP.
    Retained messages are kept and handed to a new subscriber; a will is
    sent when a client goes without saying; a client silent for one and a
    half times its keepalive is dropped; `+` and `#` in a filter, a `$`
    topic hidden from the root wildcards. An optional login, one user name
    and password for everybody.

    How it runs: one thread listens and accepts; one thread a socket client,
    each read bounded; every change of the hub - who is connected, who
    subscribes to what, what is retained - under one lock, and what a client
    is to be sent queued on it for its own thread to write. Nothing on these
    threads touches the show. A device of the show on this broker (an empty
    host, AND) has no socket: `localLink` is a serial::Link whose writes are
    handled on the writer's thread and whose reads take what the hub queued,
    so the serial table runs it as it runs a connection.
*/

#include <wfg/engine/mqtt/Packet.h>
#include <wfg/engine/serial/SerialLink.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace juce { class StreamingSocket; }

namespace wfg::mqtt
{
    struct BrokerState
    {
        std::string state = "off";      // off, listening, problem
        std::string problem;
        std::uint64_t clients = 0;      // connected now
        int port = 0;                   // the bound port while listening
    };

    class Broker
    {
    public:
        Broker();
        ~Broker();

        Broker (const Broker&) = delete;
        Broker& operator= (const Broker&) = delete;

        /*  Listens on `port` (nought for any free one, as a test wants) with
            the login clients must give - `<user> <password>`, empty for
            anyone. False, with the sentence on the state, when the port
            cannot be bound. Calling it while listening stops first. */
        bool start (int port, const std::string& login);
        void stop();

        bool listening() const noexcept { return isListening.load (std::memory_order_acquire); }
        BrokerState stateNow() const;

        /*  A LINK TO THIS BROKER WITH NO SOCKET (AND), for the serial table:
            nullptr with `problem` while the broker is off. The link speaks
            the protocol as a socket client does - CONNECT first. */
        std::unique_ptr<serial::Link> localLink (std::string& problem);

        /*  The whole hub, for tests: how many hold a subscription now. */
        std::size_t subscribers() const;

        struct Client;
        struct Hub;

    private:
        std::shared_ptr<Hub> hub;
        std::atomic<bool> isListening { false };
        std::atomic<bool> stopping { false };
        std::thread acceptor;
        std::string loginWanted;
        int portWanted = 0;

        mutable std::mutex stateLock;
        BrokerState state;

        void acceptOn (std::unique_ptr<juce::StreamingSocket> listener);
    };
}
