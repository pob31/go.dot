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

#include <wfg/engine/Engine.h>

namespace wfg
{
    Engine::Engine()
    {
        /*  The first command, and the shape every later one follows: a name, a
            sentence, a signature, a handler that returns what it applied.

            `noop` is not a placeholder. A log needs a record that provably
            changed nothing, so that "the engine was alive at tick N" and "the
            engine did something at tick N" are different statements - and the
            replay harness needs a command it can exercise before there is a
            document to mutate. */
        registry.add ({ "noop",
                        "Does nothing, and records that it did.",
                        {},
                        false,
                        [] (CommandContext&, const std::vector<osc::Value>&)
                        {
                            return Outcome::ok ({});
                        } });
    }

    Engine::~Engine() = default;

    //==============================================================================
    bool Engine::submit (std::string origin, std::string command, std::vector<osc::Value> args)
    {
        Event event;
        event.origin = std::move (origin);
        event.command = std::move (command);
        event.args = std::move (args);
        return submit (std::move (event));
    }

    std::string Engine::lastError() const
    {
        const std::lock_guard<std::mutex> lock (errorMutex);
        return lastErrorText;
    }

    //==============================================================================
    void Engine::record (const LogRecord& r)
    {
        if (r.kind != LogRecord::Kind::applied)
        {
            errors.fetch_add (1, std::memory_order_relaxed);

            /*  The text a client reads back at /godot/engine/lastError. It names
                the tick and sequence so an operator can find the record in the
                log, and the origin so two clients arguing over one node can tell
                whose write was refused. */
            std::string text = std::to_string (r.tick) + " " + std::to_string (r.seq)
                             + " " + r.origin + " " + r.reason;

            if (! r.command.empty())
                text += " " + r.command;

            const std::lock_guard<std::mutex> lock (errorMutex);
            lastErrorText = std::move (text);
        }

        if (logging && eventLog.isOpen())
            eventLog.write (r);
    }

    //==============================================================================
    LogRecord Engine::applyEvent (std::int64_t tickIndex, const Event& event)
    {
        LogRecord r;
        r.tick = tickIndex;
        r.origin = event.origin;
        r.command = event.command;

        const auto* command = registry.find (event.command);

        if (command == nullptr)
        {
            /*  The arguments are kept on the record even though nothing ran:
                a rejected write is most useful when you can see what was in it.
                Replay re-submits them and expects the same refusal. */
            r.kind = LogRecord::Kind::rejected;
            r.reason = reason::unknownCommand;
            r.args = event.args;
            return r;
        }

        auto check = CommandRegistry::checkArgs (*command, event.args);

        if (! check.ok)
        {
            r.kind = LogRecord::Kind::rejected;
            r.reason = std::move (check.reason);
            r.args = event.args;
            return r;
        }

        CommandContext context;
        context.tick = tickIndex;
        context.origin = &event.origin;

        auto outcome = command->handler (context, check.args);

        if (! outcome.applied)
        {
            r.kind = LogRecord::Kind::rejected;
            r.reason = std::move (outcome.reason);
            r.args = std::move (check.args);
            return r;
        }

        /*  AS APPLIED, not as submitted. A handler that generated an id returns
            it here, in the argument the caller left out, so that replaying this
            record produces the same id without the engine ever having to make a
            random number twice. */
        r.kind = LogRecord::Kind::applied;
        r.args = std::move (outcome.appliedArgs);
        return r;
    }

    //==============================================================================
    Engine::TickResult Engine::processTick (std::int64_t tickIndex)
    {
        tick.store (tickIndex, std::memory_order_relaxed);

        TickResult result;
        result.tick = tickIndex;

        queue.drainInto (draining);

        for (auto& entry : draining)
        {
            LogRecord r;

            if (auto* event = std::get_if<Event> (&entry))
            {
                r = applyEvent (tickIndex, *event);
            }
            else
            {
                const auto& drop = std::get<Drop> (entry);

                r.kind = LogRecord::Kind::dropped;
                r.tick = tickIndex;
                r.origin = drop.origin;
                r.reason = drop.reason;

                if (! drop.payload.empty())
                {
                    osc::Blob blob;
                    blob.bytes = drop.payload;
                    r.args.push_back (osc::Value::blob (std::move (blob)));
                }
            }

            r.seq = seq.fetch_add (1, std::memory_order_relaxed);

            switch (r.kind)
            {
                case LogRecord::Kind::applied:  ++result.applied;  break;
                case LogRecord::Kind::rejected: ++result.rejected; break;
                case LogRecord::Kind::dropped:  ++result.dropped;  break;
            }

            record (r);
        }

        draining.clear();
        return result;
    }
}
