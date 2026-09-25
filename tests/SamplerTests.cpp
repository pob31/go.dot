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

/*  SAMPLER GROUPS (PRD §3.27, Phase 6).

    GO arms the group: every media member goes onto a sampler strip, armed on a
    voice of its own, and waits for a hand. A press launches the clip on a
    strip; a release stops a hold clip after a short fade; a clip can be played
    any number of times, because its run ending frees the strip and the member
    takes it back armed. Another group arming takes over - the whole bank, or
    only the strips it needs - and eviction is a close, not a kill: a sounding
    clip finishes before its strip changes hands.

    A fake audio side whose arms complete when the test says and whose clips
    sound and stop when the test says, so every case can put the scheduler in
    the state it is about and hold it there.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  Arms complete when `completeArms` says the disk answered; a launch is a
        track that plays until the test takes it away. */
    struct FakePlayer final : cue::Player
    {
        int trackCount() const override                      { return tracks; }
        std::int64_t samplesElapsed() const override         { return 0; }
        int blockSize() const override                       { return 128; }
        int channelsPerTrack() const override                { return 2; }
        int slotCount() const override                       { return 1; }
        int sampleRate() const override                      { return 48000; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t) override
        {
            launched.push_back (track);
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            stopped.push_back (track);
            return true;
        }

        bool stopAtSample (int, int, std::int64_t) override    { return true; }
        void setLevelDb (int, double) override                  {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override               { return playing.count (track) > 0; }
        bool isArmReady (int track) const override              { return ready.count (track) > 0; }

        //  What left each track since the last take, as the output stage's peak: taken, so nought after.
        float takeOutputPeak (int track) override
        {
            const auto found = peaks.find (track);
            return found != peaks.end() ? std::exchange (found->second, 0.0f) : 0.0f;
        }

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

        int tracks = 8;
        std::vector<cue::ArmRequest> arms;
        std::vector<int> launched;
        std::vector<int> stopped;
        std::set<int> playing;
        std::set<int> ready;
        std::map<int, float> peaks;
    };

    struct Rig
    {
        explicit Rig (int trackCount = 8)
        {
            audio.tracks = trackCount;
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            tree::registerTreeCommands (engine.commands(), touches);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            runner.setDcas (&dcas);
            runner.setTouches (&touches);
            parameters.setListState (&runner.listState());

            listId = document.createList ("Sound").id;

            /*  A virtual panel of eight fader strips: the redundancy path, and a
                surface that needs no port. */
            std::vector<std::string> made;
            REQUIRE (document.createSurface ("virtual", "Panel", {}, {}, made).ok);
            strips = made;
            REQUIRE (strips.size() == 8u);

            bankA = group ("Bank A", 0, 4);
            bankB = group ("Bank B", 1, 2);
            after = document.createCue (listId, 2, "memo", "After").id;
        }

        std::string group (const std::string& name, int index, int members)
        {
            const auto id = document.createCue (listId, index, "group", name).id;
            set ("/godot/cue/" + id + "/mode", "sampler");

            for (int i = 0; i < members; ++i)
            {
                const auto member = document.createCue (id, i, "media", name + " " + std::to_string (i)).id;
                set ("/godot/cue/" + member + "/file", "clip" + std::to_string (i) + ".wav");
                membersOf[id].push_back (member);
            }

            return id;
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

        template <typename Predicate>
        bool tickUntil (Predicate ready, int bound = 400)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return true;

                tickOnce();
            }

            return ready();
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

        std::size_t runsOf (const std::string& cueId) const
        {
            return static_cast<std::size_t> (std::count_if (runs.all().begin(), runs.all().end(),
                                                            [&cueId] (const cue::Run& run)
                                                            { return run.cue == cueId; }));
        }

        /*  GO on a bank, from standby, and let its arms land: the state every
            case below starts from. */
        void arm (const std::string& groupId)
        {
            REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), groupId).ok);
            tickOnce();
            send ("go");

            const auto& members = membersOf[groupId];
            REQUIRE (tickUntil ([this, &members]
                                {
                                    return std::all_of (members.begin(), members.end(),
                                                        [this] (const std::string& m)
                                                        { return liveRunOf (m) != nullptr; });
                                }));

            audio.completeArms (engine);
            tickOnce();
            tickOnce();
        }

        /*  A pressed clip starts sounding: its arm has landed, its launch was
            placed, and the audio side says it is playing. */
        void sound (const std::string& cueId)
        {
            audio.completeArms (engine);
            REQUIRE (tickUntil ([this, &cueId]
                                {
                                    const auto* run = liveRunOf (cueId);
                                    return run != nullptr && run->state == cue::runState::playing;
                                }));
            audio.playing.insert (liveRunOf (cueId)->track);
            tickOnce();
        }

        /** And stops, on its own - the clip reached its end. */
        void silence (const std::string& cueId)
        {
            const auto* run = liveRunOf (cueId);
            REQUIRE (run != nullptr);
            audio.playing.erase (run->track);
            tickOnce();
        }

        bool holds (const cue::Run* run, const std::string& stripId) const
        {
            return run != nullptr
                     && std::find (run->claims.begin(), run->claims.end(), stripId) != run->claims.end();
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
        tree::TouchTable touches;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (23) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;

        std::string listId;
        std::string bankA;
        std::string bankB;
        std::string after;
        std::vector<std::string> strips;
        std::map<std::string, std::vector<std::string>> membersOf;
        std::int64_t tick = 1;
    };
}

//==============================================================================
TEST_CASE ("sampler: GO arms every member onto a strip, and the pointer moves on")
{
    Rig rig;
    rig.arm (rig.bankA);

    const auto& members = rig.membersOf[rig.bankA];

    for (std::size_t i = 0; i < members.size(); ++i)
    {
        INFO ("member " << i);
        const auto* run = rig.liveRunOf (members[i]);

        REQUIRE (run != nullptr);
        CHECK (run->sampler);
        CHECK (run->strip == rig.strips[i]);
        CHECK (rig.holds (run, rig.strips[i]));
        CHECK (run->state == cue::runState::armed);
        CHECK_FALSE (run->launchRequested);
        CHECK (run->track >= 0);

        /*  THE FADER FLIES TO THE MEMBER'S INITIAL LEVEL and waits there for
            a touch (author, 2026-09-23): nought by default, the level the clip
            was written at. */
        CHECK (std::abs (run->trim) < 1.0e-9);
    }

    /*  The group runs; standby went to the next sibling and never inside. */
    REQUIRE (rig.liveRunOf (rig.bankA) != nullptr);
    CHECK (rig.liveRunOf (rig.bankA)->state == cue::runState::playing);
    CHECK (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)).value_or ("") == rig.bankB);

    /*  NOTHING LAUNCHED ITSELF: a sampler's members wait for a hand. */
    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.audio.launched.empty());
}

TEST_CASE ("sampler: a member that finds no voice waits for one, and takes the next that frees")
{
    Rig rig { 2 };
    rig.arm (rig.bankA);

    const auto& members = rig.membersOf[rig.bankA];

    /*  TWO TRACKS, FOUR MEMBERS: the first two hold voices, the last two wait -
        pending, in words, and not failed. */
    const auto* third = rig.liveRunOf (members[2]);
    REQUIRE (third != nullptr);
    CHECK (third->track < 0);
    CHECK (third->state == cue::runState::armed);
    CHECK (std::find (third->pending.begin(), third->pending.end(), "voice") != third->pending.end());
    CHECK (rig.published ("/godot/slot/" + rig.strips[2] + "/word") == "pending");

    /*  AND THE RUNNING PANE SAYS WHY NOTHING WILL SOUND, and what number to
        raise (author, 2026-09-25: "Sampler show 'on 2 - pending voice' No
        sound" - on a show of one track). */
    rig.set ("/godot/audio/tracks", "2");
    rig.published ("/godot/audio/tracks");

    std::string said;

    for (const auto& row : client::model::readRuns (*rig.snapshot))
        if (row.id == third->id)
            said = row.samplerWords;

    CHECK (said == "on 3 \xc2\xb7 no free track \xc2\xb7 the show has 2");

    /*  A clip plays and ends; its track frees, and the member that was waiting
        takes it before the finished member is armed again. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    rig.sound (members[0]);
    rig.silence (members[0]);

    REQUIRE (rig.tickUntil ([&rig, &members]
                            {
                                const auto* waiting = rig.liveRunOf (members[2]);
                                return waiting != nullptr && waiting->track >= 0;
                            }));

    CHECK (rig.liveRunOf (members[2])->pending.empty());
}

TEST_CASE ("sampler: a press launches the clip on its strip, at the level its velocity asks for")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[1] + "/velocity", "true");
    rig.set ("/godot/cue/" + members[1] + "/velocityFloor", "-40");
    rig.arm (rig.bankA);

    /*  NO VELOCITY MAPPING: a press on a parked fader strip comes up to unity. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]), osc::Value::int32 (100) });
    const auto* first = rig.liveRunOf (members[0]);
    CHECK (first->launchRequested);
    CHECK (std::abs (first->trim) < 1.0e-9);

    /*  VELOCITY ON: 64 is half way from the floor to unity, in dB. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]), osc::Value::int32 (64) });
    const auto* second = rig.liveRunOf (members[1]);
    CHECK (second->launchRequested);
    CHECK (std::abs (second->trim - (-40.0 + 40.0 * 63.0 / 126.0)) < 1.0e-9);

    /*  A velocity out of range is refused; a strip with nothing on it is not a
        mistake. */
    CHECK (rig.send ("strip.press", { osc::Value::string (rig.strips[2]), osc::Value::int32 (200) })
             .rejected == 1);
    CHECK (rig.send ("strip.press", { osc::Value::string (rig.strips[7]) }).applied == 1);

    /*  And the press is a step, letter p, which the live recorder keeps. */
    CHECK (rig.published ("/godot/list/" + rig.listId + "/history").find (":p") != std::string::npos);
}

TEST_CASE ("sampler: a strip's run says how loud it left its track, after the fader, and nothing once it ends")
{
    /*  The author, 2026-09-25: "On the sampler fader displays of the D700 can
        we have a post fader level meter too?" The output stage's peak is
        taken once a tick, so each tick reads its own. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.arm (rig.bankA);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]), osc::Value::int32 (100) });
    rig.sound (members[0]);

    const auto* run = rig.liveRunOf (members[0]);
    REQUIRE (run != nullptr);
    const auto runId = run->id;
    const auto track = run->track;

    //  Half of full scale left the track in a tick: -6 dB, published to a tenth.
    rig.audio.peaks[track] = 0.5f;
    rig.tickOnce();
    CHECK (rig.runs.find (runId)->meter == doctest::Approx (-6.0206).epsilon (1e-3));
    CHECK (std::stod (rig.published ("/godot/run/" + runId + "/meter")) == doctest::Approx (-6.0));

    //  Taken and not kept: a tick with nothing out of it is silence again.
    rig.tickOnce();
    CHECK (rig.runs.find (runId)->meter == doctest::Approx (cue::Run::silentDb));

    //  And a run that has ended is silent, whatever was taken last.
    rig.audio.peaks[track] = 0.9f;
    rig.silence (members[0]);

    REQUIRE (rig.tickUntil ([&rig, &runId]
                            {
                                const auto* ended = rig.runs.find (runId);
                                return ended != nullptr && ended->isFinished();
                            }));

    rig.tickOnce();
    CHECK (rig.runs.find (runId)->meter == doctest::Approx (cue::Run::silentDb));
}

TEST_CASE ("sampler: a second press does what the clip says it does")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[1] + "/secondPress", "noop");
    rig.set ("/godot/cue/" + members[2] + "/secondPress", "stop");
    rig.arm (rig.bankA);

    for (int i = 0; i < 3; ++i)
    {
        rig.send ("strip.press", { osc::Value::string (rig.strips[static_cast<std::size_t> (i)]) });
        rig.sound (members[static_cast<std::size_t> (i)]);
    }

    const auto restartRun = rig.liveRunOf (members[0])->id;
    const auto stopsBefore = rig.audio.stopped.size();

    /*  restart, the default: the same run, taken from the top. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    CHECK (rig.liveRunOf (members[0])->id == restartRun);
    CHECK (rig.audio.stopped.size() == stopsBefore + 1);

    /*  noop: nothing. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) });
    CHECK (rig.liveRunOf (members[1])->state == cue::runState::playing);

    /*  stop: the release fade, and the clip goes. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[2]) });
    CHECK (rig.liveRunOf (members[2])->state == cue::runState::stopping);
}

TEST_CASE ("sampler: a hold clip stops when it is let go, and is armed again for the next press")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/release", "hold");
    rig.arm (rig.bankA);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.sound (members[0]);

    const auto* held = rig.liveRunOf (members[0]);
    const auto heldId = held->id;
    CHECK (held->held);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "held");

    /*  ANOTHER HAND CANNOT PRESS IT OR LET IT GO while it is held (PRD §3.27). */
    rig.send ("strip.release", { osc::Value::string (rig.strips[0]) }, "surface:OTHER");
    CHECK (rig.liveRunOf (members[0])->held);

    /*  The hand that pressed it lets go: a short fade, then the stop. */
    rig.send ("strip.release", { osc::Value::string (rig.strips[0]) }, "window");
    CHECK (rig.runs.find (heldId)->state == cue::runState::stopping);

    REQUIRE (rig.tickUntil ([&rig, &heldId] { return rig.runs.find (heldId)->isFinished(); }));

    /*  AND THE MEMBER IS BACK ON ITS STRIP, a fresh run at its initial level:
        a clip plays any number of times. */
    REQUIRE (rig.tickUntil ([&rig, &members, &heldId]
                            {
                                const auto* again = rig.liveRunOf (members[0]);
                                return again != nullptr && again->id != heldId;
                            }));

    const auto* again = rig.liveRunOf (members[0]);
    CHECK (rig.holds (again, rig.strips[0]));
    CHECK (std::abs (again->trim) < 1.0e-9);
    CHECK_FALSE (again->held);
}

TEST_CASE ("sampler: a pad let go before its clip sounded leaves the fader up, and starts nothing")
{
    /*  FOUND RECORDING THE FIXTURE, under the lift-start rule since replaced:
        a pad let go before the launch was placed left the member armed with
        its trim up, and the next tick read that as a fader lifted from the
        bottom and started the clip nobody was pressing. A touch starts a clip
        now, and the case stays: nothing but a hand landing on the fader starts
        a member that a released press left armed. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/release", "hold");
    rig.arm (rig.bankA);

    REQUIRE (rig.engine.submit ("window", "strip.press", { osc::Value::string (rig.strips[0]) }));
    REQUIRE (rig.engine.submit ("window", "strip.release", { osc::Value::string (rig.strips[0]) }));
    rig.tickOnce();

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto* run = rig.liveRunOf (members[0]);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::armed);
    CHECK_FALSE (run->launchRequested);
    CHECK (std::abs (run->trim) < 1.0e-9);
    CHECK (rig.audio.launched.empty());
}

TEST_CASE ("sampler: a bank that takes over the whole desk closes the other, and a sounding clip finishes first")
{
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];
    const auto& b = rig.membersOf[rig.bankB];
    rig.set ("/godot/cue/" + rig.bankB + "/takeover", "group");

    rig.arm (rig.bankA);
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    rig.sound (a[0]);

    const auto playingId = rig.liveRunOf (a[0])->id;
    const auto idleId = rig.liveRunOf (a[1])->id;

    rig.arm (rig.bankB);

    /*  CLOSED, NOT KILLED: the idle member is ended and its strip goes to the
        new bank at once; the sounding clip plays on, and the new member on its
        strip waits for it - pending, in words. */
    REQUIRE (rig.tickUntil ([&rig, &idleId] { return rig.runs.find (idleId)->isFinished(); }));
    CHECK (rig.runs.find (playingId)->state == cue::runState::playing);
    CHECK (rig.liveRunOf (rig.bankA)->closing);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "closing");

    REQUIRE (rig.tickUntil ([&rig, &b] { return rig.holds (rig.liveRunOf (b[1]), rig.strips[1]); }));

    const auto* waiting = rig.liveRunOf (b[0]);
    REQUIRE (waiting != nullptr);
    CHECK_FALSE (rig.holds (waiting, rig.strips[0]));

    /*  The clip ends on its own; the strip hands over; Bank A, with nothing left
        sounding, completes. */
    rig.silence (a[0]);
    REQUIRE (rig.tickUntil ([&rig, &b] { return rig.holds (rig.liveRunOf (b[0]), rig.strips[0]); }));
    REQUIRE (rig.tickUntil ([&rig] { return rig.liveRunOf (rig.bankA) == nullptr; }));
}

TEST_CASE ("sampler: strip takeover takes the strips it lands on and nothing else")
{
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];
    const auto& b = rig.membersOf[rig.bankB];
    rig.set ("/godot/cue/" + rig.bankB + "/takeover", "strip");

    rig.arm (rig.bankA);
    rig.arm (rig.bankB);

    /*  Bank B's two members land on strips one and two; Bank A keeps three and
        four, and re-arms nothing on the two it lost. */
    REQUIRE (rig.tickUntil ([&rig, &b]
                            {
                                return rig.holds (rig.liveRunOf (b[0]), rig.strips[0])
                                         && rig.holds (rig.liveRunOf (b[1]), rig.strips[1]);
                            }));

    CHECK (rig.holds (rig.liveRunOf (a[2]), rig.strips[2]));
    CHECK (rig.holds (rig.liveRunOf (a[3]), rig.strips[3]));
    CHECK (rig.liveRunOf (a[0]) == nullptr);
    CHECK (rig.liveRunOf (a[1]) == nullptr);
    CHECK_FALSE (rig.liveRunOf (rig.bankA)->closing);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.liveRunOf (a[0]) == nullptr);

    /*  REFRESH: GO on Bank A again claims its lost strips back, in its own
        mode (group, the default) - which closes Bank B. */
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), rig.bankA).ok);
    rig.tickOnce();
    rig.send ("go");

    REQUIRE (rig.tickUntil ([&rig, &a]
                            {
                                return rig.holds (rig.liveRunOf (a[0]), rig.strips[0])
                                         && rig.holds (rig.liveRunOf (a[1]), rig.strips[1]);
                            }));

    REQUIRE (rig.tickUntil ([&rig] { return rig.liveRunOf (rig.bankB) == nullptr; }));
}

TEST_CASE ("sampler: a touch starts the clip where its fader waits, and let go at the bottom stops a hold clip")
{
    /*  THE AUTHOR'S RULE OF 2026-09-23: a sample has an initial level, its
        fader flies there when it is armed and waits, and a hand landing on the
        fader starts it - at wherever the fader is by then. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/release", "hold");
    rig.set ("/godot/cue/" + members[0] + "/initialLevel", "-12");
    rig.set ("/godot/cue/" + members[1] + "/release", "playOut");
    rig.arm (rig.bankA);

    const auto trimOf = [&rig, &members] (std::size_t i)
    {
        return "/godot/run/" + rig.liveRunOf (members[i])->id + "/trim";
    };

    const auto at = [&rig, &members] (std::size_t i, double decibels)
    {
        return std::abs (rig.liveRunOf (members[i])->trim - decibels) < 1.0e-9;
    };

    //  IT WAITS AT ITS INITIAL LEVEL, and nothing sounds.
    CHECK (at (0, -12.0));
    CHECK_FALSE (rig.liveRunOf (members[0])->launchRequested);

    /*  A MOVE WITH NO HAND ON IT is a level set in advance - the page, or a
        script - and starts nothing. */
    const auto hold = trimOf (0);
    rig.send ("node.set", { osc::Value::string (hold), osc::Value::float64 (-9.0) }, "window");
    rig.tickOnce();
    rig.tickOnce();
    CHECK_FALSE (rig.liveRunOf (members[0])->launchRequested);
    CHECK (at (0, -9.0));

    /*  A HAND LANDS: touch-start, at the level the fader is at - not lifted,
        not moved, because the motor cannot move under the hand. */
    rig.send ("node.touch", { osc::Value::string (hold) }, "surface:DESK");
    rig.tickOnce();

    CHECK (rig.liveRunOf (members[0])->launchRequested);
    CHECK (at (0, -9.0));

    rig.sound (members[0]);

    /*  A DIP TO THE BOTTOM WHILE TOUCHED IS A RIDE, not a release. */
    rig.send ("node.set", { osc::Value::string (hold), osc::Value::float64 (-120.0) }, "surface:DESK");
    rig.tickOnce();
    CHECK (rig.liveRunOf (members[0])->state == cue::runState::playing);

    /*  LET GO AT THE BOTTOM: fader-stop. */
    const auto stoppingId = rig.liveRunOf (members[0])->id;
    rig.send ("node.release", { osc::Value::string (hold) }, "surface:DESK");
    rig.tickOnce();
    CHECK (rig.runs.find (stoppingId)->state == cue::runState::stopping);

    /*  A PLAY-OUT CLIP, touched, starts at its initial level - nought here -
        and taken to the bottom and let go it is muted, not stopped. */
    const auto playOut = trimOf (1);
    rig.send ("node.touch", { osc::Value::string (playOut) }, "surface:DESK");
    rig.tickOnce();
    CHECK (rig.liveRunOf (members[1])->launchRequested);
    CHECK (at (1, 0.0));

    rig.sound (members[1]);
    const auto playingId = rig.liveRunOf (members[1])->id;

    rig.send ("node.set", { osc::Value::string (playOut), osc::Value::float64 (-120.0) }, "surface:DESK");
    rig.send ("node.release", { osc::Value::string (playOut) }, "surface:DESK");
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.liveRunOf (members[1])->state == cue::runState::playing);

    /*  AND A HAND ON A CLIP ALREADY PLAYING IS A RIDE: touching the fader to
        bring the level back up restarts nothing, whatever its second press
        would do. */
    const auto launchesBefore = rig.audio.launched.size();
    rig.send ("node.touch", { osc::Value::string (playOut) }, "surface:DESK");
    rig.send ("node.set", { osc::Value::string (playOut), osc::Value::float64 (-6.0) }, "surface:DESK");
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.liveRunOf (members[1])->id == playingId);
    CHECK (rig.liveRunOf (members[1])->state == cue::runState::playing);
    CHECK (rig.audio.launched.size() == launchesBefore);
    CHECK (at (1, -6.0));
}

TEST_CASE ("sampler: one touch starts one clip, and a hand resting through a re-arm starts nothing")
{
    /*  THE CLIP ENDS UNDER A HAND THAT STAYS DOWN, and the member is armed
        again as a new run. The hand holds the old run's node - the panel keeps
        its grab for the whole ride, and a surface lets go at the handover
        rather than touching the new one - so nothing starts the new run until
        the hand leaves and lands again. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.arm (rig.bankA);

    const auto first = "/godot/run/" + rig.liveRunOf (members[0])->id + "/trim";
    const auto firstId = rig.liveRunOf (members[0])->id;

    rig.send ("node.touch", { osc::Value::string (first) }, "window");
    rig.tickOnce();
    REQUIRE (rig.liveRunOf (members[0])->launchRequested);

    rig.sound (members[0]);
    rig.silence (members[0]);

    REQUIRE (rig.tickUntil ([&rig, &members, &firstId]
                            {
                                const auto* again = rig.liveRunOf (members[0]);
                                return again != nullptr && again->id != firstId;
                            }));

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK_FALSE (rig.liveRunOf (members[0])->launchRequested);
    CHECK (rig.liveRunOf (members[0])->state == cue::runState::armed);

    //  The hand leaves, and lands on the new run's fader: that is a start.
    rig.send ("node.release", { osc::Value::string (first) }, "window");

    const auto second = "/godot/run/" + rig.liveRunOf (members[0])->id + "/trim";
    rig.send ("node.touch", { osc::Value::string (second) }, "window");
    rig.tickOnce();
    CHECK (rig.liveRunOf (members[0])->launchRequested);

    /*  THE DWELL IS NOUGHT (author, 2026-09-23: a touch starts the clip), so a
        touch counts on the tick it is seen. The number is there for the bench
        - the D700's stray touches - and this pins what it is today. */
    CHECK (cue::Runner::FaderEdge::touchDwellTicks == 0);
}

TEST_CASE ("sampler: a press on a fader somebody pulled down plays at the initial level, and a pad starts there")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/initialLevel", "-6");
    rig.set ("/godot/cue/" + members[1] + "/initialLevel", "-120");
    rig.arm (rig.bankA);

    const auto trimOf = [&rig, &members] (std::size_t i)
    {
        return "/godot/run/" + rig.liveRunOf (members[i])->id + "/trim";
    };

    /*  PULLED TO THE BOTTOM WITH NO HAND ON IT, then pressed from a pad or an
        encoder: a press is a request to hear it, so it plays at the member's
        initial level... */
    rig.send ("node.set", { osc::Value::string (trimOf (0)), osc::Value::float64 (-120.0) }, "window");
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    CHECK (rig.liveRunOf (members[0])->launchRequested);
    CHECK (std::abs (rig.liveRunOf (members[0])->trim - (-6.0)) < 1.0e-9);

    //  ...or at unity, when the initial level is the bottom too.
    CHECK (rig.liveRunOf (members[1])->trim <= -119.0);
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) }, "window");
    CHECK (std::abs (rig.liveRunOf (members[1])->trim) < 1.0e-9);
}

TEST_CASE ("sampler: a pad's clip starts at its initial level too")
{
    /*  THE SAME PANEL MADE A PAD CONTROLLER: its strips become gates, and a
        member armed on a pad starts at its initial level as one on a fader
        does - a press without velocity plays it there. */
    Rig rig;
    const auto panel = rig.document.findById (rig.strips[0]).getParent()[juce::Identifier ("id")]
                          .toString().toStdString();
    rig.set ("/godot/surface/" + panel + "/profile", "midiPads");

    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/initialLevel", "-3");
    rig.arm (rig.bankA);

    CHECK (std::abs (rig.liveRunOf (members[0])->trim - (-3.0)) < 1.0e-9);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "surface:PADS");
    CHECK (rig.liveRunOf (members[0])->launchRequested);
    CHECK (std::abs (rig.liveRunOf (members[0])->trim - (-3.0)) < 1.0e-9);

    //  And a pad is never touch-started: a touch on its node is only a ride.
    const auto trim = "/godot/run/" + rig.liveRunOf (members[1])->id + "/trim";
    rig.send ("node.touch", { osc::Value::string (trim) }, "surface:PADS");
    rig.tickOnce();
    CHECK_FALSE (rig.liveRunOf (members[1])->launchRequested);
}

TEST_CASE ("sampler: a sequence that plays itself arms a bank and goes on, and lasts until the bank is stopped")
{
    /*  A SAMPLER GROUP IS A WINDOW ON THE SIDE OF THE CUES (author,
        2026-09-23): its clips can be played at any moment, and the cue list
        goes on until the bank is stopped. At the top of a list that was always
        so - GO arms it and the pointer moves past. Inside an automatic
        sequence it was not: the sequence waited for its member to finish, and
        a bank only finishes when somebody stops it. */
    Rig rig;

    const auto scene = rig.document.createCue (rig.listId, 3, "group", "Scene").id;
    rig.set ("/godot/cue/" + scene + "/mode", "sequence");
    rig.set ("/godot/cue/" + scene + "/advance", "auto");

    const auto before = rig.document.createCue (scene, 0, "memo", "Before").id;
    const auto pads = rig.document.createCue (scene, 1, "group", "Pads").id;
    rig.set ("/godot/cue/" + pads + "/mode", "sampler");
    const auto clip = rig.document.createCue (pads, 0, "media", "Clip").id;
    rig.set ("/godot/cue/" + clip + "/file", "clip.wav");
    const auto later = rig.document.createCue (scene, 2, "memo", "Later").id;

    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), scene).ok);
    rig.tickOnce();
    rig.send ("go");

    //  THE BANK IS ARMED, AND THE SEQUENCE WENT ON PAST IT.
    REQUIRE (rig.tickUntil ([&rig, &later] { return rig.runsOf (later) > 0; }));
    REQUIRE (rig.tickUntil ([&rig, &later]
                            {
                                for (const auto& run : rig.runs.all())
                                    if (run.cue == later && run.isFinished())
                                        return true;

                                return false;
                            }));

    CHECK (rig.runsOf (before) == 1u);
    REQUIRE (rig.liveRunOf (pads) != nullptr);
    CHECK (rig.liveRunOf (pads)->state == cue::runState::playing);
    REQUIRE (rig.liveRunOf (clip) != nullptr);
    CHECK (rig.holds (rig.liveRunOf (clip), rig.strips[0]));

    //  AND THE SCENE LASTS AS LONG AS ITS PADS: nothing left to play, and not over.
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.liveRunOf (scene) != nullptr);
    CHECK_FALSE (rig.liveRunOf (scene)->isFinished());

    //  STOPPED, THE BANK ENDS - and with it the scene.
    const auto sceneId = rig.liveRunOf (scene)->id;
    rig.send ("run.stop", { osc::Value::string (rig.liveRunOf (pads)->id) });

    REQUIRE (rig.tickUntil ([&rig, &sceneId] { return rig.runs.find (sceneId)->isFinished(); }));
    CHECK (rig.liveRunOf (pads) == nullptr);
}

TEST_CASE ("sampler: a member fired by name is a press on its strip, and without one it is refused")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];

    /*  NOT ARMED: no strip under it, nowhere to play it from. */
    CHECK (rig.send ("cue.fire", { osc::Value::string (members[0]) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("needs-strip") != std::string::npos);

    rig.arm (rig.bankA);

    CHECK (rig.send ("cue.fire", { osc::Value::string (members[0]) }).applied >= 1);
    CHECK (rig.liveRunOf (members[0])->launchRequested);
    CHECK (rig.runsOf (members[0]) == 1u);
}

TEST_CASE ("sampler: a member is not a place for the pointer, and the group is")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];

    const auto refused = rig.send ("standby.set", { osc::Value::string (members[0]) });
    CHECK (refused.rejected == 1);
    CHECK (rig.engine.lastError().find ("not-a-stop") != std::string::npos);

    CHECK (rig.send ("standby.set", { osc::Value::string (rig.bankA) }).applied >= 1);
}

TEST_CASE ("sampler: a stop cue aimed at the bank disarms it, footer and all")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];

    const auto stop = rig.document.createCue (rig.listId, 3, "transport", "Disarm").id;
    rig.set ("/godot/cue/" + stop + "/target", rig.bankA);

    rig.arm (rig.bankA);
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    rig.sound (members[0]);

    rig.send ("cue.fire", { osc::Value::string (stop) });

    REQUIRE (rig.tickUntil ([&rig] { return rig.liveRunOf (rig.bankA) == nullptr; }));

    for (const auto& member : members)
        CHECK (rig.liveRunOf (member) == nullptr);

    /*  AND NOTHING IS ARMED AGAIN: the strips are free. */
    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "free");
}

TEST_CASE ("sampler: each strip says what it rides, what it is doing and whose it is")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];

    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "free");
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/target").empty());

    rig.arm (rig.bankA);

    const auto* run = rig.liveRunOf (members[0]);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "armed");
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/target")
             == "/godot/run/" + run->id + "/trim");
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/cue") == members[0]);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/holder") == run->id);
    CHECK (rig.published ("/godot/run/" + run->id + "/strip") == rig.strips[0]);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "playing");

    /*  A DCA STRIP rides its DCA, and says so. */
    const auto band = rig.document.createDca ("Band").id;
    rig.set ("/godot/slot/" + rig.strips[7] + "/role", "dca");
    CHECK (rig.published ("/godot/slot/" + rig.strips[7] + "/word") == "unassigned");
    rig.set ("/godot/slot/" + rig.strips[7] + "/dca", band);
    CHECK (rig.published ("/godot/slot/" + rig.strips[7] + "/word") == "dca");
    CHECK (rig.published ("/godot/slot/" + rig.strips[7] + "/target") == "/godot/dca/" + band + "/trim");
}

//==============================================================================
/*  A MEMBER NAMES ITS STRIP (author, 2026-09-25: "I need to specify which
    track goes where"). The pin first; everybody else on what is left, in
    member order - the placement that was the whole rule until today. */
#include <wfg/client/model/Surfaces.h>

TEST_CASE ("sampler: a member names its strip, and the others fill what is left in order")
{
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];

    rig.set ("/godot/cue/" + a[2] + "/strip", rig.strips[0]);
    rig.arm (rig.bankA);

    CHECK (rig.holds (rig.liveRunOf (a[2]), rig.strips[0]));
    CHECK (rig.holds (rig.liveRunOf (a[0]), rig.strips[1]));
    CHECK (rig.holds (rig.liveRunOf (a[1]), rig.strips[2]));
    CHECK (rig.holds (rig.liveRunOf (a[3]), rig.strips[3]));

    //  And the tree says the same strips, so the menu cannot disagree with the arm.
    CHECK (rig.published ("/godot/cue/" + a[2] + "/stripNow") == rig.strips[0]);
    CHECK (rig.published ("/godot/cue/" + a[0] + "/stripNow") == rig.strips[1]);
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/cue") == a[2]);

    /*  A PRESS ON THE PINNED STRIP PLAYS THE PINNED CLIP, which is the point
        of the pin: the hand goes to the fader somebody wrote down. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
    CHECK (rig.liveRunOf (a[2])->launchRequested);
    CHECK_FALSE (rig.liveRunOf (a[0])->launchRequested);
}

TEST_CASE ("sampler: two members naming one strip - the first has it, the other is placed as if it named none")
{
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];

    rig.set ("/godot/cue/" + a[1] + "/strip", rig.strips[5]);
    rig.set ("/godot/cue/" + a[3] + "/strip", rig.strips[5]);
    rig.arm (rig.bankA);

    CHECK (rig.holds (rig.liveRunOf (a[1]), rig.strips[5]));
    CHECK (rig.holds (rig.liveRunOf (a[0]), rig.strips[0]));
    CHECK (rig.holds (rig.liveRunOf (a[2]), rig.strips[1]));
    CHECK (rig.holds (rig.liveRunOf (a[3]), rig.strips[2]));
}

TEST_CASE ("sampler: a strip that is not a sampler strip is no pin, and the member still plays")
{
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];

    /*  A DCA STRIP rides its DCA and never a clip: a member naming one is
        placed automatically rather than left silent, and the strip goes on
        riding the DCA. */
    rig.set ("/godot/slot/" + rig.strips[6] + "/role", "dca");
    rig.set ("/godot/cue/" + a[0] + "/strip", rig.strips[6]);
    rig.arm (rig.bankA);

    CHECK (rig.holds (rig.liveRunOf (a[0]), rig.strips[0]));
    CHECK (rig.published ("/godot/cue/" + a[0] + "/stripNow") == rig.strips[0]);
    CHECK (rig.published ("/godot/slot/" + rig.strips[6] + "/cue").empty());
}

TEST_CASE ("sampler: the strip menu says what each strip carries - this group, the list before it, or free")
{
    /*  Bank A, first in the list, takes strips one to four in order. Bank B
        pins its first member to strip three and lets the second fall where
        automatic puts it - strip one. The menu of Bank B's second member must
        say so strip by strip, in words (author, 2026-09-25: "it should state
        what is the previous assignment in chronological order of the cuelist
        unless it's free"). */
    Rig rig;
    const auto& a = rig.membersOf[rig.bankA];
    const auto& b = rig.membersOf[rig.bankB];

    rig.set ("/godot/cue/" + b[0] + "/strip", rig.strips[2]);

    const auto before = rig.published ("/godot/cue/" + b[1] + "/stripsBefore");
    CHECK (before == rig.strips[0] + " " + a[0] + " " + rig.strips[1] + " " + a[1] + " "
                       + rig.strips[2] + " " + a[2] + " " + rig.strips[3] + " " + a[3]);
    CHECK (rig.published ("/godot/cue/" + a[0] + "/stripsBefore").empty());
    CHECK (rig.published ("/godot/cue/" + b[0] + "/stripNow") == rig.strips[2]);
    CHECK (rig.published ("/godot/cue/" + b[1] + "/stripNow") == rig.strips[0]);

    const auto choices = client::model::stripChoices (*rig.snapshot, b[1]);
    REQUIRE (choices.size() == 9u);             // automatic, and the eight faders

    const std::string dash = "\xe2\x80\x94";

    CHECK (choices[0].first.empty());
    CHECK (choices[0].second == "automatic " + dash + " Panel · fader 1");

    CHECK (choices[1].first == rig.strips[0]);
    CHECK (choices[1].second == "Panel · fader 1 " + dash + " previously \"Bank A 0\"");
    CHECK (choices[3].first == rig.strips[2]);
    CHECK (choices[3].second == "Panel · fader 3 " + dash + " \"Bank B 0\" in this group");
    CHECK (choices[5].second == "Panel · fader 5 " + dash + " free");

    /*  PINNED, "automatic" says what choosing it would do rather than where it
        would land: that depends on the other members, and the menu does not
        guess. */
    const auto pinned = client::model::stripChoices (*rig.snapshot, b[0]);
    CHECK (pinned[0].second == "automatic, on the next strip free");

    /*  A DCA STRIP IS NOT OFFERED: it rides its DCA. */
    rig.set ("/godot/slot/" + rig.strips[7] + "/role", "dca");
    rig.published ("/godot/cue/" + b[1] + "/stripNow");
    CHECK (client::model::stripChoices (*rig.snapshot, b[1]).size() == 8u);
}
