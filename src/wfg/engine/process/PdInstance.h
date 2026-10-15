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

    The thread is started with the eight megabytes of stack Pd's depth guards
    were written for (PatchThread.h), and since nothing else ever calls into
    the instance, it is the one thread that pushes and pops the canvases the
    instance is loading - a stack the build's copy of Pd keeps per thread for
    that reason (cmake/WfgThirdParty.cmake, §51.9).

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

#include <wfg/engine/process/PatchText.h>
#include <wfg/engine/process/ProcessInstance.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace wfg::pair { class Hold; }

namespace wfg::process
{
    inline constexpr int pdSampleRate = 12800;
    inline constexpr int pdBlocksPerTick = 4;

    class PdInstance final : public ProcessInstance
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
        ~PdInstance() override;

        PdInstance (const PdInstance&) = delete;
        PdInstance& operator= (const PdInstance&) = delete;

        /*  Each starts a job on the instance's thread and returns at once: false
            when the last job has not finished, or the instance is not in a
            state the job needs. `make` first, then `open`, any number of ticks,
            `open` again for a new text, `close` last. */
        bool make() override;
        bool open (std::string patchText) override;
        bool tick (std::vector<Input> inputs) override;
        bool close() override;

        /*  Waits for the job at most `limit`; true when it has finished. */
        bool finished (std::chrono::microseconds limit) override;

        /*  The instance is let go without being freed when this object goes:
            for when another patch is stuck, and freeing - which waits for Pd's
            shared lock - would wait for ever and stop every patch (ACK). Its
            memory is kept until Go.dot ends. */
        void leaveUnfreed() override;

        /*  Whether a job is running. */
        bool busy() const override;

        /*  What the jobs since the last call sent and printed. Call only when
            finished. */
        Outbox takeOutbox() override;

        /*  What the last finished job found. */
        bool made() const override;
        bool opened() const override;
        std::string problem() const override;

        /*  The names the open patch was bound to (PatchText's namesIn): what it
            sends that Go.dot answers, and what it hears. */
        std::vector<std::string> sends() const override;
        std::vector<std::string> receives() const override;

        /*  Pd's instances share one lock (ACK), and a job stuck inside Pd
            cannot be asked to end (ACJ). */
        bool sharesLock() const noexcept override { return true; }
        bool interrupt() override { return false; }

        struct Shared;

    private:
        bool post (int kind, std::vector<Input> inputs, std::string text);

        std::shared_ptr<Shared> shared;
    };

    //==========================================================================
    /*  A PATCH'S OWN [netsend], HELD WITH THE MACHINE (namespace draft §58,
        RD.1; AGG). A patch can open a socket of its own - [netsend -u -b] to a
        projector, [netsend] to a show controller - and those bytes leave by
        Pd's own code, past every door Go.dot holds. So the build amends Pd's
        x_net.c (cmake/WfgThirdParty.cmake): before a message or a list of bytes
        goes out of a [netsend], or back down a [netreceive]'s connections, it
        asks a hook, and while the hook says held it reports the send done and
        sends nothing - the object stays connected and the patch reads no
        failure, as every other held send is counted as sent.

        THE ONE PROCESS-WIDE THING THE HOLD HAS, and it is Pd's doing: the hook
        is a plain C function pointer with no room for which machine is asking.
        One object of this class points it at a hold while it lives, and lets
        go of the hold when it goes, the hook then reading none - so declare it
        after the hold and before whatever runs patches, and the patches stop
        before the hold does. A second one replaces the first. What the
        patch's [netsend] CONNECTS is
        not held: a datagram socket's connect sends nothing, and a stream's is a
        session the patch opens itself, which RD.1 leaves to it. */
    class NetsendHold
    {
    public:
        explicit NetsendHold (const pair::Hold& hold) noexcept;
        ~NetsendHold();

        NetsendHold (const NetsendHold&) = delete;
        NetsendHold& operator= (const NetsendHold&) = delete;
    };
}
