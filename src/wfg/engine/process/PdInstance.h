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
    ONE PATCH, ON A THREAD OF ITS OWN (namespace draft 51, ACE, ACH-ACK, ACQ, ACT).

    A process cue's patch runs in a Pure Data instance of its own, made by
    libpd, and every call into that instance is made on one thread that
    belongs to it. Pd keeps "the current instance" per thread (PDTHREADS), so a
    thread that only ever runs its own instance never has to say which; and
    that thread sets its own number format to C before it does anything,
    because Pd reads and writes numbers through the C library and under a
    French locale "2.5" would read as 2 - while the rest of Go.dot, and the
    fr_FR run of every test, keep theirs (ACQ).

    WHO WAITS FOR WHOM. The tick asks for a job - make, open, a tick, close -
    and gets an answer at once: started, or not, because the last job has not
    finished. It then waits for it at most as long as it chooses (`finished`)
    and goes on either way. A job that has not finished is the patch being
    LATE; what it sends is collected when it does finish. A patch stuck inside
    Pd - a loop that never ends - is never finished, and nothing here can stop
    it (ACJ): the instance is ABANDONED when this object goes, its thread left
    to run and told never to touch Pd again if it ever comes back.

    THE QUIET POINT (ACK). Making, opening, closing and freeing an instance
    take a lock every instance shares, and while anything waits for that lock
    every instance stops - measured (§51.6). This class does not decide when
    those jobs may run; the host does, at the start of a tick when no patch is
    mid-tick. The tests that drive one instance alone are a quiet point by
    construction.

    PD'S CLOCK IS THE TICK (ACH). The instance runs at 12 800 samples a second
    and a tick is four of Pd's 64-sample blocks: 20 ms of Pd's time, whatever
    the interface's rate.
*/

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace wfg::process
{
    inline constexpr int pdSampleRate = 12800;
    inline constexpr int pdBlocksPerTick = 4;

    /*  THE NAME MIDI TRAVELS UNDER, in and out (namespace draft §51, PC.3): an
        Input or a Sent `to` this carries one MIDI message as its bytes, each a
        number. In, it reaches Pd's [notein], [ctlin], [pgmin], [bendin],
        [touchin], [polytouchin] and [midiin]; out, it is what [noteout],
        [ctlout], [pgmout], [bendout], [touchout], [polytouchout] and
        [midiout] sent. Starts with a character no address does. */
    inline constexpr const char* midiName = "#midi";

    /*  One of Pd's atoms: a number or a word. */
    struct Atom
    {
        bool isNumber = true;
        double number = 0.0;
        std::string word;

        static Atom of (double value) { return { true, value, {} }; }
        static Atom of (std::string value) { return { false, 0.0, std::move (value) }; }

        bool operator== (const Atom& other) const
        {
            return isNumber == other.isNumber
                && (isNumber ? ! (number < other.number) && ! (other.number < number) : word == other.word);
        }
    };

    using Atoms = std::vector<Atom>;

    /*  What a patch sent to a name Go.dot listens on: the name, Pd's selector -
        "bang", "float", "symbol", "list", or a message's first word - and the
        atoms after it. */
    struct Sent
    {
        std::string to;
        std::string selector;
        Atoms atoms;
    };

    /*  Something for a patch to hear: delivered to the receive name `to` when
        the patch has one, and to its catch-all `in` as the message
        "<to> <atoms...>" when it has that. */
    struct Input
    {
        std::string to;
        Atoms atoms;
    };

    /*  What one job left behind. */
    struct Outbox
    {
        std::vector<Sent> sent;
        std::vector<std::string> printed;
        std::size_t dropped = 0;    // sends past the cap, counted and not kept
    };

    class PdInstance
    {
    public:
        struct Settings
        {
            /*  Where the instance's patch is written to be opened: a file of its
                own, in the engine's cache. */
            std::string patchFile;

            /*  Folders Pd looks in for abstractions (ACP): Go.dot's own `pd`
                folder and the show's. */
            std::vector<std::string> searchPaths;

            /*  How many sends one job keeps (ACI). */
            std::size_t maxSent = 256;

            /*  How many printed lines one job keeps. */
            std::size_t maxPrinted = 32;
        };

        explicit PdInstance (Settings settings);

        /*  Frees the instance if its thread is idle - which takes Pd's shared
            lock, so the host destroys one only at a quiet point - and abandons
            it if not. */
        ~PdInstance();

        PdInstance (const PdInstance&) = delete;
        PdInstance& operator= (const PdInstance&) = delete;

        /*  Each starts a job on the instance's thread and returns at once: false
            when the last job has not finished, or the instance is not in a
            state the job needs. `make` first, then `open`, any number of ticks,
            `open` again for a new text, `close` last. */
        bool make();
        bool open (std::string patchText);
        bool tick (std::vector<Input> inputs);
        bool close();

        /*  Waits for the job at most `limit`; true when it has finished. */
        bool finished (std::chrono::microseconds limit);

        /*  The instance is let go without being freed when this object goes:
            for when another patch is stuck, and freeing - which waits for Pd's
            shared lock - would wait for ever and stop every patch (ACK). Its
            memory is kept until Go.dot ends. */
        void leaveUnfreed();

        /*  Whether a job is running. */
        bool busy() const;

        /*  What the jobs since the last call sent and printed. Call only when
            finished. */
        Outbox takeOutbox();

        /*  What the last finished job found. */
        bool made() const;
        bool opened() const;
        std::string problem() const;

        /*  The names the open patch was bound to (PatchText's namesIn): what it
            sends that Go.dot answers, and what it hears. */
        std::vector<std::string> sends() const;
        std::vector<std::string> receives() const;

        struct Shared;

    private:
        bool post (int kind, std::vector<Input> inputs, std::string text);

        std::shared_ptr<Shared> shared;
    };
}
