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

#include <wfg/engine/tree/MountListener.h>

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/oscquery/OscQueryClient.h>
#include <wfg/engine/tree/HeardBox.h>

#include <juce_simpleweb/juce_simpleweb.h>

#include <string_view>
#include <utility>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;
        using Clock = std::chrono::steady_clock;

        /*  How long a device has to answer HOST_INFO, and to open its socket:
            a device that is there answers in a few milliseconds, and one that
            is not costs this much of the thread's time per try. */
        constexpr int askTimeoutMs = 1000;
        constexpr auto openTimeout = std::chrono::seconds { 3 };

        /*  A ping on a quiet connection, so a device unplugged without a word
            is found out: WFS-DIY pushes only what changes, and a socket that
            hears nothing for minutes is the ordinary case. */
        constexpr auto pingEvery = std::chrono::seconds { 5 };

        /*  The text of a LISTEN or an IGNORE. An address is a valid OSC address,
            which may still hold a quote or a backslash; those are escaped. */
        std::string command (std::string_view verb, const std::string& address)
        {
            std::string out = "{\"COMMAND\":\"";
            out += verb;
            out += "\",\"DATA\":\"";

            for (const auto c : address)
            {
                if (c == '"' || c == '\\')
                    out += '\\';

                out += c;
            }

            out += "\"}";
            return out;
        }

        void heardFrom (HeardBox& box, const std::string& mountId, const osc::Packet& packet)
        {
            if (packet.isBundle())
            {
                for (const auto& element : packet.elements)
                    heardFrom (box, mountId, element);

                return;
            }

            box.takeFrom (mountId, packet.address, packet.args);
        }
    }

    //==============================================================================
    struct MountListener::Session
    {
        std::string host;
        int queryPort = 0;

        std::unique_ptr<WsClient> client;
        std::shared_ptr<SimpleWeb::io_context> io;
        std::thread reader;

        std::mutex lock;
        std::shared_ptr<WsClient::Connection> connection;
        std::atomic<bool> open { false };
        std::atomic<bool> ended { false };

        /** The addresses this connection has been told to LISTEN to. */
        std::set<std::string> told;

        int failures = 0;
        Clock::time_point notBefore {};
        Clock::time_point openedAt {};
        Clock::time_point pingedAt {};

        bool active() const noexcept { return client != nullptr; }

        void send (const std::string& text, unsigned char opcode = 129)
        {
            std::shared_ptr<WsClient::Connection> to;

            {
                const std::lock_guard<std::mutex> held { lock };
                to = connection;
            }

            if (to == nullptr)
                return;

            to->send (text, [this] (const SimpleWeb::error_code& error)
                      {
                          if (error)
                              ended.store (true);
                      },
                      opcode);
        }

        void shut()
        {
            if (client == nullptr)
                return;

            client->stop();

            if (io != nullptr)
                io->stop();

            if (reader.joinable())
                reader.join();

            {
                const std::lock_guard<std::mutex> held { lock };
                connection.reset();
            }

            client.reset();
            io.reset();
            open.store (false);
            ended.store (false);
            told.clear();
        }

        ~Session() { shut(); }
    };

    //==============================================================================
    MountListener::MountListener (HeardBox& boxToFill) : box (boxToFill) {}

    MountListener::~MountListener() { stop(); }

    bool MountListener::start()
    {
        if (running.exchange (true))
            return false;

        {
            const std::lock_guard<std::mutex> held { guard };
            stopping = false;
        }

        thread = std::thread ([this] { run(); });
        return true;
    }

    void MountListener::stop()
    {
        if (! running.load())
            return;

        {
            //  Under the lock, or a wake between the thread's test and its wait is lost.
            const std::lock_guard<std::mutex> held { guard };
            stopping = true;
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false);
    }

    void MountListener::want (Wanted next)
    {
        {
            const std::lock_guard<std::mutex> held { guard };

            if (next == wanted)
                return;

            wanted = std::move (next);
            changed = true;
        }

        wake.notify_all();
    }

    std::string MountListener::statusOf (const std::string& mountId) const
    {
        const std::lock_guard<std::mutex> held { guard };
        const auto found = statuses.find (mountId);
        return found != statuses.end() ? found->second : std::string (listenStatus::off);
    }

    void MountListener::setRetryDelays (std::chrono::milliseconds first, std::chrono::milliseconds second,
                                        std::chrono::milliseconds later) noexcept
    {
        retryFirst = first;
        retrySecond = second;
        retryLater = later;
    }

    void MountListener::setStatus (const std::string& mountId, const char* word)
    {
        const std::lock_guard<std::mutex> held { guard };

        if (std::string_view (word) == listenStatus::off)
            statuses.erase (mountId);
        else
            statuses[mountId] = word;
    }

    //==============================================================================
    void MountListener::run()
    {
        for (;;)
        {
            Wanted now;

            {
                std::unique_lock<std::mutex> held { guard };
                wake.wait_for (held, std::chrono::milliseconds { 100 }, [this] { return stopping || changed; });

                if (stopping)
                    break;

                changed = false;
                now = wanted;
            }

            //  A device no longer wanted is let go of: the server drops what it was told with the socket.
            for (auto at = sessions.begin(); at != sessions.end();)
            {
                if (now.count (at->first) != 0)
                {
                    ++at;
                    continue;
                }

                at->second->shut();
                setStatus (at->first, listenStatus::off);
                at = sessions.erase (at);
            }

            for (const auto& [mountId, device] : now)
            {
                auto& session = sessions[mountId];

                if (session == nullptr)
                    session = std::make_unique<Session>();

                tend (mountId, device, *session);
            }
        }

        for (auto& [mountId, session] : sessions)
        {
            session->shut();
            setStatus (mountId, listenStatus::off);
        }

        sessions.clear();
    }

    void MountListener::tend (const std::string& mountId, const Device& device, Session& session)
    {
        const auto now = Clock::now();

        const auto failed = [&] (const char* word, bool later)
        {
            session.shut();
            ++session.failures;
            session.notBefore = now + (later || session.failures > 2 ? retryLater
                                       : session.failures == 2   ? retrySecond
                                                                 : retryFirst);
            setStatus (mountId, word);
        };

        //  Moved to another host or port: what it was told is about another box.
        if (session.active() && (session.host != device.host || session.queryPort != device.queryPort))
            session.shut();

        //  Gone under it: the socket closed or a send failed.
        if (session.active() && session.ended.load())
        {
            failed (listenStatus::connecting, false);
            return;
        }

        if (! session.active())
        {
            if (now < session.notBefore || device.addresses.empty())
                return;

            setStatus (mountId, listenStatus::connecting);

            /*  WHETHER IT LISTENS, AND WHERE: HOST_INFO's EXTENSIONS and its
                WS_PORT - WFS-DIY's is its HTTP port, which is also the default. */
            const auto reply = oscquery::OscQueryClient::get (device.host, device.queryPort, "/", "HOST_INFO",
                                                              askTimeoutMs);

            if (! reply.ok || reply.status != 200)
            {
                failed (listenStatus::unreachable, false);
                return;
            }

            const auto parsed = json::parse (reply.body);
            const auto* extensions = parsed.value.has_value() ? parsed.value->find ("EXTENSIONS") : nullptr;
            const auto* listens = extensions != nullptr ? extensions->find ("LISTEN") : nullptr;

            if (listens == nullptr || ! listens->isBool() || ! listens->asBool())
            {
                failed (listenStatus::unsupported, true);
                return;
            }

            auto wsPort = device.queryPort;

            if (const auto* port = parsed.value->find ("WS_PORT"); port != nullptr && port->isNumber()
                                                                     && port->asInt() > 0 && port->asInt() < 65536)
                wsPort = port->asInt();

            session.host = device.host;
            session.queryPort = device.queryPort;
            session.io = std::make_shared<SimpleWeb::io_context>();
            session.client = std::make_unique<WsClient> (device.host + ":" + std::to_string (wsPort) + "/");
            session.client->io_service = session.io;
            session.client->config.timeout_request = 2;

            auto* const self = &session;

            session.client->on_open = [self] (std::shared_ptr<WsClient::Connection> connection)
            {
                {
                    const std::lock_guard<std::mutex> held { self->lock };
                    self->connection = std::move (connection);
                }

                self->open.store (true);
            };

            session.client->on_message = [this, mountId] (std::shared_ptr<WsClient::Connection>,
                                                          std::shared_ptr<WsClient::InMessage> message)
            {
                /*  A BINARY FRAME IS OSC, a message or a bundle of them; a text
                    frame is the server's news about its namespace, which a
                    recording has no use for. */
                if ((message->fin_rsv_opcode & 0x0f) != 2)
                    return;

                const auto bytes = message->string();
                const auto decoded = osc::decode (bytes);

                if (decoded.ok)
                    heardFrom (box, mountId, decoded.packet);
            };

            session.client->on_close = [self] (std::shared_ptr<WsClient::Connection>, int, const std::string&)
            {
                self->ended.store (true);
            };

            session.client->on_error = [self] (std::shared_ptr<WsClient::Connection>, const SimpleWeb::error_code&)
            {
                self->ended.store (true);
            };

            session.open.store (false);
            session.ended.store (false);
            session.openedAt = now;
            session.pingedAt = now;
            session.told.clear();

            //  `start` only begins the connection on an io context of ours; the reader runs it.
            session.client->start();
            session.reader = std::thread ([io = session.io] { io->run(); });
            return;
        }

        if (! session.open.load())
        {
            if (now - session.openedAt > openTimeout)
                failed (listenStatus::unreachable, false);

            return;
        }

        //  OPEN: the difference between what it was told and what is wanted.
        for (const auto& address : device.addresses)
            if (session.told.insert (address).second)
                session.send (command ("LISTEN", address));

        for (auto at = session.told.begin(); at != session.told.end();)
        {
            if (device.addresses.count (*at) != 0)
            {
                ++at;
                continue;
            }

            session.send (command ("IGNORE", *at));
            at = session.told.erase (at);
        }

        if (now - session.pingedAt > pingEvery)
        {
            session.pingedAt = now;
            session.send ({}, 137);
        }

        session.failures = 0;
        setStatus (mountId, listenStatus::listening);
    }
}
