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
    WHERE THE APP'S PLUGIN SCAN IS (2026-09-26, the author's decision: a Scan
    button in Show settings, Plugins).

    TWO HALVES, WRITTEN FROM TWO PLACES, and the split is what keeps a replay
    honest. Whether a scan is running is the COMMANDS' to say: `plugin.scan`
    begins one and `plugin.scanned` ends it, both logged, so a replay - which
    launches nothing - still refuses a second `plugin.scan` exactly where the
    session did. How far it has got - the file, "12 of 140" - is the machine's,
    read from the scan's progress file by the job on the message thread, and
    published for whoever is watching; nothing is decided on it.

    The PluginTable's shape: a mutex, because the tick thread (the commands,
    the tree) and the message thread (the job) both touch it, and a revision
    for readers that want to know whether to look again. Nothing here is on
    the audio thread. Names no JUCE type.
*/

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::plugin
{
    struct ScanReading
    {
        /** `idle` before any scan, `scanning`, `finished`, `failed`. */
        std::string state = "idle";

        /** The show's word for the format asked for, empty for every format. */
        std::string format;

        /** The file under scan, of this scan's `total`, `done` so far. */
        std::string file;
        int done = 0;
        int total = 0;

        /** What this machine knows once the scan is over, or so far. */
        int found = 0;

        /** How many files this scan gave up on. */
        int skipped = 0;

        /** Why the scan failed, in one sentence; empty when it did not. */
        std::string problem;
    };

    class ScanTable
    {
    public:
        bool scanning() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return held.state == "scanning";
        }

        /** `plugin.scan`, applied: what was there is let go of. */
        void begin (const std::string& formatWord)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            held = {};
            held.state = "scanning";
            held.format = formatWord;
            ++revisionCount;
        }

        /** The job, from the scan's progress file - taken only while a scan runs. */
        void progress (const std::string& file, int done, int total, int found, int skipped)
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (held.state != "scanning")
                return;

            held.file = file;
            held.done = done;
            held.total = total;
            held.found = found;
            held.skipped = skipped;
            ++revisionCount;
        }

        /** `plugin.scanned`, applied: finished, or failed with its sentence. */
        void end (int found, int skipped, const std::string& problem)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            held.state = problem.empty() ? "finished" : "failed";
            held.file.clear();
            held.found = found;
            held.skipped = skipped;
            held.problem = problem;
            ++revisionCount;
        }

        ScanReading reading() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return held;
        }

        std::uint64_t revision() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return revisionCount;
        }

    private:
        mutable std::mutex mutex;
        ScanReading held;
        std::uint64_t revisionCount = 0;
    };
}
