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

#include <wfg/engine/command/EventQueue.h>

namespace wfg
{
    namespace
    {
        /*  Drops the oldest entry when the queue is at capacity. Oldest rather
            than newest: in a control stream the newest value is the one that
            matters, and an operator whose GO is the newest thing in a flooded
            queue must not be the one who loses it. */
        template <typename T>
        bool pushBounded (std::vector<Entry>& pending, std::size_t capacity,
                          std::uint64_t& dropped, T&& item)
        {
            bool madeRoom = false;

            if (pending.size() >= capacity)
            {
                pending.erase (pending.begin());
                ++dropped;
                madeRoom = true;
            }

            pending.emplace_back (std::forward<T> (item));
            return ! madeRoom;
        }
    }

    bool EventQueue::submit (Event event)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return pushBounded (pending, capacity, dropped, std::move (event));
    }

    bool EventQueue::submit (Drop drop)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return pushBounded (pending, capacity, dropped, std::move (drop));
    }

    void EventQueue::drainInto (std::vector<Entry>& out)
    {
        out.clear();

        const std::lock_guard<std::mutex> lock (mutex);
        out.swap (pending);
    }

    std::uint64_t EventQueue::droppedCount() const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return dropped;
    }

    std::size_t EventQueue::size() const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return pending.size();
    }
}
