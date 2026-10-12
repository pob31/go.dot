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
    A timecode cue's run (namespace draft §59). TC.1: what a replay sees, with
    no output. Fired, it plays; once its code is placed it counts frames on
    the tick, at the rate the show said when it fired; its length ends it on the
    tick its last frame is over; Esc and a double Esc end it as they end any
    run; and a second run of the same format takes its output over (AHC, AHL).

    No audio side and no MIDI side: the engine, the document, the run table and
    the Runner's hook, wired as `serve --no-audio` wires them. The frames
    expected are worked out by hand from the rate - at 25 a frame is two ticks
    of fifty a second - not by the code under test.

    TC.4: the same runs with an audio side to draw LTC - a fake one, recording
    what the Runner asks of it, its sample counter at 960 a tick (48 kHz at
    50 Hz) so every sample below is worked out by hand: placed a launch horizon
    ahead on the same sample as the sounds of its group, a length's stop on
    its exact frame boundary, Esc's stop, a double Esc's cut, the takeover, the
    refusals, `not-sent`, an audio restart, and the output it plays on.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Event.h>
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
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  An audio side with LTC in the test's hands: every start, stop and cut
        the Runner posts, with the sample counter when it posted it; the output
        it was told; its generation; a queue that can be full; a host that
        will not draw. A media cue arms on it and is ready when `completeArms`
        says the disk answered. */
    struct CodePlayer final : cue::Player
    {
        struct Start
        {
            std::uint64_t program = 0;
            std::int64_t zero = 0;
            std::int64_t frame = 0;
            timecode::Rate rate = timecode::Rate::fps25;
            std::int64_t postedAt = 0;
        };

        struct Stop
        {
            std::uint64_t program = 0;
            std::int64_t atSample = 0;
            bool cut = false;
            std::int64_t postedAt = 0;
        };

        int trackCount() const override                 { return 4; }
        int slotCount() const override                  { return 1; }
        int sampleRate() const override                 { return 48000; }
        int blockSize() const override                  { return 128; }
        int channelsPerTrack() const override           { return 2; }
        std::int64_t samplesElapsed() const override    { return samples; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t sample) override
        {
            launches.push_back ({ track, sample });
            playing.push_back (track);
            return true;
        }

        bool stop (int track) override                  { std::erase (playing, track); return true; }
        bool stopAtSample (int track, int, std::int64_t) override { std::erase (playing, track); return true; }
        void setLevelDb (int, double) override {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}

        bool isPlaying (int track) const override
        {
            return std::find (playing.begin(), playing.end(), track) != playing.end();
        }

        bool isArmReady (int track) const override
        {
            return std::find (ready.begin(), ready.end(), track) != ready.end();
        }

        bool placeTimecode (std::uint64_t program, std::int64_t zero, std::int64_t frame, timecode::Rate rate) override
        {
            if (! draws)
                return false;

            starts.push_back ({ program, zero, frame, rate, samples });
            return true;
        }

        bool stopTimecode (std::uint64_t program, std::int64_t atSample, bool cut) override
        {
            if (full)
            {
                ++refused;
                return false;
            }

            stops.push_back ({ program, atSample, cut, samples });
            return true;
        }

        void setTimecodeOutput (int channel, double levelDb) override
        {
            outputChannel = channel;
            outputLevelDb = levelDb;
        }

        std::uint64_t timecodeGeneration() const override { return generation; }

        //  The disk answers: every arm outstanding is ready.
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.push_back (arm.track);
            }

            arms.clear();
        }

        std::vector<Stop> stopsOf (std::uint64_t program) const
        {
            std::vector<Stop> out;

            for (const auto& stop : stops)
                if (stop.program == program)
                    out.push_back (stop);

            return out;
        }

        std::int64_t samples = 0;
        std::uint64_t generation = 1;
        bool draws = true;
        bool full = false;
        int refused = 0;

        int outputChannel = -99;
        double outputLevelDb = 0.0;

        std::vector<cue::ArmRequest> arms;
        std::vector<int> ready;
        std::vector<int> playing;
        std::vector<std::pair<int, std::int64_t>> launches;
        std::vector<Start> starts;
        std::vector<Stop> stops;
    };

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

        /*  WITH AN AUDIO SIDE (TC.4), its counter at 960 samples a tick and an
            output for LTC: one channel, after a stereo pair, so the line is
            the show's channel 2. */
        void attach()
        {
            runner.setPlayer (&player);
            runner.setSamplesPerTick (960);

            REQUIRE (document.createBus ("direct", 2).ok);
            const auto line = document.createBus ("direct", 1);
            REQUIRE (line.ok);
            ltcBus = line.id;
            REQUIRE (document.setAttribute ("/godot/sync/ltcOut", ltcBus).ok);
        }

        void tickOnce()
        {
            player.samples = tick * 960;
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
        CodePlayer player;

        std::string listId;
        std::string ltcBus;
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

//==============================================================================
/*  TC.4: LTC ON THE LINE. The lead is two ticks at 128-sample blocks and 960
    samples a tick (`launchLatencyTicks`), so a code whose GO is applied on tick
    `t` is placed by the hook of `t + 1` - as a sound is - on sample
    `(t + 3) * 960`. */
namespace
{
    constexpr std::int64_t samplesPerTick = 960;
    constexpr std::int64_t leadTicks = 2;

    //  The frame of the day 01:00:00:00 is, at 25.
    constexpr std::int64_t oneHourAt25 = 90000;

    /*  How many of the log's records are `command` for `runId`. */
    int recordsOf (TimecodeRig& rig, const std::string& command, const std::string& runId)
    {
        const auto records = LogFile::parse (rig.engine.log().contents()).records;

        return static_cast<int> (std::count_if (records.begin(), records.end(), [&] (const LogRecord& record)
        {
            return record.command == command && ! record.args.empty() && record.args.front().getString() == runId;
        }));
    }

    /*  A timeline scene holding `members`, made by the caller; its id. */
    std::string makeScene (TimecodeRig& rig)
    {
        const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
        return scene;
    }
}

TEST_CASE ("timecode line: an LTC cue fired alone is placed a launch horizon ahead, on the tick after its GO, as a sound is")
{
    TimecodeRig rig;
    rig.attach();
    const auto cue = rig.makeTimecode ("ltc");

    rig.fire (cue);
    const auto goTick = rig.tick - 1;
    CHECK (rig.player.starts.empty());

    rig.tickOnce();
    REQUIRE (rig.player.starts.size() == 1u);

    const auto& start = rig.player.starts.front();
    CHECK (start.program != 0u);
    CHECK (start.postedAt == (goTick + 1) * samplesPerTick);
    CHECK (start.zero == (goTick + 1 + leadTicks) * samplesPerTick);
    CHECK (start.frame == oneHourAt25);
    CHECK (start.rate == timecode::Rate::fps25);

    //  Said as a sound's launch says it, on the tick it was placed.
    const auto* run = rig.runOf (cue);
    CHECK (run->startedAtTick == goTick + 1);
    CHECK (run->warning.empty());

    /*  Read off the line: nought until its first sample, then a frame every
        1920 samples - two ticks - from there. */
    CHECK (run->timecode == "01:00:00:00");

    for (std::int64_t at = goTick + 2; at <= goTick + 60; ++at)
    {
        rig.tickOnce();
        const auto since = (at - goTick - 1 - leadTicks) * samplesPerTick;
        const auto frames = since > 0 ? static_cast<int> (since / 1920) : 0;

        INFO ("tick " << at << ", " << since << " samples into the code");
        CHECK (rig.runOf (cue)->timecode == label (1, 0, frames / 25, frames % 25));
        CHECK (rig.runOf (cue)->position == doctest::Approx (static_cast<double> (std::max<std::int64_t> (since, 0)) / 48000.0));
    }

    //  The show's one-channel output, at the show's level.
    CHECK (rig.player.outputChannel == 2);
    CHECK (rig.player.outputLevelDb == doctest::Approx (-18.0));
    CHECK (rig.player.stops.empty());
}

TEST_CASE ("timecode line: fired with a sound of its group still on its disk, the code waits and starts on the sound's sample")
{
    TimecodeRig rig;
    rig.attach();

    const auto scene = makeScene (rig);
    const auto song = rig.document.createCue (scene, rig.childIndex++, "media", "Song").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + song + "/file", "song.wav").ok);
    const auto code = rig.makeTimecode ("ltc", "10:00:00:00", {}, scene);

    rig.fire (scene);
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code) != nullptr && rig.runOf (song) != nullptr; }, 10));

    //  Cold: the disk has not answered, so neither the sound nor the code starts.
    for (int n = 0; n < 6; ++n)
        rig.tickOnce();

    CHECK (rig.player.launches.empty());
    CHECK (rig.player.starts.empty());
    CHECK (rig.runOf (code)->state == cue::runState::playing);

    //  It answers: both in the same hook, on one sample.
    rig.player.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&] { return ! rig.player.launches.empty(); }, 5));

    REQUIRE (rig.player.launches.size() == 1u);
    REQUIRE (rig.player.starts.size() == 1u);
    CHECK (rig.player.starts.front().zero == rig.player.launches.front().second);
    CHECK (rig.player.starts.front().postedAt == rig.player.starts.front().zero - leadTicks * samplesPerTick);
    CHECK (rig.player.starts.front().frame == 10 * oneHourAt25);
}

TEST_CASE ("timecode line: a sound of its group that fails lets the code start without it")
{
    TimecodeRig rig;
    rig.attach();

    const auto scene = makeScene (rig);
    const auto song = rig.document.createCue (scene, rig.childIndex++, "media", "Song").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + song + "/file", "song.wav").ok);
    const auto code = rig.makeTimecode ("ltc", {}, {}, scene);

    rig.fire (scene);
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (song) != nullptr; }, 10));
    rig.tickOnce();
    CHECK (rig.player.starts.empty());

    rig.submit ("run.failed", { osc::Value::string (rig.runOf (song)->id), osc::Value::string ("media-missing") });
    const auto failedAt = rig.tick - 1;

    rig.tickOnce();
    REQUIRE (rig.player.starts.size() == 1u);
    CHECK (rig.player.starts.front().zero == (failedAt + 1 + leadTicks) * samplesPerTick);
    CHECK (rig.player.launches.empty());
    CHECK (rig.runOf (code)->state == cue::runState::playing);
}

TEST_CASE ("timecode line: a length posts its stop on the first sample of the frame after its last, and ends the run once that has passed")
{
    for (const auto& [rate, word, frames] : { std::tuple { timecode::Rate::fps25, "25", std::int64_t { 50 } },
                                              std::tuple { timecode::Rate::fps2997, "29.97", std::int64_t { 60 } } })
    {
        INFO ("at " << word);

        TimecodeRig rig;
        rig.attach();
        REQUIRE (rig.document.setAttribute ("/godot/sync/frameRate", word).ok);
        const auto cue = rig.makeTimecode ("ltc", "10:00:00:00", "2");

        rig.fire (cue);
        rig.tickOnce();
        REQUIRE (rig.player.starts.size() == 1u);

        const auto& start = rig.player.starts.front();

        /*  By hand: 50 frames at 25 are 96000 samples at 48 kHz; 60 at 29.97
            are 60 * 48000 * 1001 / 30000 = 96096, a whole number. */
        const auto expected = start.zero + (frames == 50 ? 96000 : 96096);
        CHECK (timecode::frameStartSample (frames, 48000, rate) == expected - start.zero);

        const auto stops = rig.player.stopsOf (start.program);
        REQUIRE (stops.size() == 1u);
        CHECK (stops.front().atSample == expected);
        CHECK_FALSE (stops.front().cut);
        CHECK (stops.front().postedAt == start.postedAt);

        //  Playing while the last frame is drawn; done on the tick its end has passed.
        const auto endTick = (expected + samplesPerTick - 1) / samplesPerTick;

        while (rig.tick < endTick)
        {
            rig.tickOnce();
            REQUIRE (rig.runOf (cue)->state == cue::runState::playing);
        }

        rig.tickOnce();
        const auto* run = rig.runOf (cue);
        CHECK (run->state == cue::runState::done);
        CHECK (run->endedAtTick == endTick);
        CHECK (run->timecode == label (10, 0, 1, frames == 50 ? 24 : 29));
        CHECK (recordsOf (rig, "run.ended", run->id) == 1);

        //  Nothing more for the line: its stop was already where it ends.
        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (rig.player.stops.size() == 1u);
    }
}

TEST_CASE ("timecode line: Esc stops the code at its next frame boundary, as soon as can be, and the footer runs")
{
    TimecodeRig rig;
    rig.attach();

    const auto scene = makeScene (rig);
    const auto code = rig.makeTimecode ("ltc", {}, {}, scene);
    const auto footer = rig.document.createRole (scene, "footer");
    REQUIRE (footer.ok);
    const auto after = rig.document.createCue (footer.id, 0, "memo", "Lights down").id;

    rig.fire (scene);
    REQUIRE (rig.tickUntil ([&] { return ! rig.player.starts.empty(); }, 10));

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto program = rig.player.starts.front().program;
    CHECK (rig.player.stopsOf (program).empty());

    rig.submit ("run.stopAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code)->isFinished(); }, 5));
    CHECK (rig.runOf (code)->state == cue::runState::done);

    /*  For sample nought - Esc is decided on the tick, never placed ahead -
        and a stop, not a cut: the audio side ends the code on the next frame
        boundary of the block that takes it. Once. */
    const auto stops = rig.player.stopsOf (program);
    REQUIRE (stops.size() == 1u);
    CHECK (stops.front().atSample == 0);
    CHECK_FALSE (stops.front().cut);

    REQUIRE (rig.tickUntil ([&] { return rig.runOf (after) != nullptr; }, 20));
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (scene)->isFinished(); }, 50));
    CHECK (rig.player.stops.size() == 1u);
}

TEST_CASE ("timecode line: a double Esc cuts the code where it stands, in the press's own drain, and no footer runs")
{
    TimecodeRig rig;
    rig.attach();

    const auto scene = makeScene (rig);
    const auto code = rig.makeTimecode ("ltc", {}, {}, scene);
    const auto footer = rig.document.createRole (scene, "footer");
    REQUIRE (footer.ok);
    const auto after = rig.document.createCue (footer.id, 0, "memo", "Lights down").id;

    rig.fire (scene);
    REQUIRE (rig.tickUntil ([&] { return ! rig.player.starts.empty(); }, 10));
    rig.tickOnce();

    const auto program = rig.player.starts.front().program;
    const auto pressedAt = rig.tick;

    rig.submit ("run.killAll");

    const auto stops = rig.player.stopsOf (program);
    REQUIRE (stops.size() == 1u);
    CHECK (stops.front().cut);
    CHECK (stops.front().atSample == 0);
    CHECK (stops.front().postedAt == pressedAt * samplesPerTick);

    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code)->isFinished() && rig.runOf (scene)->isFinished(); }, 10));

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (after) == nullptr);
    CHECK (rig.player.stopsOf (program).size() == 1u);
}

TEST_CASE ("timecode line: a stop the audio side's queue would not take is posted again until it is taken")
{
    TimecodeRig rig;
    rig.attach();
    const auto cue = rig.makeTimecode ("ltc");

    rig.fire (cue);
    rig.tickOnce();
    REQUIRE (rig.player.starts.size() == 1u);
    const auto program = rig.player.starts.front().program;

    rig.player.full = true;
    rig.submit ("run.stopAll");
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.runOf (cue)->isFinished());
    CHECK (rig.player.refused >= 2);
    CHECK (rig.player.stops.empty());

    rig.player.full = false;
    rig.tickOnce();

    REQUIRE (rig.player.stopsOf (program).size() == 1u);
    CHECK (rig.player.stopsOf (program).front().atSample == 0);

    for (int n = 0; n < 3; ++n)
        rig.tickOnce();

    CHECK (rig.player.stops.size() == 1u);
}

TEST_CASE ("timecode line: a second LTC cue's start takes the line over, with no stop for the first, which ends on that tick")
{
    TimecodeRig rig;
    rig.attach();
    const auto first = rig.makeTimecode ("ltc", "01:00:00:00");
    const auto second = rig.makeTimecode ("ltc", "02:00:00:00");

    rig.fire (first);
    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    rig.fire (second);
    CHECK (rig.runOf (first)->state == cue::runState::playing);

    const auto placedAt = rig.tick;
    rig.tickOnce();

    REQUIRE (rig.player.starts.size() == 2u);
    const auto& taking = rig.player.starts.back();
    CHECK (taking.frame == 2 * oneHourAt25);
    CHECK (taking.zero == (placedAt + leadTicks) * samplesPerTick);
    CHECK (taking.program != rig.player.starts.front().program);

    CHECK (rig.runOf (first)->state == cue::runState::done);
    CHECK (rig.runOf (first)->endedAtTick == placedAt);
    CHECK (rig.runOf (second)->state == cue::runState::playing);

    //  The new start replaces the old code on the line: nothing stops it.
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.player.stops.empty());

    //  And Esc after it stops the code that holds the line now, only.
    rig.submit ("run.stopAll");
    rig.tickOnce();

    REQUIRE (rig.player.stops.size() == 1u);
    CHECK (rig.player.stops.front().program == taking.program);
}

TEST_CASE ("timecode line: an LTC output missing, not an output, or wider than a channel fails the run bad-route, once")
{
    const auto failsWith = [] (const std::function<void (TimecodeRig&)>& misroute, const std::string& why)
    {
        TimecodeRig rig;
        rig.attach();
        misroute (rig);

        const auto cue = rig.makeTimecode ("ltc");
        rig.fire (cue);
        rig.tickOnce();

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        const auto* run = rig.runOf (cue);
        INFO ("the run ended " << run->state << " " << run->error);
        CHECK (run->state == cue::runState::failed);
        CHECK (run->error == why);
        CHECK (rig.player.starts.empty());
        CHECK (recordsOf (rig, "run.failed", run->id) == 1);
    };

    SUBCASE ("none named")
    {
        failsWith ([] (TimecodeRig& rig) { REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", "").ok); },
                   "bad-route");
    }

    SUBCASE ("one the show does not have, written by hand")
    {
        failsWith ([] (TimecodeRig& rig)
                   {
                       auto sync = rig.document.root().getChildWithName ("Sync");
                       REQUIRE (sync.isValid());
                       sync.setProperty ("ltcOut", "Q7QQ7QQ7", nullptr);
                   },
                   "bad-route");
    }

    SUBCASE ("something that is not an output")
    {
        failsWith ([] (TimecodeRig& rig)
                   {
                       auto sync = rig.document.root().getChildWithName ("Sync");
                       REQUIRE (sync.isValid());
                       sync.setProperty ("ltcOut", juce::String (rig.listId), nullptr);
                   },
                   "bad-route");
    }

    SUBCASE ("a stereo output")
    {
        failsWith ([] (TimecodeRig& rig)
                   {
                       const auto wide = rig.document.createBus ("direct", 2);
                       REQUIRE (wide.ok);
                       REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", wide.id).ok);
                   },
                   "bad-route");
    }

    SUBCASE ("a start no door let in, with the output right")
    {
        TimecodeRig rig;
        rig.attach();
        const auto cue = rig.makeTimecode ("ltc");
        rig.document.findById (cue).setProperty ("from", "half past seven", nullptr);

        rig.fire (cue);
        rig.tickOnce();
        rig.tickOnce();

        CHECK (rig.runOf (cue)->state == cue::runState::failed);
        CHECK (rig.runOf (cue)->error == "bad-message");
        CHECK (rig.player.starts.empty());
    }
}

TEST_CASE ("timecode line: with nothing to draw it the run is not-sent, and counts its frames on the tick")
{
    SUBCASE ("no audio side at all")
    {
        TimecodeRig rig;
        const auto cue = rig.makeTimecode ("ltc");

        rig.fire (cue);
        rig.tickOnce();
        const auto placedAt = rig.tick - 1;

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        const auto* run = rig.runOf (cue);
        CHECK (run->state == cue::runState::playing);
        CHECK (run->warning == "not-sent");
        CHECK (run->timecode == label (1, 0, 0, static_cast<int> ((rig.tick - 1 - placedAt) / 2)));
    }

    SUBCASE ("an audio side that will not draw it")
    {
        TimecodeRig rig;
        rig.attach();
        rig.player.draws = false;
        const auto cue = rig.makeTimecode ("ltc");

        rig.fire (cue);
        rig.tickOnce();
        const auto placedAt = rig.tick - 1;

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        const auto* run = rig.runOf (cue);
        CHECK (run->state == cue::runState::playing);
        CHECK (run->warning == "not-sent");
        CHECK (run->startedAtTick == -1);
        CHECK (run->timecode == label (1, 0, 0, static_cast<int> ((rig.tick - 1 - placedAt) / 2)));

        rig.submit ("run.stopAll");
        rig.tickOnce();
        CHECK (rig.runOf (cue)->state == cue::runState::done);
        CHECK (rig.player.stops.empty());
    }
}

TEST_CASE ("timecode line: a stop cue's fade on a timecode run stops it at once, on a frame boundary, and ends both runs")
{
    TimecodeRig rig;
    rig.attach();
    const auto code = rig.makeTimecode ("ltc");
    const auto stop = rig.document.createCue (rig.listId, rig.index++, "transport", "Code out").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stop + "/target", code).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stop + "/verb", "fade").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stop + "/duration", "5").ok);

    rig.fire (code);
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.player.starts.size() == 1u);
    const auto program = rig.player.starts.front().program;

    rig.fire (stop);
    const auto firedAt = rig.tick - 1;

    //  Both: the code, and the cue that stopped it - a fade of nothing held for five seconds.
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (code)->isFinished(); }, 10));
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (stop)->isFinished(); }, 10));
    CHECK (rig.runOf (code)->state == cue::runState::done);
    CHECK (rig.runOf (code)->endedAtTick <= firedAt + 2);
    CHECK (rig.runOf (stop)->state == cue::runState::done);
    CHECK (rig.runOf (stop)->endedAtTick <= firedAt + 2);

    const auto stops = rig.player.stopsOf (program);
    REQUIRE (stops.size() == 1u);
    CHECK (stops.front().atSample == 0);
    CHECK_FALSE (stops.front().cut);
}

TEST_CASE ("timecode line: an audio restart ends the run, as it ends a sound, and posts nothing to the new line")
{
    SUBCASE ("the generator made ready again")
    {
        TimecodeRig rig;
        rig.attach();
        const auto cue = rig.makeTimecode ("ltc");

        rig.fire (cue);
        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        REQUIRE (rig.runOf (cue)->state == cue::runState::playing);

        ++rig.player.generation;
        const auto movedAt = rig.tick;
        rig.tickOnce();

        CHECK (rig.runOf (cue)->state == cue::runState::done);
        CHECK (rig.runOf (cue)->endedAtTick == movedAt);
        CHECK (rig.player.stops.empty());
    }

    SUBCASE ("another player handed in")
    {
        TimecodeRig rig;
        rig.attach();
        const auto cue = rig.makeTimecode ("ltc");

        rig.fire (cue);
        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CodePlayer other;
        rig.runner.setPlayer (&other);
        rig.tickOnce();

        CHECK (rig.runOf (cue)->state == cue::runState::done);
        CHECK (other.stops.empty());
        CHECK (rig.player.stops.empty());

        //  And the new one is told where the code plays.
        CHECK (other.outputChannel == 2);
        rig.runner.setPlayer (&rig.player);
    }
}

TEST_CASE ("timecode line: the output follows an edit of ltcOut and ltcLevel, and is none for a two-channel output")
{
    TimecodeRig rig;
    rig.attach();
    rig.tickOnce();

    CHECK (rig.player.outputChannel == 2);
    CHECK (rig.player.outputLevelDb == doctest::Approx (-18.0));

    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcLevel", "-12").ok);
    rig.tickOnce();
    CHECK (rig.player.outputLevelDb == doctest::Approx (-12.0));

    //  Another one-channel output, after the first: channel 3.
    const auto other = rig.document.createBus ("direct", 1);
    REQUIRE (other.ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", other.id).ok);
    rig.tickOnce();
    CHECK (rig.player.outputChannel == 3);

    const auto wide = rig.document.createBus ("direct", 2);
    REQUIRE (wide.ok);
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", wide.id).ok);
    rig.tickOnce();
    CHECK (rig.player.outputChannel == -1);

    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", "").ok);
    rig.tickOnce();
    CHECK (rig.player.outputChannel == -1);

    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", rig.ltcBus).ok);
    rig.tickOnce();
    CHECK (rig.player.outputChannel == 2);
    CHECK (rig.player.outputLevelDb == doctest::Approx (-12.0));
}

TEST_CASE ("timecode line: a replay of a session on the line reaches the same runs, ended and failed on the same ticks")
{
    TimecodeRig rig;
    rig.attach();

    const auto scene = makeScene (rig);
    const auto song = rig.document.createCue (scene, rig.childIndex++, "media", "Song").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + song + "/file", "song.wav").ok);
    const auto inScene = rig.makeTimecode ("ltc", "03:00:00:00", {}, scene);

    const auto first = rig.makeTimecode ("ltc", "01:00:00:00");
    const auto timed = rig.makeTimecode ("ltc", "05:00:00:00", "0.5");

    rig.fire (first);
    for (int n = 0; n < 7; ++n)
        rig.tickOnce();

    //  Takes the line over, then its length ends it.
    rig.fire (timed);
    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    //  Waits for its sound, then takes the line over too.
    rig.fire (scene);
    for (int n = 0; n < 4; ++n)
        rig.tickOnce();

    rig.player.completeArms (rig.engine);
    for (int n = 0; n < 6; ++n)
        rig.tickOnce();

    rig.submit ("run.stopAll");
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    //  And one refused, the output taken away.
    REQUIRE (rig.document.setAttribute ("/godot/sync/ltcOut", "").ok);
    rig.fire (first);
    for (int n = 0; n < 3; ++n)
        rig.tickOnce();

    CHECK (rig.player.starts.size() == 3u);

    for (const auto& cueId : { first, timed, inScene })
        for (const auto& run : rig.runs.all())
            if (run.cue == cueId)
            {
                INFO ("run " << run.id << " of " << cueId);
                CHECK (run.isFinished());
            }

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
        CHECK (again->error == run.error);
        CHECK (again->endedAtTick == run.endedAtTick);
        CHECK (again->startedAtTick == run.startedAtTick);
    }
}
