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

#include <wfg/engine/tree/PresetScanner.h>

#include <utility>

namespace wfg::tree
{
    PresetScanner::~PresetScanner()
    {
        stop();
    }

    bool PresetScanner::start()
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);

        thread = std::thread ([this] { run(); });
        return true;
    }

    void PresetScanner::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        //  Under the lock, so the thread cannot miss the wake between its test and its wait.
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping.store (true, std::memory_order_relaxed);
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);
    }

    void PresetScanner::rescan()
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            rescanWanted = true;
        }

        wake.notify_one();
    }

    std::optional<std::vector<Preset>> PresetScanner::take()
    {
        if (! ready.load (std::memory_order_acquire))
            return std::nullopt;

        const std::lock_guard<std::mutex> lock { guard };
        ready.store (false, std::memory_order_relaxed);
        return std::exchange (finished, std::nullopt);
    }

    PresetScanner::Status PresetScanner::status() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return current;
    }

    void PresetScanner::setStatus (Status now)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            current = std::move (now);
        }

        statusStamp.fetch_add (1, std::memory_order_acq_rel);
    }

    void PresetScanner::run()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed) || rescanWanted; });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                rescanWanted = false;
            }

            /*  Outside the lock: this is the part that touches the disk. The
                quick read, as serve's start reads, so the rows say the same
                thing whichever read made them. */
            auto read = readPresetFolder (yoursPath, true, "yours");

            {
                const std::lock_guard<std::mutex> lock { guard };
                finished = std::move (read);
            }

            ready.store (true, std::memory_order_release);
        }
    }
}
