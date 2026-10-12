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
    A timecode cue's run, with no output yet (namespace draft §59, TC.1): what
    a replay sees. Fired, it plays; once its code is placed it counts frames on
    the tick, at the rate the show said when it fired; its length ends it on the
    tick its last frame is over; Esc and a double Esc end it as they end any
    run; and a second run of the same format takes its output over (AHC, AHL).

    No audio side and no MIDI side: the engine, the document, the run table and
    the Runner's hook, wired as `serve --no-audio` wires them. The frames
    expected are worked out by hand from the rate - at 25 a frame is two ticks
    of fifty a second - not by the code under test.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    struct TimecodeRig
    {
        TimecodeRig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            listId = document.createList ("Show").id;
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);
        }

        /*  A timecode cue at the end of `parent` (the list when empty), with
            what it is born with. */
        std::string makeTimecode (const std::string& format, const std::string& from = {},
                                  const std::string& length = {}, const std::string& parent = {})
        {
            doc::ShowDocument::Attributes born { { "format", format } };

            if (! from.empty())
                born.emplace_back ("from", from);

            if (! length.empty())
                born.emplace_back ("length", length);

            const auto made = document.createCue (parent.empty() ? listId : parent,
                                                  parent.empty() ? index++ : childIndex++,
                                                  "timecode", {}, {}, born);
            REQUIRE (made.ok);
            return made.id;
        }

        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
        }

        void submit (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            tickOnce();
        }

        void fire (const std::string& cueId)
        {
            submit ("cue.fire", { osc::Value::string (cueId) });
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            const cue::Run* last = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    last = &run;

            return last;
        }

        bool tickUntil (const std::function<bool()>& done, int ticks = 300)
        {
            for (int n = 0; n < ticks; ++n)
            {
                if (done())
                    return true;

                tickOnce();
            }

            return done();
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (59);
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId;
        int index = 0;
        int childIndex = 0;
        std::int64_t tick = 1;
    };

    /*  The label `frames` past `start`, by hand: two digits each, colons. */
    std::string label (int hours, int minutes, int seconds, int frames, char beforeFrames = ':')
    {
        const auto two = [] (int value) { return std::string (value < 10 ? "0" : "") + std::to_string (value); };
        return two (hours) + ":" + two (minutes) + ":" + two (seconds) + beforeFrames + two (frames);
    }
}

TEST_CASE ("timecode run: fired, it plays and reads its start, then counts a frame every two ticks at 25")
{
    TimecodeRig rig;
    const auto cue = rig.makeTimecode ("ltc");

    rig.fire (cue);
    const auto* run = rig.runOf (cue);
    REQUIRE (run != nullptr);
    CHECK (run->kind == "timecode");
    CHECK (run->state == cue::runState::playing);
    CHECK (run->timecode == "01:00:00:00");

    //  Placed on the next tick: nought frames, then one every second tick.
    const auto placedAt = rig.tick;

    for (int elapsed = 0; elapsed <= 120; ++elapsed)
    {
        rig.tickOnce();
        run = rig.runOf (cue);
        REQUIRE (run != nullptr);

        const auto frames = elapsed / 2;            // 25 frames a second, 50 ticks a second
        INFO ("tick " << placedAt + elapsed << ", " << elapsed << " ticks after it was placed");
        CHECK (run->timecode == label (1, 0, frames / 25, frames % 25));
        CHECK (run->state == cue::runState::playing);
    }

    //  And its position is seconds since it was placed.
    CHECK (run->position > 2.39);
    CHECK (run->position < 2.41);
}

TEST_CASE ("timecode run: the rate is the show's when it fires, and drop-frame reads with a semicolon")
{
    TimecodeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/sync/frameRate", "29.97df").ok);
    const auto cue = rig.makeTimecode ("mtc", "00:00:59:00");

    rig.fire (cue);
    CHECK (rig.runOf (cue)->timecode == label (0, 0, 59, 0, ';'));

    /*  A change of rate after the GO reaches the next run, never this one
        (AHM): the receiver is locked to what it started at. */
    REQUIRE (rig.document.setAttribute ("/godot/sync/frameRate", "25").ok);

    rig.tickOnce();                                 // placed
    const auto placedAt = rig.tick - 1;

    /*  Fifty ticks are one second, and at 30000/1001 frames a second that is
        29.97 frames - 29 whole ones; the 30th lands on the 51st tick, and
        00:00:59;29 + 1 is 00:01:00;02 at drop-frame. */
    while (rig.tick <= placedAt + 50)
        rig.tickOnce();

    CHECK (rig.runOf (cue)->timecode == label (0, 0, 59, 29, ';'));

    rig.tickOnce();
    CHECK (rig.runOf (cue)->timecode == label (0, 1, 0, 2, ';'));

    //  A label the rate lacks starts at the next one there is (AHF).
    TimecodeRig other;
    REQUIRE (other.document.setAttribute ("/godot/sync/frameRate", "29.97df").ok);
    const auto lacking = other.makeTimecode ("ltc", "00:01:00:00");
    other.fire (lacking);
    CHECK (other.runOf (lacking)->timecode == label (0, 1, 0, 2, ';'));
}

TEST_CASE ("timecode run: a length of two seconds ends the run done on the tick its last frame is over")
{
    TimecodeRig rig;
    const auto cue = rig.makeTimecode ("ltc", "10:00:00:00", "2");

    rig.fire (cue);
    rig.tickOnce();                                 // placed
    const auto placedAt = rig.tick - 1;

    //  Fifty frames at 25 are a hundred ticks: playing through the 99th.
    while (rig.tick < placedAt + 100)
    {
        rig.tickOnce();
        REQUIRE (rig.runOf (cue)->state == cue::runState::playing);
    }

    CHECK (rig.runOf (cue)->timecode == label (10, 0, 1, 24));

    rig.tickOnce();                                 // tick placedAt + 100
    const auto* run = rig.runOf (cue);
    CHECK (run->state == cue::runState::done);
    CHECK (run->endedAtTick == placedAt + 100);

    //  The last frame a reader saw stays on the run.
    CHECK (run->timecode == label (10, 0, 1, 24));

    /*  One ending, recorded: a `run.ended` from the engine, once. */
    const auto records = LogFile::parse (rig.engine.log().contents()).records;
    CHECK (std::count_if (records.begin(), records.end(), [&run] (const LogRecord& record)
    {
        return record.command == "run.ended" && ! record.args.empty() && record.args.front().getString() == run->id;
    }) == 1);

    //  Rounded up to whole frames: 2 s at 29.97 is 59.94 frames, so 60.
    TimecodeRig ntsc;
    REQUIRE (ntsc.document.setAttribute ("/godot/sync/frameRate", "29.97").ok);
    const auto other = ntsc.makeTimecode ("mtc", {}, "2");
    ntsc.fire (other);
    ntsc.tickOnce();
    const auto otherPlaced = ntsc.tick - 1;

    /*  The 60th frame is over when 60 * 1001 / 30000 s have run - 2.002 s,
        which the tick reaches at its 101st: 101 * 30000 / 50050 = 60.5. */
    while (ntsc.tick < otherPlaced + 101)
    {
        ntsc.tickOnce();
        REQUIRE (ntsc.runOf (other)->state == cue::runState::playing);
    }

    ntsc.tickOnce();
    CHECK (ntsc.runOf (other)->state == cue::runState::done);
    CHECK (ntsc.runOf (other)->timecode == label (1, 0, 1, 29));
}

TEST_CASE ("timecode run: Esc ends it, and the footer of its group runs after it")
{
    TimecodeRig rig;
    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);

    const auto code = rig.makeTimecode ("ltc", {}, {}, scene);
    const auto footer = rig.document.createRole (scene, "footer");
    REQUIRE (footer.ok);
    const auto after = rig.document.createCue (footer.id, 0, "memo", "Lights down").id;
    REQUIRE (! after.empty());

    rig.fire (scene);
    REQUIRE (rig.tickUntil ([&] { const auto* run = rig.runOf (code); return run != nullptr && run->state == cue::runState::playing; }, 10));

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (code)->state == cue::runState::playing);
    CHECK (rig.runOf (after) == nullptr);

    rig.submit ("run.stopAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code)->isFinished(); }, 5));
    CHECK (rig.runOf (code)->state == cue::runState::done);

    //  Normal completion entered early (§4.4): the footer runs.
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (after) != nullptr; }, 20));
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (scene)->isFinished(); }, 50));
}

TEST_CASE ("timecode run: a double Esc ends it at once, and no footer runs")
{
    TimecodeRig rig;
    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);

    const auto code = rig.makeTimecode ("mtc", {}, {}, scene);
    const auto footer = rig.document.createRole (scene, "footer");
    REQUIRE (footer.ok);
    const auto after = rig.document.createCue (footer.id, 0, "memo", "Lights down").id;

    const auto alone = rig.makeTimecode ("ltc");

    rig.fire (scene);
    rig.fire (alone);
    REQUIRE (rig.tickUntil ([&] { const auto* run = rig.runOf (code); return run != nullptr && run->state == cue::runState::playing; }, 10));

    rig.submit ("run.killAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code)->isFinished() && rig.runOf (alone)->isFinished(); }, 3));
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (scene)->isFinished(); }, 10));

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (after) == nullptr);
}

TEST_CASE ("timecode run: a second LTC cue takes the output over when it starts, and MTC beside it does not")
{
    TimecodeRig rig;
    const auto first = rig.makeTimecode ("ltc", "01:00:00:00");
    const auto second = rig.makeTimecode ("ltc", "02:00:00:00");
    const auto midi = rig.makeTimecode ("mtc", "03:00:00:00");

    rig.fire (first);
    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    //  MTC beside it: a different output, no takeover.
    rig.fire (midi);
    rig.tickOnce();
    CHECK (rig.runOf (first)->state == cue::runState::playing);
    CHECK (rig.runOf (midi)->state == cue::runState::playing);

    rig.fire (second);
    CHECK (rig.runOf (first)->state == cue::runState::playing);    // not at its GO (AHL)

    const auto placedAt = rig.tick;
    rig.tickOnce();                                                 // the second is placed, the first ends

    CHECK (rig.runOf (second)->state == cue::runState::playing);
    CHECK (rig.runOf (second)->timecode == "02:00:00:00");
    CHECK (rig.runOf (first)->state == cue::runState::done);
    CHECK (rig.runOf (first)->endedAtTick == placedAt);
    CHECK (rig.runOf (midi)->state == cue::runState::playing);

    //  The first keeps where it stopped; the second and the MTC count on.
    const auto stoppedAt = rig.runOf (first)->timecode;
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (first)->timecode == stoppedAt);
    CHECK (rig.runOf (second)->timecode == "02:00:00:05");
    CHECK (rig.runOf (midi)->state == cue::runState::playing);

    //  The same cue fired again is a second run of its format, and takes over too.
    rig.fire (second);
    rig.tickOnce();

    std::vector<const cue::Run*> seconds;
    for (const auto& run : rig.runs.all())
        if (run.cue == second)
            seconds.push_back (&run);

    REQUIRE (seconds.size() == 2u);
    CHECK (seconds[0]->state == cue::runState::done);
    CHECK (seconds[1]->state == cue::runState::playing);
}

TEST_CASE ("timecode run: a replay of the session reaches the same runs, ended on the same ticks")
{
    TimecodeRig rig;
    const auto first = rig.makeTimecode ("ltc", "01:00:00:00");
    const auto second = rig.makeTimecode ("ltc", "02:00:00:00");
    const auto timed = rig.makeTimecode ("mtc", "05:00:00:00", "0.5");
    const auto endless = rig.makeTimecode ("mtc", "06:00:00:00");

    rig.fire (first);
    for (int n = 0; n < 7; ++n)
        rig.tickOnce();

    rig.fire (timed);
    rig.fire (second);
    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    rig.fire (endless);
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    rig.submit ("run.stopAll");
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    for (const auto& cueId : { first, second, timed, endless })
    {
        INFO ("cue " << cueId);
        REQUIRE (rig.runOf (cueId) != nullptr);
        CHECK (rig.runOf (cueId)->state == cue::runState::done);
    }

    /*  The session again from its log alone, with no hook: `wfg replay`'s
        shape. The takeover, the length's end and Esc's sweep are records the
        hook submitted, so the replay applies each where the session did. */
    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    TimecodeRig fresh;
    REQUIRE (doc::CanonicalXml::read (show, fresh.document).ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);

    for (const auto& run : rig.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = fresh.runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->state == run.state);
        CHECK (again->kind == run.kind);
        CHECK (again->endedAtTick == run.endedAtTick);
    }
}
