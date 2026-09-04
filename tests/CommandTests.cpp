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

/*
    The command registry, its type policy, and the queue every mutation arrives
    on.

    The type policy is the part worth testing hardest. It decides what a client
    can get away with, and both directions are dangerous: too strict and an OSC
    sender that can only emit floats cannot set an integer; too lax and "12"
    becomes 12 and a typo becomes a cue.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/command/EventQueue.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    Command makeCommand (std::string name, std::vector<CommandParam> params = {})
    {
        return Command { std::move (name), "a test command", std::move (params), true,
                         [] (CommandContext&, const std::vector<osc::Value>& args)
                         {
                             return Outcome::ok (args);
                         } };
    }
}

//==============================================================================
TEST_CASE ("command registry: lookup, ordering and replacement")
{
    CommandRegistry registry;

    CHECK (registry.find ("nothing.here") == nullptr);

    registry.add (makeCommand ("zebra.walk"));
    registry.add (makeCommand ("alpha.run"));

    REQUIRE (registry.size() == 2);

    // Registration order, not alphabetical: the /godot/cmd namespace and
    // `wfg commands` list themselves in the order a human grouped them.
    CHECK (registry.all()[0].name == "zebra.walk");
    CHECK (registry.all()[1].name == "alpha.run");

    REQUIRE (registry.find ("alpha.run") != nullptr);
    CHECK (registry.find ("alpha.run")->name == "alpha.run");
    CHECK (registry.find ("Alpha.Run") == nullptr);   // names are case-sensitive

    // Replacing in place, so exactly one entry can ever claim a name.
    auto replacement = makeCommand ("zebra.walk");
    replacement.description = "replaced";
    registry.add (std::move (replacement));

    CHECK (registry.size() == 2);
    CHECK (registry.all()[0].description == "replaced");
    CHECK (registry.all()[1].name == "alpha.run");
}

//==============================================================================
TEST_CASE ("command signature: arity is checked before anything runs")
{
    const auto command = makeCommand ("two.required",
                                      { { "a", 'i', false }, { "b", 's', false } });

    CHECK_FALSE (CommandRegistry::checkArgs (command, {}).ok);
    CHECK (CommandRegistry::checkArgs (command, {}).reason == reason::arity);

    CHECK_FALSE (CommandRegistry::checkArgs (command, { osc::Value::int32 (1) }).ok);

    CHECK (CommandRegistry::checkArgs (command,
                                       { osc::Value::int32 (1), osc::Value::string ("x") }).ok);

    // One too many is an error, not something to ignore: an extra argument
    // usually means the caller thinks it is calling a different command.
    const auto tooMany = CommandRegistry::checkArgs (command,
                                                     { osc::Value::int32 (1),
                                                       osc::Value::string ("x"),
                                                       osc::Value::int32 (2) });
    CHECK_FALSE (tooMany.ok);
    CHECK (tooMany.reason == reason::arity);
}

TEST_CASE ("command signature: optional parameters may be left out")
{
    const auto command = makeCommand ("one.optional",
                                      { { "id", 's', false }, { "name", 's', true } });

    CHECK (CommandRegistry::checkArgs (command, { osc::Value::string ("A") }).ok);
    CHECK (CommandRegistry::checkArgs (command, { osc::Value::string ("A"),
                                                  osc::Value::string ("B") }).ok);
    CHECK_FALSE (CommandRegistry::checkArgs (command, {}).ok);
}

//==============================================================================
TEST_CASE ("command signature: numbers coerce within their family and nowhere else")
{
    const auto wantsInt   = makeCommand ("wants.int",   { { "v", 'i', false } });
    const auto wantsFloat = makeCommand ("wants.float", { { "v", 'f', false } });

    // A sender that can only emit floats can still set an integer, and the
    // other way round. This is the whole reason the coercion exists.
    {
        const auto check = CommandRegistry::checkArgs (wantsInt, { osc::Value::float32 (3.0f) });
        REQUIRE (check.ok);
        REQUIRE (check.args.size() == 1);
        CHECK (check.args[0].isInt32());
        CHECK (check.args[0].getInt32() == 3);
    }

    {
        const auto check = CommandRegistry::checkArgs (wantsFloat, { osc::Value::int32 (3) });
        REQUIRE (check.ok);
        CHECK (check.args[0].isFloat32());
        CHECK (check.args[0].getFloat32() == doctest::Approx (3.0f));
    }

    // A string is NOT a number, however number-shaped it looks.
    const auto fromString = CommandRegistry::checkArgs (wantsInt, { osc::Value::string ("12") });
    CHECK_FALSE (fromString.ok);
    CHECK (fromString.reason == reason::typeMismatch);

    // Nor is a bool, a nil or an impulse.
    CHECK_FALSE (CommandRegistry::checkArgs (wantsInt, { osc::Value::boolean (true) }).ok);
    CHECK_FALSE (CommandRegistry::checkArgs (wantsInt, { osc::Value::nil() }).ok);
    CHECK_FALSE (CommandRegistry::checkArgs (wantsInt, { osc::Value::impulse() }).ok);
}

TEST_CASE ("command signature: a boolean parameter accepts T, F and an int")
{
    const auto command = makeCommand ("wants.bool", { { "v", 'T', false } });

    for (const auto& v : { osc::Value::boolean (true), osc::Value::boolean (false),
                           osc::Value::int32 (0), osc::Value::int32 (1) })
    {
        const auto check = CommandRegistry::checkArgs (command, { v });
        REQUIRE (check.ok);
        CHECK (check.args[0].isBool());
    }

    CHECK (CommandRegistry::checkArgs (command, { osc::Value::int32 (1) }).args[0].getBool());
    CHECK_FALSE (CommandRegistry::checkArgs (command, { osc::Value::int32 (0) }).args[0].getBool());
    CHECK_FALSE (CommandRegistry::checkArgs (command, { osc::Value::string ("true") }).ok);
}

TEST_CASE ("command signature: a non-finite number is refused once, here")
{
    const auto command = makeCommand ("wants.float", { { "v", 'f', false } });
    const auto check = CommandRegistry::checkArgs (command,
                                                   { osc::Value::float32 (std::numeric_limits<float>::infinity()) });

    CHECK_FALSE (check.ok);
    CHECK (check.reason == reason::nonFinite);
    CHECK (check.args.empty());
}

//==============================================================================
TEST_CASE ("event queue: entries come out in the order they went in")
{
    EventQueue queue;
    std::vector<Entry> drained;

    queue.drainInto (drained);
    CHECK (drained.empty());

    for (int i = 0; i < 5; ++i)
        queue.submit (Event { "cli", "noop", { osc::Value::int32 (i) } });

    queue.drainInto (drained);
    REQUIRE (drained.size() == 5);

    for (int i = 0; i < 5; ++i)
        CHECK (std::get<Event> (drained[static_cast<std::size_t> (i)]).args[0].getInt32() == i);

    // Draining empties it.
    queue.drainInto (drained);
    CHECK (drained.empty());
}

TEST_CASE ("event queue: drops are queued alongside events, in place")
{
    EventQueue queue;

    queue.submit (Event { "cli", "noop", {} });
    queue.submit (Drop { "udp:127.0.0.1:9000", reason::malformedPacket, { 0x01, 0x02 } });
    queue.submit (Event { "cli", "noop", {} });

    std::vector<Entry> drained;
    queue.drainInto (drained);

    REQUIRE (drained.size() == 3);
    CHECK (std::holds_alternative<Event> (drained[0]));
    CHECK (std::holds_alternative<Drop> (drained[1]));
    CHECK (std::holds_alternative<Event> (drained[2]));
    CHECK (std::get<Drop> (drained[1]).payload.size() == 2);
}

TEST_CASE ("event queue: a flood drops the oldest and says so")
{
    /*  A misbehaving client must not grow the queue without limit, and the
        operator's most recent GO must not be the entry that loses. */
    EventQueue queue { 4 };

    for (int i = 0; i < 6; ++i)
        queue.submit (Event { "cli", "noop", { osc::Value::int32 (i) } });

    CHECK (queue.droppedCount() == 2);

    std::vector<Entry> drained;
    queue.drainInto (drained);

    REQUIRE (drained.size() == 4);
    CHECK (std::get<Event> (drained[0]).args[0].getInt32() == 2);
    CHECK (std::get<Event> (drained[3]).args[0].getInt32() == 5);
}

TEST_CASE ("event queue: many producers, one consumer, nothing lost")
{
    /*  The real shape: the OSCQuery server threads, the UDP receiver and the
        command line all submit while the tick thread drains. Nothing here
        checks interleaving - that is not defined between threads - only that
        every submission arrives exactly once, and that each producer's own
        submissions keep their order. */
    constexpr int producers = 4;
    constexpr int perProducer = 250;

    EventQueue queue { producers * perProducer * 2 };
    std::atomic<bool> go { false };
    std::vector<std::thread> threads;

    for (int p = 0; p < producers; ++p)
    {
        threads.emplace_back ([&queue, &go, p]
        {
            while (! go.load())
                std::this_thread::yield();

            for (int i = 0; i < perProducer; ++i)
                queue.submit (Event { "cli", "noop",
                                      { osc::Value::int32 (p), osc::Value::int32 (i) } });
        });
    }

    go.store (true);

    for (auto& t : threads)
        t.join();

    std::vector<Entry> drained;
    queue.drainInto (drained);

    CHECK (queue.droppedCount() == 0);
    REQUIRE (drained.size() == producers * perProducer);

    std::vector<int> lastSeen (producers, -1);
    std::vector<int> counts (producers, 0);

    for (const auto& entry : drained)
    {
        const auto& event = std::get<Event> (entry);
        const auto p = static_cast<std::size_t> (event.args[0].getInt32());
        const auto i = event.args[1].getInt32();

        REQUIRE (p < static_cast<std::size_t> (producers));
        CHECK (i == lastSeen[p] + 1);      // this producer's own order is intact
        lastSeen[p] = i;
        ++counts[p];
    }

    for (int p = 0; p < producers; ++p)
        CHECK (counts[static_cast<std::size_t> (p)] == perProducer);
}
