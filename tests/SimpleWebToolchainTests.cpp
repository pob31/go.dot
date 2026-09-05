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
    Does juce_simpleweb work, on this platform, in THIS build?

    Nothing here tests Go.dot. It tests the toolchain: that a third submodule
    with a nested one of its own, an inlined build recipe, a stubbed
    JuceHeader.h and an emptied link-libraries property actually produce a
    server that serves. PR 1.D exists to prove that in isolation, so that when
    the OSCQuery server lands in PR 1.9 a failure means the OSCQuery server is
    wrong - and not that the module was never really working on macOS.

    THREE THINGS ARE CHECKED, and each one is a way the wiring could be silently
    half-done:

      * the TLS-off define reached OUR translation units, not merely the
        module's. Compiled, not asserted at runtime: if SIMPLEWEB_SECURE_SUPPORTED
        arrives undefined or as 1, this file does not build.
      * an HTTP GET on the loopback returns what the handler wrote. That is the
        whole of what OSCQuery's `GET /` will be.
      * a WebSocket message goes to the server and a reply comes back on the
        same port. One port for both is the reason this module is here at all.

    THE `deps.no-openssl` CTEST IS THE FOURTH CHECK and it deliberately lives in
    tests/CMakeLists.txt rather than here: it inspects the SHIPPED BINARY's
    dynamic dependencies, which is a question about the link line and not about
    anything a C++ test can observe from inside its own process.

    THE OTHER NEW SUBMODULE IS CHECKED IN SpatcoreToolchainTests.cpp, in a
    translation unit of its own, and it has to be. spatcore's
    RtThreadPriority.h includes <windows.h>; asio, which arrives here through
    juce_simpleweb, includes <winsock2.h>. The two do not coexist - windows.h
    pulls in winsock.h, which redefines what winsock2.h has already declared -
    and the failure is a wall of redefinition errors on one platform only.
    Keeping them in separate files is the whole mitigation.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <juce_core/juce_core.h>
#include <juce_simpleweb/juce_simpleweb.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
    The TLS-off define, checked at COMPILE time.

    wfg::deps sets SIMPLEWEB_SECURE_SUPPORTED=0 so that the module never
    compiles asio's OpenSSL paths. If that define stopped reaching our TUs, the
    symptom in PR 1.9 would be an unresolved SSL symbol at link time on one
    platform, with nothing pointing at the cause. This turns it into a message
    naming the thing that is wrong.
*/
#ifndef SIMPLEWEB_SECURE_SUPPORTED
 #error "SIMPLEWEB_SECURE_SUPPORTED did not reach this translation unit - see wfg::deps in cmake/WfgThirdParty.cmake"
#endif

static_assert (SIMPLEWEB_SECURE_SUPPORTED == 0,
               "Go.dot builds juce_simpleweb with TLS off and ships no OpenSSL; "
               "see cmake/WfgThirdParty.cmake section 2b");

/*
    And the define that lets asio compile as C++20 at all.

    std::result_of was removed in C++20. asio still reaches for it and gates its
    own switch to std::invoke_result on `defined(ASIO_MSVC)` rather than on the
    language version, so clang and GCC never take that branch on their own. The
    first PR 1.D run proved the asymmetry: Windows green, macOS a wall of "no
    template named 'result_of'" from inside asio, on identical source.

    Guarded here as well as set in wfg::deps because the two failures look
    nothing alike. Losing the define is a hundred errors deep inside a vendor
    header; this is one line naming the cause.
*/
#ifndef ASIO_HAS_STD_INVOKE_RESULT
 #error "ASIO_HAS_STD_INVOKE_RESULT did not reach this TU - asio will use std::result_of, which C++20 removed. See cmake/WfgThirdParty.cmake"
#endif

static_assert (ASIO_HAS_STD_INVOKE_RESULT == 1,
               "asio must use std::invoke_result: std::result_of is gone in C++20, "
               "and TE requires C++20");

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

    /** Answers every GET with a fixed body, which is all OSCQuery's shape needs. */
    struct FixedResponse final : public SimpleWebSocketServer::RequestHandler
    {
        juce::String body { "{\"NAME\":\"Go.dot\"}" };

        bool handleHTTPRequest (std::shared_ptr<HttpServer::Response> response,
                                std::shared_ptr<HttpServer::Request> request) override
        {
            if (request->method != "GET")
                return false;

            response->write (SimpleWeb::StatusCode::success_ok, body.toStdString(),
                             { { "Content-Type", "application/json" } });
            return true;
        }
    };

    /** Echoes every text frame back with a prefix, so a reply proves a round trip. */
    struct EchoingServer final : public SimpleWebSocketServer::Listener
    {
        SimpleWebSocketServer server;

        std::mutex mutex;
        std::vector<juce::String> received;
        std::atomic<int> opened { 0 };

        EchoingServer() { server.addWebSocketListener (this); }
        ~EchoingServer() override { server.removeWebSocketListener (this); server.stop(); }

        void connectionOpened (const juce::String&) override { ++opened; }

        void messageReceived (const juce::String& id, const juce::String& message) override
        {
            {
                const std::lock_guard<std::mutex> lock { mutex };
                received.push_back (message);
            }

            server.sendTo ("echo:" + message, id);
        }

        std::size_t count()
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return received.size();
        }
    };

    struct RecordingClient final : public SimpleWebSocketClient::Listener
    {
        SimpleWebSocketClient client;

        std::mutex mutex;
        std::vector<juce::String> received;
        std::atomic<bool> open { false };

        RecordingClient() { client.addWebSocketListener (this); }
        ~RecordingClient() override { client.removeWebSocketListener (this); client.stop(); }

        void connectionOpened() override { open = true; }

        void messageReceived (const juce::String& message) override
        {
            const std::lock_guard<std::mutex> lock { mutex };
            received.push_back (message);
        }

        std::size_t count()
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return received.size();
        }
    };
}

//==============================================================================
TEST_CASE ("simpleweb: an HTTP GET on the loopback returns what the handler wrote")
{
    EchoingServer rig;
    FixedResponse handler;
    rig.server.addHTTPRequestHandler (&handler);

    rig.server.start (0);

    REQUIRE (waitUntil ([&rig] { return rig.server.isConnected; }));

    const auto url = juce::URL ("http://127.0.0.1:" + juce::String (rig.server.port) + "/");

    auto stream = url.createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (10000));

    REQUIRE (stream != nullptr);

    const auto body = stream->readEntireStreamAsString();

    rig.server.removeHTTPRequestHandler (&handler);
    rig.server.stop();

    CHECK (body == handler.body);
}

TEST_CASE ("simpleweb: start(0) binds an ephemeral port and says which one")
{
    /*  This test used to assert the opposite, and the flip is the point.

        juce_simpleweb's start callback receives the port asio actually bound
        and compared it against the one the caller had asked for:

            isConnected = port == _port;

        For a fixed port the two agree. For port 0 they never do, so a server
        that was listening perfectly well reported isConnected == false for the
        rest of its life, and the one number needed to reach it was discarded.
        PR 1.D shipped with that behaviour pinned by a characterisation test and
        a note saying it would go red the day somebody fixed it.

        Fixed in the fork at b72ec94 - `port = _port; isConnected = true;`,
        which is what the comparison already meant for a fixed port and the only
        way to learn the answer for an ephemeral one. This is now the test that
        the fix is really in the pinned submodule, so a bad re-pin fails here
        rather than in PR 1.10's harness.

        It matters beyond tests: `wfg serve --http-port=0` prints its bound port
        for the black-box driver, and binding 0 is how two Go.dot instances
        coexist on one machine. */
    EchoingServer rig;

    rig.server.start (0);

    REQUIRE (waitUntil ([&rig] { return rig.server.isConnected; }));

    const auto granted = rig.server.port;

    INFO ("granted port: " << granted);
    CHECK (granted > 0);
    CHECK (granted != 0);

    //  And it is reachable at the port it reported, which is the whole claim.
    FixedResponse handler;
    rig.server.addHTTPRequestHandler (&handler);

    const auto url = juce::URL ("http://127.0.0.1:" + juce::String (granted) + "/");

    auto stream = url.createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (10000));

    REQUIRE (stream != nullptr);
    const auto body = stream->readEntireStreamAsString();

    rig.server.removeHTTPRequestHandler (&handler);
    rig.server.stop();

    CHECK (body == handler.body);
}

TEST_CASE ("simpleweb: a WebSocket message goes out and a reply comes back, on the same port")
{
    /*  ONE PORT FOR BOTH is the whole reason this module is a dependency.
        OSCQuery puts HTTP and WebSocket on the same port, and juce's own
        StreamingSocket cannot be talked into serving both. */
    EchoingServer rig;
    rig.server.start (0);

    REQUIRE (waitUntil ([&rig] { return rig.server.isConnected; }));

    RecordingClient client;
    client.client.start ("127.0.0.1:" + juce::String (rig.server.port));

    REQUIRE (waitUntil ([&client] { return client.open.load(); }));
    REQUIRE (waitUntil ([&rig] { return rig.opened.load() > 0; }));

    client.client.send (juce::String ("/godot/cmd/standby/next"));

    REQUIRE (waitUntil ([&rig] { return rig.count() > 0; }));
    REQUIRE (waitUntil ([&client] { return client.count() > 0; }));

    client.client.stop();
    rig.server.stop();

    {
        const std::lock_guard<std::mutex> lock { rig.mutex };
        REQUIRE (rig.received.size() == 1);
        CHECK (rig.received[0] == "/godot/cmd/standby/next");
    }

    const std::lock_guard<std::mutex> lock { client.mutex };
    REQUIRE (client.received.size() == 1);
    CHECK (client.received[0] == "echo:/godot/cmd/standby/next");
}
