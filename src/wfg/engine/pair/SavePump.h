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
    `pairing.xml` WRITTEN OFF THE TICK THREAD (namespace draft §58, RD.4).

    The audio settings' shape (audio/SettingsPump.h): a pairing command changes
    the settings on the tick thread and posts what the file should hold; this
    writes it on the message thread, forty milliseconds later at most, and the
    work it was given - serve's - submits `pair.saved` with what came of it. A
    disk that takes its time takes the message thread's, never the show's.

    EVERY REQUEST WRITTEN, IN ORDER, each a whole copy of the settings, so each
    command is answered by its own `pair.saved` and the file ends as the last
    one left it. And what is still queued when serve ends is written as the
    pump goes - a role set a moment before Ctrl-C is still the role the next
    launch reads. (A process killed outright runs no destructor; the request
    was forty milliseconds from the disk.)
*/

#include <wfg/engine/pair/PairSettings.h>

#include <juce_events/juce_events.h>

#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace wfg::pair
{
    class SavePump final : private juce::Timer
    {
    public:
        using Work = std::function<void (const PairSettings&)>;

        explicit SavePump (Work work) : perform (std::move (work)) { startTimer (40); }

        ~SavePump() override
        {
            stopTimer();
            writeQueued();
        }

        SavePump (const SavePump&) = delete;
        SavePump& operator= (const SavePump&) = delete;

        /** The tick thread's: what the file should hold now. */
        void post (const PairSettings& settings)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            queued.push_back (settings);
        }

    private:
        void timerCallback() override { writeQueued(); }

        void writeQueued()
        {
            std::deque<PairSettings> work;

            {
                const std::lock_guard<std::mutex> lock (mutex);
                work.swap (queued);
            }

            for (const auto& settings : work)
                perform (settings);
        }

        Work perform;
        std::mutex mutex;
        std::deque<PairSettings> queued;
    };
}
