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

#include <wfg/engine/oscquery/OscQueryServer.h>
#include <wfg/engine/oscquery/Subscriptions.h>

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscAddress.h>

#include <juce_simpleweb/juce_simpleweb.h>

#include <algorithm>
#include <string>
#include <vector>

namespace wfg::oscquery
{
    namespace
    {
        /*  THE QUERY COMES FROM `request->query_string`, NOT FROM THE PATH.

            Simple-Web-Server splits them when it parses the request line
            (common/utility.hpp:254-275): `path` is everything up to the `?` and
            `query_string` is everything after it. An earlier version of this
            file searched `path` for a `?`, found none, and therefore treated
            every attribute query as a plain GET - `?VALUE` on a container came
            back as 200 with the whole subtree instead of 204, and `?HOST_INFO`
            returned the tree instead of the host block. Both looked like
            working replies, which is what made it worth a comment.

            Nothing here re-parses or re-encodes either field. juce::URL would
            turn OSCQuery's bare `?HOST_INFO` into `?HOST_INFO=`, which no
            server matching the spec will recognise; WFS-DIY hit exactly that
            and abandoned juce::URL in its own OSCQuery client. */

        /*  An OSCQuery attribute query is a BARE key: `?VALUE`, not `?VALUE=`.
            Anything carrying a `=` or a `&` is a form submission, which this
            server has no notion of, and is refused rather than half-read. */
        bool isBareKey (const std::string& query)
        {
            return ! query.empty()
                     && query.find ('=') == std::string::npos
                     && query.find ('&') == std::string::npos;
        }

        constexpr const char* jsonMime = "application/json";
        constexpr const char* textMime = "text/plain";
    }

    //==========================================================================
    struct OscQueryServer::Impl final : public SimpleWebSocketServer::Listener,
                                        public SimpleWebSocketServer::RequestHandler
    {
        SimpleWebSocketServer server;
        Subscriptions subscriptions;
        Namespace* target = nullptr;
        OscQueryServer* owner = nullptr;

        Impl() { server.addWebSocketListener (this); }

        ~Impl() override
        {
            server.removeWebSocketListener (this);
            server.removeHTTPRequestHandler (this);
        }

        //======================================================================
        //  HTTP.
        bool handleHTTPRequest (std::shared_ptr<HttpServer::Response> response,
                                std::shared_ptr<HttpServer::Request> request) override
        {
            if (target == nullptr)
                return false;

            if (request->method != "GET")
            {
                /*  405 and not 404. The resource is there; the verb is not one
                    this server has. A client told 404 would conclude the node
                    had gone away and stop asking. */
                response->write (SimpleWeb::StatusCode::client_error_method_not_allowed,
                                 "Go.dot serves GET only\n",
                                 { { "Content-Type", textMime } });
                return true;
            }

            const auto& path = request->path;
            const auto& query = request->query_string;

            //  ?HOST_INFO is a question about the SERVER, and takes no path.
            if (query == "HOST_INFO")
            {
                tree::OscQueryJson::HostInfo info;
                info.oscPort = target->oscPort();
                info.wsPort = owner != nullptr ? owner->boundPort() : 0;

                response->write (SimpleWeb::StatusCode::success_ok,
                                 tree::OscQueryJson::hostInfo (info),
                                 { { "Content-Type", jsonMime } });
                return true;
            }

            const auto snapshot = target->snapshot();

            if (snapshot == nullptr)
            {
                response->write (SimpleWeb::StatusCode::server_error_service_unavailable,
                                 "no snapshot has been published yet\n",
                                 { { "Content-Type", textMime } });
                return true;
            }

            /*  A pattern is refused before anything looks it up, and with a
                reason that says which thing was wrong. Phase 1 resolves an
                address to exactly one node; a client sending `/godot/cue/*` has
                asked for something Go.dot does not do, rather than named a node
                that is not there, and 404 would send it looking for a typo. */
            if (osc::containsWildcard (path))
            {
                response->write (SimpleWeb::StatusCode::client_error_bad_request,
                                 "Go.dot does not dispatch address patterns\n",
                                 { { "Content-Type", textMime } });
                return true;
            }

            if (! query.empty())
                return attributeQuery (response, snapshot, path, query);

            const auto json = tree::OscQueryJson::describe (*snapshot, path);

            if (json.empty())
            {
                response->write (SimpleWeb::StatusCode::client_error_not_found,
                                 "no such node: " + path + "\n",
                                 { { "Content-Type", textMime } });
                return true;
            }

            response->write (SimpleWeb::StatusCode::success_ok, json,
                             { { "Content-Type", jsonMime } });
            return true;
        }

        bool attributeQuery (std::shared_ptr<HttpServer::Response> response,
                             const std::shared_ptr<const tree::TreeSnapshot>& snapshot,
                             const std::string& path,
                             const std::string& query)
        {
            if (! isBareKey (query))
            {
                response->write (SimpleWeb::StatusCode::client_error_bad_request,
                                 "an OSCQuery attribute query is a bare key, "
                                 "such as ?VALUE\n",
                                 { { "Content-Type", textMime } });
                return true;
            }

            const auto attribute = tree::OscQueryJson::attribute (*snapshot, path, query);

            switch (attribute.result)
            {
                case tree::OscQueryJson::AttributeResult::found:
                    response->write (SimpleWeb::StatusCode::success_ok, attribute.json,
                                     { { "Content-Type", jsonMime } });
                    return true;

                case tree::OscQueryJson::AttributeResult::noSuchNode:
                    response->write (SimpleWeb::StatusCode::client_error_not_found,
                                     "no such node: " + path + "\n",
                                     { { "Content-Type", textMime } });
                    return true;

                case tree::OscQueryJson::AttributeResult::noSuchAttribute:
                    response->write (SimpleWeb::StatusCode::client_error_bad_request,
                                     "not an OSCQuery attribute: " + query + "\n",
                                     { { "Content-Type", textMime } });
                    return true;

                case tree::OscQueryJson::AttributeResult::notPresent:
                    /*  204 No Content, and the distinction is the point. The
                        node is real and the attribute is real; this node just
                        does not carry it. 404 would say the node is gone and a
                        JSON `null` would say the value IS null. */
                    response->write (SimpleWeb::StatusCode::success_no_content, "");
                    return true;
            }

            return false;
        }

        //======================================================================
        //  WebSocket.
        void connectionClosed (const juce::String& id, int, const juce::String&) override
        {
            const ConnectionId connection = originOf (id);

            subscriptions.drop (connection);

            /*  And the touches. A surface that crashed mid-gesture would
                otherwise leave a node gated against everybody for the rest of
                the show (PRD 3.16). */
            if (target != nullptr)
                target->forget (connection);
        }

        void connectionError (const juce::String& id, int, const juce::String&) override
        {
            connectionClosed (id, 0, {});
        }

        /*  Text frames are OSCQuery's control protocol: LISTEN and IGNORE. */
        void messageReceived (const juce::String& id, const juce::String& message) override
        {
            const ConnectionId connection = originOf (id);
            const auto parsed = json::parse (message.toStdString());

            if (! parsed.value.has_value() || ! parsed.value->isObject())
                return;

            const auto* command = parsed.value->find ("COMMAND");
            const auto* data = parsed.value->find ("DATA");

            if (command == nullptr || ! command->isString()
                || data == nullptr || ! data->isString())
                return;

            const auto& address = data->asString();

            /*  A pattern is not a subscription. The decoder refuses patterns
                everywhere else, and accepting one here would make this the only
                place in the engine where a wildcard meant something. */
            if (! osc::isValidAddress (address))
                return;

            if (command->asString() == "LISTEN")
                subscriptions.listen (connection, address);
            else if (command->asString() == "IGNORE")
                subscriptions.ignore (connection, address);
        }

        /*  Binary frames are OSC: a node write or a command. */
        void dataReceived (const juce::String& id, const juce::MemoryBlock& data) override
        {
            if (target == nullptr)
                return;

            if (owner != nullptr)
                owner->inbound.fetch_add (1, std::memory_order_relaxed);

            const auto decoded = osc::decode (static_cast<const std::uint8_t*> (data.getData()),
                                              data.getSize());

            /*  A malformed frame is dropped here and NOT forwarded. It never
                became a command, so there is nothing to reject; the engine's
                `X` record is written by whoever owns the log, from the reason
                and the bytes. Guessing at it would be inventing a message the
                client never sent, which is the one thing the codec exists to
                refuse. */
            if (! decoded.ok)
                return;

            target->write (originOf (id), decoded.packet);
        }

        //======================================================================
        /*  juce_simpleweb's connection id is already `<ip>:<port>`
            (SimpleWebSocketServer.cpp:275), which is exactly the shape the
            event log's origin wants. Prefixed rather than rebuilt so the two
            can never drift. */
        static ConnectionId originOf (const juce::String& id)
        {
            return "ws:" + id.toStdString();
        }
    };

    //==========================================================================
    OscQueryServer::OscQueryServer() : impl (std::make_unique<Impl>())
    {
        impl->owner = this;
    }

    OscQueryServer::~OscQueryServer()
    {
        stop();
    }

    //==========================================================================
    bool OscQueryServer::start (int portToBind, Namespace& nameSpace)
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        if (portToBind < 0)
            return false;

        impl->target = &nameSpace;
        impl->server.addHTTPRequestHandler (impl.get());
        impl->server.start (portToBind);

        /*  isConnected is set from the server's own thread once asio has bound.
            Waited on rather than assumed: a caller that started making requests
            immediately would race the bind and see connection refused. */
        for (int i = 0; i < 2000 && ! impl->server.isConnected; ++i)
            juce::Thread::sleep (5);

        if (! impl->server.isConnected)
        {
            impl->server.stop();
            impl->server.removeHTTPRequestHandler (impl.get());
            impl->target = nullptr;
            return false;
        }

        /*  The port the server actually got, which is only the same as the one
            asked for when a non-zero one was asked for. Read from the module
            rather than echoed back from the argument: that echo is exactly the
            bug the fork fix removed, and reproducing it here would put it back
            one layer up. */
        port.store (impl->server.port, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);
        return true;
    }

    void OscQueryServer::stop()
    {
        if (! running.exchange (false, std::memory_order_relaxed))
            return;

        impl->server.removeHTTPRequestHandler (impl.get());
        impl->server.stop();
        impl->target = nullptr;
        port.store (0, std::memory_order_relaxed);
    }

    //==========================================================================
    void OscQueryServer::publishChanges (const tree::TreeDiff& diff,
                                         const tree::TreeSnapshot& current,
                                         const std::string& causedBy)
    {
        if (! running.load (std::memory_order_relaxed) || impl->target == nullptr)
            return;

        /*  Structure first, then values. A client told that a node's value
            changed before it has been told the node exists has to guess; told
            in this order it never does. */
        for (const auto& address : diff.added)
            broadcastText ("{\"COMMAND\": \"PATH_ADDED\", \"DATA\": \"" + address + "\"}");

        for (const auto& address : diff.removed)
            broadcastText ("{\"COMMAND\": \"PATH_REMOVED\", \"DATA\": \"" + address + "\"}");

        for (const auto& address : diff.valueChanged)
        {
            const auto* node = current.find (address);

            if (node == nullptr || ! node->value.has_value())
                continue;

            /*  ONE message per node per tick, carrying the value it HAS now -
                not the succession it passed through during the tick. That is
                the whole of the coalescing, and it falls out of reading the
                published snapshot rather than the stream of writes. */
            const auto packet = osc::Packet::message (address, { *node->value });

            std::string error;
            const auto encoded = osc::encode (packet, error);

            if (! encoded.has_value())
                continue;       // a value the wire cannot carry; nothing to send

            for (const auto& connection : impl->subscriptions.listenersOf (address))
            {
                if (! impl->target->shouldPush (connection, address, causedBy))
                    continue;

                impl->server.sendTo (juce::MemoryBlock (encoded->data(), encoded->size()),
                                     juce::String (connection.substr (3)));   // drop "ws:"
                outbound.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    void OscQueryServer::broadcastText (const std::string& message)
    {
        impl->server.send (juce::String (message));
        outbound.fetch_add (1, std::memory_order_relaxed);
    }

    //==========================================================================
    std::size_t OscQueryServer::connectionCount() const
    {
        return impl->subscriptions.connectionCount();
    }
}
