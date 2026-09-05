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
    The thread that asks, so that the tick thread never has to wait for an
    answer.

    WHY A THREAD AT ALL. An HTTP GET to a device that has gone away costs the
    whole of its timeout to find out - seconds, not milliseconds. The tick
    thread runs at 50 Hz and owns the model; a syscall with a deadline on it is
    a show that stops while a projector is being polite. So the asking happens
    here, and the ANSWER comes back the way everything else the machine learns
    comes back: as a command, applied on the tick it arrived, logged like any
    other (§3.15).

    THAT IS WHY THIS CLASS SUBMITS RATHER THAN RETURNS. A read-back is a state
    transition - somebody else's box said something - and the whole replay
    guarantee rests on those being events. `wfg replay` re-injects the read-back
    from the log and the verification comes out the same, on the same tick, with
    no network and no device in the room. A probe that handed its answer
    straight to the Runner would have made a verified cue the one thing in the
    engine that could not be replayed.

    ONE REQUEST PER ADDRESS IN FLIGHT. The Runner asks again every tick while a
    verified cue is waiting - fifty times a second - and a queue that took all
    of them would still be answering the first second's worth a minute later.
    A duplicate is dropped rather than queued, which turns "ask until it
    answers" into "keep one question outstanding".

    IT NEVER TOUCHES THE MODEL, the document, the mount table or the tree. It
    holds a host, a port and an address, and it speaks to an Engine. That is the
    whole of its contact with Go.dot.
*/

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace wfg
{
    class Engine;
}

namespace wfg::tree
{
    class MountProbe
    {
    public:
        explicit MountProbe (Engine& engineToReportTo) noexcept : engine (&engineToReportTo) {}
        ~MountProbe();

        MountProbe (const MountProbe&) = delete;
        MountProbe& operator= (const MountProbe&) = delete;

        /** Everything one question needs. Copied at request time. */
        struct Question
        {
            std::string mountId;
            std::string host;
            int queryPort = 0;
            std::string address;
            std::string typeTag;
        };

        /*  Starts the thread. Idempotent; false if it was already running. */
        bool start();

        /** Stops it and joins. The destructor calls it. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        /*  Tick thread. Asks for one node to be read back, unless the same
            address is already outstanding.

            False when it was dropped as a duplicate, which is the ordinary case
            for all but the first tick of a wait and is not a failure. */
        bool ask (const Question&);

        /** How many questions are waiting or in flight. Diagnostics. */
        std::size_t outstanding() const;

        /** How long one exchange is given. */
        void setTimeout (int milliseconds) noexcept { timeoutMs = milliseconds; }

    private:
        void run();

        Engine* engine = nullptr;

        mutable std::mutex guard;
        std::condition_variable wake;
        std::deque<Question> queued;
        std::set<std::string> inFlight;          // by address

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };
        std::atomic<int> timeoutMs { 2000 };

        std::thread thread;
    };
}
