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
    ONE PROGRAM A RUN RUNS, AS THE HOST SEES IT (namespace draft §51, §66 AMP).

    A process cue's patch and a script cue's script are each run by an
    instance of their own, on a thread of their own, and the host
    (ProcessHost.h) drives every one of them the same way: it asks for a job -
    make, open, a tick, close - and is answered at once, started or not; it
    waits for the job at most as long as it chooses; and it takes what the job
    sent. Pd's instance (PdInstance.h) is one; a script's (ScriptInstance.h) is
    another.

    TWO THINGS TELL THEM APART, and the host asks both:

      - `sharesLock`: Pd's instances share one lock as they are made, opened,
        closed and freed (ACK), so those steps wait for a quiet point, and once
        one of them is stuck nothing that shares the lock is opened again. A
        script's instance shares nothing with any other.

      - `interrupt`: a job that runs past being stuck can be asked to end. Pd's
        cannot (ACJ); a script's can (AMQ), and its instance is then closed and
        freed as any other.
*/

#include <wfg/engine/process/PatchText.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace wfg::process
{
    /*  THE NAME MIDI TRAVELS UNDER, in and out (namespace draft §51, PC.3): an
        Input or a Sent `to` this carries one MIDI message as its bytes, each a
        number. In, it reaches Pd's [notein], [ctlin], [pgmin], [bendin],
        [touchin], [polytouchin] and [midiin], and a script's godot.midi.hear;
        out, it is what [noteout], [ctlout], [pgmout], [bendout], [touchout],
        [polytouchout] and [midiout] sent, or godot.midi.send. Starts with a
        character no address does. */
    inline constexpr const char* midiName = "#midi";

    /*  What a program sent to a name Go.dot listens on: the name, Pd's selector -
        "bang", "float", "symbol", "list", or a message's first word - and the
        atoms after it. A script's sends are written as a patch's would be. */
    struct Sent
    {
        std::string to;
        std::string selector;
        Atoms atoms;
    };

    /*  Something for a program to hear: delivered to the receive name `to` when
        it has one, and to its catch-all - Pd's `in`, a script's hearAll - as
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

    class ProcessInstance
    {
    public:
        virtual ~ProcessInstance() = default;

        ProcessInstance() = default;
        ProcessInstance (const ProcessInstance&) = delete;
        ProcessInstance& operator= (const ProcessInstance&) = delete;

        /*  Each starts a job on the instance's thread and returns at once: false
            when the last job has not finished, or the instance is not in a
            state the job needs. `make` first, then `open`, any number of ticks,
            `open` again for a new text, `close` last. */
        virtual bool make() = 0;
        virtual bool open (std::string text) = 0;
        virtual bool tick (std::vector<Input> inputs) = 0;
        virtual bool close() = 0;

        /*  Waits for the job at most `limit`; true when it has finished. */
        virtual bool finished (std::chrono::microseconds limit) = 0;

        /*  The instance is let go without waiting for it when this object goes.
            One that shares the lock is never freed - freeing would wait for the
            lock a stuck patch holds; one that does not is freed by its own
            thread once its job ends. */
        virtual void leaveUnfreed() = 0;

        /*  Whether a job is running. */
        virtual bool busy() const = 0;

        /*  What the jobs since the last call sent and printed. Call only when
            finished. */
        virtual Outbox takeOutbox() = 0;

        /*  What the last finished job found. */
        virtual bool made() const = 0;
        virtual bool opened() const = 0;
        virtual std::string problem() const = 0;

        /*  The names the open program sends to that Go.dot answers, and the
            names it hears. */
        virtual std::vector<std::string> sends() const = 0;
        virtual std::vector<std::string> receives() const = 0;

        /*  Whether making, opening, closing and freeing take a lock every
            instance of its kind shares (ACK). */
        virtual bool sharesLock() const noexcept = 0;

        /*  Asks the job running now to end as soon as it can; false when this
            kind of instance cannot be asked (Pd). May be called from any
            thread. */
        virtual bool interrupt() = 0;
    };
}
