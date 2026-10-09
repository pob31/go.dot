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
    EVERY RUNNING PATCH, ONCE A TICK (namespace draft 51, ACE, ACI-ACK).

    The Runner says which process runs should be running and with which text;
    this makes it so, one Pd instance per run, and hands back what each patch
    sent. It is the tick thread's, and one call a tick does all of it, never
    waiting longer than the budget in all:

      1. What finished since the last tick is collected - a patch that was late
         sends now - and a patch still not back is late again; late for
         `stuckAfter` ticks in a row it is STUCK.

      2. AT A QUIET POINT - no patch mid-tick, none stuck - patches are made,
         opened, opened again on a new text and closed: one step each per tick,
         each on its own thread, all waited for until the deadline. These take
         the lock every instance shares (ACK), so nothing is ticked in a tick
         whose steps are still running.

      3. Every running patch is handed this tick's inputs and its four blocks,
         and waited for until the deadline. One that has not finished is late:
         its inputs wait, the newest per address.

    STUCK IS LATE FOR `stuckAfter` TICKS AND AS LONG IN TIME - fifty ticks and
    a second - which in a show are the same thing; a test's ticks take no time,
    and are judged by the clock as the show's are.

    ONCE A PATCH IS STUCK NOTHING IS OPENED OR CLOSED AGAIN (ACK): waiting for
    the shared lock would stop every patch. A run that would need it fails
    with `pd-held`; one that ends has its instance let go unfreed.

    No clock of its own and no Engine: the Runner passes the deadline's length,
    the inputs and the threshold, and turns what comes back into records.
*/

#include <wfg/engine/process/PdInstance.h>
#include <wfg/engine/midi/MidiSink.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wfg::process
{
    /*  A process run that should be running, and the patch it should run. */
    struct WantedProcess
    {
        std::string run;
        std::string cue;
        std::string patch;
    };

    /*  What a patch may hear this tick: what devices reported since the last
        tick, and the rows of the show its receives name, as they stand - the
        host passes a row on only when it differs from what that patch was last
        handed. */
    struct TickInputs
    {
        std::vector<Input> heard;
        std::vector<Input> rows;
    };

    /*  WHAT ARRIVED ON THE MIDI PORTS (PC.3), kept for the tick: the input
        thread's consumer puts each message here after the surfaces have
        declined it, and the Runner's hook takes them all once a tick and hands
        each to the patches whose cue listens on its port. At most `cap`
        messages wait; past it the oldest go, so a flood with no patch to read
        it is a bounded one. */
    class MidiInbox
    {
    public:
        static constexpr std::size_t cap = 1024;

        void push (std::string port, midi::Bytes bytes)
        {
            const std::lock_guard<std::mutex> lock { guard };
            if (waiting.size() >= cap)
                waiting.erase (waiting.begin());
            waiting.emplace_back (std::move (port), std::move (bytes));
        }

        std::vector<std::pair<std::string, midi::Bytes>> take()
        {
            const std::lock_guard<std::mutex> lock { guard };
            return std::exchange (waiting, {});
        }

    private:
        std::mutex guard;
        std::vector<std::pair<std::string, midi::Bytes>> waiting;
    };

    /*  One run's tick, back to the Runner. */
    struct ProcessResult
    {
        std::string run;
        std::vector<Sent> sent;
        std::vector<std::string> printed;

        /*  THE LAST VALUE ON EACH NAMED PORT (PC.3): what the patch last sent to
            each name Go.dot answers, and what it was last handed at each name it
            hears - "name value value", a line each, in name order, at most 64.
            What the canvas draws on a line (PC.8). */
        std::string ports;
        std::size_t dropped = 0;       // past the cap, this tick
        std::string state;             // starting, running, late, stuck
        int lateTicks = 0;             // since it started
        std::uint64_t droppedTotal = 0;

        /*  Set once, on the tick it happened: `process-stuck` - this patch
            stuck - or `pd-held` - another patch is stuck, so this one cannot
            be opened. The Runner fails the run with it. */
        std::string failure;
    };

    class ProcessHost
    {
    public:
        struct Settings
        {
            /*  Where each patch is written to be opened, as <cue>.pd. */
            std::string cacheFolder;

            /*  Folders Pd looks in for abstractions (ACP). */
            std::vector<std::string> searchPaths;
        };

        explicit ProcessHost (Settings settings);

        /*  Frees each instance at a quiet point, and lets go of it unfreed when
            a patch is stuck or it is busy (PdInstance::leaveUnfreed). */
        ~ProcessHost();

        ProcessHost (const ProcessHost&) = delete;
        ProcessHost& operator= (const ProcessHost&) = delete;

        /*  The receives a run's open patch names: what the Runner looks rows up
            for. */
        using InputsFor = std::function<TickInputs (const std::string& run, const std::vector<std::string>& receives)>;

        std::vector<ProcessResult> tick (const std::vector<WantedProcess>& wanted, const InputsFor& inputsFor,
                                         std::chrono::microseconds budget, int stuckAfter);

        /*  Whether a patch is stuck: nothing will be opened or closed again. */
        bool anyStuck() const noexcept { return stuck; }

        /*  How many instances exist, made or being made. */
        std::size_t instances() const noexcept { return slots.size(); }

    private:
        enum class Phase { needsMake, needsOpen, running, closing };

        struct Slot
        {
            std::string run;
            std::string cue;
            std::string text;           // what is open, or about to be
            std::string wanted;         // what the Runner asks for
            Phase phase = Phase::needsMake;
            bool busy = false;          // a job is out
            bool ticking = false;       // the job out is a tick
            bool failed = false;        // failure reported; nothing more is done
            bool stuck = false;
            int lateTicks = 0;
            int lateInARow = 0;
            std::chrono::steady_clock::time_point posted {};   // when the job out was asked for
            std::uint64_t droppedTotal = 0;
            std::vector<Input> pending; // inputs waiting while it is late, newest per address
            std::map<std::string, Atoms> rowsHanded;
            std::map<std::string, Atoms> portValues;   // the ports readout's
            std::unique_ptr<PdInstance> instance;
            ProcessResult result;       // what this tick has gathered
        };

        void collect (Slot& slot);
        void stepped (Slot& slot);
        static void remember (std::vector<Input>& pending, Input input);

        Settings settings;
        std::vector<std::unique_ptr<Slot>> slots;
        bool stuck = false;
    };
}
