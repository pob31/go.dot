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
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/oscquery/OscQueryServer.h>
#include <wfg/engine/oscquery/Subscriptions.h>

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <juce_core/juce_core.h>
#include <juce_simpleweb/juce_simpleweb.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

        return std::make_shared<const tree::TreeSnapshot> (7, nodes,
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
    /** One HTTP GET, returning the status and the body. */
    struct HttpReply
    {
        int status = 0;
        std::string body;
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
        fifteen lines and it tells the truth. */
    HttpReply get (int port, const std::string& target)
    {
        HttpReply reply;

        juce::StreamingSocket socket;

        if (! socket.connect ("127.0.0.1", port, 10000))
            return reply;

        const std::string request = "GET " + target + " HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";

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

        return reply;
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
