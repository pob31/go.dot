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
    THE THREAD THAT READS YOUR PRESETS AGAIN (namespace draft §76, ARZ).

    Your folder - `Documents/Go.dot/Device presets` unless serve was told
    another - is read at the start beside the shipped one, and again only when
    somebody asks: the Network tab shown, its PRESET menu opened, a preset just
    saved, `preset.rescan`. Never on a timer. The shipped folder is not read
    again at all: it changes only with the program.

    A FILE READ IS DISK TIME AND A PARSE, and the tick does neither: the reading
    happens here, and what was read waits until the tick takes it (`take`), on
    which the table swaps your part in (`PresetTable::adoptYours`). Two asks
    before a reading starts are one reading; an ask during one is another
    after it, so a file saved while the folder was being read is never missed.

    IT NEVER TOUCHES THE TABLE. It holds a folder and hands over presets.
*/

#include <wfg/engine/tree/PresetTable.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wfg::tree
{
    class PresetScanner
    {
    public:
        PresetScanner() = default;
        ~PresetScanner();

        PresetScanner (const PresetScanner&) = delete;
        PresetScanner& operator= (const PresetScanner&) = delete;

        /*  Your folder, absolute. Set before `start`, and never after: the
            thread reads it without a lock. */
        void setFolder (std::string folderPath) { yoursPath = std::move (folderPath); }
        const std::string& folder() const noexcept { return yoursPath; }

        /** Starts the thread. Idempotent; false if it was already running. */
        bool start();

        /** Stops it and joins. The destructor calls it. */
        void stop();

        /** Any thread: reads your folder again, soon. */
        void rescan();

        /*  Tick thread: the latest reading, once, or nothing when none has
            finished since the last take. One relaxed load when there is
            nothing, which is every tick but a handful. */
        std::optional<std::vector<Preset>> take();

        /*  WHAT THE LAST SAVE OR READING SAID, for `network/presetsProblem`
            and `network/presetsSaved`: a sentence when something failed, the
            `<slug>@<version>` last saved. `revision` moves with each change,
            so the tick copies it only then. */
        struct Status
        {
            std::string problem {};
            std::string saved {};
        };

        Status status() const;
        std::uint64_t statusRevision() const noexcept { return statusStamp.load (std::memory_order_acquire); }

    private:
        void run();
        void setStatus (Status now);

        std::string yoursPath;

        mutable std::mutex guard;
        std::condition_variable wake;
        bool rescanWanted = false;

        std::optional<std::vector<Preset>> finished;
        std::atomic<bool> ready { false };

        Status current;
        std::atomic<std::uint64_t> statusStamp { 0 };

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };

        std::thread thread;
    };
}
