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
    The engine, seen from outside: submit work, advance the tick, read the
    result. Everything else in src/wfg/engine is an implementation detail of
    these few calls.

    THE ONE RULE THIS CLASS EXISTS TO ENFORCE (PRD 3.15, 4.11): every mutation
    of engine state is a named command, submitted as an event, applied by the
    tick thread in arrival order, and written to the log with the tick it landed
    on. There is no setter anywhere else. A subsystem that wants to change
    something registers a command and submits it like everyone else.

    THREADS.
      submit()       any thread, never blocks on the model, lock-bounded.
      processTick()  the tick thread, and only ever one thread. It owns the
                     model: it is the only writer and the only direct reader.
      the rest       const, cheap, safe from any thread (counters are atomic).

    This header names no JUCE or Tracktion type, and must not start: Phase 5's
    UI client and Phase 9's out-of-process plugin scanner link this library, and
    a vendor type in this surface drags 31 translation units of headers into
    each of them. Vendor types live on the implementation side of it.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/command/EventQueue.h>
#include <wfg/engine/log/EventLog.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wfg
{
    class Engine
    {
    public:
        Engine();
        ~Engine();

        Engine (const Engine&) = delete;
        Engine& operator= (const Engine&) = delete;

        //======================================================================
        /** The command set. Registration is construction-time only; see
            CommandRegistry for why there is no unregister. */
        CommandRegistry& commands() noexcept              { return registry; }
        const CommandRegistry& commands() const noexcept  { return registry; }

        //======================================================================
        /** Any thread. False when the queue was full and an older entry was
            dropped to make room. */
        bool submit (Event event)  { return queue.submit (std::move (event)); }
        bool submit (Drop drop)    { return queue.submit (std::move (drop)); }

        /** Convenience for the CLI, tests and internal work. */
        bool submit (std::string origin, std::string command, std::vector<osc::Value> args = {});

        //======================================================================
        /*  What one tick did. Returned rather than logged-and-forgotten so a
            test can assert on it without parsing the log, and so the CLI can
            report a failure the moment it happens. */
        struct TickResult
        {
            std::int64_t tick = 0;
            std::size_t applied = 0;
            std::size_t rejected = 0;
            std::size_t dropped = 0;

            /*  The origin of every event applied this tick, WHEN THERE WAS
                EXACTLY ONE. Empty when nothing was applied, and empty when two
                or more origins wrote.

                It exists for echo suppression, and the emptiness in the
                multiple case is the careful part. A push is withheld from the
                client that caused it, because its fader is already there. With
                two writers in one tick there is no single cause, and blaming
                either one would withhold a change it did NOT make - a surface
                left showing a stale value with nothing to correct it, which is
                worse than the extra message suppression was saving.

                So: suppress when the cause is unambiguous, and send to
                everybody when it is not. The cost of being wrong that way is
                one redundant push. Per-address attribution is what would remove
                even that, and it belongs with Phase 6's real surfaces rather
                than here. */
            std::string soleOrigin;

            std::size_t total() const noexcept { return applied + rejected + dropped; }
        };

        /*  Tick thread only. Drains everything submitted since the last call and
            applies it, in arrival order, stamping each with this tick index.

            Deliberately takes the index rather than reading a clock: the tick
            thread derives it from the sample counter, the replay tool takes it
            from the log, and a test names it outright. Nothing in here reads a
            wall clock, which is what makes a replay reproducible. */
        TickResult processTick (std::int64_t tick);

        /** The last tick index processTick was given. */
        std::int64_t currentTick() const noexcept { return tick.load (std::memory_order_relaxed); }

        /** Records written so far, all kinds. Also the next sequence number. */
        std::uint64_t sequence() const noexcept { return seq.load (std::memory_order_relaxed); }

        //======================================================================
        /*  Rejections, surfaced for clients: OSC has no reply channel and the
            OSCQuery proposal defines none, so a client that writes something
            unacceptable can only find out by reading it back. These become
            /godot/engine/errorCount and /godot/engine/lastError. */
        std::uint64_t errorCount() const noexcept { return errors.load (std::memory_order_relaxed); }
        std::string lastError() const;

        //======================================================================
        EventLog& log() noexcept { return eventLog; }

        /** Turns logging off, for a replay that must not record itself. */
        void setLogging (bool shouldLog) noexcept { logging = shouldLog; }
        bool isLogging() const noexcept { return logging; }

    private:
        LogRecord applyEvent (std::int64_t tickIndex, const Event& event);
        void record (const LogRecord& r);

        CommandRegistry registry;
        EventQueue queue;
        EventLog eventLog;

        std::vector<Entry> draining;          // reused; tick thread only

        std::atomic<std::int64_t> tick { -1 };
        std::atomic<std::uint64_t> seq { 0 };
        std::atomic<std::uint64_t> errors { 0 };
        bool logging = true;

        mutable std::mutex errorMutex;
        std::string lastErrorText;
    };
}
