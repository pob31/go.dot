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
    The engine skeleton: submit, apply on a tick, record, replay.

    The last of those is the one Phase 1 is finally judged on - "the event log
    replays the session bit-for-bit" - so the replay cases here are not a
    formality. They are written against a log the engine wrote itself, because
    that is the claim: a log this engine produced, fed back to a fresh one,
    produces the same log again.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/SampleClock.h>
#include <wfg/engine/log/Replay.h>

#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    /*  A command that generates something the caller did not supply, which is
        the case replay has to survive: the id must come back out of the log
        rather than being made again. Deterministic here (a counter, not a
        random number) so the test is about the log, not about randomness. */
    void addCountingCommand (Engine& engine, std::shared_ptr<int> counter)
    {
        engine.commands().add ({ "test.create",
                                 "Creates something and names it, generating the name if asked",
                                 { { "name", 's', true } },
                                 true,
                                 [counter] (CommandContext&, const std::vector<osc::Value>& args)
                                 {
                                     if (! args.empty())
                                         return Outcome::ok (args);

                                     const auto generated = "GEN" + std::to_string ((*counter)++);
                                     return Outcome::ok ({ osc::Value::string (generated) });
                                 } });
    }

    void addRefusingCommand (Engine& engine)
    {
        engine.commands().add ({ "test.refuse",
                                 "Always refuses, with a fixed reason",
                                 {},
                                 true,
                                 [] (CommandContext&, const std::vector<osc::Value>&)
                                 {
                                     return Outcome::rejected (reason::unknownId);
                                 } });
    }
}

//==============================================================================
TEST_CASE ("engine: a fresh engine knows noop and nothing else")
{
    const Engine engine;

    CHECK (engine.commands().size() == 1);
    REQUIRE (engine.commands().find ("noop") != nullptr);
    CHECK_FALSE (engine.commands().find ("noop")->mutates);
    CHECK (engine.currentTick() == -1);
    CHECK (engine.sequence() == 0);
    CHECK (engine.errorCount() == 0);
}

TEST_CASE ("engine: a submitted command is applied on the next tick and recorded")
{
    Engine engine;
    engine.log().openInMemory ({});

    engine.submit (origin::cli, "noop");

    // Nothing happens until the tick: the queue is the only road in, and the
    // tick thread is the only thing that drives down it.
    CHECK (engine.sequence() == 0);

    const auto result = engine.processTick (5);

    CHECK (result.tick == 5);
    CHECK (result.applied == 1);
    CHECK (result.rejected == 0);
    CHECK (result.dropped == 0);
    CHECK (engine.currentTick() == 5);
    CHECK (engine.sequence() == 1);
    CHECK (engine.log().contents() == "# wfg-log 1\nA 5 0 cli noop\n");
}

TEST_CASE ("engine: events are applied in arrival order and numbered without gaps")
{
    Engine engine;
    engine.log().openInMemory ({});

    for (int i = 0; i < 3; ++i)
        engine.submit (origin::cli, "noop");

    engine.processTick (0);

    for (int i = 0; i < 2; ++i)
        engine.submit (origin::cli, "noop");

    engine.processTick (1);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 5);

    for (std::size_t i = 0; i < parsed.records.size(); ++i)
        CHECK (parsed.records[i].seq == i);

    CHECK (parsed.records[2].tick == 0);
    CHECK (parsed.records[3].tick == 1);
}

//==============================================================================
TEST_CASE ("engine: an unknown command is rejected, recorded and counted")
{
    Engine engine;
    engine.log().openInMemory ({});

    engine.submit (origin::cli, "no.such.command", { osc::Value::int32 (1) });
    const auto result = engine.processTick (3);

    CHECK (result.applied == 0);
    CHECK (result.rejected == 1);
    CHECK (engine.errorCount() == 1);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 1);
    CHECK (parsed.records[0].kind == LogRecord::Kind::rejected);
    CHECK (parsed.records[0].reason == reason::unknownCommand);

    // The arguments are kept even though nothing ran: a rejected write is most
    // useful when you can see what was in it.
    REQUIRE (parsed.records[0].args.size() == 1);

    // And a client can find out, which over OSC it otherwise could not.
    CHECK (engine.lastError().find (reason::unknownCommand) != std::string::npos);
    CHECK (engine.lastError().find ("no.such.command") != std::string::npos);
}

TEST_CASE ("engine: a bad signature is rejected before the handler runs")
{
    Engine engine;
    engine.log().openInMemory ({});

    bool handlerRan = false;

    engine.commands().add ({ "test.needsInt", "wants one int", { { "v", 'i', false } }, true,
                             [&handlerRan] (CommandContext&, const std::vector<osc::Value>& args)
                             {
                                 handlerRan = true;
                                 return Outcome::ok (args);
                             } });

    engine.submit (origin::cli, "test.needsInt", { osc::Value::string ("12") });
    engine.processTick (0);

    CHECK_FALSE (handlerRan);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 1);
    CHECK (parsed.records[0].reason == reason::typeMismatch);
}

TEST_CASE ("engine: a handler may refuse, and its reason reaches the log")
{
    Engine engine;
    engine.log().openInMemory ({});
    addRefusingCommand (engine);

    engine.submit (origin::cli, "test.refuse");
    const auto result = engine.processTick (0);

    CHECK (result.rejected == 1);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 1);
    CHECK (parsed.records[0].reason == reason::unknownId);
}

TEST_CASE ("engine: a dropped packet keeps its place in the stream")
{
    Engine engine;
    engine.log().openInMemory ({});

    engine.submit (origin::cli, "noop");
    engine.submit (Drop { "udp:127.0.0.1:9000", reason::malformedPacket, { 0x2f, 0x22 } });
    engine.submit (origin::cli, "noop");

    const auto result = engine.processTick (2);

    CHECK (result.applied == 2);
    CHECK (result.dropped == 1);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 3);
    CHECK (parsed.records[0].kind == LogRecord::Kind::applied);
    CHECK (parsed.records[1].kind == LogRecord::Kind::dropped);
    CHECK (parsed.records[2].kind == LogRecord::Kind::applied);
    CHECK (parsed.records[1].seq == 1);   // sequence is monotonic across kinds
}

//==============================================================================
TEST_CASE ("engine: the log records what was APPLIED, not what was submitted")
{
    /*  The property the whole replay design rests on. A handler that generates
        an identifier puts it into the record, in the argument the caller left
        out, so replaying that record needs no randomness. */
    Engine engine;
    engine.log().openInMemory ({});
    addCountingCommand (engine, std::make_shared<int> (1));

    engine.submit (origin::cli, "test.create");            // no name given
    engine.processTick (0);

    const auto parsed = LogFile::parse (engine.log().contents());
    REQUIRE (parsed.records.size() == 1);
    REQUIRE (parsed.records[0].args.size() == 1);
    CHECK (parsed.records[0].args[0].getString() == "GEN1");
}

//==============================================================================
TEST_CASE ("replay: an empty log replays to an empty log")
{
    Engine engine;
    const auto result = replay (engine, LogFile::parse ("# wfg-log 1\n"));

    CHECK (result.ok);
    CHECK (result.recordsReplayed == 0);
    CHECK (result.mismatches.empty());
}

TEST_CASE ("replay: a session reproduces its own log, record for record")
{
    /*  Record a session, then replay it into a fresh engine and require the
        second log to equal the first. Both engines carry the same command set,
        which is the one precondition replay has. */
    auto record = [] (Engine& engine)
    {
        engine.submit (origin::cli, "noop");
        engine.submit ("ws:127.0.0.1:51234", "test.create");
        engine.processTick (0);

        engine.submit ("udp:127.0.0.1:9000", "no.such.command", { osc::Value::int32 (1) });
        engine.submit (Drop { "udp:127.0.0.1:9000", reason::malformedPacket, { 0x01 } });
        engine.processTick (4);

        engine.submit (origin::cli, "test.refuse");
        engine.submit (origin::cli, "test.create", { osc::Value::string ("named by hand") });
        engine.processTick (9);
    };

    Engine session;
    session.log().openInMemory ({});
    addCountingCommand (session, std::make_shared<int> (1));
    addRefusingCommand (session);
    record (session);

    const auto original = LogFile::parse (session.log().contents());
    REQUIRE (original.errors.empty());
    REQUIRE (original.records.size() == 6);

    Engine fresh;
    addCountingCommand (fresh, std::make_shared<int> (1));
    addRefusingCommand (fresh);

    const auto result = replay (fresh, original);

    for (const auto& m : result.mismatches)
        INFO (m);

    CHECK (result.mismatches.empty());
    CHECK (result.ok);
    CHECK (result.recordsReplayed == original.records.size());

    // Ticks, sequence numbers, origins, reasons and generated names all came
    // back the same, so the two logs are the same bytes.
    CHECK (result.producedLog == session.log().contents());
}

TEST_CASE ("replay: replaying twice gives the same result both times")
{
    Engine session;
    session.log().openInMemory ({});
    addCountingCommand (session, std::make_shared<int> (1));

    session.submit (origin::cli, "test.create");
    session.processTick (0);
    session.submit (origin::cli, "test.create");
    session.processTick (1);

    const auto original = LogFile::parse (session.log().contents());

    std::string first, second;

    {
        Engine engine;
        addCountingCommand (engine, std::make_shared<int> (1));
        const auto r = replay (engine, original);
        CHECK (r.ok);
        first = r.producedLog;
    }

    {
        Engine engine;
        addCountingCommand (engine, std::make_shared<int> (1));
        const auto r = replay (engine, original);
        CHECK (r.ok);
        second = r.producedLog;
    }

    CHECK (first == second);
}

TEST_CASE ("replay: a log the engine could not have written is reported, not repaired")
{
    /*  A record whose outcome does not match what this engine does - here, an
        applied record for a command that does not exist - must fail loudly.
        Silently "fixing" it would let a broken fixture pass forever. */
    Engine engine;

    const auto tampered = LogFile::parse ("# wfg-log 1\nA 0 0 cli no.such.command\n");
    const auto result = replay (engine, tampered);

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.mismatches.empty());
    CHECK (result.mismatches[0].find ("record 0") != std::string::npos);
}

//==============================================================================
TEST_CASE ("sample clock: the counter is monotonic and exact")
{
    ManualClock clock;

    CHECK (clock.samplesElapsed() == 0);

    clock.advance (128);
    clock.advance (128);
    CHECK (clock.samplesElapsed() == 256);

    // Large values: a two-hour show at 96 kHz is about 7e8 samples, and a
    // 32-bit counter would have wrapped long before a festival week is out.
    clock.setSamplesElapsed (4'000'000'000LL);
    clock.advance (1);
    CHECK (clock.samplesElapsed() == 4'000'000'001LL);
}

TEST_CASE ("sample clock: advancing from another thread is seen by the reader")
{
    ManualClock clock;

    std::thread producer ([&clock]
    {
        for (int i = 0; i < 1000; ++i)
            clock.advance (64);
    });

    producer.join();
    CHECK (clock.samplesElapsed() == 64000);
}
