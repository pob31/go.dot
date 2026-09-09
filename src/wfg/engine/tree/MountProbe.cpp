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

#include <wfg/engine/tree/MountProbe.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/oscquery/OscQueryClient.h>

#include <utility>
#include <vector>

namespace wfg::tree
{
    MountProbe::~MountProbe()
    {
        stop();
    }

    bool MountProbe::start()
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);

        thread = std::thread ([this] { run(); });
        return true;
    }

    void MountProbe::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        stopping.store (true, std::memory_order_relaxed);
        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> lock { guard };
        queued.clear();
        inFlight.clear();
    }

    //==============================================================================
    std::string MountProbe::keyOf (const Question& question)
    {
        return (question.observation ? "o " : "v ") + question.address;
    }

    bool MountProbe::ask (const Question& question)
    {
        if (question.host.empty() || question.queryPort <= 0 || question.address.empty())
            return false;

        {
            const std::lock_guard<std::mutex> lock { guard };

            /*  ONE QUESTION PER ADDRESS. The Runner asks again on every tick a
                verified cue is waiting, which is fifty times a second; a queue
                that accepted all of them would spend the rest of the show
                answering the first second. */
            if (! inFlight.insert (keyOf (question)).second)
                return false;

            queued.push_back (question);
        }

        wake.notify_one();
        return true;
    }

    std::size_t MountProbe::outstanding() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return inFlight.size();
    }

    //==============================================================================
    void MountProbe::run()
    {
        for (;;)
        {
            Question question;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed)
                                                 || ! queued.empty(); });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                question = queued.front();
                queued.pop_front();
            }

            /*  OUTSIDE THE LOCK, because this is the part that takes seconds.
                Holding it here would make `ask` - which runs on the tick thread
                - block behind a device that has gone away, which is the whole
                thing this class exists to prevent. */
            const auto value = oscquery::OscQueryClient::readValue (
                                 question.host, question.queryPort, question.address,
                                 question.typeTag, timeoutMs.load (std::memory_order_relaxed));

            {
                const std::lock_guard<std::mutex> lock { guard };
                inFlight.erase (keyOf (question));
            }

            /*  NOTHING IS SUBMITTED WHEN NOTHING ANSWERED, and that is
                deliberate rather than a gap. A device that did not reply has not
                told us anything, and a log record saying so would be a record of
                the network rather than of the show - §3.15 keeps those out. What
                a silence means is the CUE's business: it waits, and its own
                timeout is what turns silence into a failure with a reason.

                A reply that arrived is a state transition and goes in the log,
                whether or not it is the value anybody hoped for. */
            if (! value.has_value())
                continue;

            if (engine == nullptr)
                continue;

            /*  THE SAME RECORD FOR BOTH, WITH A FLAG. An observation is the
                same fact - "the target says this node holds that" - reached for
                a different reason, and a second command name would have meant
                two handlers, two replay paths and two ways for the table to
                learn the same thing. The trailing argument says which store it
                lands in, and is absent for a verify so that every log written
                before this existed still replays. */
            std::vector<osc::Value> args { osc::Value::string (question.mountId),
                                           osc::Value::string (question.address),
                                           *value };

            if (question.observation)
                args.push_back (osc::Value::boolean (true));

            engine->submit ("mount:" + question.mountId, "mount.readback", std::move (args));
        }
    }
}
