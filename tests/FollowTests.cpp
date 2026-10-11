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
    A BACKUP THAT FOLLOWS ITS MAIN (namespace draft §58, RD.6; AFT, AGD, AGF).

    What is pinned here: the gate's table - a client's command passed on, a
    device's ignored, the engine's, the main's and this machine's own applied,
    and all of it applied when the machine does not follow; the relay reading
    back as the event it was; the fixed delay - every record of one main tick
    on one local tick, the main's gaps kept to the tick, a record that missed
    its slot applied at once and counted, Δ chosen from the beats; a record the
    backup applies otherwise than the main named in a sentence; an engine
    record that writes nothing to the show ending nobody's drag, and one that
    does being a step of its own; and two engines on one show joined by the
    real stream and receiver over a link with jitter, the backup refusing
    exactly what the main refused, its run table the main's on every tick
    after the delay, and its undo history the main's after an `undo`.

    Every case runs under fr_FR as well as C: a record's numbers are text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/UndoStep.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/Follow.h>
#include <wfg/engine/pair/PeerLink.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using Clock = pair::LinkClock;
    using namespace std::chrono_literals;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }

    const Clock::time_point t0 = Clock::time_point {} + std::chrono::hours { 1000 };

    Command plain (std::string name, bool machine = false)
    {
        Command command;
        command.name = std::move (name);
        command.machineLocal = machine;
        command.params = { { "anything", '*', true, true } };
        command.handler = [] (CommandContext&, const std::vector<osc::Value>& args) { return Outcome::ok (args); };
        return command;
    }

    /** Every applied or refused record a log holds, in order, as (origin, command). */
    std::vector<std::pair<std::string, std::string>> written (Engine& engine)
    {
        std::vector<std::pair<std::string, std::string>> out;

        for (const auto& record : LogFile::parse (engine.log().contents()).records)
            if (record.kind != LogRecord::Kind::dropped)
                out.emplace_back (record.origin, record.command);

        return out;
    }

    LogRecord mainRecord (std::int64_t tick, std::string command, std::vector<osc::Value> args = {},
                          std::string from = "cli")
    {
        LogRecord r;
        r.kind = LogRecord::Kind::applied;
        r.tick = tick;
        r.origin = std::move (from);
        r.command = std::move (command);
        r.args = std::move (args);
        return r;
    }
}

//==============================================================================
TEST_CASE ("follow: the gate's table - a client's passed on, a device's ignored, the engine's, the main's and the machine's applied")
{
    const auto go = plain ("go");
    const auto configure = plain ("audio.configure", true);
    const auto nodeSet = plain ("node.set");

    //  This machine's clients: passed on to the main (AFT).
    for (const auto* from : { "window", "cli", "pd", "ws:10.0.0.5:51000", "surface:K4QW8YSC", "surface:spacemouse" })
    {
        INFO (from);
        CHECK (pair::gateFor (from, &go, {}) == pair::Gate::passOn);
    }

    //  A device's: the main heard it too (AGD), and anything the table does not know.
    for (const auto* from : { "udp:10.0.0.9:9000", "mount:K3PV7WRB", "serial:S0000001", "link:E0HQDX01",
                              "midi:P0000001", "clock", "process:RUN00001", "peer:ws:10.0.0.5:51000", "someone" })
    {
        INFO (from);
        CHECK (pair::gateFor (from, &go, {}) == pair::Gate::ignore);
    }

    //  The engine's, the main's and a replay's: applied here.
    for (const auto* from : { "engine", "replay", "main:cli", "main:udp:10.0.0.9:9000", "main:peer:ws:10.0.0.5:51000" })
    {
        INFO (from);
        CHECK (pair::gateFor (from, &go, {}) == pair::Gate::apply);
    }

    //  This machine's own settings, whoever asked - a client or a device.
    CHECK (pair::gateFor ("ws:10.0.0.5:51000", &configure, {}) == pair::Gate::apply);
    CHECK (pair::gateFor ("udp:10.0.0.9:9000", &configure, {}) == pair::Gate::apply);
    CHECK (pair::gateFor ("window", &nodeSet, { s ("/godot/audio/outputDevice"), s ("Speakers") }) == pair::Gate::apply);
    CHECK (pair::gateFor ("window", &nodeSet, { s ("/godot/cue/B3N8R5TW/name"), s ("Thunder") }) == pair::Gate::passOn);

    //  A command nobody registered: by its origin alone.
    CHECK (pair::gateFor ("ws:10.0.0.5:51000", nullptr, {}) == pair::Gate::passOn);
    CHECK (pair::gateFor ("udp:10.0.0.9:9000", nullptr, {}) == pair::Gate::ignore);
}

TEST_CASE ("follow: the gate in front of the engine - taken while following, and every event applied when not")
{
    Engine engine;
    engine.log().openInMemory ({});
    engine.commands().add (plain ("go"));
    engine.commands().add (plain ("node.set"));
    engine.commands().add (plain ("audio.configure", true));

    std::vector<std::vector<std::uint8_t>> passed;
    auto pathOpen = true;

    pair::FollowGate gate { engine.commands(), [&] (std::vector<std::uint8_t> bytes)
                            {
                                if (! pathOpen)
                                    return false;

                                passed.push_back (std::move (bytes));
                                return true;
                            } };
    engine.setGate ([&gate] (Event& event) { return gate.take (event); });

    const std::vector<std::string> clients { "window", "cli", "pd", "ws:10.0.0.5:51000", "surface:K4QW8YSC" };
    const std::vector<std::string> devices { "udp:10.0.0.9:9000", "midi:P0000001", "mount:K3PV7WRB",
                                             "serial:S0000001", "link:E0HQDX01", "clock" };
    const std::vector<std::string> here { "engine", "main:cli", "replay" };

    const auto submitAll = [&]
    {
        for (const auto& from : clients)
            CHECK (engine.submit (from, "go", { s (from) }));

        for (const auto& from : devices)
            CHECK (engine.submit (from, "go", { s (from) }));

        for (const auto& from : here)
            CHECK (engine.submit (from, "go", { s (from) }));

        CHECK (engine.submit ("ws:10.0.0.5:51000", "audio.configure", { s ("machine's own") }));
    };

    SUBCASE ("not following: everything is applied, nothing passed on or ignored")
    {
        CHECK_FALSE (engine.isGated());
        submitAll();
        engine.processTick (1);

        std::string seen;
        for (const auto& [from, command] : written (engine))
            seen += from + " " + command + "; ";

        INFO (seen);
        CHECK (written (engine).size() == clients.size() + devices.size() + here.size() + 1);
        CHECK (passed.empty());
        CHECK (gate.passedOn() == 0);
        CHECK (gate.ignored() == 0);
    }

    SUBCASE ("following: the clients' passed on, the devices' ignored, the rest applied")
    {
        engine.setGated (true);
        submitAll();
        engine.processTick (1);

        std::vector<std::pair<std::string, std::string>> expected;

        for (const auto& from : here)
            expected.emplace_back (from, "go");

        expected.emplace_back ("ws:10.0.0.5:51000", "audio.configure");
        CHECK (written (engine) == expected);

        CHECK (gate.passedOn() == static_cast<std::int64_t> (clients.size()));
        CHECK (gate.ignored() == static_cast<std::int64_t> (devices.size()));
        CHECK (gate.lost() == 0);

        /*  EACH RELAY READS BACK AS THE EVENT IT WAS: one message to
            /godot/pair/relay carrying the line of an applied record, the
            client's origin and its arguments, as the main's stream reads it. */
        REQUIRE (passed.size() == clients.size());

        for (std::size_t i = 0; i < clients.size(); ++i)
        {
            const auto decoded = osc::decode (passed[i].data(), passed[i].size());
            REQUIRE (decoded.ok);
            REQUIRE (decoded.packet.address == pair::wire::relay);
            REQUIRE (decoded.packet.args.size() == 1u);

            const auto line = LogRecord::fromLine (decoded.packet.args[0].getString());
            REQUIRE (line.has_value());
            CHECK (line->kind == LogRecord::Kind::applied);
            CHECK (line->origin == clients[i]);
            CHECK (line->command == "go");
            CHECK (line->args == std::vector<osc::Value> { s (clients[i]) });
        }

        SUBCASE ("and with no path to the main, a client's command is lost and counted, never applied here")
        {
            pathOpen = false;
            CHECK (engine.submit ("window", "go", { s ("lost") }));
            engine.processTick (2);
            CHECK (gate.lost() == 1);
            CHECK (written (engine).size() == expected.size());
        }

        SUBCASE ("and open again: applied, every one")
        {
            engine.setGated (false);
            CHECK (engine.submit ("udp:10.0.0.9:9000", "go"));
            CHECK (engine.submit ("window", "go"));
            engine.processTick (2);
            CHECK (written (engine).size() == expected.size() + 2);
        }
    }
}

TEST_CASE ("follow: a relay reaches a main as peer:<origin>, and a machine that does not lead ignores it")
{
    pair::Stream stream { "S1" };
    std::vector<LogRecord> relayed;
    stream.setRelayed ([&relayed] (const LogRecord& record) { relayed.push_back (record); });

    Event event { "ws:10.0.0.5:51000", "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s ("a, \"quoted\" note") } };
    const auto bytes = pair::relayOf (event);
    const auto decoded = osc::decode (bytes.data(), bytes.size());
    REQUIRE (decoded.ok);

    stream.received ("ws:10.0.0.2:50001", decoded.packet, t0);
    CHECK (relayed.empty());
    CHECK (stream.ignored() == 1);

    stream.setLeading (true);
    stream.received ("ws:10.0.0.2:50001", decoded.packet, t0);
    REQUIRE (relayed.size() == 1u);
    CHECK (relayed[0].origin == event.origin);
    CHECK (relayed[0].command == event.command);
    CHECK (relayed[0].args == event.args);
    CHECK (stream.relaysTaken() == 1);
}

//==============================================================================
TEST_CASE ("follow: every record of a main tick on one local tick, the main's gaps kept, a late one at once and counted")
{
    std::vector<std::pair<std::int64_t, Event>> submitted;    // (local tick, event)
    std::int64_t localTick = 0;

    pair::Follower follower { [&] (Event event)
                              {
                                  submitted.emplace_back (localTick, std::move (event));
                                  return true;
                              } };

    /*  THE MAIN'S TICK T BEGINS AT t0 + 20 ms T; ITS BEAT ARRIVES 3 TO 9 MS
        LATER. The backup's tick L is due at t0 + 20 ms (L - 500) + 11 ms: its
        clock started 500 ticks after the main's, and is 11 ms out of phase. */
    const auto mainAt = [] (std::int64_t mainTick) { return t0 + 20ms * mainTick; };
    const auto dueAt = [] (std::int64_t local) { return t0 + 20ms * (local - 500) + 11ms; };

    follower.start();
    CHECK (follower.isFollowing());

    //  The records come before the delay is fixed: they wait for it.
    follower.record (mainRecord (3, "node.set", { s ("/godot/list/standby"), s ("B3N8R5TW") }));

    std::int64_t mainTick = 0;

    for (localTick = 500; localTick < 520; ++localTick)
    {
        while (mainAt (mainTick) + 9ms <= dueAt (localTick))
        {
            follower.beat (mainTick, mainAt (mainTick) + (mainTick % 2 == 0 ? 3ms : 9ms));
            ++mainTick;
        }

        follower.beforeTick (localTick, dueAt (localTick), 0.0004);
    }

    REQUIRE (follower.delayFixed());

    /*  e = arrival - main tick, in local ticks: 500 + (3..9 ms - 11 ms) / 20 ms,
        so 499.6 to 499.9 - the latest seen is 499.9, a slot a tick after it
        is 501, and Δ is 501 less the clocks' 499.6 (less half a 0.4 ms trip),
        to the nearest tick: 1. */
    CHECK (follower.offset() == 501);
    CHECK (follower.delay() == 1);
    CHECK (follower.spread() == doctest::Approx (0.3).epsilon (0.01));

    //  The record of tick 3 had its slot at 504, long gone: it went at once, late.
    REQUIRE (submitted.size() == 1u);
    CHECK (follower.late() == 1);
    CHECK (submitted[0].second.origin == "main:cli");

    submitted.clear();

    //  Three records of tick 30, one of 31, one of 55: slots 531, 532, 556.
    follower.record (mainRecord (30, "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s ("one") }));
    follower.record (mainRecord (30, "go"));
    follower.record (mainRecord (30, "go"));
    follower.record (mainRecord (31, "go.doh"));
    follower.record (mainRecord (55, "go"));

    for (localTick = 520; localTick < 560; ++localTick)
        follower.beforeTick (localTick, dueAt (localTick), 0.0004);

    REQUIRE (submitted.size() == 5u);
    CHECK (submitted[0].first == 531);
    CHECK (submitted[1].first == 531);
    CHECK (submitted[2].first == 531);
    CHECK (submitted[3].first == 532);
    CHECK (submitted[4].first == 556);
    CHECK (submitted[0].second.command == "node.set");
    CHECK (submitted[3].second.command == "go.doh");
    CHECK (follower.late() == 1);

    SUBCASE ("a record that arrives after its slot goes in at once, with the rest of its tick")
    {
        submitted.clear();
        follower.record (mainRecord (40, "go"));    // slot 541, and the backup is at 560
        follower.record (mainRecord (40, "go.doh"));
        follower.record (mainRecord (80, "go"));    // slot 581

        localTick = 560;
        follower.beforeTick (localTick, dueAt (localTick), 0.0004);

        REQUIRE (submitted.size() == 2u);
        CHECK (submitted[0].first == 560);
        CHECK (submitted[1].first == 560);
        CHECK (follower.late() == 3);

        for (localTick = 561; localTick < 590; ++localTick)
            follower.beforeTick (localTick, dueAt (localTick), 0.0004);

        REQUIRE (submitted.size() == 3u);
        CHECK (submitted[2].first == 581);
    }

    SUBCASE ("the link lost: what waits goes in at once, and nothing more is held")
    {
        submitted.clear();
        follower.record (mainRecord (70, "go"));
        follower.stop();

        CHECK_FALSE (follower.isFollowing());
        CHECK (submitted.size() == 1u);
        CHECK (follower.waiting() == 0u);
    }
}

TEST_CASE ("follow: Δ is the beats' spread and a tick's margin - more on a network that jitters more")
{
    /*  THE BACKUP'S TICK L IS DUE 10 MS AFTER THE MAIN'S TICK L - 100 BEGAN,
        and a beat leaves the main `lag` after its tick began: read as a tick
        of the backup's clock it arrives at 100 + (lag - 10 ms) / 20 ms past
        the main's tick. */
    const auto measure = [] (std::chrono::milliseconds earliest, std::chrono::milliseconds latest, double roundTrip)
    {
        pair::Follower follower { [] (Event) { return true; } };
        follower.start();

        for (std::int64_t local = 100; local < 140; ++local)
        {
            const auto mainTick = local - 101;
            const auto lag = mainTick % 2 == 0 ? earliest : latest;
            follower.beat (mainTick, t0 + 20ms * mainTick + lag);
            follower.beforeTick (local, t0 + 20ms * (local - 100) + 10ms, roundTrip);
        }

        REQUIRE (follower.delayFixed());
        return std::make_pair (follower.offset(), follower.delay());
    };

    /*  A quiet network, 1 to 3 ms: arrivals at 99.55 to 99.65, the slot a tick
        after the latest - 101 - and Δ 101 less 99.55 and a quarter of a
        millisecond's half trip, to the nearest tick: one. */
    CHECK (measure (1ms, 3ms, 0.0005) == std::make_pair (std::int64_t { 101 }, 1));

    //  1 to 15 ms: the latest at 100.25, so the slot is 102, and Δ two.
    CHECK (measure (1ms, 15ms, 0.0005) == std::make_pair (std::int64_t { 102 }, 2));

    //  A network that sometimes takes 32 ms: 99.6 to 101.1, slot 103, Δ three.
    CHECK (measure (2ms, 32ms, 0.002) == std::make_pair (std::int64_t { 103 }, 3));

    //  No round trip yet: the offset waits for three times the beats, then is fixed without one.
    pair::Follower patient { [] (Event) { return true; } };
    patient.start();

    for (std::int64_t local = 100; local < 115; ++local)
    {
        patient.beat (local - 101, t0 + 20ms * (local - 101) + 2ms);
        patient.beforeTick (local, t0 + 20ms * (local - 100) + 10ms, 0.0);
    }

    CHECK_FALSE (patient.delayFixed());

    for (std::int64_t local = 115; local < 140; ++local)
    {
        patient.beat (local - 101, t0 + 20ms * (local - 101) + 2ms);
        patient.beforeTick (local, t0 + 20ms * (local - 100) + 10ms, 0.0);
    }

    CHECK (patient.delayFixed());
    CHECK (patient.offset() == 101);
    CHECK (patient.delay() == 1);
}

TEST_CASE ("follow: a record applied otherwise than on the main is counted and said, the first in a sentence")
{
    pair::Follower follower { [] (Event) { return true; } };
    follower.start();

    for (std::int64_t local = 100; local < 115; ++local)
    {
        follower.beat (local - 101, t0 + 20ms * (local - 101) + 2ms);
        follower.beforeTick (local, t0 + 20ms * (local - 100), 0.001);
    }

    REQUIRE (follower.delayFixed());

    auto taken = mainRecord (20, "go", { s ("RUN00001") });
    auto refused = mainRecord (20, "go");
    refused.kind = LogRecord::Kind::rejected;
    refused.reason = "too-soon";

    follower.record (taken);
    follower.record (refused);
    follower.record (taken);

    const auto slot = 20 + follower.offset();
    follower.beforeTick (slot, t0 + 20ms * (slot - 100), 0.001);
    REQUIRE (follower.waiting() == 0u);

    //  As the backup writes them: the first alike, the second applied, the third with another run.
    auto first = taken;
    first.origin = "main:cli";
    follower.applied (first);
    CHECK (follower.differences() == 0);
    CHECK (follower.differs().empty());

    //  A record that is not the main's is no business of the comparison.
    follower.applied (mainRecord (121, "run.ended", { s ("RUN00001") }, "engine"));
    CHECK (follower.differences() == 0);

    auto second = taken;
    second.origin = "main:cli";
    follower.applied (second);
    CHECK (follower.differences() == 1);
    CHECK (follower.differs() == "1 record applied otherwise than on the main; the first, go at the main's tick 20:"
                                 " refused there (too-soon), applied here");

    auto third = taken;
    third.origin = "main:cli";
    third.args = { s ("RUN00009") };
    follower.applied (third);
    CHECK (follower.differences() == 2);
    CHECK (follower.differs().rfind ("2 records applied otherwise than on the main; the first, go at the main's tick 20", 0) == 0);
}

//==============================================================================
/*  THE UNDO DEBT RD.3 LEFT (§58.7): an engine record that writes nothing to the
    show - `audio.armed`, whose moment is when THIS machine's media was ready -
    ends nobody's drag; one that writes the show is a step of its own. */
TEST_CASE ("follow: an engine record that writes nothing ends no drag; one that writes the show is a step of its own")
{
    Engine engine;
    engine.log().openInMemory ({});
    doc::ShowDocument document;
    doc::registerDocumentCommands (engine.commands(), document);

    engine.commands().add ({ "audio.armed", "The engine's report, writing nothing.", { { "run", 's', false } }, false,
                             [] (CommandContext&, const std::vector<osc::Value>& args) { return Outcome::ok (args); } });

    engine.setBeforeApply ([&document] (const Command& command, const Event& event,
                                        const std::vector<osc::Value>& args, std::int64_t tick)
                           { cue::beginUndoStep (document, nullptr, command, event, args, tick); });

    const auto listId = document.createList ("Sound").id;
    const auto cueId = document.createCue (listId, 0, "memo", "Lights").id;
    const auto preWait = "/godot/cue/" + cueId + "/preWait";

    std::int64_t tick = 10;
    const auto drag = [&] (double seconds)
    {
        REQUIRE (engine.submit ("cli", "node.set", { s (preWait), osc::Value::float64 (seconds) }));
        engine.processTick (tick++);
    };

    const auto history = [&document] { return document.history (doc::UndoDomain::document).getUndoDescriptions(); };
    const auto before = history();

    drag (0.1);
    drag (0.2);

    REQUIRE (engine.submit (origin::engine, "audio.armed", { s ("RUN00001") }));
    engine.processTick (tick++);

    //  Refused, too - a report for a run that is gone - and still nothing.
    REQUIRE (engine.submit (origin::engine, "audio.armed", {}));
    engine.processTick (tick++);

    drag (0.3);
    drag (0.4);

    //  ONE STEP for the whole drag.
    CHECK (history().size() == before.size() + 1);

    SUBCASE ("and one undo takes the whole drag back")
    {
        REQUIRE (document.undo (doc::UndoDomain::document).has_value());
        CHECK (document.getAttribute (preWait) == std::optional<std::string> ("0"));
    }

    SUBCASE ("an engine write to the show is a step of its own, after the operator's")
    {
        drag (0.5);
        CHECK (history().size() == before.size() + 1);

        REQUIRE (engine.submit (origin::engine, "node.set", { s ("/godot/cue/" + cueId + "/notes"), s ("by the engine") }));
        engine.processTick (tick++);

        CHECK (history().size() == before.size() + 2);

        //  Taken back alone: the drag stays where the hand left it.
        REQUIRE (document.undo (doc::UndoDomain::document).has_value());
        CHECK (document.getAttribute ("/godot/cue/" + cueId + "/notes") == std::optional<std::string> (""));
        CHECK (document.getAttribute (preWait) == std::optional<std::string> ("0.5"));

        REQUIRE (document.undo (doc::UndoDomain::document).has_value());
        CHECK (document.getAttribute (preWait) == std::optional<std::string> ("0"));
    }
}

//==============================================================================
namespace
{
    /*  The audio side of a machine with nobody listening: arms complete when the
        test says, nothing else happens by itself (GoTests' FakePlayer, cut down
        to what a run table needs). */
    struct QuietPlayer final : cue::Player
    {
        int trackCount() const override                     { return 8; }
        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }
        int slotCount() const override                      { return 1; }
        bool launchAtSample (int, int, std::int64_t) override { return true; }
        bool stop (int) override                            { return true; }
        bool stopAtSample (int, int, std::int64_t) override { return true; }
        void setLevelDb (int, double) override              {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                 { return false; }
        bool isArmReady (int track) const override          { return ready.count (track) > 0; }
        std::int64_t samplesElapsed() const override        { return samples; }
        int blockSize() const override                      { return 128; }
        int sampleRate() const override                     { return 48000; }
        int channelsPerTrack() const override               { return 2; }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed", { s (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        std::vector<cue::ArmRequest> arms;
        std::set<int> ready;
        std::int64_t samples = 0;
    };

    /*  ONE MACHINE: its own engine, show, run table and sound, the show read
        from one text, the undo rule the serve's (cue::beginUndoStep). */
    struct Machine
    {
        explicit Machine (const std::string& showText)
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::CanonicalXml::read (showText, document).ok);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);

            engine.setBeforeApply ([this] (const Command& command, const Event& event,
                                           const std::vector<osc::Value>& args, std::int64_t tick)
                                   { cue::beginUndoStep (document, nullptr, command, event, args, tick); });
        }

        void tickOnce (std::int64_t tick)
        {
            audio.samples = tick * 960;
            runner.beforeTick (engine, tick);
            engine.processTick (tick);
            audio.completeArms (engine);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
        QuietPlayer audio;
    };

    struct RunLine
    {
        std::string id, cue, state, parent;
        bool operator== (const RunLine& other) const
        {
            return id == other.id && cue == other.cue && state == other.state && parent == other.parent;
        }
    };

    std::vector<RunLine> linesOf (const cue::RunTable& runs)
    {
        std::vector<RunLine> out;

        for (const auto& run : runs.all())
            out.push_back ({ run.id, run.cue, run.state, run.parent });

        return out;
    }

    /*  THE WIRE AS A TCP CONNECTION IS: in order, each frame arriving no sooner
        than the one before it, a random 0 to 15 ms after it left. */
    struct JitteryWire
    {
        struct Frame
        {
            Clock::time_point at;
            osc::Packet packet;
        };

        explicit JitteryWire (unsigned seed) : random (seed) {}

        pair::Stream::Send send()
        {
            return [this] (const std::string&, const std::vector<std::uint8_t>& bytes)
            {
                const auto decoded = osc::decode (bytes.data(), bytes.size());

                if (! decoded.ok)
                    return false;

                auto at = sentAt + std::chrono::microseconds (jitter (random));
                at = std::max (at, lastArrival);
                lastArrival = at;
                frames.push_back ({ at, decoded.packet });
                return true;
            };
        }

        void deliverUpTo (Clock::time_point now, pair::Receiver& receiver)
        {
            while (! frames.empty() && frames.front().at <= now)
            {
                receiver.deliver ("127.0.0.1:5010", frames.front().packet, frames.front().at);
                frames.pop_front();
            }
        }

        std::mt19937 random;
        std::uniform_int_distribution<int> jitter { 0, 15000 };
        Clock::time_point sentAt {}, lastArrival {};
        std::deque<Frame> frames;
    };
}

TEST_CASE ("follow: two engines, one show, a link with jitter - the backup refuses what the main refused, its runs are the main's a delay later, its undo the main's")
{
    std::string text, listId, thunder, house, preshow, rain, lights;

    {
        doc::ShowDocument show;
        listId = show.createList ("Sound").id;
        thunder = show.createCue (listId, 0, "media", "Thunder").id;
        REQUIRE (show.setAttribute ("/godot/cue/" + thunder + "/file", "thunder.wav").ok);
        house = show.createCue (listId, 1, "memo", "House to half").id;
        preshow = show.createCue (listId, 2, "group", "Preshow").id;
        REQUIRE (show.setAttribute ("/godot/cue/" + preshow + "/advance", "auto").ok);

        for (int n = 0; n < 3; ++n)
            REQUIRE (show.createCue (preshow, n, "memo", "Pre " + std::to_string (n)).ok);

        rain = show.createCue (listId, 3, "media", "Rain").id;
        REQUIRE (show.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);
        lights = show.createCue (listId, 4, "memo", "Lights").id;
        REQUIRE (show.createCue (listId, 5, "memo", "After").ok);

        //  Half a second between GOs, a second for Doh!: the windows the delay must keep.
        REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0.5").ok);
        REQUIRE (show.setAttribute ("/godot/list/dohWindow", "1").ok);

        text = doc::CanonicalXml::write (show);
    }

    Machine main { text }, backup { text };

    /*  ONE SALT, as RD.8 will seat it; each machine's own randomness otherwise,
        the backup's a draw ahead. */
    backup.runIds.drawSeed();
    backup.runner.runNames().setSalt (main.runner.runNames().salt());

    pair::Stream stream { "S1" };
    stream.setLeading (true);
    main.engine.setAfterRecord ([&stream] (const LogRecord& record, const Command* command) { stream.keep (record, command); });

    unsigned seed = 7;
    SUBCASE ("one evening's jitter")    { seed = 7; }
    SUBCASE ("another evening's")       { seed = 2026; }

    JitteryWire wire { seed };
    stream.setSend (wire.send());

    pair::Receiver receiver;
    pair::Follower follower { [&backup] (Event event) { return backup.engine.submit (std::move (event)); } };
    receiver.setOnRecord ([&follower] (std::uint64_t, const LogRecord& record) { follower.record (record); });
    receiver.setOnBeat ([&follower] (std::int64_t mainTick, Clock::time_point at) { follower.beat (mainTick, at); });

    std::vector<std::vector<std::uint8_t>> passedOn;
    pair::FollowGate gate { backup.engine.commands(), [&passedOn] (std::vector<std::uint8_t> bytes)
                            {
                                passedOn.push_back (std::move (bytes));
                                return true;
                            } };
    backup.engine.setGate ([&gate] (Event& event) { return gate.take (event); });
    backup.engine.setAfterRecord ([&follower] (const LogRecord& record, const Command*) { follower.applied (record); });

    stream.received ("ws:127.0.0.1:50001", osc::Packet::message (pair::wire::hello,
                                                                 { s ("backup"), s ("backup"), osc::Value::int64 (0), s ("") }),
                     t0);

    //  THE OPERATOR'S EVENING, on the main's ticks; the backup is never told any of it.
    std::map<std::int64_t, std::vector<Event>> evening;
    const auto at = [&evening] (std::int64_t tick, std::string command, std::vector<osc::Value> args = {})
    {
        evening[tick].push_back ({ "cli", std::move (command), std::move (args) });
    };

    const auto standbyRow = cue::standbyAddressOf (listId);
    std::int64_t t = 60;
    at (t, "node.set", { s (standbyRow), s (thunder) });
    at (t += 5, "go");                                         // Thunder
    at (t += 24, "go");                                        // a tick inside the debounce: refused
    at (t += 1, "go");                                         // and at its edge: House
    at (t += 10, "go.doh");                                    // inside Doh's window: House taken back
    at (t += 3, "go.doh");                                     // inside its own debounce: refused
    at (t += 40, "go");                                        // House again
    at (t += 30, "go");                                        // the automatic sequence, its members the engine's
    at (t += 50, "go.doh");                                    // a second after: too late, refused
    at (t += 30, "go");                                        // Rain
    at (t += 1, "go");                                         // too soon
    at (t += 30, "run.stopAll");                               // Esc

    /*  A DRAG ACROSS THIRTY TICKS on a row of the show, with an engine record
        that writes nothing landing mid-drag on each machine at a moment of its
        own - the main's on the drag's eleventh tick, the backup's on its
        eighteenth - and an undo after it. */
    const auto preWait = "/godot/cue/" + lights + "/preWait";
    const auto dragFrom = t + 40;

    for (int n = 0; n < 30; ++n)
        at (dragFrom + n, "node.set", { s (preWait), osc::Value::float64 (0.01 * (n + 1)) });

    const auto mainArmedAt = dragFrom + 10;
    const auto backupArmedAtMainTick = dragFrom + 17;
    at (dragFrom + 35, "undo");
    const auto lastMainTick = dragFrom + 60;

    //  THE TWO CLOCKS: the backup's counter started 300 ticks later, 7 ms out of phase.
    const auto mainDue = [] (std::int64_t tick) { return t0 + 20ms * tick; };
    const auto backupDue = [] (std::int64_t local) { return t0 + 20ms * (local - 300) + 7ms; };

    std::map<std::int64_t, std::vector<RunLine>> mainRuns;
    std::int64_t mainTick = 0, localTick = 300, compared = 0;
    std::string differs;
    bool backupArmed = false;

    while (mainTick <= lastMainTick + 20)
    {
        if (mainDue (mainTick) <= backupDue (localTick))
        {
            if (const auto due = evening.find (mainTick); due != evening.end())
                for (const auto& event : due->second)
                    REQUIRE (main.engine.submit (event));

            if (mainTick == mainArmedAt)
                main.engine.submit (origin::engine, "audio.armed", { s ("RUN00000"), osc::Value::int32 (0) });

            main.tickOnce (mainTick);
            wire.sentAt = mainDue (mainTick) + 2ms;
            stream.afterTick (mainTick, 1.0);
            mainRuns[mainTick] = linesOf (main.runs);
            ++mainTick;
            continue;
        }

        const auto now = backupDue (localTick);
        wire.deliverUpTo (now, receiver);
        receiver.drain (now, 1.0);

        if (receiver.isLinked (now, 1.0) && ! follower.isFollowing())
        {
            follower.start();
            backup.engine.setGated (true);
        }

        follower.beforeTick (localTick, now, 0.0004);

        if (follower.delayFixed() && ! backupArmed && localTick == backupArmedAtMainTick + follower.offset())
        {
            backup.engine.submit (origin::engine, "audio.armed", { s ("RUN00000"), osc::Value::int32 (0) });
            backupArmed = true;
        }

        backup.tickOnce (localTick);

        if (follower.delayFixed() && differs.empty())
            if (const auto mainSide = mainRuns.find (localTick - follower.offset()); mainSide != mainRuns.end())
            {
                ++compared;

                if (! (linesOf (backup.runs) == mainSide->second))
                    differs = "local tick " + std::to_string (localTick) + " against the main's "
                            + std::to_string (mainSide->first) + ": " + std::to_string (backup.runs.all().size())
                            + " runs against " + std::to_string (mainSide->second.size());
            }

        ++localTick;
    }

    INFO ("offset " << follower.offset() << ", delay " << follower.delay() << ", spread " << follower.spread());
    REQUIRE (follower.delayFixed());
    CHECK (follower.delay() >= 1);
    CHECK (follower.delay() <= 2);
    CHECK (follower.late() == 0);
    CHECK (follower.waiting() == 0u);
    CHECK (backupArmed);

    //  The backup was never told anything by a client of its own, and passed nothing on.
    CHECK (passedOn.empty());

    //  ITS RUNS WERE THE MAIN'S, a delay later, on every tick.
    CHECK (differs == "");
    CHECK (compared > 300);
    CHECK (main.runs.all().size() > 5u);

    /*  IT REFUSED EXACTLY WHAT THE MAIN REFUSED: the operator's records, in
        order, with the same outcome, reason and arguments as applied - a GO
        naming the same runs, an undo the same step. */
    CHECK (follower.differences() == 0);
    CHECK (follower.differs() == "");

    const auto mainLog = LogFile::parse (main.engine.log().contents()).records;
    const auto backupLog = LogFile::parse (backup.engine.log().contents()).records;

    std::vector<LogRecord> operatorRecords, followed;

    for (const auto& record : mainLog)
        if (record.origin == "cli")
            operatorRecords.push_back (record);

    for (const auto& record : backupLog)
        if (record.origin == "main:cli")
            followed.push_back (record);

    REQUIRE (followed.size() == operatorRecords.size());

    std::multiset<std::string> refusals;

    for (std::size_t i = 0; i < followed.size(); ++i)
    {
        INFO ("record " << i << ": " << operatorRecords[i].toLine() << " / " << followed[i].toLine());
        CHECK (followed[i].kind == operatorRecords[i].kind);
        CHECK (followed[i].reason == operatorRecords[i].reason);
        CHECK (followed[i].command == operatorRecords[i].command);
        CHECK (followed[i].args == operatorRecords[i].args);
        CHECK (followed[i].tick == operatorRecords[i].tick + follower.offset());

        if (operatorRecords[i].kind == LogRecord::Kind::rejected)
            refusals.insert (operatorRecords[i].command + " " + operatorRecords[i].reason);
    }

    //  And there were refusals to agree on: a GO too soon, twice, and Doh! too soon and too late.
    CHECK (refusals == std::multiset<std::string> { "go too-soon", "go too-soon", "go.doh too-soon", "go.doh too-late" });

    //  ITS UNDO WAS THE MAIN'S: the whole drag one step, taken back on both, the same history left.
    CHECK (main.document.getAttribute (preWait) == std::optional<std::string> ("0"));
    CHECK (backup.document.getAttribute (preWait) == main.document.getAttribute (preWait));
    CHECK (backup.document.history (doc::UndoDomain::document).getUndoDescriptions()
           == main.document.history (doc::UndoDomain::document).getUndoDescriptions());
    CHECK (backup.document.history (doc::UndoDomain::document).getRedoDescriptions()
           == main.document.history (doc::UndoDomain::document).getRedoDescriptions());
}

//==============================================================================
TEST_CASE ("follow: what following costs a backup's tick - sixty-four of the main's records, and an idle tick")
{
    /*  WHAT A FOLLOWING BACKUP'S TICK GAINS: the main's records due this tick
        made into events and submitted through the gate - one acquire load and
        an origin's first letters each - and, as each is written, compared with
        the main's. Sixty-four in one tick, the busy case; and the tick with
        nothing due, every other one. The engine's own work on the records is
        the main's too and is not counted. The check is loose, for a runner
        busy with other suites; the figure is the measurement. */
    Engine engine;
    engine.commands().add (plain ("go"));
    engine.commands().add (plain ("node.set"));

    pair::FollowGate gate { engine.commands(), [] (std::vector<std::uint8_t>) { return true; } };
    engine.setGate ([&gate] (Event& event) { return gate.take (event); });
    engine.setGated (true);

    pair::Follower follower { [&engine] (Event event) { return engine.submit (std::move (event)); } };
    engine.setAfterRecord ([&follower] (const LogRecord& record, const Command*) { follower.applied (record); });

    follower.start();

    for (std::int64_t local = 100; local < 115; ++local)
    {
        follower.beat (local - 101, t0 + 20ms * (local - 101) + 2ms);
        follower.beforeTick (local, t0 + 20ms * (local - 100), 0.001);
    }

    REQUIRE (follower.delayFixed());

    std::vector<double> busy, compare, idle;
    std::int64_t mainTick = 20;

    for (int round = 0; round < 30; ++round, mainTick += 2)
    {
        for (int i = 0; i < 64; ++i)
            follower.record (i % 2 == 0 ? mainRecord (mainTick, "go", { s ("R8M2K4Q1"), s ("R8M2K4Q2") }, "udp:10.0.0.9:9000")
                                        : mainRecord (mainTick, "node.set", { s ("/godot/cue/B3N8R5TW/name"),
                                                                              s ("Walk-in, house to half") },
                                                      "ws:192.168.1.20:53211"));

        const auto slot = mainTick + follower.offset();
        const auto a = std::chrono::steady_clock::now();
        follower.beforeTick (slot, t0 + 20ms * (slot - 100), 0.001);
        const auto b = std::chrono::steady_clock::now();
        engine.processTick (slot);
        const auto c = std::chrono::steady_clock::now();
        follower.beforeTick (slot + 1, t0 + 20ms * (slot + 1 - 100), 0.001);
        const auto d = std::chrono::steady_clock::now();
        engine.processTick (slot + 1);

        busy.push_back (std::chrono::duration<double, std::micro> (b - a).count());
        compare.push_back (std::chrono::duration<double, std::micro> (c - b).count());
        idle.push_back (std::chrono::duration<double, std::micro> (d - c).count());
    }

    std::sort (busy.begin(), busy.end());
    std::sort (compare.begin(), compare.end());
    std::sort (idle.begin(), idle.end());

    MESSAGE ("64 of the main's records submitted: median " << busy[busy.size() / 2] << " us, best " << busy.front()
             << " us; the tick that applies and compares them (the engine's own work included): median "
             << compare[compare.size() / 2] << " us; an idle tick: median " << idle[idle.size() / 2] << " us");

    CHECK (follower.differences() == 0);
    CHECK (follower.late() == 0);
    CHECK (follower.waiting() == 0u);
    CHECK (busy[busy.size() / 2] < 5000.0);
}
