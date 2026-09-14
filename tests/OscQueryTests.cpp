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

/*
    OSCQuery: what a client actually gets.

    DRIVEN THROUGH THE SEAM, with a hand-built namespace of six nodes rather
    than a whole engine. That is what `Namespace` is for: these tests can state
    exactly what the tree contains, so an assertion about a 204 is about the
    server's status codes and not about whether some fixture happened to leave a
    description empty. Standing up a ShowDocument to test an HTTP status is how
    a test comes to fail for a reason it was never about.

    EVERY SERVER BINDS PORT 0. ctest runs in parallel and a fixed port makes a
    suite that cannot run twice at once. That this works at all is recent - see
    SimpleWebToolchainTests.cpp - and one case below asserts it directly, so a
    submodule re-pin that lost the fix fails here too.

    THE STATUS CODES ARE THE POINT of the HTTP half. OSCQuery gives four
    different answers to four different questions, and a server that collapses
    them into 200-or-404 makes a client guess: 204 means the node is real and
    carries no such attribute, 400 means the attribute is not one the protocol
    has, 404 means nothing lives there. Each is asserted separately.

    AND THE ROUTES BESIDE THE TREE (PR 5.8). A route is a GET the server hands
    to a function before the tree is consulted. The fake namespace is enough to
    pin where one is matched and that its bytes go out untouched; the timbre
    route is then asked directly - it is a pure function over a `MediaInfo` -
    and once through a running server, so each half is tested where it can
    fail and the join between them once.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/oscquery/OscQueryServer.h>
#include <wfg/engine/oscquery/Subscriptions.h>
#include <wfg/engine/oscquery/TimbreRoute.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <juce_core/juce_core.h>
#include <juce_simpleweb/juce_simpleweb.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wfg;
using namespace wfg::oscquery;

namespace
{
    template <typename Predicate>
    bool waitUntil (Predicate predicate,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds { 10000 })
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds { 5 });
        }

        return predicate();
    }

    //==========================================================================
    /*  Six nodes, chosen so that every status code has a node that produces it.

        A container (no VALUE, no TYPE, no RANGE) for the 204s; a ranged number
        for the RANGE case; a string with a description; and an event, which is
        a node with an ACCESS and almost nothing else. */
    std::shared_ptr<const tree::TreeSnapshot> buildTree()
    {
        auto nodes = std::make_shared<std::vector<tree::Node>>();

        auto container = [] (std::string address, std::string description)
        {
            tree::Node n;
            n.address = std::move (address);
            n.description = std::move (description);
            n.kind = tree::Kind::container;
            n.access = tree::Access::none;
            return n;
        };

        nodes->push_back (container ("/", ""));
        nodes->push_back (container ("/godot", "Go.dot"));
        nodes->push_back (container ("/godot/engine", "Runtime, read-only"));
        nodes->push_back (container ("/godot/cmd", "Commands"));
        nodes->push_back (container ("/godot/cmd/standby", "The standby pointer"));

        {
            tree::Node n;
            n.address = "/godot/engine/tick";
            n.description = "The current tick index";
            n.kind = tree::Kind::state;
            n.access = tree::Access::read;
            n.typeTags = "h";
            n.values = { osc::Value::int64 (12) };
            nodes->push_back (n);
        }

        {
            tree::Node n;
            n.address = "/godot/engine/level";
            n.kind = tree::Kind::state;
            n.access = tree::Access::readWrite;
            n.typeTags = "f";
            n.values = { osc::Value::float32 (0.25f) };
            n.hasMinimum = true;
            n.minimum = 0.0;
            n.hasMaximum = true;
            n.maximum = 1.0;
            n.unit = "linear";
            nodes->push_back (n);
        }

        {
            tree::Node n;
            n.address = "/godot/cmd/standby/next";
            n.description = "Move standby to the next cue";
            n.kind = tree::Kind::event;
            n.access = tree::Access::write;
            nodes->push_back (n);
        }

        /*  SORTED BY ADDRESS, because TreeSnapshot::find is a lower_bound and
            says the ordering is a precondition (TreeSnapshot.cpp:26-28). In the
            engine ParameterTree guarantees it; this fixture is the one other
            thing that builds a snapshot, so it has to honour it here.

            Getting it wrong does not fail loudly - it makes SOME lookups work
            and others return null, depending on where the binary search
            happens to land. Three tests in this file failed that way before the
            sort was added, all of them looking like missing nodes. */
        std::sort (nodes->begin(), nodes->end(),
                   [] (const tree::Node& a, const tree::Node& b)
                   {
                       return a.address < b.address;
                   });

        /*  No mounted half: these cases are about /godot and what a client
            reads off it. An empty one is a complete one. */
        return std::make_shared<const tree::TreeSnapshot> (
            7, nodes, std::make_shared<const std::vector<tree::Node>>(),
            std::vector<tree::Node> {});
    }

    //==========================================================================
    /** The seam, implemented for the tests: it records rather than does. */
    struct FakeNamespace final : public Namespace
    {
        std::shared_ptr<const tree::TreeSnapshot> tree = buildTree();

        std::mutex mutex;
        std::vector<std::pair<std::string, osc::Packet>> writes;
        std::vector<std::string> forgotten;

        /*  What shouldPush() will refuse, so the tests can drive touch gating
            and echo suppression without a TouchTable. */
        std::string touchedBy;
        std::string touchedAddress;

        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override { return tree; }

        void write (const std::string& origin, const osc::Packet& packet) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            writes.emplace_back (origin, packet);
        }

        void forget (const std::string& origin) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            forgotten.push_back (origin);
        }

        bool shouldPush (const std::string& toOrigin,
                         const std::string& address,
                         const std::string& causedBy) const override
        {
            if (toOrigin == causedBy)
                return false;                       // it already knows

            return ! (toOrigin == touchedBy && address == touchedAddress);
        }

        int oscPort() const override { return 8010; }

        std::size_t writeCount()
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return writes.size();
        }
    };

    //==========================================================================
    /*  Field names are case-insensitive (RFC 9110 §5.1), so both the keys of
        the map below and every lookup into it go through here. A test that
        asked for "content-type" and a server that wrote "Content-Type" would
        otherwise disagree about a header they agree about. */
    std::string lowerCased (std::string text)
    {
        std::transform (text.begin(), text.end(), text.begin(),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
        return text;
    }

    /** One HTTP GET, returning the status, the headers and the body. */
    struct HttpReply
    {
        int status = 0;
        std::string body;

        /*  THE HEADERS ARE KEPT, and they were not always. This helper used to
            read the status line and throw the rest of the block away, which was
            enough while every assertion in this file was about a status code -
            but `Content-Type` is the half of a reply that a change to the MIME
            table breaks, and it is entirely invisible to a test that reads only
            the body. `Cache-Control` is the second: the timbre route asserts on
            it both ways - `no-cache` on a pyramid, `no-store` on a refusal -
            and a pyramid marked to be kept for good would show the old colours
            after the analysis changed, as a refusal that lost its `no-store`
            would keep a bar grey long after the analyser had coloured it.

            A MULTIMAP, so a field sent twice is seen twice. A route's handler
            may name a field the server writes itself, and the only way to
            prove the server dropped the handler's copy is to count. */
        std::multimap<std::string, std::string> headers;

        /** The value of one header, empty if the reply carries no such field. */
        std::string header (const std::string& name) const
        {
            const auto key = lowerCased (name);
            const auto found = headers.lower_bound (key);

            return found != headers.end() && found->first == key ? found->second : std::string();
        }

        /** How many times the reply carries a field. */
        std::size_t headerCount (const std::string& name) const
        {
            return headers.count (lowerCased (name));
        }
    };

    /*  One GET, over a raw socket, reading the status line off the wire.

        NOT juce::URL, and the reason is the thing being tested. This file
        asserts that a 204 is a 204 and a 400 is a 400, and juce::URL was
        observed reporting 200 for the server's 204 - so a test written through
        it would have been measuring juce's normalisation rather than Go.dot's
        status codes, and would have passed just as happily if the server had
        answered 200 all along.

        juce::URL is wrong for this in a second way too, already documented in
        the server: it re-encodes OSCQuery's bare `?HOST_INFO` as `?HOST_INFO=`,
        which the server then does not match. WFS-DIY hit exactly that and
        abandoned juce::URL in its own client.

        So: write the request, read the response, parse the first line. It is
        fifteen lines and it tells the truth.

        ANY METHOD, because a route has to be shown to answer a POST with the
        same 405 as the tree does; `get` below is what nearly every case calls.
        A request with no body says so, rather than leaving the server to
        decide what a missing length means. */
    HttpReply sendRequest (int port, const std::string& method, const std::string& target)
    {
        HttpReply reply;

        juce::StreamingSocket socket;

        if (! socket.connect ("127.0.0.1", port, 10000))
            return reply;

        const std::string request = method + " " + target + " HTTP/1.1\r\n"
                                    + "Host: 127.0.0.1\r\n"
                                    + (method == "GET" ? "" : "Content-Length: 0\r\n")
                                    + "Connection: close\r\n"
                                    + "\r\n";

        if (socket.write (request.data(), static_cast<int> (request.size()))
              != static_cast<int> (request.size()))
            return reply;

        std::string response;
        char buffer[4096];

        for (;;)
        {
            const auto ready = socket.waitUntilReady (true, 10000);

            if (ready <= 0)
                break;

            const auto read = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);

            if (read <= 0)
                break;

            response.append (buffer, static_cast<std::size_t> (read));
        }

        socket.close();

        //  "HTTP/1.1 204 No Content" -> 204
        const auto firstSpace = response.find (' ');

        if (firstSpace == std::string::npos)
            return reply;

        reply.status = std::atoi (response.c_str() + firstSpace + 1);

        const auto bodyStart = response.find ("\r\n\r\n");

        if (bodyStart != std::string::npos)
            reply.body = response.substr (bodyStart + 4);

        /*  The header block is every line between the status line and the blank
            line that ends it. A field is a name, a colon, and a value whose
            leading whitespace is not part of it (RFC 9110 §5.5); a line without
            a colon is not a field and is skipped rather than guessed at.

            `substr` and `emplace` throughout, so a value containing a colon -
            which `Date` does, three times - keeps all of it. */
        const auto headerEnd = bodyStart == std::string::npos ? response.size() : bodyStart;

        auto lineStart = response.find ("\r\n");

        if (lineStart != std::string::npos)
            lineStart += 2;

        while (lineStart != std::string::npos && lineStart < headerEnd)
        {
            auto lineEnd = response.find ("\r\n", lineStart);

            if (lineEnd == std::string::npos || lineEnd > headerEnd)
                lineEnd = headerEnd;

            const auto colon = response.find (':', lineStart);

            if (colon != std::string::npos && colon < lineEnd)
            {
                auto value = response.substr (colon + 1, lineEnd - colon - 1);

                while (! value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase (value.begin());

                reply.headers.emplace (lowerCased (response.substr (lineStart, colon - lineStart)),
                                       std::move (value));
            }

            lineStart = lineEnd + 2;
        }

        return reply;
    }

    HttpReply get (int port, const std::string& target)
    {
        return sendRequest (port, "GET", target);
    }

    //==========================================================================
    struct Client final : public SimpleWebSocketClient::Listener
    {
        SimpleWebSocketClient socket;

        std::mutex mutex;
        std::vector<juce::String> text;
        std::vector<std::vector<std::uint8_t>> binary;
        std::atomic<bool> open { false };

        Client() { socket.addWebSocketListener (this); }
        ~Client() override { socket.removeWebSocketListener (this); socket.stop(); }

        void connectionOpened() override { open = true; }

        void messageReceived (const juce::String& message) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            text.push_back (message);
        }

        void dataReceived (const juce::MemoryBlock& data) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
            binary.emplace_back (bytes, bytes + data.getSize());
        }

        std::size_t binaryCount()
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return binary.size();
        }

        std::size_t textCount()
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return text.size();
        }

        void connect (int port)
        {
            socket.start ("127.0.0.1:" + juce::String (port));
        }
    };

    /** A started server plus its namespace, torn down in the right order. */
    struct Rig
    {
        FakeNamespace nameSpace;
        OscQueryServer server;

        Rig() { started = server.start (0, nameSpace); }

        /*  With something set on the server before it starts - a route, which
            can be added then and never afterwards. */
        explicit Rig (const std::function<void (OscQueryServer&)>& configure)
        {
            configure (server);
            started = server.start (0, nameSpace);
        }

        ~Rig() { server.stop(); }

        bool started = false;
        int port() const { return server.boundPort(); }
    };
}

//==============================================================================
TEST_CASE ("oscquery: the server binds an ephemeral port and serves the tree there")
{
    Rig rig;

    REQUIRE (rig.started);
    REQUIRE (rig.port() > 0);
    CHECK (rig.server.isRunning());

    const auto reply = get (rig.port(), "/");

    CHECK (reply.status == 200);

    /*  Parsed rather than string-matched. A test that greps for a substring
        passes on malformed JSON that happens to contain it, and this reply is
        the one thing every OSCQuery client in the world will parse. */
    const auto parsed = json::parse (reply.body);

    INFO (parsed.error);
    REQUIRE (parsed.value.has_value());
    REQUIRE (parsed.value->isObject());

    const auto* fullPath = parsed.value->find ("FULL_PATH");
    REQUIRE (fullPath != nullptr);
    CHECK (fullPath->asString() == "/");

    CHECK (parsed.value->find ("CONTENTS") != nullptr);

    /*  AND IT SAYS IT IS JSON, which nothing in this file asserted until the
        helper above started keeping headers. A browser fetching the tree from
        the page served on this same port reads `Content-Type` before it reads a
        byte of the body, and a reply that arrived as `text/plain` would parse
        in a test and be refused by `fetch`. */
    CHECK (reply.header ("Content-Type") == "application/json");
}

TEST_CASE ("oscquery: HOST_INFO says who this is and where to reach it")
{
    Rig rig;
    REQUIRE (rig.started);

    const auto reply = get (rig.port(), "/?HOST_INFO");

    CHECK (reply.status == 200);

    const auto parsed = json::parse (reply.body);

    INFO (parsed.error);
    REQUIRE (parsed.value.has_value());

    const auto* name = parsed.value->find ("NAME");
    const auto* oscPort = parsed.value->find ("OSC_PORT");
    const auto* wsPort = parsed.value->find ("WS_PORT");
    const auto* transport = parsed.value->find ("OSC_TRANSPORT");

    REQUIRE (name != nullptr);
    CHECK (name->asString() == "Go.dot");

    REQUIRE (oscPort != nullptr);
    CHECK (static_cast<int> (oscPort->asNumber()) == 8010);

    /*  The WS port is the HTTP port, because OSCQuery puts both on one and that
        is the entire reason juce_simpleweb is a dependency. Asserting it equals
        the bound port is asserting that claim. */
    REQUIRE (wsPort != nullptr);
    CHECK (static_cast<int> (wsPort->asNumber()) == rig.port());

    REQUIRE (transport != nullptr);
    CHECK (transport->asString() == "UDP");

    const auto* extensions = parsed.value->find ("EXTENSIONS");
    REQUIRE (extensions != nullptr);
    REQUIRE (extensions->isObject());

    //  The absent ones are as load-bearing as the present ones: a client reads
    //  this to decide what not to try.
    const auto* listen = extensions->find ("LISTEN");
    const auto* critical = extensions->find ("CRITICAL");

    REQUIRE (listen != nullptr);
    CHECK (listen->asBool());

    REQUIRE (critical != nullptr);
    CHECK_FALSE (critical->asBool());       // Phase 1 speaks UDP only
}

//==============================================================================
TEST_CASE ("oscquery: four questions, four different answers")
{
    Rig rig;
    REQUIRE (rig.started);

    SUBCASE ("200 — the attribute is there")
    {
        const auto reply = get (rig.port(), "/godot/engine/tick?VALUE");

        CHECK (reply.status == 200);

        const auto parsed = json::parse (reply.body);
        REQUIRE (parsed.value.has_value());
        CHECK (parsed.value->find ("VALUE") != nullptr);
    }

    SUBCASE ("404 — nothing lives at that address")
    {
        CHECK (get (rig.port(), "/godot/nope").status == 404);
        CHECK (get (rig.port(), "/godot/nope?VALUE").status == 404);
    }

    SUBCASE ("400 — that is not an OSCQuery attribute")
    {
        /*  The node is perfectly real. Answering 404 here would send a client
            hunting for a typo in an address that is correct. */
        CHECK (get (rig.port(), "/godot/engine/tick?WOBBLE").status == 400);
    }

    SUBCASE ("204 — the node is real and carries no such attribute")
    {
        /*  A container has no VALUE and no TYPE. This is the answer that is
            easiest to get wrong and worst to get wrong: 404 would say the node
            had gone away, and a JSON null would say the value IS null. */
        CHECK (get (rig.port(), "/godot/engine?VALUE").status == 204);
        CHECK (get (rig.port(), "/godot/engine?TYPE").status == 204);

        //  A number with no description, and a node with no unit.
        CHECK (get (rig.port(), "/godot/engine/level?DESCRIPTION").status == 204);
        CHECK (get (rig.port(), "/godot/engine/tick?RANGE").status == 204);

        /*  And CLIPMODE, always. Go.dot does not clip: a write out of range is
            rejected and logged, because a cue that silently became a different
            cue is worse than one that refused. Answering "none" would be a
            claim about clipping behaviour a client might rely on. */
        CHECK (get (rig.port(), "/godot/engine/level?CLIPMODE").status == 204);
    }

    SUBCASE ("ACCESS is the one attribute that can never answer 204")
    {
        //  Every node has one, including a container.
        CHECK (get (rig.port(), "/godot/engine?ACCESS").status == 200);
        CHECK (get (rig.port(), "/godot/engine/tick?ACCESS").status == 200);
        CHECK (get (rig.port(), "/godot/cmd/standby/next?ACCESS").status == 200);
    }

    SUBCASE ("a pattern is refused as a pattern, not as a missing node")
    {
        /*  A RAW star, which is what a client actually sends: `*` is a legal
            URI path character. An earlier version of this test percent-encoded
            it as %2A, and juce passed that through verbatim - so the server saw
            no star at all and answered 404, and the test was measuring nothing.

            400 and not 404 is the whole point: the client has asked for
            something Go.dot does not do, rather than named a node that is not
            there, and 404 would send it hunting for a typo in an address that
            is spelled correctly. */
        const auto reply = get (rig.port(), "/godot/cue/*/name");

        INFO (reply.body);
        CHECK (reply.status == 400);
        CHECK (reply.body.find ("pattern") != std::string::npos);
    }
}

TEST_CASE ("oscquery: RANGE and UNIT come back when the node has them")
{
    Rig rig;
    REQUIRE (rig.started);

    const auto range = get (rig.port(), "/godot/engine/level?RANGE");
    CHECK (range.status == 200);

    const auto parsedRange = json::parse (range.body);
    INFO (parsedRange.error);
    REQUIRE (parsedRange.value.has_value());
    CHECK (parsedRange.value->find ("RANGE") != nullptr);

    const auto unit = get (rig.port(), "/godot/engine/level?UNIT");
    CHECK (unit.status == 200);
}

//==============================================================================
TEST_CASE ("oscquery: LISTEN brings pushes, IGNORE stops them")
{
    Rig rig;
    REQUIRE (rig.started);

    Client client;
    client.connect (rig.port());
    REQUIRE (waitUntil ([&client] { return client.open.load(); }));

    //  Nothing is pushed to a connection that has not asked.
    tree::TreeDiff diff;
    diff.valueChanged.push_back ("/godot/engine/tick");

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");
    std::this_thread::sleep_for (std::chrono::milliseconds { 200 });
    CHECK (client.binaryCount() == 0);

    //  LISTEN, and the next tick's change arrives.
    client.socket.send (juce::String (
        "{\"COMMAND\": \"LISTEN\", \"DATA\": \"/godot/engine/tick\"}"));

    REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 1; }));

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");

    REQUIRE (waitUntil ([&client] { return client.binaryCount() == 1; }));

    //  And what arrived is the node's value, as OSC.
    {
        const std::lock_guard<std::mutex> lock { client.mutex };
        const auto decoded = osc::decode (client.binary[0].data(), client.binary[0].size());

        INFO (decoded.error);
        REQUIRE (decoded.ok);
        CHECK (decoded.packet.address == "/godot/engine/tick");
        REQUIRE (decoded.packet.args.size() == 1);
        CHECK (decoded.packet.args[0] == osc::Value::int64 (12));
    }

    //  IGNORE, and it stops.
    client.socket.send (juce::String (
        "{\"COMMAND\": \"IGNORE\", \"DATA\": \"/godot/engine/tick\"}"));

    REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 0; }));

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");
    std::this_thread::sleep_for (std::chrono::milliseconds { 200 });

    CHECK (client.binaryCount() == 1);      // still just the one
}

TEST_CASE ("oscquery: a client is not told what it just did")
{
    /*  Echo suppression, which is the difference between a control surface and
        a fight. A client that moved a fader already has it there; sending the
        value back is what makes the slider jump under the hand holding it.

        The test needs the connection's ORIGIN, which is `ws:<ip>:<port>` with
        an ephemeral port nobody here chose. It learns it the way the engine
        does: the client sends one OSC frame, and the origin the server stamped
        on it is recorded by the fake namespace. */
    Rig rig;
    REQUIRE (rig.started);

    Client client;
    client.connect (rig.port());
    REQUIRE (waitUntil ([&client] { return client.open.load(); }));

    client.socket.send (juce::String (
        "{\"COMMAND\": \"LISTEN\", \"DATA\": \"/godot/engine/tick\"}"));
    REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 1; }));

    //  One frame, purely to learn what this connection is called.
    std::string error;
    const auto encoded = osc::encode (osc::Packet::message ("/godot/cmd/standby/next"), error);
    REQUIRE (encoded.has_value());

    client.socket.send (reinterpret_cast<const char*> (encoded->data()),
                        static_cast<int> (encoded->size()));
    REQUIRE (waitUntil ([&rig] { return rig.nameSpace.writeCount() == 1; }));

    std::string origin;
    {
        const std::lock_guard<std::mutex> lock { rig.nameSpace.mutex };
        origin = rig.nameSpace.writes.front().first;
    }

    INFO ("this connection is " << origin);
    REQUIRE_FALSE (origin.empty());

    tree::TreeDiff diff;
    diff.valueChanged.push_back ("/godot/engine/tick");

    //  Somebody else caused it: the change arrives.
    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");
    REQUIRE (waitUntil ([&client] { return client.binaryCount() == 1; }));

    //  THIS connection caused it: nothing arrives.
    rig.server.publishChanges (diff, *rig.nameSpace.tree, origin);
    std::this_thread::sleep_for (std::chrono::milliseconds { 300 });
    CHECK (client.binaryCount() == 1);

    //  And touch gating, which is the same refusal from the other direction:
    //  the client is holding the node, so it is not corrected mid-gesture even
    //  when somebody else moved it.
    rig.nameSpace.touchedBy = origin;
    rig.nameSpace.touchedAddress = "/godot/engine/tick";

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");
    std::this_thread::sleep_for (std::chrono::milliseconds { 300 });
    CHECK (client.binaryCount() == 1);

    //  Released, and the next change reaches it again.
    rig.nameSpace.touchedBy.clear();
    rig.nameSpace.touchedAddress.clear();

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");
    REQUIRE (waitUntil ([&client] { return client.binaryCount() == 2; }));
}

TEST_CASE ("oscquery: two clients get two origins, and only one of them is echoed")
{
    /*  The plan named this case and the earlier tests did not cover it: they
        used ONE WebSocket client and blamed a non-WebSocket writer, which
        exercises the comparison but never the thing that makes it safe.

        WHAT IS ACTUALLY BEING CHECKED is that two connections get DIFFERENT
        origins. Every origin here is `ws:<ip>:<port>`, and both clients arrive
        from the same loopback address - so the port is the only thing telling
        them apart. Drop it, key suppression on the address alone, and two
        surfaces on one machine (or two behind one NAT, which is the case that
        actually happens in a venue) become one client: a fader moved on the
        first would go silent on the second, which reads as a dropped message
        and is nearly impossible to diagnose from either end.

        So: both listen, one writes, and the OTHER must hear about it. */
    Rig rig;
    REQUIRE (rig.started);

    Client first;
    Client second;

    first.connect (rig.port());
    second.connect (rig.port());

    REQUIRE (waitUntil ([&first] { return first.open.load(); }));
    REQUIRE (waitUntil ([&second] { return second.open.load(); }));

    first.socket.send (juce::String (
        "{\"COMMAND\": \"LISTEN\", \"DATA\": \"/godot/engine/tick\"}"));
    second.socket.send (juce::String (
        "{\"COMMAND\": \"LISTEN\", \"DATA\": \"/godot/engine/tick\"}"));

    REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 2; }));

    //  Learn what the FIRST connection is called, the way the engine does.
    std::string error;
    const auto encoded = osc::encode (osc::Packet::message ("/godot/cmd/standby/next"), error);
    REQUIRE (encoded.has_value());

    first.socket.send (reinterpret_cast<const char*> (encoded->data()),
                       static_cast<int> (encoded->size()));

    REQUIRE (waitUntil ([&rig] { return rig.nameSpace.writeCount() == 1; }));

    std::string firstOrigin;
    {
        const std::lock_guard<std::mutex> lock { rig.nameSpace.mutex };
        firstOrigin = rig.nameSpace.writes.front().first;
    }

    //  And the second, so the two can be compared.
    second.socket.send (reinterpret_cast<const char*> (encoded->data()),
                        static_cast<int> (encoded->size()));

    REQUIRE (waitUntil ([&rig] { return rig.nameSpace.writeCount() == 2; }));

    std::string secondOrigin;
    {
        const std::lock_guard<std::mutex> lock { rig.nameSpace.mutex };
        secondOrigin = rig.nameSpace.writes.back().first;
    }

    INFO ("first:  " << firstOrigin);
    INFO ("second: " << secondOrigin);

    CHECK (firstOrigin.rfind ("ws:", 0) == 0);
    CHECK (secondOrigin.rfind ("ws:", 0) == 0);

    /*  The whole point. Same address, different port, therefore different
        origin - and this is the assertion that fails first if the port ever
        stops being part of it. */
    CHECK (firstOrigin != secondOrigin);

    //  Now blame the first. The second must still be told.
    tree::TreeDiff diff;
    diff.valueChanged.push_back ("/godot/engine/tick");

    rig.server.publishChanges (diff, *rig.nameSpace.tree, firstOrigin);

    REQUIRE (waitUntil ([&second] { return second.binaryCount() >= 1; }));

    CHECK (second.binaryCount() >= 1);
    CHECK (first.binaryCount() == 0);

    first.socket.stop();
    second.socket.stop();
}

TEST_CASE ("oscquery: a binary frame becomes a write, and a malformed one does not")
{
    Rig rig;
    REQUIRE (rig.started);

    Client client;
    client.connect (rig.port());
    REQUIRE (waitUntil ([&client] { return client.open.load(); }));

    std::string error;
    const auto packet = osc::Packet::message ("/godot/cmd/standby/next");
    const auto encoded = osc::encode (packet, error);

    INFO (error);
    REQUIRE (encoded.has_value());

    client.socket.send (reinterpret_cast<const char*> (encoded->data()),
                        static_cast<int> (encoded->size()));

    REQUIRE (waitUntil ([&rig] { return rig.nameSpace.writeCount() == 1; }));

    {
        const std::lock_guard<std::mutex> lock { rig.nameSpace.mutex };
        const auto& [origin, received] = rig.nameSpace.writes.front();

        CHECK (received.address == "/godot/cmd/standby/next");

        /*  The origin carries the port as well as the address. Two clients
            behind one NAT share an address, and echo suppression keyed on the
            address alone would silence a message for a surface that never sent
            it. */
        INFO ("origin: " << origin);
        CHECK (origin.rfind ("ws:", 0) == 0);
        CHECK (origin.find ("127.0.0.1:") != std::string::npos);
    }

    //  A malformed frame reaches the decoder and stops there.
    const std::vector<std::uint8_t> rubbish { '/', 'x', 0, 0, ',', 'f', 'f', 'f' };
    client.socket.send (reinterpret_cast<const char*> (rubbish.data()),
                        static_cast<int> (rubbish.size()));

    std::this_thread::sleep_for (std::chrono::milliseconds { 300 });
    CHECK (rig.nameSpace.writeCount() == 1);        // still just the good one
}

TEST_CASE ("oscquery: a structural change is announced before values follow")
{
    Rig rig;
    REQUIRE (rig.started);

    Client client;
    client.connect (rig.port());
    REQUIRE (waitUntil ([&client] { return client.open.load(); }));

    tree::TreeDiff diff;
    diff.added.push_back ("/godot/cue/B3N8R5TW");
    diff.removed.push_back ("/godot/cue/D9FH2JKA");

    rig.server.publishChanges (diff, *rig.nameSpace.tree, "cli");

    REQUIRE (waitUntil ([&client] { return client.textCount() >= 2; }));

    const std::lock_guard<std::mutex> lock { client.mutex };

    /*  PATH_ADDED before PATH_REMOVED, and both before any value. A client told
        a node's value changed before it has been told the node exists has to
        guess; told in this order it never does. */
    CHECK (client.text[0].contains ("PATH_ADDED"));
    CHECK (client.text[0].contains ("B3N8R5TW"));
    CHECK (client.text[1].contains ("PATH_REMOVED"));
}

//==============================================================================
TEST_CASE ("oscquery: a disconnect takes its subscriptions and its touches with it")
{
    /*  Not housekeeping. juce_simpleweb's connection ids are `<ip>:<port>` and
        a loopback port is reused within seconds, so a table that outlived its
        connection would hand a previous client's subscriptions to whoever got
        the same port next. And PRD 3.16 has a disconnect release every touch
        that origin held, or a surface that crashed mid-gesture leaves a node
        gated against everybody for the rest of the show. */
    Rig rig;
    REQUIRE (rig.started);

    {
        Client client;
        client.connect (rig.port());
        REQUIRE (waitUntil ([&client] { return client.open.load(); }));

        client.socket.send (juce::String (
            "{\"COMMAND\": \"LISTEN\", \"DATA\": \"/godot/engine/tick\"}"));

        REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 1; }));
    }   // the client goes away here

    REQUIRE (waitUntil ([&rig] { return rig.server.connectionCount() == 0; }));

    const std::lock_guard<std::mutex> lock { rig.nameSpace.mutex };
    REQUIRE_FALSE (rig.nameSpace.forgotten.empty());
    CHECK (rig.nameSpace.forgotten.front().rfind ("ws:", 0) == 0);
}

TEST_CASE ("oscquery: start is refused twice, and stop is safe to repeat")
{
    FakeNamespace nameSpace;
    OscQueryServer server;

    CHECK_FALSE (server.isRunning());
    server.stop();                                  // never started

    REQUIRE (server.start (0, nameSpace));
    CHECK (server.isRunning());
    CHECK (server.boundPort() > 0);

    CHECK_FALSE (server.start (0, nameSpace));      // already running

    server.stop();
    server.stop();
    CHECK_FALSE (server.isRunning());
    CHECK (server.boundPort() == 0);
}

//==============================================================================
/*  THE ROUTES, and first the hook on its own, behind a handler that answers
    whatever the path tells it to. What is pinned here is the server's half of
    the bargain - where a route is matched, what reaches it, that its reply
    goes out as it was written - so the handler knows nothing about pyramids,
    which is also true of the server. */
namespace
{
    /*  What a route's handler was asked: recorded on the HTTP thread, read by
        the test once the reply is back, and under a lock both times, because
        a reply arriving on a socket orders nothing in the memory model. */
    struct Seen
    {
        std::mutex mutex;
        std::vector<std::pair<std::string, std::string>> requests;

        void record (const std::string& path, const std::string& query)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            requests.emplace_back (path, query);
        }

        bool saw (const std::string& path, const std::string& query)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return std::find (requests.begin(), requests.end(), std::make_pair (path, query))
                     != requests.end();
        }

        bool sawPath (const std::string& path)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return std::any_of (requests.begin(), requests.end(),
                                [&path] (const std::pair<std::string, std::string>& asked)
                                {
                                    return asked.first == path;
                                });
        }
    };

    /*  Ten bytes, two of them nought and one of them 0xff: a body that a
        pointer and a terminator anywhere between the handler and the socket
        would cut short after four. */
    std::string awkwardBytes()
    {
        return std::string ("zero\0one\0\xff", 10);
    }

    /*  Answers with the status its last segment spells when that is a number,
        200 otherwise; throws a `std::runtime_error` when it spells `throw`,
        and an int - nothing derived from `std::exception` at all - when it
        spells `throw-int`. Whatever it answers carries a body the server must
        not cut, a field of its own, and three fields that are the server's to
        write - so the server must drop them. */
    RouteReply answerAsTold (const std::string& path, const std::string& query, Seen& seen)
    {
        seen.record (path, query);

        const auto last = path.substr (path.rfind ('/') + 1);

        if (last == "throw")
            throw std::runtime_error ("a handler that fails");

        /*  The throw that only the server's own catch stands in front of:
            every catch juce_simpleweb has names `std::exception`. */
        if (last == "throw-int")
            throw 42;

        RouteReply reply;
        reply.status = (! last.empty() && std::isdigit (static_cast<unsigned char> (last.front())) != 0)
                         ? std::atoi (last.c_str())
                         : 200;
        reply.contentType = "application/x-go-dot-test";
        reply.body = awkwardBytes();
        reply.headers = { { "X-Route", "answered" },
                          { "Content-Length", "3" },
                          { "content-type", "text/html" },
                          { "Transfer-Encoding", "chunked" } };
        return reply;
    }
}

TEST_CASE ("oscquery: a route answers its prefix and below it, byte for byte, and nothing else")
{
    auto seen = std::make_shared<Seen>();

    Rig rig ([seen] (OscQueryServer& server)
    {
        server.serveRoute ("/media", [seen] (const std::string& path, const std::string& query)
                                     {
                                         return answerAsTold (path, query, *seen);
                                     });

        /*  And one at the client's own prefix, which must never be reached:
            `/ui` is answered before any route is looked at. */
        server.serveRoute ("/ui", [seen] (const std::string& path, const std::string& query)
                                  {
                                      return answerAsTold (path, query, *seen);
                                  });
    });

    REQUIRE (rig.started);

    SUBCASE ("the reply goes out as the handler wrote it")
    {
        const auto reply = get (rig.port(), "/media");

        CHECK (reply.status == 200);
        CHECK (reply.header ("Content-Type") == "application/x-go-dot-test");
        CHECK (reply.headerCount ("Content-Type") == 1u);
        CHECK (reply.header ("X-Route") == "answered");

        /*  Every byte, the noughts included, and a length that counts them:
            the handler's own `Content-Length: 3` was dropped, not believed -
            and so was its `Transfer-Encoding: chunked`, which would have
            stopped the server writing a length at all and left a client
            trying to read ten plain bytes as chunks. */
        REQUIRE (awkwardBytes().size() == 10u);
        CHECK (reply.body == awkwardBytes());
        CHECK (reply.header ("Content-Length") == "10");
        CHECK (reply.headerCount ("Content-Length") == 1u);
        CHECK (reply.headerCount ("Transfer-Encoding") == 0u);
    }

    SUBCASE ("its status, and a 500 for one it may not use or for a handler that throws")
    {
        CHECK (get (rig.port(), "/media/400").status == 400);
        CHECK (get (rig.port(), "/media/404").status == 404);
        CHECK (get (rig.port(), "/media/500").status == 500);
        CHECK (get (rig.port(), "/media/418").status == 500);

        /*  A `std::exception` is one juce_simpleweb would have caught by
            itself, and then dropped the request: what the server's own catch
            buys here is this 500 where the client would have read an empty
            reply - and a `no-store` on it, like every refusal a route writes.
            The thread was never at risk from this throw, and the tree is
            asked once more only to show that nothing else changed; the throw
            that would have ended the thread is the next case's. */
        const auto thrown = get (rig.port(), "/media/throw");

        CHECK (thrown.status == 500);
        CHECK (thrown.header ("Cache-Control") == "no-store");
        CHECK (get (rig.port(), "/godot/engine/tick?VALUE").status == 200);
    }

    SUBCASE ("a throw that is not a std::exception is a 500 too, and the port lives on")
    {
        /*  The case the catch is really for. An int passes every catch
            juce_simpleweb has, and without the server's own it would leave
            the io service's `run()` and end the thread that carries the tree
            and every WebSocket, with the server still believing it was
            connected. Were that catch to go, this case would not merely fail:
            the tree below would go unanswered, and the rig's `stop()` would
            then spin for ever waiting on a thread that is gone - a hang, which
            is the bug, reproduced. */
        const auto thrown = get (rig.port(), "/media/throw-int");

        CHECK (thrown.status == 500);
        CHECK (thrown.header ("Content-Type") == "text/plain");
        CHECK (thrown.header ("Cache-Control") == "no-store");
        CHECK (seen->sawPath ("/media/throw-int"));

        //  And on the same server, a tree address is answered as ever.
        CHECK (get (rig.port(), "/godot/engine/tick?VALUE").status == 200);
    }

    SUBCASE ("the query reaches the route whole, before the tree could refuse it")
    {
        /*  A `=` and a `&` - a form submission, to the tree, and refused as
            one there - handed over untouched. */
        CHECK (get (rig.port(), "/media/a/b?level=2&x").status == 200);
        CHECK (seen->saw ("/media/a/b", "level=2&x"));

        //  HOST_INFO asked of a route's path is the route's question.
        CHECK (get (rig.port(), "/media?HOST_INFO").body == awkwardBytes());
        CHECK (seen->saw ("/media", "HOST_INFO"));

        /*  And the star of a pattern, which the tree refuses with a 400 of
            its own, reaches the route as the request spelled it. */
        CHECK (get (rig.port(), "/media/*/timbre").status == 200);
        CHECK (seen->saw ("/media/*/timbre", ""));
    }

    SUBCASE ("a path that only begins with the same letters is the tree's")
    {
        const auto reply = get (rig.port(), "/mediaserver");

        INFO (reply.body);
        CHECK (reply.status == 404);
        CHECK (reply.body.find ("no such node") != std::string::npos);
        CHECK_FALSE (seen->sawPath ("/mediaserver"));
    }

    SUBCASE ("the client is answered first, and a POST is still a 405")
    {
        const auto client = get (rig.port(), "/ui");

        INFO (client.body);
        CHECK (client.status == 404);
        CHECK (client.body.find ("no client is being served") != std::string::npos);
        CHECK_FALSE (seen->sawPath ("/ui"));

        CHECK (sendRequest (rig.port(), "POST", "/media").status == 405);
        CHECK_FALSE (seen->sawPath ("/media"));
    }
}

TEST_CASE ("oscquery: a route is added before the server runs, at a real prefix, or not at all")
{
    const auto answer = [] (const std::string&, const std::string&)
    {
        RouteReply reply;
        reply.status = 200;
        reply.body = "route\n";
        return reply;
    };

    Rig rig ([&answer] (OscQueryServer& server)
    {
        server.serveRoute ("", answer);                 // every path there is
        server.serveRoute ("/", answer);                // the same
        server.serveRoute ("/late/", answer);           // a slash it would want twice
        server.serveRoute ("/slash/", answer);          // the same, and asked for below
        server.serveRoute ("/nothing", RouteHandler {});
    });

    REQUIRE (rig.started);

    /*  Too late: the HTTP thread may already be walking the routes, and a
        vector must not grow under a reader. */
    rig.server.serveRoute ("/late", answer);

    //  So the tree is still the tree...
    const auto root = get (rig.port(), "/");

    CHECK (root.status == 200);
    CHECK (root.body != "route\n");
    CHECK (get (rig.port(), "/godot/engine/tick?VALUE").status == 200);

    //  ...and none of the refused prefixes answers anything but "no such node".
    for (const auto* refusedPath : { "/late", "/late/x", "/nothing" })
    {
        const auto reply = get (rig.port(), refusedPath);

        INFO (refusedPath);
        CHECK (reply.status == 404);
        CHECK (reply.body.find ("no such node") != std::string::npos);
    }

    /*  A PREFIX WITH A SLASH LAST, asked for at the only paths it could have
        matched had it been taken: itself, and itself and a slash - which, for
        a prefix already ending in one, is a doubled slash. None of the paths
        asked so far is either, so a server that took `/late/` would have
        passed every check above. Neither reaches the handler. What the tree
        makes of them is the tree's business and not pinned here - only that
        the tree answered, and not the route. */
    for (const auto* slashedPath : { "/slash/", "/slash//x" })
    {
        const auto reply = get (rig.port(), slashedPath);

        INFO (slashedPath);
        INFO (reply.body);
        CHECK (reply.status != 0);
        CHECK (reply.body != "route\n");
    }
}

//==============================================================================
/*  THE TIMBRE ROUTE, asked directly. It is a pure function over a
    `MediaInfo`, so these cases build one from an empty show and no media
    folder - nothing is read from a disk - and publish its records by hand,
    the way the analyser does: one file with a hash and a pyramid, and one
    with a hash and none, which the analyser never publishes and the route must
    refuse all the same. */
namespace
{
    namespace timbre = audio::timbre;

    /*  The sha256 of "test", of nothing, and of "hello": real digests, so the
        route is fed the shape it will meet. Only the first is analysed. */
    constexpr const char* analysedHash = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    constexpr const char* hashWithoutPyramid = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    constexpr const char* unknownHash = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

    /*  3.5 seconds at 44.1 kHz: 151 frames of a hop each, and so three
        levels - 151, 76 and 38 - and a length with a fraction in it, which a
        French locale would write with a comma if anything let it. */
    constexpr std::uint32_t chosenRate = 44100;
    constexpr std::uint64_t chosenSamples = 154350;
    constexpr std::size_t chosenFrameCount = 151;

    /*  Frames whose bytes the test chose, so it knows every one. Each tenth
        is silent - hue, saturation and lightness nought, and the first one's
        peak as well, so the body opens on four zero bytes it must carry
        rather than stop at - and the rest spread across the byte range, 0xff
        included. */
    std::vector<timbre::Frame> chosenFrames (std::size_t count)
    {
        std::vector<timbre::Frame> frames (count);

        for (std::size_t i = 0; i < count; ++i)
        {
            auto& frame = frames[i];
            frame.peak = static_cast<std::uint8_t> ((i * 11) % 256);

            if (i % 10 == 0)
                continue;

            frame.hue = static_cast<std::uint8_t> ((i * 37) % 256);
            frame.saturation = static_cast<std::uint8_t> (255 - i % 256);
            frame.lightness = static_cast<std::uint8_t> ((40 + i) % 256);
        }

        return frames;
    }

    struct AnalysedMedia
    {
        doc::ShowDocument document;
        audio::MediaInfo media { document, std::string() };

        std::shared_ptr<const audio::TimbrePyramid> pyramid =
            std::make_shared<audio::TimbrePyramid> (
                timbre::pyramidOf (chosenFrames (chosenFrameCount), chosenRate, chosenSamples));

        AnalysedMedia()
        {
            audio::MediaRecord sounding;
            sounding.seconds = 3.5;
            sounding.contentHash = analysedHash;
            sounding.pyramid = pyramid;
            media.publish ("thunder.wav", std::move (sounding));

            audio::MediaRecord hashedOnly;
            hashedOnly.contentHash = hashWithoutPyramid;
            media.publish ("rain.wav", std::move (hashedOnly));
        }
    };

    std::string timbrePath (const std::string& hash)
    {
        return std::string (mediaRoutePrefix) + "/" + hash + "/timbre";
    }

    /*  A field of a reply the route built, by a name compared without case,
        as the server will send it. */
    std::string fieldOf (const RouteReply& reply, const std::string& name)
    {
        for (const auto& [field, value] : reply.headers)
            if (lowerCased (field) == lowerCased (name))
                return value;

        return {};
    }

    std::size_t littleEndianAt (const std::vector<std::uint8_t>& bytes, std::size_t at,
                                std::size_t width)
    {
        std::size_t value = 0;

        for (std::size_t i = width; i > 0; --i)
            value = (value << 8) | static_cast<std::size_t> (bytes[at + i - 1]);

        return value;
    }

    /*  THE BYTES THE `.tpy` HOLDS FOR ONE LEVEL, found through the file's own
        table of levels - a frame count and an offset, four bytes each, from
        byte 32 (Timbre.h) - rather than by working out where they ought to be.
        Empty when the table points outside the file. */
    std::string levelBytesInFile (const std::vector<std::uint8_t>& file, std::size_t level)
    {
        constexpr std::size_t tableStart = 32;
        constexpr std::size_t entrySize = 8;

        if (file.size() < tableStart + entrySize * (level + 1))
            return {};

        const auto frameCount = littleEndianAt (file, tableStart + entrySize * level, 4);
        const auto offset = littleEndianAt (file, tableStart + entrySize * level + 4, 4);

        if (offset + 4 * frameCount > file.size())
            return {};

        return std::string (reinterpret_cast<const char*> (file.data() + offset), 4 * frameCount);
    }

    /*  EVERY REFUSAL LOOKS THE SAME: its status, one line of plain text, and
        a `no-store` and nothing else - a 404 for a hash that is not analysed
        yet is a 200 a minute later, and a cached one would keep a bar grey. */
    void checkRefusal (const RouteReply& reply, int status, const std::string& asked)
    {
        INFO ("asked: " << asked);
        INFO ("answered: " << reply.body);

        CHECK (reply.status == status);
        CHECK (reply.contentType == "text/plain");
        CHECK (fieldOf (reply, "Cache-Control") == "no-store");
        CHECK (reply.headers.size() == 1u);

        REQUIRE_FALSE (reply.body.empty());
        CHECK (reply.body.back() == '\n');
        CHECK (std::count (reply.body.begin(), reply.body.end(), '\n') == 1);
    }

    /*  EVERY 200 LOOKS THE SAME TOO: kept, but asked for again before it is
        reused. Not a year and `immutable`, which is what this said first: the
        URL names the audio and not the analysis, a bumped
        `timbre::formatVersion` rebuilds the `.tpy` under the same name, and a
        copy kept for good would show the colours from before the change. */
    constexpr const char* askedAgain = "no-cache";
}

TEST_CASE ("timbre route: a level is the file's own bytes, and INFO is its header")
{
    AnalysedMedia analysed;
    const auto& pyramid = *analysed.pyramid;

    /*  The fixture, pinned: the frame count follows from the samples, and the
        levels from halving down to 64 or fewer. */
    REQUIRE (pyramid.frames() == chosenFrameCount);
    REQUIRE (pyramid.levels.size() == 3u);

    const auto file = timbre::write (pyramid);
    const auto path = timbrePath (analysedHash);

    for (std::size_t level = 0; level < pyramid.levels.size(); ++level)
    {
        INFO ("level " << level);

        const auto reply = answerTimbreRoute (analysed.media, path, "level=" + std::to_string (level));

        CHECK (reply.status == 200);
        CHECK (reply.contentType == "application/octet-stream");
        CHECK (fieldOf (reply, "Cache-Control") == askedAgain);

        /*  Four bytes a frame, hue, saturation, lightness, peak, spelled out
            from the frames here - and then the same bytes the file writer puts
            in the `.tpy`, which is the claim the route makes. */
        std::string expected;

        for (const auto& frame : pyramid.levels[level])
            for (const auto part : { frame.hue, frame.saturation, frame.lightness, frame.peak })
                expected.push_back (static_cast<char> (part));

        CHECK (reply.body.size() == 4 * pyramid.levels[level].size());
        CHECK (reply.body == expected);

        const auto inFile = levelBytesInFile (file, level);

        REQUIRE_FALSE (inFile.empty());
        CHECK (reply.body == inFile);
    }

    //  The first frame is silent with no peak: the body opens on four noughts.
    CHECK (answerTimbreRoute (analysed.media, path, "level=0").body.substr (0, 4)
           == std::string (4, '\0'));

    const auto info = answerTimbreRoute (analysed.media, path, "INFO");

    CHECK (info.status == 200);
    CHECK (info.contentType == "application/json");
    CHECK (fieldOf (info, "Cache-Control") == askedAgain);

    /*  The exact text first - the keys in the documented order, no spaces, and
        3.5 with a point under either locale this suite runs in. The version is
        spelled from the constant rather than as a digit: it is bumped with
        every change to the analysis, and this test is about where it goes, not
        which it is... */
    CHECK (info.body == std::string ("{\"sha256\":\"") + analysedHash + "\","
                        "\"formatVersion\":" + std::to_string (timbre::formatVersion) + ","
                        "\"seconds\":3.5,\"sampleRate\":44100,\"window\":2048,\"hop\":1024,"
                        "\"levels\":[{\"frames\":151,\"bytes\":604},"
                                    "{\"frames\":76,\"bytes\":304},"
                                    "{\"frames\":38,\"bytes\":152}]}");

    //  ...then parsed, as a client would read it.
    const auto parsed = json::parse (info.body);

    INFO (parsed.error);
    REQUIRE (parsed.ok());
    REQUIRE (parsed.value->isObject());

    const auto* sha256 = parsed.value->find ("sha256");
    const auto* analysisVersion = parsed.value->find ("formatVersion");
    const auto* seconds = parsed.value->find ("seconds");
    const auto* sampleRate = parsed.value->find ("sampleRate");
    const auto* window = parsed.value->find ("window");
    const auto* hop = parsed.value->find ("hop");
    const auto* levels = parsed.value->find ("levels");

    REQUIRE (sha256 != nullptr);
    CHECK (sha256->asString() == analysedHash);

    /*  The half of what the bytes depend on that the URL does not name. */
    REQUIRE (analysisVersion != nullptr);
    CHECK (analysisVersion->asInt() == static_cast<int> (timbre::formatVersion));

    REQUIRE (seconds != nullptr);
    CHECK (seconds->asNumber() == doctest::Approx (3.5));

    REQUIRE (sampleRate != nullptr);
    CHECK (sampleRate->asInt() == 44100);

    REQUIRE (window != nullptr);
    CHECK (window->asInt() == timbre::windowSize);

    REQUIRE (hop != nullptr);
    CHECK (hop->asInt() == timbre::hopSize);

    REQUIRE (levels != nullptr);
    REQUIRE (levels->isArray());
    REQUIRE (levels->size() == pyramid.levels.size());

    for (std::size_t level = 0; level < pyramid.levels.size(); ++level)
    {
        INFO ("level " << level);

        const auto* entry = levels->at (level);
        REQUIRE (entry != nullptr);

        const auto* frameCount = entry->find ("frames");
        const auto* byteCount = entry->find ("bytes");

        REQUIRE (frameCount != nullptr);
        REQUIRE (byteCount != nullptr);

        CHECK (static_cast<std::size_t> (frameCount->asInt()) == pyramid.levels[level].size());
        CHECK (static_cast<std::size_t> (byteCount->asInt()) == 4 * pyramid.levels[level].size());

        //  And a level asked for is exactly as many bytes as INFO promised.
        CHECK (answerTimbreRoute (analysed.media, path, "level=" + std::to_string (level)).body.size()
               == static_cast<std::size_t> (byteCount->asInt()));
    }
}

TEST_CASE ("timbre route: the shape, then the hash, then the question, then the records")
{
    AnalysedMedia analysed;
    const auto& media = analysed.media;
    const std::string known { analysedHash };
    const auto path = timbrePath (known);

    SUBCASE ("404 - a path this route does not have")
    {
        /*  Including one whose segment is not a hash at all: the shape is
            checked first, so a path that is wrong twice is told the first. */
        for (const auto& asked : std::vector<std::string> {
                 "/media", "/media/", "/media/" + known, "/media/" + known + "/",
                 "/media/" + known + "/timbre/", "/media/" + known + "/timbre/extra",
                 "/media/" + known + "/other", "/media//timbre", "/media/timbre",
                 "/media/a/" + known + "/timbre", "/media/nothex/other" })
            checkRefusal (answerTimbreRoute (media, asked, "INFO"), 404, asked);
    }

    SUBCASE ("400 - a segment that is not a content hash")
    {
        auto upperCase = known;
        upperCase[1] = 'F';                     // it was 'f'

        auto notHex = known;
        notHex[1] = 'g';

        for (const auto& hash : std::vector<std::string> {
                 known.substr (0, 63), known + "0", upperCase, notHex, "timbre" })
            checkRefusal (answerTimbreRoute (media, timbrePath (hash), "INFO"), 400, hash);
    }

    SUBCASE ("400 - a question this route does not answer")
    {
        for (const auto& query : std::vector<std::string> {
                 "", "level=", "level=-1", "level=x", "level=1&x=2", "VALUE", "HOST_INFO",
                 "info", "INFO=", "levels=1", "level=+1", "level= 1", "level=1 ",
                 "level=1234567890" })
            checkRefusal (answerTimbreRoute (media, path, query), 400, "?" + query);

        /*  And before the records are looked at: a hash nobody has analysed,
            asked for badly, is told that it asked badly. */
        checkRefusal (answerTimbreRoute (media, timbrePath (unknownHash), "level=x"), 400,
                      "an unknown hash, asked badly");
    }

    SUBCASE ("404 - asked well, and not here")
    {
        checkRefusal (answerTimbreRoute (media, timbrePath (unknownHash), "INFO"), 404,
                      "the header of a hash nobody has analysed");
        checkRefusal (answerTimbreRoute (media, timbrePath (unknownHash), "level=0"), 404,
                      "a level of a hash nobody has analysed");

        checkRefusal (answerTimbreRoute (media, timbrePath (hashWithoutPyramid), "INFO"), 404,
                      "the header of a hash with no pyramid");
        checkRefusal (answerTimbreRoute (media, timbrePath (hashWithoutPyramid), "level=0"), 404,
                      "a level of a hash with no pyramid");

        /*  The level count is one past the last level, and nine nines is the
            most a level can be spelled with. The last real level is a 200, so
            the line is drawn where it should be. */
        const auto levelCount = analysed.pyramid->levels.size();

        checkRefusal (answerTimbreRoute (media, path, "level=" + std::to_string (levelCount)), 404,
                      "the level count");
        checkRefusal (answerTimbreRoute (media, path, "level=999999999"), 404, "nine nines");

        CHECK (answerTimbreRoute (media, path, "level=" + std::to_string (levelCount - 1)).status
               == 200);
    }

    SUBCASE ("a 404 becomes a 200 once the analyser has been by, which is why none is kept")
    {
        checkRefusal (answerTimbreRoute (media, timbrePath (unknownHash), "INFO"), 404, "before");

        audio::MediaRecord late;
        late.contentHash = unknownHash;
        late.pyramid = analysed.pyramid;
        analysed.media.publish ("imported.wav", std::move (late));

        const auto after = answerTimbreRoute (media, timbrePath (unknownHash), "INFO");

        CHECK (after.status == 200);
        CHECK (fieldOf (after, "Cache-Control") == askedAgain);
        CHECK (after.body.find (unknownHash) != std::string::npos);
    }
}

TEST_CASE ("timbre route: through the server, a level is the file's bytes and says to ask again before reuse")
{
    /*  Declared before the rig, so it outlives the server that reads it. */
    AnalysedMedia analysed;

    Rig rig ([&analysed] (OscQueryServer& server)
    {
        server.serveRoute (mediaRoutePrefix,
                           [&analysed] (const std::string& path, const std::string& query)
                           {
                               return answerTimbreRoute (analysed.media, path, query);
                           });
    });

    REQUIRE (rig.started);

    const auto file = timbre::write (*analysed.pyramid);
    const auto level = get (rig.port(), timbrePath (analysedHash) + "?level=0");

    CHECK (level.status == 200);
    CHECK (level.header ("Content-Type") == "application/octet-stream");
    CHECK (level.header ("Cache-Control") == askedAgain);
    CHECK (level.header ("Content-Length") == std::to_string (4 * analysed.pyramid->frames()));

    const auto inFile = levelBytesInFile (file, 0);

    REQUIRE_FALSE (inFile.empty());
    CHECK (level.body == inFile);
    CHECK (level.body.substr (0, 4) == std::string (4, '\0'));

    /*  A refusal says so on the wire as well: `no-store`, and the route's own
        reason - `level=1&x=2` reached it, rather than the tree's rule that an
        attribute query is a bare key. */
    const auto badly = get (rig.port(), timbrePath (analysedHash) + "?level=1&x=2");

    INFO (badly.body);
    CHECK (badly.status == 400);
    CHECK (badly.header ("Content-Type") == "text/plain");
    CHECK (badly.header ("Cache-Control") == "no-store");
    CHECK (badly.body.find ("?INFO or ?level=") != std::string::npos);

    const auto missing = get (rig.port(), timbrePath (unknownHash) + "?INFO");

    CHECK (missing.status == 404);
    CHECK (missing.header ("Cache-Control") == "no-store");
}

//==============================================================================
TEST_CASE ("subscriptions: the table on its own")
{
    Subscriptions subs;

    CHECK (subs.connectionCount() == 0);
    CHECK (subs.totalSubscriptions() == 0);

    CHECK (subs.listen ("ws:127.0.0.1:1", "/a"));
    CHECK_FALSE (subs.listen ("ws:127.0.0.1:1", "/a"));     // already listening
    CHECK (subs.listen ("ws:127.0.0.1:1", "/b"));
    CHECK (subs.listen ("ws:127.0.0.1:2", "/a"));

    CHECK (subs.connectionCount() == 2);
    CHECK (subs.totalSubscriptions() == 3);

    /*  Sorted, and that is not cosmetic: these pushes are recorded in the event
        log, and a log that has to replay byte for byte cannot have its order
        decided by a hash map's iteration. */
    CHECK (subs.listenersOf ("/a")
             == std::vector<std::string> { "ws:127.0.0.1:1", "ws:127.0.0.1:2" });
    CHECK (subs.listenersOf ("/b") == std::vector<std::string> { "ws:127.0.0.1:1" });
    CHECK (subs.listenersOf ("/nobody").empty());

    CHECK (subs.isListening ("ws:127.0.0.1:1", "/a"));
    CHECK_FALSE (subs.isListening ("ws:127.0.0.1:2", "/b"));

    CHECK (subs.heldBy ("ws:127.0.0.1:1") == std::vector<std::string> { "/a", "/b" });

    CHECK (subs.ignore ("ws:127.0.0.1:1", "/a"));
    CHECK_FALSE (subs.ignore ("ws:127.0.0.1:1", "/a"));     // not listening now
    CHECK_FALSE (subs.ignore ("ws:127.0.0.1:9", "/a"));     // never was

    //  A connection listening to nothing is not a connection.
    CHECK (subs.ignore ("ws:127.0.0.1:2", "/a"));
    CHECK (subs.connectionCount() == 1);

    const auto had = subs.drop ("ws:127.0.0.1:1");
    CHECK (had == std::vector<std::string> { "/b" });
    CHECK (subs.connectionCount() == 0);
    CHECK (subs.drop ("ws:127.0.0.1:1").empty());           // gone already
}
