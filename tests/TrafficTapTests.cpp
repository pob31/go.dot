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
    THE NETWORK MONITOR'S TAP (author, 2026-09-30): what crossed the wire, kept
    in a ring any thread writes and one window drains. What is asserted is the
    promise that lets it sit on the GO path's roads at all - nothing kept while
    nobody listens, nothing waited for when the ring is full, and every capture
    either kept or counted - and that the UDP socket really hands it both
    directions.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/monitor/TrafficTap.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    bool keep (monitor::TrafficTap& tap, std::uint8_t value)
    {
        const std::uint8_t bytes[] { value, 1, 2 };
        return tap.record (monitor::Direction::in, monitor::Medium::midi, monitor::Road::midi,
                           "Pads", bytes, sizeof bytes);
    }
}

TEST_CASE ("traffic tap: nothing is kept while nobody listens, and what is kept comes out in order")
{
    monitor::TrafficTap tap;

    CHECK_FALSE (tap.isListening());
    CHECK_FALSE (keep (tap, 0x90));

    std::vector<monitor::Capture> out;
    CHECK (tap.drain (out) == 0u);
    CHECK (tap.dropped() == 0u);       // not listening is not a loss

    tap.setListening (true);

    for (std::uint8_t n = 0; n < 5; ++n)
        CHECK (keep (tap, n));

    REQUIRE (tap.drain (out) == 5u);

    for (std::uint8_t n = 0; n < 5; ++n)
    {
        CHECK (out[n].bytes[0] == n);
        CHECK (out[n].size == 3u);
        CHECK (out[n].peerName() == "Pads");
        CHECK (out[n].direction == monitor::Direction::in);
        CHECK (out[n].medium == monitor::Medium::midi);
        CHECK (out[n].wallMicros > 0);
    }

    //  Drained is gone.
    out.clear();
    CHECK (tap.drain (out) == 0u);
}

TEST_CASE ("traffic tap: a full ring refuses and counts rather than waiting, and a long packet is cut and says so")
{
    monitor::TrafficTap tap;
    tap.setListening (true);

    for (std::size_t n = 0; n < monitor::TrafficTap::capacity; ++n)
        REQUIRE (keep (tap, static_cast<std::uint8_t> (n)));

    CHECK_FALSE (keep (tap, 0xFF));
    CHECK_FALSE (keep (tap, 0xFF));
    CHECK (tap.dropped() == 2u);

    //  Room again once the reader has taken some.
    std::vector<monitor::Capture> out;
    CHECK (tap.drain (out, 10) == 10u);
    CHECK (keep (tap, 0x80));

    //  Cut at maxBytes, the full length remembered.
    monitor::TrafficTap another;
    another.setListening (true);

    const std::vector<std::uint8_t> long_ (monitor::maxBytes + 100, 0x42);
    REQUIRE (another.record (monitor::Direction::out, monitor::Medium::osc, monitor::Road::udp,
                             std::string (200, 'x'), long_.data(), long_.size()));

    std::vector<monitor::Capture> cut;
    REQUIRE (another.drain (cut) == 1u);
    CHECK (cut[0].size == monitor::maxBytes);
    CHECK (cut[0].fullSize == long_.size());
    CHECK (cut[0].truncated());
    CHECK (cut[0].peerName().size() == monitor::maxPeer);
}

TEST_CASE ("traffic tap: many writers at once lose nothing that is not counted")
{
    monitor::TrafficTap tap;
    tap.setListening (true);

    constexpr int writers = 4;
    constexpr int each = 3000;

    std::atomic<bool> go { false };
    std::atomic<int> kept { 0 };
    std::vector<std::thread> threads;

    for (int w = 0; w < writers; ++w)
        threads.emplace_back ([&tap, &go, &kept, w]
        {
            while (! go.load())
                std::this_thread::yield();

            for (int n = 0; n < each; ++n)
                if (keep (tap, static_cast<std::uint8_t> (w)))
                    kept.fetch_add (1);
        });

    std::vector<monitor::Capture> out;
    go.store (true);

    //  The reader drains while they write, as the window does.
    auto drained = std::size_t { 0 };

    for (int pass = 0; pass < 2000 && drained + tap.dropped() < static_cast<std::size_t> (writers * each); ++pass)
    {
        drained += tap.drain (out);
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }

    for (auto& thread : threads)
        thread.join();

    drained += tap.drain (out);

    //  Every capture is either in `out` or counted as dropped, and none twice.
    CHECK (drained == static_cast<std::size_t> (kept.load()));
    CHECK (drained + tap.dropped() == static_cast<std::size_t> (writers * each));
    CHECK (out.size() == drained);

    //  And every capture kept is one a writer made.
    for (const auto& capture : out)
        CHECK (capture.bytes[0] < writers);
}

TEST_CASE ("traffic tap: a peer is written without allocating, host then port")
{
    CHECK (monitor::peerText ("192.168.1.20", 9000).view() == "192.168.1.20:9000");
    CHECK (monitor::peerText ("", 0).view() == ":0");

    //  A host too long for the buffer keeps its port.
    const auto long_ = monitor::peerText (std::string (200, 'h'), 65535).view();
    CHECK (long_.size() <= monitor::maxPeer);
    CHECK (long_.substr (long_.size() - 6) == ":65535");
}

TEST_CASE ("traffic tap: the UDP socket shows it both what arrives and what it sends")
{
    monitor::TrafficTap tap;
    tap.setListening (true);

    std::atomic<int> arrived { 0 };
    osc::UdpEndpoint socket;
    socket.setTap (&tap);
    REQUIRE (socket.start (0, [&arrived] (osc::Datagram) { arrived.fetch_add (1); }));

    std::string error;
    const auto bytes = osc::encode (osc::Packet::message ("/desk/fader", { osc::Value::float32 (0.75f) }), error);
    REQUIRE (bytes.has_value());

    REQUIRE (socket.send ("127.0.0.1", socket.boundPort(), *bytes));

    for (int wait = 0; wait < 200 && arrived.load() == 0; ++wait)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    REQUIRE (arrived.load() == 1);

    std::vector<monitor::Capture> out;
    REQUIRE (tap.drain (out) == 2u);

    /*  One out and one in, in whichever order the two threads reached the
        ring: the send records as it returns, the receive as it reads. */
    const auto& sent = out[0].direction == monitor::Direction::out ? out[0] : out[1];
    const auto& got = out[0].direction == monitor::Direction::out ? out[1] : out[0];

    CHECK (sent.direction == monitor::Direction::out);
    CHECK (sent.peerName() == "127.0.0.1:" + std::to_string (socket.boundPort()));
    CHECK (got.direction == monitor::Direction::in);
    CHECK (got.road == monitor::Road::udp);

    for (const auto& capture : out)
    {
        const auto decoded = osc::decode (capture.bytes, capture.size);
        REQUIRE (decoded.ok);
        CHECK (decoded.packet.address == "/desk/fader");
    }

    //  Not listening, the same traffic keeps nothing.
    tap.setListening (false);
    REQUIRE (socket.send ("127.0.0.1", socket.boundPort(), *bytes));

    for (int wait = 0; wait < 200 && arrived.load() == 1; ++wait)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    out.clear();
    CHECK (tap.drain (out) == 0u);
}
