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

#include <wfg/engine/mqtt/Broker.h>

#include <wfg/engine/serial/Slip.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>

namespace wfg::mqtt
{
    namespace
    {
        constexpr int readWaitMs = 10;
        constexpr int acceptWaitMs = 200;
        constexpr std::size_t largestPacket = serial::slip::largestFrame;

        using Clock = std::chrono::steady_clock;

        std::string encodedOrNothing (const Packet& packet)
        {
            std::string problem;
            if (const auto bytes = encode (packet, problem))
                return std::string (bytes->begin(), bytes->end());
            return {};
        }
    }

    //==========================================================================
    /*  ONE CLIENT, socket or local: what the hub knows of it, and what is
        queued for it. `lock` guards `outgoing` and `closed`; the rest is the
        hub's, under the hub's lock. */
    struct Broker::Client
    {
        std::uint64_t number = 0;
        bool connected = false;              // CONNECT taken, CONNACK 0 sent
        std::string clientId;
        std::uint16_t keepAlive = 0;
        std::optional<Will> will;
        std::vector<std::string> filters;
        Clock::time_point lastHeard = Clock::now();
        bool local = false;

        std::mutex lock;
        std::condition_variable woken;
        std::string outgoing;
        bool closed = false;

        void queue (const std::string& bytes)
        {
            {
                const std::lock_guard<std::mutex> held (lock);
                outgoing += bytes;
            }
            woken.notify_all();
        }

        void close()
        {
            {
                const std::lock_guard<std::mutex> held (lock);
                closed = true;
            }
            woken.notify_all();
        }

        bool isClosed()
        {
            const std::lock_guard<std::mutex> held (lock);
            return closed;
        }
    };

    //==========================================================================
    /*  THE HUB: every client, every subscription, what is retained, under
        one lock. A packet from any thread is handled here; what it causes is
        queued on the clients it reaches. */
    struct Broker::Hub
    {
        std::mutex lock;
        std::vector<std::shared_ptr<Client>> clients;
        std::map<std::string, std::string> retained;    // topic -> payload
        std::string login;                               // "<user> <password>", empty for anyone
        std::uint64_t nextNumber = 1;
        std::uint64_t anonymous = 1;

        std::shared_ptr<Client> add (bool local)
        {
            auto client = std::make_shared<Client>();
            client->local = local;
            const std::lock_guard<std::mutex> held (lock);
            client->number = nextNumber++;
            clients.push_back (client);
            return client;
        }

        /*  Gone: its will published if it went without saying, its place
            given up. Under the lock. */
        void removeHeld (const std::shared_ptr<Client>& client, bool sayingGoodbye)
        {
            const auto at = std::find (clients.begin(), clients.end(), client);
            if (at == clients.end())
                return;
            clients.erase (at);

            if (! sayingGoodbye && client->connected && client->will.has_value())
            {
                const auto will = *client->will;
                publishHeld (will.topic, will.payload, will.retain, nullptr);
            }

            client->connected = false;
            client->close();
        }

        void remove (const std::shared_ptr<Client>& client, bool sayingGoodbye)
        {
            const std::lock_guard<std::mutex> held (lock);
            removeHeld (client, sayingGoodbye);
        }

        /*  A PUBLISH TO EVERYONE WHOSE FILTER MATCHES, at QoS 0, the retain
            flag off on a live delivery; kept, or forgotten for an empty
            payload, when the publisher asked. Under the lock. */
        void publishHeld (const std::string& topic, const std::string& payload, bool retain, const Client* from)
        {
            if (retain)
            {
                if (payload.empty())
                    retained.erase (topic);
                else
                    retained[topic] = payload;
            }

            const auto bytes = encodedOrNothing (Packet::publishOf (topic, payload, 0, false));
            if (bytes.empty())
                return;

            for (const auto& client : clients)
            {
                if (! client->connected)
                    continue;

                const bool wants = std::any_of (client->filters.begin(), client->filters.end(),
                                                [&topic] (const std::string& filter) { return topicMatches (filter, topic); });
                if (wants)
                    client->queue (bytes);
            }

            juce::ignoreUnused (from);
        }

        bool loginAccepted (const Packet& connect) const
        {
            if (login.empty())
                return true;

            const auto space = login.find (' ');
            const auto user = login.substr (0, space);
            const auto password = space == std::string::npos ? std::string {} : login.substr (space + 1);

            return connect.user.has_value() && *connect.user == user
                && connect.password.value_or (std::string {}) == password;
        }

        /*  ONE PACKET FROM ONE CLIENT. False when the client is to be closed
            after what was queued for it went out. */
        bool handle (const std::shared_ptr<Client>& client, const Packet& packet)
        {
            const std::lock_guard<std::mutex> held (lock);
            client->lastHeard = Clock::now();

            if (! client->connected && packet.type != Type::connect)
                return false;                         // §3.1: CONNECT first, or nothing

            switch (packet.type)
            {
                case Type::connect:
                {
                    if (client->connected)
                        return false;                 // a second CONNECT is a protocol violation

                    if (packet.protocolLevel != 4 && packet.protocolLevel != 3)
                    {
                        client->queue (encodedOrNothing (Packet::connackWith (connack::badProtocol)));
                        return false;
                    }

                    if (! loginAccepted (packet))
                    {
                        client->queue (encodedOrNothing (Packet::connackWith (connack::badLogin)));
                        return false;
                    }

                    /*  §3.1.3.1: an empty id is allowed with a clean session,
                        and given one here; without one it is refused. */
                    if (packet.clientId.empty() && ! packet.cleanSession)
                    {
                        client->queue (encodedOrNothing (Packet::connackWith (connack::badIdentifier)));
                        return false;
                    }

                    client->clientId = packet.clientId.empty() ? "anonymous-" + std::to_string (anonymous++) : packet.clientId;
                    client->keepAlive = packet.keepAlive;
                    client->will = packet.will;

                    /*  §3.1.4: a client of the same id already connected is
                        disconnected - the newcomer is the one that is there. */
                    for (const auto& other : clients)
                        if (other != client && other->connected && other->clientId == client->clientId)
                        {
                            other->connected = false;
                            other->close();
                        }

                    client->connected = true;
                    client->queue (encodedOrNothing (Packet::connackWith (connack::accepted, false)));
                    return true;
                }

                case Type::publish:
                    publishHeld (packet.topic, packet.payload, packet.retain, client.get());
                    if (packet.qos == 1)
                        client->queue (encodedOrNothing (Packet::acknowledgement (Type::puback, packet.packetId)));
                    else if (packet.qos == 2)
                        client->queue (encodedOrNothing (Packet::acknowledgement (Type::pubrec, packet.packetId)));
                    return true;

                case Type::pubrel:
                    client->queue (encodedOrNothing (Packet::acknowledgement (Type::pubcomp, packet.packetId)));
                    return true;

                case Type::subscribe:
                {
                    std::vector<std::uint8_t> codes;
                    std::vector<std::string> added;

                    for (const auto& [filter, qos] : packet.filters)
                    {
                        if (! validFilter (filter))
                        {
                            codes.push_back (subscribeFailed);
                            continue;
                        }

                        if (std::find (client->filters.begin(), client->filters.end(), filter) == client->filters.end())
                            client->filters.push_back (filter);
                        added.push_back (filter);
                        codes.push_back (0);
                        juce::ignoreUnused (qos);
                    }

                    client->queue (encodedOrNothing (Packet::subackWith (packet.packetId, std::move (codes))));

                    /*  §3.3.1.3: what is retained under a new subscription is
                        sent at once, its retain flag set. */
                    for (const auto& [topic, payload] : retained)
                        if (std::any_of (added.begin(), added.end(),
                                         [&topic] (const std::string& filter) { return topicMatches (filter, topic); }))
                            client->queue (encodedOrNothing (Packet::publishOf (topic, payload, 0, true)));
                    return true;
                }

                case Type::unsubscribe:
                    for (const auto& [filter, qos] : packet.filters)
                    {
                        juce::ignoreUnused (qos);
                        client->filters.erase (std::remove (client->filters.begin(), client->filters.end(), filter),
                                               client->filters.end());
                    }
                    client->queue (encodedOrNothing (Packet::acknowledgement (Type::unsuback, packet.packetId)));
                    return true;

                case Type::pingreq:
                    client->queue (encodedOrNothing (Packet::plain (Type::pingresp)));
                    return true;

                case Type::disconnect:
                    client->will.reset();                 // §3.14: the will is discarded
                    return false;

                case Type::puback:
                case Type::pubrec:
                case Type::pubcomp:
                    return true;                          // a QoS 0 broker sends nothing these answer; taken

                case Type::connack:
                case Type::suback:
                case Type::unsuback:
                case Type::pingresp:
                    return false;                         // a client does not send these
            }

            return false;
        }

        /*  Clients silent past one and a half times their keepalive: gone
            without saying. */
        std::vector<std::shared_ptr<Client>> silent()
        {
            std::vector<std::shared_ptr<Client>> gone;
            const std::lock_guard<std::mutex> held (lock);
            const auto now = Clock::now();

            for (const auto& client : clients)
                if (client->connected && client->keepAlive > 0 && ! client->local)
                {
                    const auto limit = std::chrono::milliseconds (static_cast<long long> (client->keepAlive) * 1500);
                    if (now - client->lastHeard > limit)
                        gone.push_back (client);
                }

            return gone;
        }

        std::uint64_t connectedCount()
        {
            const std::lock_guard<std::mutex> held (lock);
            return static_cast<std::uint64_t> (std::count_if (clients.begin(), clients.end(),
                                                               [] (const std::shared_ptr<Client>& c) { return c->connected; }));
        }
    };

    //==========================================================================
    namespace
    {
        /*  BYTES IN, PACKETS HANDLED: the stream of one client cut by the
            codec's own framing, each packet to the hub; a malformed one, or
            one too big, closes the client (§4.8). */
        struct Reader
        {
            std::string partial;

            /*  False when the client is to be closed. */
            bool feed (const std::string& bytes, Broker::Hub& hub, const std::shared_ptr<Broker::Client>& client)
            {
                partial += bytes;

                for (;;)
                {
                    std::string problem;
                    const auto whole = frameLength (reinterpret_cast<const std::uint8_t*> (partial.data()), partial.size(), problem);

                    if (! problem.empty() || whole > largestPacket)
                        return false;
                    if (whole == 0 || partial.size() < whole)
                        return true;

                    const auto decoded = decode (reinterpret_cast<const std::uint8_t*> (partial.data()), whole);
                    partial.erase (0, whole);

                    /*  A CONNECT IN A PROTOCOL THIS BROKER DOES NOT READ -
                        MQTT 5, most likely - is answered CONNACK 1 before
                        the close (§3.1.2.2), so the client is told rather
                        than left wondering. */
                    if (! decoded.ok && decoded.problem == reason::badProtocol && ! client->connected)
                        client->queue (encodedOrNothing (Packet::connackWith (connack::badProtocol)));

                    if (! decoded.ok || ! hub.handle (client, decoded.packet))
                        return false;
                }
            }
        };

        /*  ONE SOCKET CLIENT ON ITS OWN THREAD: reads bounded, what the hub
            queued written, closed when the hub says, when the socket goes,
            or when the broker stops. */
        void serveSocket (std::unique_ptr<juce::StreamingSocket> socket, std::shared_ptr<Broker::Hub> hub,
                          std::shared_ptr<Broker::Client> client, const std::atomic<bool>& stopping)
        {
            Reader reader;
            bool sayingGoodbye = false;
            bool keepReading = true;

            while (! stopping && socket->isConnected())
            {
                if (keepReading)
                {
                    const auto ready = socket->waitUntilReady (true, readWaitMs);
                    if (ready < 0)
                        break;

                    if (ready > 0)
                    {
                        char buffer[4096];
                        const auto got = socket->read (buffer, static_cast<int> (sizeof (buffer)), false);
                        if (got <= 0)
                            break;

                        if (! reader.feed (std::string (buffer, static_cast<std::size_t> (got)), *hub, client))
                        {
                            /*  Closed by the hub - a DISCONNECT, a refusal, a
                                malformed packet: what was queued goes out, then
                                the socket. A DISCONNECT dropped the will. */
                            sayingGoodbye = ! client->will.has_value() || ! client->connected;
                            keepReading = false;
                        }
                    }
                }

                std::string toWrite;
                bool closed = false;
                {
                    std::unique_lock<std::mutex> held (client->lock);
                    if (keepReading)
                        client->woken.wait_for (held, std::chrono::milliseconds (1),
                                                [&] { return ! client->outgoing.empty() || client->closed; });
                    toWrite.swap (client->outgoing);
                    closed = client->closed;
                }

                if (! toWrite.empty() && socket->write (toWrite.data(), static_cast<int> (toWrite.size())) != static_cast<int> (toWrite.size()))
                    break;

                if (! keepReading || closed)
                    break;
            }

            socket->close();
            hub->remove (client, sayingGoodbye);
        }

        /*  THE LINK WITH NO SOCKET (AND): the serial table's worker writes a
            CONNECT and the rest as a socket client would, each packet handled
            on the worker's own thread; what the hub queues is what the worker
            reads. Closed by the hub - the broker stopping, a refusal - it
            reads nothing and the worker opens it again, which asks the broker
            for a fresh one. */
        class LocalLink final : public serial::Link
        {
        public:
            LocalLink (std::shared_ptr<Broker::Hub> h, std::shared_ptr<Broker::Client> c)
                : hub (std::move (h)), client (std::move (c)) {}

            ~LocalLink() override
            {
                hub->remove (client, false);
            }

            std::optional<std::string> read (std::chrono::milliseconds wait) override
            {
                std::unique_lock<std::mutex> held (client->lock);
                client->woken.wait_for (held, wait, [this] { return ! client->outgoing.empty() || client->closed; });

                if (! client->outgoing.empty())
                {
                    std::string out;
                    out.swap (client->outgoing);
                    return out;
                }

                if (client->closed)
                {
                    why = "the broker closed the link";
                    return std::nullopt;
                }

                return std::string {};
            }

            bool write (const std::string& bytes) override
            {
                if (client->isClosed())
                    return false;

                if (! reader.feed (bytes, *hub, client))
                {
                    client->close();
                    why = "the broker refused what was sent";
                    return false;
                }

                return true;
            }

            std::string problem() const override { return why; }

        private:
            std::shared_ptr<Broker::Hub> hub;
            std::shared_ptr<Broker::Client> client;
            Reader reader;
            std::string why;
        };
    }

    //==========================================================================
    Broker::Broker() : hub (std::make_shared<Hub>()) {}

    Broker::~Broker()
    {
        stop();
    }

    bool Broker::start (int port, const std::string& login)
    {
        stop();

        {
            const std::lock_guard<std::mutex> held (hub->lock);
            hub->login = login;
        }

        loginWanted = login;
        portWanted = port;
        stopping = false;

        auto listener = std::make_unique<juce::StreamingSocket>();

        if (! listener->createListener (port))
        {
            const std::lock_guard<std::mutex> held (stateLock);
            state.state = "problem";
            state.problem = "port " + std::to_string (port) + " cannot be bound - taken by another program, most likely";
            state.port = 0;
            return false;
        }

        {
            const std::lock_guard<std::mutex> held (stateLock);
            state.state = "listening";
            state.problem.clear();
            state.port = listener->getBoundPort();
        }

        isListening.store (true, std::memory_order_release);
        acceptor = std::thread ([this, socket = std::move (listener)]() mutable { acceptOn (std::move (socket)); });
        return true;
    }

    void Broker::acceptOn (std::unique_ptr<juce::StreamingSocket> listener)
    {
        std::vector<std::thread> serving;
        auto lastSweep = Clock::now();

        while (! stopping)
        {
            const auto ready = listener->waitUntilReady (true, acceptWaitMs);

            if (ready > 0)
            {
                if (auto* raw = listener->waitForNextConnection())
                {
                    std::unique_ptr<juce::StreamingSocket> socket (raw);
                    auto client = hub->add (false);
                    serving.emplace_back ([owned = std::move (socket), hubHeld = hub, client, this]() mutable
                                          {
                                              serveSocket (std::move (owned), hubHeld, client, stopping);
                                          });
                }
            }
            else if (ready < 0)
            {
                break;
            }

            /*  Once a second: the silent dropped, the threads of the gone
                joined. */
            if (Clock::now() - lastSweep > std::chrono::seconds (1))
            {
                lastSweep = Clock::now();
                for (const auto& client : hub->silent())
                    hub->remove (client, false);
            }

            {
                const std::lock_guard<std::mutex> held (stateLock);
                state.clients = hub->connectedCount();
            }
        }

        listener->close();

        for (auto& thread : serving)
            if (thread.joinable())
                thread.join();
    }

    void Broker::stop()
    {
        stopping = true;

        if (acceptor.joinable())
            acceptor.join();

        /*  Every client let go, the local ones told: their links read
            nothing and open again, which asks a broker that is off and is
            told so. */
        std::vector<std::shared_ptr<Client>> all;
        {
            const std::lock_guard<std::mutex> held (hub->lock);
            all = hub->clients;
        }
        for (const auto& client : all)
            hub->remove (client, true);

        isListening.store (false, std::memory_order_release);
        const std::lock_guard<std::mutex> held (stateLock);
        state = BrokerState {};
    }

    BrokerState Broker::stateNow() const
    {
        const std::lock_guard<std::mutex> held (stateLock);
        auto now = state;
        if (listening())
            now.clients = hub->connectedCount();
        return now;
    }

    std::unique_ptr<serial::Link> Broker::localLink (std::string& problem)
    {
        if (! listening())
        {
            problem = "Go.dot's broker is off";
            return nullptr;
        }

        return std::make_unique<LocalLink> (hub, hub->add (true));
    }

    std::size_t Broker::subscribers() const
    {
        const std::lock_guard<std::mutex> held (hub->lock);
        return static_cast<std::size_t> (std::count_if (hub->clients.begin(), hub->clients.end(),
                                                         [] (const std::shared_ptr<Client>& c) { return c->connected && ! c->filters.empty(); }));
    }
}
