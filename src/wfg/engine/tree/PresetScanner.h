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

    AND IT WRITES YOUR PRESETS (namespace draft §76, ARY): `preset.save` decides
    on the tick what to save and as what, and the file is read, given its
    header and written here - the disk again - then your folder is read again
    so the menu offers it. What a save said is in `status`.

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
    /*  ONE PRESET TO SAVE (§76, ARY), as `preset.save` decided it on the tick:
        which file, under which name, and what the device's rows say about it.
        `vendor` and `model` empty keep the file's own, else the device's name;
        `knownVersion` is the highest version of `slug` this machine holds,
        yours or shipped, and the saved one is one more than it or than the
        file's own, whichever is higher. */
    struct PresetSave
    {
        std::string source {};        ///< the device's description, absolute
        std::string slug {};
        std::string vendor {};
        std::string model {};
        std::string deviceName {};
        std::string host {};
        std::string transport = "udp";
        std::string wire = "osc";
        std::string framing = "length";
        std::string readback {};
        int port = 0;
        int queryPort = 0;
        std::string fromPreset {};    ///< `<slug>@<version>` the device was made from, or empty
        int knownVersion = 0;
        std::string date {};          ///< YYYY-MM-DD, for the SOURCES line
    };

    /*  THE SAVE ITSELF, without the thread: the file read, the header made and
        put in, the text checked as a preset and written to `folder/<slug>.json`.
        Empty when it worked, else the sentence. Public for the tests. */
    std::string savePresetNow (const PresetSave& save, const std::string& folderPath, std::string& savedAs);

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

        /** Any thread: saves a preset to your folder, then reads it again. */
        void save (PresetSave job);

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
        std::vector<PresetSave> saves;

        std::optional<std::vector<Preset>> finished;
        std::atomic<bool> ready { false };

        Status current;
        std::atomic<std::uint64_t> statusStamp { 0 };

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };

        std::thread thread;
    };
}
