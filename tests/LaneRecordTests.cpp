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
    A LANE RECORDED FROM A FADER (namespace draft §20.9).

    The pure half first - a ride's segments, its thinning, its splice into the
    lane it replaces - then the commands against a real document, then the
    pass itself: the Runner's hook with a fake audio side whose sample clock
    the test moves, the touch table a surface fills, and the live door a
    fader's `node.set` goes through - the whole path but the MIDI.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneCommands.h>
#include <wfg/engine/cue/LaneRecording.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    constexpr const char* ride = "/godot/surface/laneRide";

    //==========================================================================
    /*  The audio side with its clock in the test's hands: arms complete when
        the disk answers, and the sample count is whatever the case says. */
    struct FakePlayer final : cue::Player
    {
        int trackCount() const override                      { return 4; }
        std::int64_t samplesElapsed() const override         { return samples; }
        int blockSize() const override                       { return 128; }
        int channelsPerTrack() const override                { return 2; }
        int slotCount() const override                       { return 4; }
        int sampleRate() const override                      { return 48000; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }
        bool launchAtSample (int, int, std::int64_t) override    { return true; }

        /*  `tails`: a stopped voice rings on - its EQ and inserts' tail - and
            the audio side still says it plays, so its run stays `stopping`. */
        bool stop (int track) override
        {
            ++stops;

            if (! tails)
                playing.erase (track);

            return true;
        }

        /*  A kill empties the voice's EQ and inserts, where a stop lets them
            ring (namespace draft §23.6): counted apart. */
        bool kill (int track) override
        {
            ++kills;
            playing.erase (track);
            return true;
        }

        bool stopAtSample (int, int, std::int64_t) override    { return true; }
        void setLevelDb (int, double levelDb) override          { lastLevel = levelDb; }
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override               { return playing.count (track) > 0; }
        bool isArmReady (int track) const override              { return ready.count (track) > 0; }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        std::int64_t samples = 0;
        double lastLevel = 0.0;
        int stops = 0;
        int kills = 0;
        bool tails = false;
        std::vector<cue::ArmRequest> arms;
        std::set<int> playing;
        std::set<int> ready;
    };

    //==========================================================================
    struct Rig
    {
        /*  `populate` false is a bare rig, for a replay: the show is read from
            the session's own canonical XML instead. */
        explicit Rig (bool populate = true)
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document, nullptr, &lanes));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            cue::registerLaneCommands (engine.commands(), engine, runner, document, lanes);
            tree::registerTreeCommands (engine.commands(), touches);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            runner.setTouches (&touches);
            runner.setLanes (&lanes);
            parameters.setLanes (&lanes);

            if (! populate)
                return;

            listId = document.createList ("Sound").id;
            mediaId = document.createCue (listId, 0, "media", "Bed").id;
            memoId = document.createCue (listId, 1, "memo", "Note").id;
            set ("/godot/cue/" + mediaId + "/file", "bed.wav");

            std::vector<std::string> made;
            REQUIRE (document.createSurface ("virtual", "Panel", {}, {}, made).ok);
            REQUIRE (made.size() >= 2u);
            strips = made;
        }

        void set (const std::string& address, const std::string& text)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, text).ok, address << " = " << text);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult send (const std::string& name, std::vector<osc::Value> args = {},
                                 const std::string& from = "cli")
        {
            REQUIRE (engine.submit (from, name, std::move (args)));
            return tickOnce();
        }

        /*  A tick with the sound a tick further on - the clock the pass samples
            the hand against. */
        void play (int ticks)
        {
            for (int n = 0; n < ticks; ++n)
            {
                audio.samples += 960;
                tickOnce();
            }
        }

        /*  Armed, taken and a pass running: the state every pass case starts
            from, the voice launched and sounding. */
        std::string startPass()
        {
            REQUIRE (send ("lane.arm", { osc::Value::string (mediaId) }).applied >= 1);
            REQUIRE (send ("lane.take", { osc::Value::string (strips[0]) }, "window").applied >= 1);
            REQUIRE (send ("lane.record").applied >= 1);

            const auto runId = lanes.run;
            REQUIRE_FALSE (runId.empty());

            audio.completeArms (engine);

            for (int n = 0; n < 10 && runs.find (runId)->launchedAtSample <= 0; ++n)
                tickOnce();

            REQUIRE (runs.find (runId)->launchedAtSample > 0);
            audio.samples = runs.find (runId)->launchedAtSample;
            audio.playing.insert (runs.find (runId)->track);
            tickOnce();

            return runId;
        }

        std::vector<doc::LanePoint> laneOfCue() const
        {
            return laneOf (mediaId);
        }

        std::vector<doc::LanePoint> laneOf (const std::string& cueId) const
        {
            return doc::readLevelLane (document.getAttribute ("/godot/cue/" + cueId + "/levelLane")
                                           .value_or ("")).points;
        }

        /*  A RUN'S VOICE LANDS: its arms answered, its launch placed, the
            clock at the launch and the audio side saying it plays - what a
            pass, a press or a seek waits for before anything is heard. */
        void land (const std::string& runId)
        {
            audio.completeArms (engine);

            for (int n = 0; n < 10 && runs.find (runId)->launchedAtSample <= 0; ++n)
                tickOnce();

            REQUIRE (runs.find (runId)->launchedAtSample > 0);
            audio.samples = std::max (audio.samples, runs.find (runId)->launchedAtSample);
            audio.playing.insert (runs.find (runId)->track);
            tickOnce();
        }

        /*  A SAMPLER BANK OF TWO, GONE TO and armed - its members on the
            panel's first two strips, waiting for a hand. A script presses GO
            here, faster than any hand: the show says the GO window is off. */
        std::string samplerBank()
        {
            set ("/godot/list/goDebounce", "0");

            const auto bank = document.createCue (listId, 2, "group", "Bank").id;
            set ("/godot/cue/" + bank + "/mode", "sampler");

            for (int i = 0; i < 2; ++i)
            {
                const auto member = document.createCue (bank, i, "media", "Clip " + std::to_string (i)).id;
                set ("/godot/cue/" + member + "/file", "clip" + std::to_string (i) + ".wav");
                members.push_back (member);
            }

            REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), bank).ok);
            tickOnce();
            REQUIRE (send ("go").applied >= 1);

            for (int n = 0; n < 20 && (liveRunOf (members[0]) == nullptr || liveRunOf (members[1]) == nullptr); ++n)
                tickOnce();

            REQUIRE (liveRunOf (members[0]) != nullptr);
            REQUIRE (liveRunOf (members[1]) != nullptr);

            audio.completeArms (engine);
            tickOnce();
            tickOnce();
            return bank;
        }

        /** The newest unfinished run of a cue, or null. */
        const cue::Run* liveRunOf (const std::string& cueId) const
        {
            const cue::Run* newest = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId && ! run.isFinished())
                    newest = runs.find (run.id);

            return newest;
        }

        /*  A range on the cue, `loops` passes of it, made through the
            document as the window's table makes one. */
        void addRange (double in, double out, int loops = 1)
        {
            const auto made = document.createRange (mediaId, in, out);
            REQUIRE (made.ok);
            set ("/godot/range/" + made.id + "/loops", std::to_string (loops));
        }

        std::string published (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        FakePlayer audio;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        cue::LaneTable lanes;
        tree::TouchTable touches;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (29) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;

        std::string listId, mediaId, memoId;
        std::vector<std::string> strips;
        std::vector<std::string> members;
        std::int64_t tick = 1;
    };
}

//==============================================================================
TEST_CASE ("lane record: a ride's samples climb, a wrap starts a new segment, one instant is one sample")
{
    std::vector<cue::RideSegment> segments;

    cue::appendRide (segments, 1.00, -6.0);
    cue::appendRide (segments, 1.02, -7.0);
    cue::appendRide (segments, 1.02, -8.0);     // the same instant: the level moves, no second point
    cue::appendRide (segments, 1.04, -9.0);
    cue::appendRide (segments, 0.50, -10.0);    // a loop's wrap: the second falls back
    cue::appendRide (segments, 0.52, -11.0);

    REQUIRE (segments.size() == 2u);
    REQUIRE (segments[0].size() == 3u);
    CHECK (segments[0][1].levelDb == doctest::Approx (-8.0));
    CHECK (segments[1].size() == 2u);
}

TEST_CASE ("lane record: a ride is thinned to the straight lines that stand for it, and keeps its turns")
{
    //  A steady ramp is its two ends.
    cue::RideSegment ramp;

    for (int k = 0; k <= 100; ++k)
        ramp.push_back ({ k * 0.02, -0.2 * k });

    const auto thinRamp = cue::thinRide (ramp, 0.1);
    REQUIRE (thinRamp.size() == 2u);
    CHECK (thinRamp.back().levelDb == doctest::Approx (-20.0));

    //  A V keeps its bottom; noise under a tenth of a decibel does not survive.
    cue::RideSegment vee;

    for (int k = 0; k <= 100; ++k)
        vee.push_back ({ k * 0.02, -std::abs (k - 50) * 0.2 + ((k % 2) != 0 ? 0.04 : 0.0) });

    const auto thinVee = cue::thinRide (vee, 0.1);
    REQUIRE (thinVee.size() == 3u);
    CHECK (thinVee[1].seconds == doctest::Approx (1.0));
}

TEST_CASE ("lane record: a pass replaces the stretch it rode, joined to the curve either side")
{
    //  Into NO LANE: the ride, and unity either side of it.
    const auto intoNothing = cue::spliceRide ({}, { { { 2.0, -6.0 }, { 3.0, -6.0 } } }, 0.05, 0.1);

    REQUIRE (intoNothing.size() == 4u);
    CHECK (intoNothing.front().seconds == doctest::Approx (1.95));
    CHECK (intoNothing.front().levelDb == doctest::Approx (0.0));
    CHECK (intoNothing.back().seconds == doctest::Approx (3.05));
    CHECK (doc::laneLevelDb (intoNothing, 2.5) == doctest::Approx (-6.0));
    CHECK (doc::laneLevelDb (intoNothing, 5.0) == doctest::Approx (0.0));

    //  Into A LANE: what was not ridden is exactly what it was.
    const std::vector<doc::LanePoint> dip { { 1.0, 0.0 }, { 4.0, -30.0 }, { 8.0, -30.0 } };
    const auto over = cue::spliceRide (dip, { { { 2.0, -12.0 }, { 3.0, -12.0 } } }, 0.05, 0.1);

    CHECK (doc::laneLevelDb (over, 1.5) == doctest::Approx (doc::laneLevelDb (dip, 1.5)));
    CHECK (doc::laneLevelDb (over, 1.95) == doctest::Approx (doc::laneLevelDb (dip, 1.95)));
    CHECK (doc::laneLevelDb (over, 2.5) == doctest::Approx (-12.0));
    CHECK (doc::laneLevelDb (over, 3.05) == doctest::Approx (doc::laneLevelDb (dip, 3.05)));
    CHECK (doc::laneLevelDb (over, 6.0) == doctest::Approx (-30.0));

    //  A LATER SEGMENT OVER THE SAME STRETCH is what was heard last.
    const auto twice = cue::spliceRide ({}, { { { 2.0, -6.0 }, { 3.0, -6.0 } },
                                              { { 2.2, -20.0 }, { 2.8, -20.0 } } }, 0.05, 0.1);
    CHECK (doc::laneLevelDb (twice, 2.5) == doctest::Approx (-20.0));
    CHECK (doc::laneLevelDb (twice, 2.05) == doctest::Approx (-6.0));

    //  A join that would reach before the file starts is left out.
    const auto atTheTop = cue::spliceRide ({}, { { { 0.0, -6.0 }, { 1.0, -6.0 } } }, 0.05, 0.1);
    CHECK (atTheTop.front().seconds == doctest::Approx (0.0));

    //  And the text is a lane the judge takes, however close two points came.
    CHECK (doc::readLevelLane (cue::laneText (over)).problem.empty());
    CHECK (doc::readLevelLane (cue::laneText ({ { 1.0, 0.0 }, { 1.00001, -3.0 }, { 2.0, -6.0 } }))
             .problem.empty());
}

//==============================================================================
TEST_CASE ("lane record: arming, taking and freeing a fader say no where they must")
{
    Rig rig;

    CHECK (rig.send ("lane.arm", { osc::Value::string ("NOTACUE1") }).rejected == 1);
    CHECK (rig.send ("lane.arm", { osc::Value::string (rig.memoId) }).rejected == 1);

    //  Nothing waits, so nothing can be taken, and no pass without a fader.
    CHECK (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }).rejected == 1);
    CHECK (rig.send ("lane.record").rejected == 1);

    REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
    CHECK (rig.lanes.waiting());
    CHECK (rig.send ("lane.take", { osc::Value::string ("NOTASTR1") }).rejected == 1);

    //  The first fader touched is taken, and a second is not waited for.
    REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[1]) }).applied == 1);
    CHECK (rig.lanes.taken());
    CHECK (rig.lanes.strip() == rig.strips[1]);
    CHECK (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }).rejected == 1);

    //  Freed, it is nobody's.
    REQUIRE (rig.send ("lane.free").applied == 1);
    CHECK_FALSE (rig.lanes.taken());
    CHECK (rig.lanes.cue().empty());

    //  A lane is the show's: the lock refuses arming it.
    rig.set ("/godot/document/locked", "true");
    CHECK (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).rejected == 1);
}

TEST_CASE ("lane record: the taken strip rides the lane's node, which sits where the lane starts")
{
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -10 4 -30");
    rig.set ("/godot/cue/" + rig.mediaId + "/startOffset", "2");

    REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
    CHECK (rig.published ("/godot/surface/lane") == rig.mediaId);
    CHECK (rig.published ("/godot/surface/laneFader").empty());

    REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }).applied == 1);
    rig.tickOnce();

    CHECK (rig.published ("/godot/surface/laneFader") == rig.strips[0]);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/target") == ride);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "lane");
    CHECK (rig.published ("/godot/slot/" + rig.strips[1] + "/target") != ride);

    //  DG: where the lane starts - the start offset's second, -20 dB.
    CHECK (rig.lanes.rideDb == doctest::Approx (-20.0));
    CHECK (rig.published (ride) == "-20");

    //  A hand setting the fader before any pass is heard by nobody.
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-3.0) }, "window");
    CHECK (rig.lanes.rideDb == doctest::Approx (-20.0));
}

//==============================================================================
TEST_CASE ("lane record: a pass writes the ride from its first touch, latched until it stops, in one step")
{
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 0 20 -20");

    const auto runId = rig.startPass();
    CHECK (rig.lanes.recording);
    CHECK (rig.published ("/godot/surface/laneRecording") == "true");

    //  UNTOUCHED, THE FADER READS THE LANE: a second in, about -1 dB.
    rig.play (50);
    CHECK (rig.lanes.rideDb < -0.5);
    CHECK (rig.lanes.rideDb > -1.5);

    //  THE TOUCH: the hand's level is heard at once.
    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");
    rig.play (50);

    CHECK (rig.lanes.touched);
    CHECK (rig.runs.find (runId)->laneDb == doctest::Approx (-6.0));
    CHECK (rig.runs.find (runId)->level == doctest::Approx (-6.0));

    //  A move, then the hand lets go: LATCHED at the last value (DH).
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-12.0) }, "surface:PANEL");
    rig.play (50);
    rig.send ("node.release", { osc::Value::string (ride) }, "surface:PANEL");
    rig.play (50);

    CHECK (rig.runs.find (runId)->laneDb == doctest::Approx (-12.0));

    //  Nothing written yet: a pass is one decision, written when it ends.
    CHECK (rig.laneOfCue().size() == 2u);

    //  THE HAND ENDS IT: the lane is written once, and the cue is stopped.
    //  (This rig cuts no transactions, as serve's hook does: one is opened
    //  here, so the undo below takes the pass and nothing before it.)
    rig.document.beginTransaction ("lane.stop", rig.tick, "window", {});
    rig.send ("lane.stop", {}, "window");
    rig.tickOnce();

    CHECK_FALSE (rig.lanes.recording);

    const auto lane = rig.laneOfCue();
    REQUIRE (lane.size() > 2u);

    //  Before the touch, the old ramp; while held at -6, -6; after the move, -12, held to the end.
    CHECK (doc::laneLevelDb (lane, 0.5) == doctest::Approx (-0.5).epsilon (0.05));
    CHECK (doc::laneLevelDb (lane, 1.8) == doctest::Approx (-6.0));
    CHECK (doc::laneLevelDb (lane, 2.5) == doctest::Approx (-12.0));
    CHECK (doc::laneLevelDb (lane, 3.9) == doctest::Approx (-12.0));

    //  And after the pass, the old curve again.
    CHECK (doc::laneLevelDb (lane, 15.0) == doctest::Approx (-15.0));

    const auto* run = rig.runs.find (runId);
    CHECK ((run == nullptr || run->state == cue::runState::stopping || run->isFinished()));

    //  ONE STEP OF UNDO takes the pass away.
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.laneOfCue().size() == 2u);
}

TEST_CASE ("lane record: a pass nobody touched writes nothing, and a killed one drops the ride")
{
    SUBCASE ("untouched")
    {
        Rig rig;
        rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -3");

        rig.startPass();
        rig.play (30);
        rig.send ("lane.stop", {}, "window");
        rig.tickOnce();

        CHECK_FALSE (rig.lanes.recording);
        CHECK (rig.laneOfCue().size() == 1u);
    }

    SUBCASE ("killed - double Esc drops every action (§4.4)")
    {
        Rig rig;
        rig.startPass();

        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
        rig.play (30);

        rig.send ("run.killAll", {}, "window");
        rig.tickOnce();

        CHECK_FALSE (rig.lanes.recording);
        CHECK (rig.laneOfCue().empty());
    }

    SUBCASE ("Esc keeps it")
    {
        Rig rig;
        rig.startPass();

        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
        rig.play (30);

        rig.send ("run.stopAll", {}, "window");
        rig.tickOnce();

        CHECK_FALSE (rig.lanes.recording);
        CHECK_FALSE (rig.lanes.taken());
        REQUIRE_FALSE (rig.laneOfCue().empty());
        CHECK (doc::laneLevelDb (rig.laneOfCue(), 0.3) == doctest::Approx (-9.0));
    }
}

TEST_CASE ("lane record: a double Esc lets the taken fader go - its strip rides what it rode before, and nothing moves it again")
{
    /*  THE ASSOCIATION IS DROPPED WITH EVERYTHING ELSE (2026-10-02, K4, the
        author: "double Esc would throw away the fader association"; namespace
        draft §23.15, overruling JE of §23.11). A double Esc lets the fader go
        as `lane.free` would: the strip goes back to what it rode before it was
        taken, the lane is forgotten, the ride node leaves the tree - and the
        fader is not sent to the lane's start first, as it was under JE. The
        killed pass's ride is dropped as before (DM). From the press's own
        handler, so a replay frees it in the same record. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -20");

    /*  The strip rides a DCA's trim before it is taken: what it goes back to. */
    const auto band = rig.document.createDca ("Band");
    REQUIRE (band.ok);
    rig.set ("/godot/slot/" + rig.strips[0] + "/role", "dca");
    rig.set ("/godot/slot/" + rig.strips[0] + "/dca", band.id);

    const auto target = "/godot/slot/" + rig.strips[0] + "/target";
    const auto before = rig.published (target);
    REQUIRE (before == "/godot/dca/" + band.id + "/trim");

    SUBCASE ("in a pass")
    {
        rig.startPass();

        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
        rig.play (30);

        REQUIRE (rig.published (ride) == "-9");
        REQUIRE (rig.published (target) == ride);
    }

    SUBCASE ("taken, no pass")
    {
        REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied >= 1);
        REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window").applied >= 1);
        rig.tickOnce();

        REQUIRE (rig.published (target) == ride);
        REQUIRE (rig.published (ride) == "-20");
    }

    SUBCASE ("armed, still waiting for a touch")
    {
        /*  The review of K4: a lane armed and waiting is disarmed as well - the
            next fader touched after the press is touched, not taken. */
        REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied >= 1);
        REQUIRE (rig.lanes.waiting());
        REQUIRE (rig.published ("/godot/surface/lane") == rig.mediaId);
    }

    REQUIRE (rig.send ("run.killAll", {}, "window").rejected == 0);

    //  Let go in the press's own drain: nothing waits for a hook.
    CHECK_FALSE (rig.lanes.taken());
    CHECK_FALSE (rig.lanes.waiting());
    CHECK_FALSE (rig.lanes.recording);
    CHECK (rig.lanes.cue().empty());

    rig.play (3);

    CHECK (rig.published (target) == before);
    CHECK (rig.published ("/godot/surface/laneFader").empty());
    CHECK (rig.published ("/godot/surface/lane").empty());
    CHECK (rig.published ("/godot/surface/laneRecording") == "false");
    CHECK (rig.published (ride).empty());                  // the ride node is gone, not at -20

    rig.play (50);

    CHECK (rig.published (target) == before);
    CHECK (rig.published (ride).empty());
    CHECK_FALSE (rig.lanes.taken());
    CHECK (rig.laneOfCue().size() == 1u);       // the drawn lane, any ride dropped (DM)

    //  And a pass needs a fader again.
    CHECK (rig.send ("lane.record").rejected == 1);
}

TEST_CASE ("lane record: a session with a double Esc in the middle of a pass replays record for record")
{
    /*  THE FREE IS THE HANDLER'S (KO; the review of K4 asked for the proof):
        `run.killAll` lets the fader go inside its own handler, so a replay -
        which runs no hook - frees it in the same record, and the records
        after it (the hand's writes on a ride that has gone, a new arming)
        are answered the same way. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -20");
    rig.startPass();

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
    rig.play (30);

    rig.send ("run.killAll", {}, "window");
    rig.play (5);
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-4.0) }, "surface:PANEL");
    rig.send ("lane.record");                               // refused: no fader
    rig.send ("lane.arm", { osc::Value::string (rig.mediaId) });
    rig.play (3);

    REQUIRE_FALSE (rig.lanes.taken());
    REQUIRE (rig.lanes.waiting());

    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    Rig fresh { false };
    const auto read = doc::CanonicalXml::read (show, fresh.document);

    for (const auto& problem : read.problems)
        MESSAGE (problem);

    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);
    CHECK (result.recordsReplayed == result.recordsExpected);
    CHECK_FALSE (fresh.lanes.taken());
    CHECK_FALSE (fresh.lanes.recording);
    CHECK (fresh.lanes.cue() == rig.mediaId);
}

TEST_CASE ("lane record: Esc ends the pass and gives the fader back, and the pass it ended keeps its ride")
{
    /*  THE STOP GIVES IT BACK, HOWEVER IT STOPS (2026-10-05, QX, the author's;
        namespace draft §30.4), replacing K4's "a single Esc would keep the
        association" (§23.15), which this case pinned until then. Esc ends the
        pass as the cue ending would - the ride written (DM) - and the strip
        rides what it rode before it was taken; the next pass is a new arm
        and a new touch. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -20");

    const auto target = "/godot/slot/" + rig.strips[0] + "/target";
    const auto before = rig.published (target);

    rig.startPass();

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
    rig.play (30);

    REQUIRE (rig.published (target) == ride);
    REQUIRE (rig.send ("run.stopAll", {}, "window").rejected == 0);
    rig.play (60);                              // past the panic fade (1 s)

    CHECK_FALSE (rig.lanes.recording);
    CHECK_FALSE (rig.lanes.taken());
    CHECK (rig.lanes.cue().empty());
    CHECK (rig.published (target) == before);
    CHECK (rig.published ("/godot/surface/laneFader").empty());
    CHECK (rig.published ("/godot/surface/lane").empty());

    REQUIRE_FALSE (rig.laneOfCue().empty());
    CHECK (doc::laneLevelDb (rig.laneOfCue(), 0.3) == doctest::Approx (-9.0));

    //  The next pass needs a fader taken again.
    CHECK (rig.send ("lane.record").rejected == 1);
    CHECK_FALSE (rig.lanes.recording);
}

TEST_CASE ("lane record: the hand ending a pass stops its cue gracefully, and its tail rings")
{
    /*  A STOP, NOT A KILL (2026-10-02, K4, the author's "graceful stop";
        namespace draft §23.15, overruling GB of §23.6). Rec pressed again, or
        the window's stop, writes the ride and then ends the cue as the pane's
        stop does - `run.stop`, an abort that owes no post-wait (§23.13) - so
        the voice is stopped and its EQ and inserts ring out, where a kill
        emptied them; and the run is not marked killed. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -3");

    const auto runId = rig.startPass();

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
    rig.play (30);

    const auto stopsBefore = rig.audio.stops;
    REQUIRE (rig.audio.kills == 0);

    rig.send ("lane.stop", {}, "window");
    rig.play (3);

    CHECK_FALSE (rig.lanes.recording);
    CHECK_FALSE (rig.lanes.taken());                    // and the fader given back (QX)

    const auto* run = rig.runs.find (runId);
    REQUIRE (run != nullptr);
    CHECK (rig.audio.kills == 0);                       // the tail is not cut
    CHECK (rig.audio.stops > stopsBefore);              // the voice is stopped
    CHECK_FALSE (run->killed);
    CHECK_FALSE (run->skipFooter);
    CHECK (run->stopEndsWait);                          // the pane's stop: an abort (§23.13)

    //  And the ride is written, as before.
    REQUIRE_FALSE (rig.laneOfCue().empty());
    CHECK (doc::laneLevelDb (rig.laneOfCue(), 0.3) == doctest::Approx (-9.0));
}

TEST_CASE ("lane record: a touch with no move latches where the fader was, not at a level left over")
{
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -7");

    const auto runId = rig.startPass();

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.play (10);

    CHECK (rig.lanes.touched);
    CHECK (rig.runs.find (runId)->laneDb == doctest::Approx (-7.0));
}

//==============================================================================
/*  THE BUG ROUND OF 2026-10-05 (namespace draft §30, items 2 and 3; §30.4).
    The author recorded a lane on a 647-second sample in a sampler bank, with
    four ranges, from its own strip: the points all stayed "together at the
    initial time", it "didn't even work when starting from 0s", and the fader
    "was not released back to its sampler". The cases below walk the paths
    that took them there - a sampler member, ranges, a retake, every end of a
    pass - which the cases above, one plain cue on one slot, never did. */

namespace
{
    /*  The fields of `/godot/surface/lanePass`: tick, cue, how, and for a pass
        kept, the points and the seconds they span. */
    std::vector<std::string> fieldsOf (const std::string& line)
    {
        std::vector<std::string> out;
        std::string word;

        for (const auto c : line)
        {
            if (c == ' ')
            {
                if (! word.empty())
                    out.push_back (word);

                word.clear();
            }
            else
            {
                word += c;
            }
        }

        if (! word.empty())
            out.push_back (word);

        return out;
    }
}

TEST_CASE ("lane record: a sampler member sounding under a pulled-down fader is recorded from its start, and heard")
{
    /*  Item 3: a pass with no `from` - the D700's Rec, or the window's from
        nought - took the run `cue.fire` handed back as it found it. A sampler
        member playing out under a fader pulled to the bottom is sounding
        (play-out mutes, it does not stop), so the pass recorded wherever the
        sample had got to, at -120 dB. Now it is moved to the cue's start, and
        the fader's bottom is lifted as a press lifts it. */
    Rig rig;
    rig.samplerBank();

    const auto member = rig.members[0];
    const auto* holder = rig.liveRunOf (member);
    REQUIRE (holder != nullptr);
    REQUIRE (holder->strip == rig.strips[0]);

    const auto runId = holder->id;

    //  The clip pressed and sounding two seconds, then its fader pulled to the bottom.
    REQUIRE (rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "surface:PANEL").rejected == 0);
    rig.land (runId);
    rig.play (100);
    REQUIRE (rig.runs.find (runId)->position > 1.5);

    rig.send ("node.set", { osc::Value::string ("/godot/run/" + runId + "/trim"), osc::Value::float64 (-120.0) },
              "surface:PANEL");
    rig.play (5);
    REQUIRE (rig.runs.find (runId)->trim <= -118.0);
    REQUIRE_FALSE (rig.runs.find (runId)->isFinished());

    //  Its own strip taken for its lane, as the author did, and a pass with no `from`.
    REQUIRE (rig.send ("lane.arm", { osc::Value::string (member) }).applied == 1);
    REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "surface:PANEL").applied == 1);
    REQUIRE (rig.send ("lane.record").applied == 1);

    //  The same run, moved to the cue's start, and its fader lifted to the member's initial level.
    CHECK (rig.lanes.run == runId);
    CHECK (rig.runs.find (runId)->launchedAtSample == 0);
    CHECK (rig.runs.find (runId)->position < 0.01);
    CHECK (rig.runs.find (runId)->trim == doctest::Approx (0.0));

    rig.land (runId);
    CHECK (rig.runs.find (runId)->position < 0.1);
    CHECK (rig.runs.find (runId)->level > -60.0);

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");
    rig.play (50);
    rig.send ("lane.stop", {}, "surface:PANEL");
    rig.tickOnce();

    //  Written from the start, over about the second it was ridden - not from two seconds in.
    const auto lane = rig.laneOf (member);
    REQUIRE (lane.size() >= 2u);
    CHECK (lane.front().seconds < 0.2);
    CHECK (lane.back().seconds > 0.9);
    CHECK (lane.back().seconds < 1.4);
    CHECK (doc::laneLevelDb (lane, 0.5) == doctest::Approx (-6.0));

    //  And the fader is the sample's again (QX).
    CHECK_FALSE (rig.lanes.taken());
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/target") != ride);
}

TEST_CASE ("lane record: a pass started while the last pass's cue still rings out plays, and is not over on its first tick")
{
    /*  Item 3: since K4 the hand's end of a pass is a graceful `run.stop`, so a
        cue with a tail is still `stopping` a moment later - and a retake then
        was handed that run, read it as stopped, and ended on its first tick,
        writing nothing. The pass now takes the run back from its stop. */
    Rig rig;
    rig.audio.tails = true;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -3");

    const auto runId = rig.startPass();

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
    rig.play (30);
    rig.send ("node.release", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("lane.stop", {}, "window");
    rig.play (2);

    REQUIRE (rig.runs.find (runId)->state == cue::runState::stopping);
    REQUIRE_FALSE (rig.lanes.taken());          // QX: a retake is a new arm and a new touch

    REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
    REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window").applied == 1);
    REQUIRE (rig.send ("lane.record").applied == 1);

    CHECK (rig.lanes.run == runId);
    CHECK (rig.runs.find (runId)->state == cue::runState::playing);

    rig.land (runId);
    rig.play (10);

    CHECK (rig.lanes.recording);
    CHECK (rig.runs.find (runId)->state == cue::runState::playing);

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-12.0) }, "surface:PANEL");
    rig.play (30);
    rig.send ("lane.stop", {}, "window");
    rig.tickOnce();

    CHECK_FALSE (rig.lanes.recording);
    CHECK (doc::laneLevelDb (rig.laneOfCue(), 0.4) == doctest::Approx (-12.0));
}

TEST_CASE ("lane record: a cue with ranges is recorded from the second asked, inside its range, and its points start there")
{
    /*  Item 3: a pass from 21.5 s on a cue whose third range is 20 to 30 s
        started at 20 - the seek landed at the start of the range that held
        the second. It lands at the second now, by K8's `sliceFrom`. A session
        a minute old, so the slice's start can be dated back before the launch. */
    Rig rig;
    rig.audio.samples = 48000 * 60;

    rig.addRange (0.0, 10.0);
    rig.addRange (10.0, 20.0, 2);
    rig.addRange (20.0, 30.0);
    rig.addRange (30.0, 60.0);

    REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
    REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window").applied == 1);
    REQUIRE (rig.send ("lane.record", { osc::Value::float64 (21.5) }).applied == 1);

    const auto runId = rig.lanes.run;
    const auto* run = rig.runs.find (runId);
    REQUIRE (run != nullptr);

    CHECK (run->startRange == 2);
    CHECK (run->sliceFrom == doctest::Approx (1.5));
    REQUIRE_FALSE (rig.audio.arms.empty());
    CHECK (rig.audio.arms.back().startSlot == 2);
    CHECK (rig.audio.arms.back().sliceOffset == doctest::Approx (1.5));

    rig.land (runId);
    CHECK (rig.runs.find (runId)->range == 2);
    CHECK (std::abs (rig.runs.find (runId)->position - 21.5) < 0.05);

    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");
    rig.play (50);
    rig.send ("lane.stop", {}, "window");
    rig.tickOnce();

    const auto lane = rig.laneOfCue();
    REQUIRE (lane.size() >= 2u);
    CHECK (std::abs (lane.front().seconds - 21.45) < 0.1);
    CHECK (doc::laneLevelDb (lane, 21.0) == doctest::Approx (0.0));
    CHECK (doc::laneLevelDb (lane, 22.0) == doctest::Approx (-6.0));
}

TEST_CASE ("lane record: a seek on a cue with ranges lands at the second asked - its pass kept, a gap to the next range, past the end in the last")
{
    /*  Items 3 and 10, the arithmetic S8's scrubbing builds on (§30.4): a
        second inside the range the run is in keeps the pass it is on; inside
        another range, that range's first pass; in a gap, the next range's
        start; at or past the last out-point, a hair inside the last range -
        never the top of the file, where the first range's start used to send
        it. */
    Rig rig;
    rig.audio.samples = 48000 * 100;

    rig.addRange (0.0, 10.0);
    rig.addRange (10.0, 20.0, 3);
    rig.addRange (25.0, 30.0);

    REQUIRE (rig.send ("cue.fire", { osc::Value::string (rig.mediaId) }).applied >= 1);
    const auto* fired = rig.liveRunOf (rig.mediaId);
    REQUIRE (fired != nullptr);

    const auto runId = fired->id;
    rig.land (runId);

    const auto seek = [&rig, &runId] (double seconds)
    {
        REQUIRE (rig.send ("run.seek", { osc::Value::string (runId), osc::Value::float64 (seconds) }).applied == 1);
    };

    const auto now = [&rig, &runId] { return rig.runs.find (runId); };

    //  Into another range: at the second, on its first pass.
    seek (12.0);
    CHECK (now()->range == 1);
    CHECK (now()->sliceFrom == doctest::Approx (2.0));
    CHECK (now()->position == doctest::Approx (12.0));
    rig.land (runId);
    CHECK (std::abs (now()->position - 12.0) < 0.05);

    //  Twelve seconds on, the second pass of that range, four seconds in.
    rig.play (600);
    REQUIRE (now()->rangeIteration == 2);
    REQUIRE (std::abs (now()->position - 14.0) < 0.1);

    //  Inside it again: the pass it was on is kept.
    seek (15.0);
    CHECK (now()->range == 1);
    CHECK (now()->rangeIteration == 2);
    CHECK (now()->sliceFrom == doctest::Approx (15.0));
    rig.land (runId);
    rig.play (2);
    CHECK (now()->rangeIteration == 2);
    CHECK (std::abs (now()->position - 15.0) < 0.1);

    //  A gap between ranges: the next range's start.
    seek (22.0);
    CHECK (now()->range == 2);
    CHECK (now()->position == doctest::Approx (25.0));
    CHECK (now()->sliceFrom == doctest::Approx (0.0));

    //  Past the end, and on the last out-point itself: a hair inside the last range, not the top.
    for (const auto past : { 40.0, 30.0 })
    {
        seek (past);
        CHECK (now()->range == 2);
        CHECK (now()->position > 29.9);
        CHECK (now()->position < 30.0);
        CHECK (now()->sliceFrom == doctest::Approx (4.999));
    }

    //  And back to the first range, its first pass.
    seek (5.0);
    CHECK (now()->range == 0);
    CHECK (now()->rangeIteration == 1);
    CHECK (now()->sliceFrom == doctest::Approx (5.0));
}

TEST_CASE ("lane record: the fader is the sample's again however the pass ends, and what the pass ended in is published")
{
    /*  QX (the author's decision, 2026-10-05): the touch takes the fader and
        the end of the pass gives it back - kept, nobody riding it, a kill, Esc
        (above), the cue ending on its own. And each end says what it was, for
        the window to say in words (§30.4). */
    Rig rig;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -3");

    const auto target = "/godot/slot/" + rig.strips[0] + "/target";
    const auto before = rig.published (target);
    REQUIRE (rig.published ("/godot/surface/lanePass").empty());

    std::string how;

    SUBCASE ("kept - the hand's stop, with what it wrote")
    {
        rig.startPass();
        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");
        rig.play (50);
        rig.send ("lane.stop", {}, "window");
        rig.tickOnce();
        how = "kept";

        const auto fields = fieldsOf (rig.published ("/godot/surface/lanePass"));
        REQUIRE (fields.size() == 6u);
        //  Read as the log writes numbers - a full stop in every locale.
        CHECK (osc::parseDouble (fields[3]).value_or (0.0) >= 2.0);
        CHECK (osc::parseDouble (fields[4]).value_or (9.0) < 0.1);
        CHECK (osc::parseDouble (fields[5]).value_or (0.0) > 0.9);
        CHECK (osc::parseDouble (fields[5]).value_or (9.0) < 1.3);
    }

    SUBCASE ("untouched - the cue ended on its own, nobody riding it")
    {
        const auto runId = rig.startPass();
        rig.play (30);
        rig.audio.playing.erase (rig.runs.find (runId)->track);
        rig.play (3);
        how = "untouched";
    }

    SUBCASE ("dropped - the pane's kill")
    {
        const auto runId = rig.startPass();
        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");
        rig.play (30);
        rig.send ("run.kill", { osc::Value::string (runId) }, "window");
        rig.play (3);
        how = "dropped";
        CHECK (rig.laneOfCue().size() == 1u);
    }

    CHECK_FALSE (rig.lanes.recording);
    CHECK_FALSE (rig.lanes.taken());
    CHECK (rig.lanes.cue().empty());
    CHECK (rig.published (target) == before);
    CHECK (rig.published ("/godot/surface/laneFader").empty());

    const auto fields = fieldsOf (rig.published ("/godot/surface/lanePass"));
    REQUIRE (fields.size() >= 3u);
    CHECK (fields[1] == rig.mediaId);
    CHECK (fields[2] == how);
}

TEST_CASE ("lane record: locking the show lets a waiting or taken fader go, and a pass under way ends before its fader does")
{
    /*  DN said the lock frees the fader, and only the refusals were built: a
        fader taken for a lane the lock will not let anybody record played
        nothing for as long as the show stayed locked (§30.4). Let go by
        `lane.free`, the engine's, so the log says why. */
    Rig rig;

    SUBCASE ("taken")
    {
        REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
        REQUIRE (rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window").applied == 1);
        REQUIRE (rig.lanes.taken());

        rig.set ("/godot/document/locked", "true");
        rig.play (2);

        CHECK_FALSE (rig.lanes.taken());
        CHECK (rig.lanes.cue().empty());
        CHECK (rig.published ("/godot/surface/laneFader").empty());
        CHECK (rig.engine.log().contents().find ("engine lane.free") != std::string::npos);
    }

    SUBCASE ("waiting for a touch")
    {
        REQUIRE (rig.send ("lane.arm", { osc::Value::string (rig.mediaId) }).applied == 1);
        REQUIRE (rig.lanes.waiting());

        rig.set ("/godot/document/locked", "true");
        rig.play (2);

        CHECK_FALSE (rig.lanes.waiting());
        CHECK (rig.lanes.cue().empty());
    }

    SUBCASE ("a pass under way: it runs on, and its end gives the fader back, writing nothing the lock keeps")
    {
        rig.startPass();
        rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
        rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-6.0) }, "surface:PANEL");

        rig.set ("/godot/document/locked", "true");
        rig.play (20);

        CHECK (rig.lanes.recording);
        CHECK (rig.lanes.taken());

        rig.send ("lane.stop", {}, "window");
        rig.tickOnce();

        CHECK_FALSE (rig.lanes.recording);
        CHECK_FALSE (rig.lanes.taken());
        CHECK (rig.laneOfCue().empty());

        const auto fields = fieldsOf (rig.published ("/godot/surface/lanePass"));
        REQUIRE (fields.size() >= 3u);
        CHECK (fields[2] == "locked");
    }
}

TEST_CASE ("lane record: a session of passes - a retake under a ringing stop, the cue ending on its own, the lock - replays record for record")
{
    /*  Everything §30.4 moved is a handler's or rides a record: the pass's
        start (`startLanePass`, from the run table and the document), its end
        and what it ended in (`lane.stop`'s arguments), the lock's let-go
        (`lane.free` from the engine). So a replay, which runs no hook,
        answers every record the night did. */
    Rig rig;
    rig.audio.tails = true;
    rig.set ("/godot/cue/" + rig.mediaId + "/levelLane", "0 -3");

    const auto runId = rig.startPass();
    rig.send ("node.touch", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("node.set", { osc::Value::string (ride), osc::Value::float64 (-9.0) }, "surface:PANEL");
    rig.play (30);
    rig.send ("node.release", { osc::Value::string (ride) }, "surface:PANEL");
    rig.send ("lane.stop", {}, "window");
    rig.play (2);

    rig.send ("lane.arm", { osc::Value::string (rig.mediaId) });
    rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window");
    rig.send ("lane.record", { osc::Value::float64 (0.5) });
    rig.land (runId);
    rig.play (20);
    rig.audio.playing.erase (rig.runs.find (runId)->track);
    rig.play (3);

    rig.send ("lane.arm", { osc::Value::string (rig.mediaId) });
    rig.send ("lane.take", { osc::Value::string (rig.strips[0]) }, "window");
    rig.set ("/godot/document/locked", "true");
    rig.play (3);

    REQUIRE_FALSE (rig.lanes.taken());

    const auto log = rig.engine.log().contents();
    REQUIRE (log.find ("lane.stop s:\"kept\" i:") != std::string::npos);
    REQUIRE (log.find ("lane.stop s:\"untouched\"") != std::string::npos);
    REQUIRE (log.find ("engine lane.free") != std::string::npos);

    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (log);
    REQUIRE (original.errors.empty());

    Rig fresh { false };
    const auto read = doc::CanonicalXml::read (show, fresh.document);

    for (const auto& problem : read.problems)
        MESSAGE (problem);

    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);
    CHECK (result.recordsReplayed == result.recordsExpected);
    CHECK_FALSE (fresh.lanes.taken());
    CHECK (fresh.lanes.lastPass == rig.lanes.lastPass);
}
