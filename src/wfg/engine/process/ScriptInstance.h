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
    ONE SCRIPT, ON A THREAD OF ITS OWN (namespace draft §66, AMP-AMS).

    A script cue's script runs in an interpreter of its own - JavaScript's or
    Lua's (`ScriptLanguage`) - and every call into it is made on one thread
    that belongs to it, as a patch's are on its own (PdInstance.h): the host
    asks for a job and is answered at once, waits for it at most as long as it
    chooses, and takes what it sent.

    WHAT IS THE SAME FOR EVERY LANGUAGE is here: the thread, with the stack a
    patch has and its numbers in C (ACQ); the jobs; the caps on what one job
    keeps (ACI); and the interrupt (AMQ). What a language does - read the
    script, hand it a tick, say what it hears - is the language's.

    OPENING IS STARTING AGAIN. Each `open` throws the last interpreter away and
    makes a new one, so an edit runs the script from its beginning with nothing
    of the last one left (ACM), as a patch opened again does.

    NOTHING SHARED, SO NO QUIET POINT. Two scripts' interpreters have nothing in
    common, and a script never waits for a patch nor a patch for a script.

    A STUCK SCRIPT IS INTERRUPTED. `interrupt` asks the interpreter, from the
    host's thread, to end what it is running; the job then ends with the
    language's complaint, and the instance is closed and freed as any other.
    `leaveUnfreed` interrupts too, and the instance's own thread frees it once
    its job has ended.
*/

#include <wfg/engine/process/ProcessInstance.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::process
{
    /*  WHERE A SCRIPT'S SENDS AND PRINTS GO while a job runs, kept to the caps:
        a send past `maxSent` is counted and dropped (ACI), a line past
        `maxPrinted` is dropped. On the instance's thread only. */
    class ScriptOutput
    {
    public:
        ScriptOutput (std::size_t maxSentToUse, std::size_t maxPrintedToUse);

        void send (Sent sent);
        void print (std::string line);

        Outbox take();

    private:
        std::size_t maxSent;
        std::size_t maxPrinted;
        Outbox outbox;
    };

    /*  ONE SCRIPT'S INTERPRETER, as the instance drives it. Made, used and
        destroyed on the instance's thread; only `interrupt` is called from
        another. */
    class ScriptLanguage
    {
    public:
        virtual ~ScriptLanguage() = default;

        ScriptLanguage() = default;
        ScriptLanguage (const ScriptLanguage&) = delete;
        ScriptLanguage& operator= (const ScriptLanguage&) = delete;

        /*  Reads the script and runs what is at its top level - where it says
            what it hears - with its sends and prints going to the output the
            language was made with. False, with `problem` set, when the script
            does not read or what runs at its top level fails; the problem says
            on which line. */
        virtual bool load (const std::string& source, std::string& problem) = 0;

        /*  One tick: each input handed to what hears it, then what runs every
            tick and what was due on this one. A failure in one of the script's
            own functions is printed and the rest go on. */
        virtual void tick (const std::vector<Input>& inputs) = 0;

        /*  The addresses the script hears, as it last said. */
        virtual std::vector<std::string> receives() = 0;

        /*  Asks what is running to end as soon as it can. From any thread. */
        virtual void interrupt() noexcept = 0;
    };

    /*  Makes a language's interpreter, writing to `output`. */
    using MakeScriptLanguage = std::function<std::unique_ptr<ScriptLanguage> (ScriptOutput& output)>;

    class ScriptInstance final : public ProcessInstance
    {
    public:
        struct Settings
        {
            /*  How many sends one job keeps (ACI). */
            std::size_t maxSent = 256;

            /*  How many printed lines one job keeps. */
            std::size_t maxPrinted = 32;
        };

        ScriptInstance (MakeScriptLanguage makeLanguage, Settings settings);

        /*  Closes the interpreter if the thread is idle and waits for it; when
            not, interrupts it and leaves the thread to free it. */
        ~ScriptInstance() override;

        ScriptInstance (const ScriptInstance&) = delete;
        ScriptInstance& operator= (const ScriptInstance&) = delete;

        bool make() override;
        bool open (std::string source) override;
        bool tick (std::vector<Input> inputs) override;
        bool close() override;

        bool finished (std::chrono::microseconds limit) override;
        void leaveUnfreed() override;
        bool busy() const override;
        Outbox takeOutbox() override;

        bool made() const override;
        bool opened() const override;
        std::string problem() const override;

        /*  A script's sends are not known before it sends them: none. */
        std::vector<std::string> sends() const override;
        std::vector<std::string> receives() const override;

        bool sharesLock() const noexcept override { return false; }
        bool interrupt() override;

        struct Shared;

    private:
        bool post (int kind, std::vector<Input> inputs, std::string text);

        std::shared_ptr<Shared> shared;
    };
}
