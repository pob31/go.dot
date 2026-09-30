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
    WHAT CROSSED THE WIRE, kept for a window to show: every OSC message and
    every MIDI message the process received or sent, as the bytes that went
    (author, 2026-09-30: "Should we add a network monitor similar to the one in
    WFS-DIY?" - OSC and MIDI, in and out).

    A RING WITH A FIXED NUMBER OF FIXED-SIZE SLOTS, filled by whatever thread
    the traffic is on and emptied by the one window reading it - and NOT the
    mutex `EventQueue` argues for. The argument there is that dropping an
    operator's GO is worse than waiting a microsecond; here it runs the other
    way. A line in a monitor may be lost to a burst and nobody is harmed, while
    a sender or a receiver made to wait for a window would be the monitor
    reaching into the GO path (PRD §4.1). So a full ring refuses and COUNTS,
    and the window says how many it did not see.

    NOTHING WHEN NOBODY IS LOOKING. `record` is one relaxed atomic load and a
    return while no window listens, which is every show that is running rather
    than being debugged. Listening, it copies at most `maxBytes` of the packet
    and the peer's name into a slot: no allocation, no lock, no formatting.
    What the bytes MEAN is decoded on the reader's side, with the same codec
    the engine reads them with, on the window's time.

    ANY NUMBER OF WRITERS, ONE READER: the UDP receiver, the tick thread's
    sends, the web page's socket, the MIDI input callback and the MIDI sending
    thread all record; the window alone drains. The ring is Dmitry Vyukov's
    bounded queue - one sequence number per slot - which never makes a writer
    wait on another writer or on the reader.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace wfg::monitor
{
    enum class Direction : std::uint8_t { in, out };
    enum class Medium : std::uint8_t { osc, midi };

    /*  WHICH ROAD IT TOOK: the UDP socket every device and surface uses, the
        web page's own socket (the page is a client like any other, and a busy
        one), or a MIDI port. */
    enum class Road : std::uint8_t { udp, page, midi };

    /** How much of one packet is kept: enough for any cue's message, cut after that. */
    inline constexpr std::size_t maxBytes = 448;

    /** How long a peer's name may be: an address and port, or a MIDI port's name. */
    inline constexpr std::size_t maxPeer = 64;

    struct Capture
    {
        std::int64_t wallMicros = 0;        ///< when, as microseconds of the system clock
        Direction direction = Direction::in;
        Medium medium = Medium::osc;
        Road road = Road::udp;
        std::uint32_t fullSize = 0;         ///< how long it was on the wire
        std::uint16_t size = 0;             ///< how much of it is in `bytes`
        std::uint8_t peerLength = 0;
        char peer[maxPeer] {};
        std::uint8_t bytes[maxBytes] {};

        bool truncated() const noexcept { return fullSize > size; }
        std::string_view peerName() const noexcept { return { peer, peerLength }; }
    };

    /*  "HOST:PORT" IN A FIXED BUFFER, for a sender on the tick thread, which
        should not allocate a string to say where a datagram went. */
    struct PeerText
    {
        char text[maxPeer] {};
        std::size_t length = 0;

        std::string_view view() const noexcept { return { text, length }; }
    };

    PeerText peerText (std::string_view host, int port) noexcept;

    class TrafficTap
    {
    public:
        /** A power of two: 2048 slots, a megabyte, allocated once and never again. */
        static constexpr std::size_t capacity = 2048;

        TrafficTap();
        ~TrafficTap();

        TrafficTap (const TrafficTap&) = delete;
        TrafficTap& operator= (const TrafficTap&) = delete;

        /*  WHETHER ANYBODY IS LOOKING: the window sets it while it is open and
            recording, and clears it when it closes or pauses. */
        void setListening (bool listening) noexcept;
        bool isListening() const noexcept { return listening.load (std::memory_order_relaxed); }

        /*  ANY THREAD. Never blocks and never allocates. False when nobody is
            listening, or when the ring is full - the second counted in
            `dropped`, since that is a line the window will not show. */
        bool record (Direction direction, Medium medium, Road road, std::string_view peer,
                     const std::uint8_t* data, std::size_t size) noexcept;

        /*  THE ONE READER. Appends what has arrived, oldest first, at most
            `most` of them, and answers how many. */
        std::size_t drain (std::vector<Capture>& into, std::size_t most = capacity);

        /** How many captures were refused for want of room since the tap was made. */
        std::uint64_t dropped() const noexcept { return lost.load (std::memory_order_relaxed); }

    private:
        struct Slot
        {
            std::atomic<std::size_t> sequence { 0 };
            Capture capture;
        };

        std::unique_ptr<Slot[]> slots;
        std::atomic<std::size_t> writeAt { 0 };
        std::atomic<std::size_t> readAt { 0 };
        std::atomic<bool> listening { false };
        std::atomic<std::uint64_t> lost { 0 };
    };
}
