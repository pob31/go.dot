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
    The one road into the engine: many producers, one consumer.

    Producers are the OSCQuery server threads, the UDP receiver, the command
    line and the replay tool. The consumer is the tick thread, which swaps the
    whole queue out once per tick and applies what it finds, in arrival order.

    A mutex, not a lock-free ring, and that is a deliberate choice rather than a
    placeholder. Nothing that touches this queue is real-time: PRD 4.2's lipogram
    binds the audio callback, and the audio callback's only contact with the
    control plane is one atomic add on a sample counter (see clock/). The GO path
    must not BLOCK (4.1), which this satisfies - a submit takes the lock for the
    length of a move and a push_back, never for the length of a tick.

    What a lock-free ring would buy is bounded worst-case latency under
    contention; what it would cost is a fixed capacity, and dropping an
    operator's GO because a burst of tablet traffic filled a ring is a worse
    failure than waiting a microsecond for a mutex.

    Bounded, all the same: a flood from a misbehaving client must not grow the
    queue without limit. Over the cap, the OLDEST pending events are dropped and
    counted, because in a control stream the newest values are the ones that
    matter, and the drop is visible at /godot/engine/errorCount rather than
    silent.
*/

#include <wfg/engine/command/Event.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace wfg
{
    class EventQueue
    {
    public:
        /*  4096 events is about eighty ticks' worth of a 50 Hz stream from every
            client a booth is likely to have. A queue longer than that means the
            tick thread has stopped, and no amount of buffering will save it. */
        static constexpr std::size_t defaultCapacity = 4096;

        explicit EventQueue (std::size_t capacityToUse = defaultCapacity)
            : capacity (capacityToUse) {}

        /** Any thread. Returns false if the queue was full and an older event
            had to be dropped to make room. */
        bool submit (Event event);

        /** Any thread. A packet that never became an event; see Drop. */
        bool submit (Drop drop);

        /*  Tick thread. Moves everything pending into `out` (which is cleared
            first) and leaves the queue empty. One lock per tick, whatever the
            traffic. */
        void drainInto (std::vector<Entry>& out);

        /** How many submissions have been dropped for want of room, ever. */
        std::uint64_t droppedCount() const;

        /** Approximate; for diagnostics only. */
        std::size_t size() const;

    private:
        mutable std::mutex mutex;
        std::vector<Entry> pending;
        std::size_t capacity;
        std::uint64_t dropped = 0;
    };
}
