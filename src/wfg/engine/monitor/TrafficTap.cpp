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

#include <wfg/engine/monitor/TrafficTap.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>

namespace wfg::monitor
{
    namespace
    {
        static_assert ((TrafficTap::capacity & (TrafficTap::capacity - 1)) == 0,
                       "the ring's capacity is a power of two, so a position masks into a slot");

        constexpr std::size_t mask = TrafficTap::capacity - 1;
    }

    PeerText peerText (std::string_view host, int port) noexcept
    {
        PeerText out;

        //  Room for the colon and five digits after however much of the host fits.
        const auto hostLength = std::min (host.size(), maxPeer - 7);
        std::memcpy (out.text, host.data(), hostLength);
        out.length = hostLength;
        out.text[out.length++] = ':';

        const auto written = std::to_chars (out.text + out.length, out.text + maxPeer, port);

        if (written.ec == std::errc())
            out.length = static_cast<std::size_t> (written.ptr - out.text);

        return out;
    }

    TrafficTap::TrafficTap()
        : slots (std::make_unique<Slot[]> (capacity))
    {
        //  Each slot starts expecting the write at its own position.
        for (std::size_t at = 0; at < capacity; ++at)
            slots[at].sequence.store (at, std::memory_order_relaxed);
    }

    TrafficTap::~TrafficTap() = default;

    void TrafficTap::setListening (bool on) noexcept
    {
        listening.store (on, std::memory_order_relaxed);
    }

    bool TrafficTap::record (Direction direction, Medium medium, Road road, std::string_view peer,
                             const std::uint8_t* data, std::size_t size) noexcept
    {
        if (! listening.load (std::memory_order_relaxed))
            return false;

        /*  CLAIM A SLOT: the one at the write position, if the reader has
            finished with it. Another writer taking it first only moves this one
            on to the next; a slot the reader has not emptied yet means the ring
            is full, and this capture is dropped rather than waited for. */
        auto position = writeAt.load (std::memory_order_relaxed);
        Slot* slot = nullptr;

        for (;;)
        {
            slot = &slots[position & mask];

            const auto sequence = slot->sequence.load (std::memory_order_acquire);

            if (sequence == position)
            {
                if (writeAt.compare_exchange_weak (position, position + 1, std::memory_order_relaxed))
                    break;
            }
            else if (sequence < position)
            {
                lost.fetch_add (1, std::memory_order_relaxed);
                return false;
            }
            else
            {
                position = writeAt.load (std::memory_order_relaxed);
            }
        }

        auto& capture = slot->capture;

        capture.wallMicros = std::chrono::duration_cast<std::chrono::microseconds> (
                                 std::chrono::system_clock::now().time_since_epoch()).count();
        capture.direction = direction;
        capture.medium = medium;
        capture.road = road;
        capture.fullSize = static_cast<std::uint32_t> (std::min<std::size_t> (size, 0xFFFFFFFFu));
        capture.size = static_cast<std::uint16_t> (std::min (size, maxBytes));
        capture.peerLength = static_cast<std::uint8_t> (std::min (peer.size(), maxPeer));

        if (capture.size > 0 && data != nullptr)
            std::memcpy (capture.bytes, data, capture.size);

        if (capture.peerLength > 0)
            std::memcpy (capture.peer, peer.data(), capture.peerLength);

        //  Handed to the reader: the sequence says the slot is full.
        slot->sequence.store (position + 1, std::memory_order_release);
        return true;
    }

    std::size_t TrafficTap::drain (std::vector<Capture>& into, std::size_t most)
    {
        std::size_t taken = 0;
        auto position = readAt.load (std::memory_order_relaxed);

        while (taken < most)
        {
            auto& slot = slots[position & mask];

            //  Not written yet, or still being written: everything before it has been taken.
            if (slot.sequence.load (std::memory_order_acquire) != position + 1)
                break;

            into.push_back (slot.capture);

            //  Free again, for the write a whole ring later.
            slot.sequence.store (position + capacity, std::memory_order_release);
            ++position;
            ++taken;
        }

        readAt.store (position, std::memory_order_relaxed);
        return taken;
    }
}
