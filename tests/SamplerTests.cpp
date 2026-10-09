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
#include "FakeVideoSink.h"

#include <wfg/client/model/Text.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/engine/video/DcaOpacity.h>
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
#include <wfg/engine/log/EventLog.h>
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
        std::int64_t samplesElapsed() const override         { return elapsed; }
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

        //  The audio's clock: still unless a case moves it, as a movie's playhead needs (§49).
        std::int64_t elapsed = 0;

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

            /*  A script presses GO here, as fast as the ticks come; a show that
                says nothing refuses a GO inside half a second (2026-09-28). */
            set ("/godot/list/goDebounce", "0");
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

TEST_CASE ("sampler: a soloed clip holds its bank - the other strips start nothing until it stops")
{
    /*  The author, 2026-09-25: "The solo switch could be engaged on a track
        to prevent other faders in the bank to trigger ... turned off once the
        sample has finished playing or is stopped." */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.arm (rig.bankA);

    const auto* first = rig.liveRunOf (members[0]);
    REQUIRE (first != nullptr);
    const auto soloed = first->id;

    //  A TOGGLE, and the value it came to is what is applied: on, off, on again.
    CHECK (rig.send ("run.solo", { osc::Value::string (soloed) }).applied == 1);
    CHECK (rig.runs.find (soloed)->solo);
    CHECK (rig.send ("run.solo", { osc::Value::string (soloed) }).applied == 1);
    CHECK_FALSE (rig.runs.find (soloed)->solo);
    rig.send ("run.solo", { osc::Value::string (soloed), osc::Value::boolean (true) });
    CHECK (rig.runs.find (soloed)->solo);
    CHECK (rig.published ("/godot/run/" + soloed + "/solo") == "true");

    //  ANOTHER STRIP OF THE BANK starts nothing: the press is applied and launches nothing.
    CHECK (rig.send ("strip.press", { osc::Value::string (rig.strips[1]), osc::Value::int32 (100) })
             .applied == 1);
    CHECK_FALSE (rig.liveRunOf (members[1])->launchRequested);

    //  Nor a member fired by name, which is a press on its strip.
    rig.send ("cue.fire", { osc::Value::string (members[2]) });
    CHECK_FALSE (rig.liveRunOf (members[2])->launchRequested);

    //  THE SOLOED STRIP ITSELF starts, and keeps its solo while it sounds.
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]), osc::Value::int32 (100) });
    rig.sound (members[0]);
    CHECK (rig.runs.find (soloed)->solo);

    //  IT LETS GO WHEN THE CLIP STOPS, and the bank is free again.
    rig.silence (members[0]);
    REQUIRE (rig.tickUntil ([&rig, &soloed] { return ! rig.runs.find (soloed)->solo; }));

    rig.send ("strip.press", { osc::Value::string (rig.strips[1]), osc::Value::int32 (100) });
    CHECK (rig.liveRunOf (members[1])->launchRequested);

    //  A clip that has stopped takes no solo, and a run that is no sampler clip is refused one.
    CHECK (rig.send ("run.solo", { osc::Value::string (soloed), osc::Value::boolean (true) }).rejected == 0);
    CHECK_FALSE (rig.runs.find (soloed)->solo);

    const auto* bank = rig.runs.find (rig.liveRunOf (members[1])->parent);
    REQUIRE (bank != nullptr);
    CHECK (rig.send ("run.solo", { osc::Value::string (bank->id) }).rejected == 1);
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

TEST_CASE ("sampler: a bank on a running act's row is the bank made ready there, and takes over as it arms")
{
    /*  A BANK ON AN ACT'S ROW (2026-10-01, namespace draft §23.9, J2). The
        horizon makes it ready under the running act, and the GO on its row now
        adopts that block, which the act launches as it launches every member -
        through the door a prepared group nested in another comes in by. That
        door let the group out of its hold and did nothing more, so a bank that
        takes over the whole desk armed beside the bank it should have closed:
        its takeover must not depend on the road the GO took. */
    Rig rig;
    rig.arm (rig.bankA);
    const auto bankARun = rig.liveRunOf (rig.bankA)->id;
    const auto idlePad = rig.liveRunOf (rig.membersOf[rig.bankA][1])->id;

    const auto act = rig.document.createCue (rig.listId, 3, "group", "Act").id;
    const auto opening = rig.document.createCue (act, 0, "memo", "Opening").id;
    const auto bank = rig.document.createCue (act, 1, "group", "Bank C").id;
    rig.set ("/godot/cue/" + bank + "/mode", "sampler");
    rig.set ("/godot/cue/" + bank + "/takeover", "group");
    const auto pad = rig.document.createCue (bank, 0, "media", "Bank C 0").id;
    rig.set ("/godot/cue/" + pad + "/file", "clip0.wav");

    //  Into the act on its opening line; the walk stands on the bank's row, made ready under the act.
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), opening).ok);
    rig.tickOnce();
    rig.send ("go");
    REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)).value_or ("") == bank);
    REQUIRE (rig.tickUntil ([&rig, &bank] { return rig.runs.preparedRunOf (bank) != nullptr; }));

    const auto block = rig.runs.preparedRunOf (bank)->id;
    REQUIRE (rig.liveRunOf (act) != nullptr);
    REQUIRE (rig.runs.find (block)->parent == rig.liveRunOf (act)->id);

    rig.send ("go");                                            // the bank's row
    REQUIRE (rig.tickUntil ([&rig, &pad] { return rig.liveRunOf (pad) != nullptr; }));

    //  The bank the horizon made ready, and it closed the other as it armed.
    REQUIRE (rig.liveRunOf (bank) != nullptr);
    CHECK (rig.liveRunOf (bank)->id == block);
    CHECK (rig.runsOf (bank) == 1u);
    CHECK (rig.runs.find (bankARun)->closing);

    //  So Bank A, closing, ends a pad nobody is playing - a close, not a kill of what sounds.
    CHECK (rig.tickUntil ([&rig, &idlePad] { return rig.runs.find (idlePad)->isFinished(); }));
}

TEST_CASE ("sampler: a bank that is the first row of an act nobody has entered takes over when the GO on its row enters the act")
{
    /*  THE OTHER ROAD INTO THE THIRD DOOR (2026-10-01, namespace draft §23.9,
        IB and IC): the GO on the bank's row enters the act, adopting the act's
        block, and the bank's block - adopted by the same GO - is launched by the
        act when its members begin. With a line in the act's header those begin
        a few ticks later, and the bank's block, left marked for them, was given
        back by the pointer the GO had moved on: the act then launched a revoked
        run, and the bank never armed. */
    for (const auto withHeader : { false, true })
    {
        INFO (std::string (withHeader ? "a line in the act's header" : "nothing in the act's header"));
        Rig rig;
        rig.arm (rig.bankA);
        const auto bankARun = rig.liveRunOf (rig.bankA)->id;
        const auto idlePad = rig.liveRunOf (rig.membersOf[rig.bankA][1])->id;

        const auto act = rig.document.createCue (rig.listId, 3, "group", "Act").id;

        if (withHeader)
        {
            const auto header = rig.document.createRole (act, "header");
            REQUIRE (header.ok);
            rig.document.createCue (header.id, 0, "memo", "House to half");
        }

        const auto bank = rig.document.createCue (act, 0, "group", "Bank C").id;
        rig.set ("/godot/cue/" + bank + "/mode", "sampler");
        rig.set ("/godot/cue/" + bank + "/takeover", "group");
        const auto pad = rig.document.createCue (bank, 0, "media", "Bank C 0").id;
        rig.set ("/godot/cue/" + pad + "/file", "clip0.wav");
        const auto later = rig.document.createCue (act, 1, "memo", "Later").id;

        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), bank).ok);
        rig.tickOnce();
        REQUIRE (rig.tickUntil ([&rig, &bank] { return rig.runs.preparedRunOf (bank) != nullptr; }));

        const auto block = rig.runs.preparedRunOf (bank)->id;
        REQUIRE (rig.runs.preparedRunOf (act) != nullptr);
        REQUIRE (rig.runs.find (block)->parent == rig.runs.preparedRunOf (act)->id);

        rig.send ("go");                                        // the bank's row, entering the act
        REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)).value_or ("") == later);

        CHECK (rig.tickUntil ([&rig, &pad] { return rig.liveRunOf (pad) != nullptr; }));

        REQUIRE (rig.liveRunOf (bank) != nullptr);
        CHECK (rig.liveRunOf (bank)->id == block);
        CHECK (rig.runs.find (block)->warning != cue::runWarning::revoked);
        CHECK (rig.runsOf (bank) == 1u);
        CHECK (rig.runs.find (bankARun)->closing);
        CHECK (rig.tickUntil ([&rig, &idlePad] { return rig.runs.find (idlePad)->isFinished(); }));
    }
}

TEST_CASE ("sampler: a bank made ready inside an act nobody has entered closes nothing before a GO")
{
    /*  A HEADER THE HORIZON TAKES AHEAD (2026-10-01, namespace draft §23.9,
        IE): a bed in the act's header puts the act's block in its preparing
        phase, which launched every child of the block that was not a sound -
        the bank's own block among them, let out of its hold with no GO pressed,
        and since IB taking over as it was: the other bank closed, its idle pad
        ended, by a pointer resting on a row. */
    Rig rig;
    rig.arm (rig.bankA);
    const auto bankARun = rig.liveRunOf (rig.bankA)->id;
    const auto idlePad = rig.liveRunOf (rig.membersOf[rig.bankA][1])->id;

    const auto act = rig.document.createCue (rig.listId, 3, "group", "Act").id;
    const auto header = rig.document.createRole (act, "header");
    REQUIRE (header.ok);
    const auto bed = rig.document.createCue (header.id, 0, "media", "Bed").id;
    rig.set ("/godot/cue/" + bed + "/file", "clip3.wav");

    const auto bank = rig.document.createCue (act, 0, "group", "Bank C").id;
    rig.set ("/godot/cue/" + bank + "/mode", "sampler");
    rig.set ("/godot/cue/" + bank + "/takeover", "group");
    const auto pad = rig.document.createCue (bank, 0, "media", "Bank C 0").id;
    rig.set ("/godot/cue/" + pad + "/file", "clip0.wav");

    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), bank).ok);
    rig.tickOnce();
    REQUIRE (rig.tickUntil ([&rig, &bank, &bed] { return rig.runs.preparedRunOf (bank) != nullptr
                                                          && rig.liveRunOf (bed) != nullptr; }));

    const auto block = rig.runs.preparedRunOf (bank)->id;

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (block)->state == cue::runState::preparing);
    CHECK (rig.runs.liveRunOf (bank) == nullptr);
    CHECK (rig.liveRunOf (pad) == nullptr);
    CHECK_FALSE (rig.runs.find (bankARun)->closing);
    CHECK_FALSE (rig.runs.find (idlePad)->isFinished());
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

TEST_CASE ("sampler: a clip's level lane rides under its strip's fader, each its own term")
{
    /*  THE AUTHOR'S "SAMPLES" (namespace draft §20.1, decision CX): a sampler
        member is a media cue, so it has a lane like any other, and the lane is
        a term of its level beside the hand's trim - the fader moves the trim
        and not the lane, the lane moves neither. This rig's clock stands at
        nought, so the lane is read where the clip starts. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/initialLevel", "-6");
    rig.set ("/godot/cue/" + members[0] + "/levelLane", "0 -10 4 -30");
    rig.arm (rig.bankA);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.sound (members[0]);

    const auto* run = rig.liveRunOf (members[0]);
    REQUIRE (run != nullptr);

    CHECK (std::abs (run->trim - (-6.0)) < 1.0e-9);
    CHECK (std::abs (run->laneDb - (-10.0)) < 1.0e-9);
    CHECK (std::abs (run->level - (-16.0)) < 1.0e-9);

    //  The hand on the fader moves its own term, and the lane stays where it was drawn.
    const auto trim = "/godot/run/" + run->id + "/trim";
    rig.send ("node.set", { osc::Value::string (trim), osc::Value::float64 (-3.0) }, "window");
    rig.tickOnce();

    run = rig.liveRunOf (members[0]);
    REQUIRE (run != nullptr);
    CHECK (std::abs (run->laneDb - (-10.0)) < 1.0e-9);
    CHECK (std::abs (run->level - (-13.0)) < 1.0e-9);
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

TEST_CASE ("sampler: a member is not a place for the pointer, and parking on one lands on the group")
{
    /*  Refused `not-a-stop` until 2026-09-26, when the author asked for a cue
        the pointer cannot stand on to "move the pointer to the group instead
        of showing an error". The member is still not a stop - the walk never
        enters a bank - but the gesture lands on the bank that holds it. */
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    const auto standby = [&rig]
    {
        return rig.document.findById (rig.listId)["standby"].toString().toStdString();
    };

    CHECK (rig.send ("standby.set", { osc::Value::string (members[0]) }).applied >= 1);
    CHECK (standby() == rig.bankA);

    CHECK (rig.send ("standby.set", { osc::Value::string (rig.bankA) }).applied >= 1);
    CHECK (standby() == rig.bankA);

    /*  THE DOCUMENT'S OWN DOOR stays strict: a value written to the node is a
        value, and a door that stored a different one would be a lie. */
    CHECK (rig.send ("node.set", { osc::Value::string ("/godot/list/" + rig.listId + "/standby"),
                                   osc::Value::string (members[0]) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("not-a-stop") != std::string::npos);
    CHECK (standby() == rig.bankA);
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

//==============================================================================
/*  DOH! AND THE PADS (PRD §3.32; the author, 2026-09-30, with the red team's
    reading of his words, D1): a pad fired by name or by a trigger is a trigger
    on the GO's list, and a hand on a pad of the bank the GO armed is that GO
    being played - either way Doh! is refused with its sentence, and the clip
    plays on. A pad on a bank the GO did not arm is not counted. Failed before
    D1: `go.doh` was an unknown command. */
TEST_CASE ("go.doh: a pad of the bank the GO armed is that GO being played, and a pad fired by name is a trigger")
{
    Rig rig;

    SUBCASE ("a pad of a bank armed before the GO: not counted")
    {
        rig.arm (rig.bankA);

        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), rig.after).ok);
        rig.tickOnce();
        REQUIRE (rig.send ("go").rejected == 0);

        REQUIRE (rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }).rejected == 0);
        CHECK (rig.send ("go.doh").rejected == 0);
    }

    SUBCASE ("the GO armed the bank: a hand on its pad, a fader's touch, or a fire by name refuse the Doh")
    {
        rig.arm (rig.bankA);
        const auto& member = rig.membersOf[rig.bankA][0];

        SUBCASE ("a hand on the pad")
        {
            REQUIRE (rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }).rejected == 0);
        }

        SUBCASE ("the fader's touch, which the engine presses")
        {
            REQUIRE (rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "engine").rejected == 0);
        }

        SUBCASE ("the member fired by name")
        {
            REQUIRE (rig.send ("cue.fire", { osc::Value::string (member) }).rejected == 0);
        }

        /*  AND BY A TRIGGER (§24, GI): `trigger.fire`'s own sampler branch notes
            the press only when it was applied - a net for a road no case
            reached. */
        SUBCASE ("a trigger on the member")
        {
            const auto trigger = rig.document.createTrigger (member, "osc");
            REQUIRE (trigger.ok);
            rig.set ("/godot/trigger/" + trigger.id + "/address", "/pads/one");
            REQUIRE (rig.send ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 0);
        }

        rig.sound (member);

        CHECK (rig.send ("go.doh").rejected == 1);
        CHECK (rig.engine.lastError().find ("trigger-after-go") != std::string::npos);

        const auto* clip = rig.liveRunOf (member);
        REQUIRE (clip != nullptr);
        CHECK_FALSE (clip->takenBack);
        CHECK (clip->state == cue::runState::playing);
    }

    SUBCASE ("a pad of another bank fired by name is a trigger on the list too")
    {
        rig.arm (rig.bankA);

        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), rig.after).ok);
        rig.tickOnce();
        REQUIRE (rig.send ("go").rejected == 0);

        REQUIRE (rig.send ("cue.fire", { osc::Value::string (rig.membersOf[rig.bankA][1]) }).rejected == 0);
        CHECK (rig.send ("go.doh").rejected == 1);
        CHECK (rig.engine.lastError().find ("trigger-after-go") != std::string::npos);
    }
}

//==============================================================================
TEST_CASE ("go.doh: a bank the GO closed opens again; one already finished is armed again")
{
    /*  Doh! D3 (2026-10-03, PRD §3.32, namespace draft §24.13; the design's test
        17). The GO armed a bank that takes over the whole desk and closed the
        one before it: a Doh opens that one again while it still plays a clip,
        and fires it again - nobody's GO, its identifier on the Doh's record -
        once it has finished. Run on the engine sources of 5fd0e76 with this
        case, it failed: the closed bank stayed closed, or gone. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.bankB + "/takeover", "group");
    rig.arm (rig.bankA);

    const auto old = rig.liveRunOf (rig.bankA)->id;

    SUBCASE ("a clip still sounding: the bank opens again")
    {
        rig.send ("strip.press", { osc::Value::string (rig.strips[0]) });
        rig.sound (rig.membersOf[rig.bankA][0]);

        rig.arm (rig.bankB);
        REQUIRE (rig.runs.find (old)->closing);

        REQUIRE (rig.send ("go.doh").rejected == 0);
        CHECK_FALSE (rig.runs.find (old)->closing);
        CHECK_FALSE (rig.runs.find (old)->isFinished());
    }

    SUBCASE ("nothing sounding: the bank had finished, and is armed again")
    {
        rig.arm (rig.bankB);
        REQUIRE (rig.tickUntil ([&rig, &old] { return rig.runs.find (old)->isFinished(); }));

        REQUIRE (rig.send ("go.doh").rejected == 0);

        const auto* again = rig.liveRunOf (rig.bankA);
        REQUIRE (again != nullptr);
        CHECK (again->id != old);
        CHECK (again->goSerial == 0u);

        std::vector<std::string> made;

        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.kind == LogRecord::Kind::applied && record.command == "go.doh")
                for (const auto& arg : record.args)
                    made.push_back (arg.getString());

        CHECK (std::find (made.begin(), made.end(), again->id) != made.end());
    }
}

TEST_CASE ("go.doh: a bank the GO closed, fired again, does not run its header again - nothing sent again to a device left to its operator")
{
    /*  D3's review, sixth finding (2026-10-03, namespace draft §24.13). A bank
        Doh! fires again had already run its header before the GO; run again,
        its header's network cue went out a second time to a lighting desk left
        to its operator. The bank is armed again with its header passed over.
        Written first and run on bd25dd5 with D4 on top: the header ran twice. */
    Rig rig;
    rig.set ("/godot/cue/" + rig.bankB + "/takeover", "group");

    tree::MountDeclaration desk;
    desk.id = "QX7DESK0";
    desk.prefix = "/lx";
    desk.host = "127.0.0.1";
    desk.port = 9;
    REQUIRE (rig.mounts.declare (desk).ok);
    rig.runner.setMounts (&rig.mounts, nullptr);
    REQUIRE (rig.document.createMount ("/lx", "", "QX7DESK0").ok);
    rig.set ("/godot/mount/QX7DESK0/port", "9");

    const auto header = rig.document.createRole (rig.bankA, "header");
    REQUIRE (header.ok);
    const auto lights = rig.document.createCue (header.id, 0, "osc", "Bank lights").id;
    rig.set ("/godot/cue/" + lights + "/address", "/lx/bank");
    rig.set ("/godot/cue/" + lights + "/value", "i:1");

    rig.arm (rig.bankA);
    REQUIRE (rig.runsOf (lights) == 1u);
    const auto old = rig.liveRunOf (rig.bankA)->id;

    rig.arm (rig.bankB);
    REQUIRE (rig.tickUntil ([&rig, &old] { return rig.runs.find (old)->isFinished(); }));

    REQUIRE (rig.send ("go.doh").rejected == 0);

    const auto* again = rig.liveRunOf (rig.bankA);
    REQUIRE (again != nullptr);
    CHECK (again->id != old);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runsOf (lights) == 1u);
    CHECK (rig.liveRunOf (rig.bankA) != nullptr);
}

//==============================================================================
/*  PICTURES IN A SAMPLER GROUP (namespace draft §49). */
TEST_CASE ("sampler: a sound locked to a movie stands in a bank beside its movie and takes no strip (§49, ABE)")
{
    Rig rig;
    const auto clips = rig.group ("Clips", 3, 0);

    const auto movie = rig.document.createCue (clips, 0, "video", "Clip", {},
                                               { { "source", "movie" }, { "file", "clip.mov" } });
    REQUIRE (movie.ok);

    /*  AS A CONVERSION MAKES IT (Conversion.cpp): a sound made straight after
        the movie, then locked to it. */
    const auto sound = rig.document.createCue (clips, 1, "media", "Clip sound");
    REQUIRE (sound.ok);
    rig.set ("/godot/cue/" + sound.id + "/lockedTo", movie.id);

    const auto plain = rig.document.createCue (clips, 2, "media", "Plain");
    REQUIRE (plain.ok);
    rig.set ("/godot/cue/" + plain.id + "/file", "plain.wav");

    //  Its movie plays it, never a strip: no strip, where a sound beside it has one.
    CHECK (rig.published ("/godot/cue/" + sound.id + "/stripNow").empty());
    CHECK_FALSE (rig.published ("/godot/cue/" + plain.id + "/stripNow").empty());
}

namespace
{
    /*  PICTURES ON THE RIG'S STRIPS (namespace draft §49): a canvas, a picture
        side of the test's own, and video members made into a bank. */
    struct Pictures
    {
        explicit Pictures (Rig& rigToUse) : rig (rigToUse)
        {
            rig.runner.setVideo (&sink);
            canvas = rig.document.createCanvas ("Stage").id;
            REQUIRE_FALSE (canvas.empty());
        }

        std::string member (const std::string& bank, const std::string& source, const std::string& file = {})
        {
            std::vector<std::pair<std::string, std::string>> attributes { { "source", source }, { "canvas", canvas } };

            if (! file.empty())
                attributes.emplace_back ("file", file);

            auto& members = rig.membersOf[bank];
            const auto made = rig.document.createCue (bank, static_cast<int> (members.size()), "video",
                                                      "A " + source, {}, attributes);
            REQUIRE (made.ok);
            members.push_back (made.id);
            return made.id;
        }

        //  The DCA factor last placed on a picture's layer: how solid its fader leaves it.
        double factorOf (const std::string& runId)
        {
            const auto& placed = sink.geometry[runId][video::Property::dca];
            return placed.empty() ? 1.0 : placed.back().value;
        }

        /*  A MOVIE'S SOUND, as a conversion makes it (Conversion.cpp): straight
            after the movie, then locked to it. Made after every member, so the
            movie is the last of them. */
        std::string soundOf (const std::string& bank, const std::string& movie, const std::string& file)
        {
            const auto made = rig.document.createCue (bank, static_cast<int> (rig.membersOf[bank].size()), "media",
                                                      "Its sound");
            REQUIRE (made.ok);
            rig.set ("/godot/cue/" + made.id + "/file", file);
            rig.set ("/godot/cue/" + made.id + "/lockedTo", movie);
            return made.id;
        }

        Rig& rig;
        testing::FakeVideoSink sink;
        std::string canvas;
    };
}

TEST_CASE ("sampler: a picture member is armed onto its strip with no voice, and a sound beside it takes the track (§49)")
{
    Rig rig { 1 };
    Pictures pictures { rig };

    const auto bank = rig.group ("Mixed", 3, 0);
    const auto still = pictures.member (bank, "picture", "still.png");
    const auto sound = rig.document.createCue (bank, 1, "media", "Thunder").id;
    rig.set ("/godot/cue/" + sound + "/file", "thunder.wav");
    rig.membersOf[bank].push_back (sound);

    rig.arm (bank);

    /*  ON ITS STRIP, ARMED, WITH NO TRACK: a picture has no voice to hold, and
        the one track the show has is the sound's. */
    const auto* picture = rig.liveRunOf (still);
    REQUIRE (picture != nullptr);
    CHECK (picture->sampler);
    CHECK (picture->track < 0);
    CHECK (picture->state == cue::runState::armed);
    CHECK (rig.holds (picture, rig.strips[0]));

    const auto* voice = rig.liveRunOf (sound);
    REQUIRE (voice != nullptr);
    CHECK (rig.holds (voice, rig.strips[1]));
    CHECK (voice->track == 0);

    CHECK (rig.published ("/godot/cue/" + still + "/stripNow") == rig.strips[0]);

    /*  AND IT STAYS THE ONE RUN: armed once, not spawned again every tick. */
    for (int n = 0; n < 100; ++n)
        rig.tickOnce();

    CHECK (rig.runsOf (still) == 1u);
    CHECK (rig.runsOf (sound) == 1u);
    CHECK (rig.liveRunOf (still)->track < 0);
}

TEST_CASE ("sampler: a press brings a picture of any kind up, and its fader is how solid it is (§49, AAV, AAW)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const std::vector<std::pair<std::string, std::string>> kinds {
        { "fill", "" }, { "mask", "" }, { "picture", "still.png" }, { "movie", "clip.mov" }, { "capture", "" } };

    std::vector<std::string> members;

    for (const auto& [source, file] : kinds)
        members.push_back (pictures.member (bank, source, file));

    //  The second waits six dB down its fader.
    rig.set ("/godot/cue/" + members[1] + "/initialLevel", "-6");

    rig.arm (bank);

    for (std::size_t n = 0; n < members.size(); ++n)
        rig.send ("strip.press", { osc::Value::string (rig.strips[n]) }, "window");

    rig.tickOnce();
    rig.tickOnce();

    for (std::size_t n = 0; n < members.size(); ++n)
    {
        INFO ("a " << kinds[n].first);
        const auto* run = rig.liveRunOf (members[n]);
        REQUIRE (run != nullptr);
        CHECK (run->state == cue::runState::playing);

        const auto* shown = pictures.sink.shownFor (run->id);
        REQUIRE (shown != nullptr);
        CHECK (shown->source == kinds[n].first);
    }

    /*  WHERE ITS FADER WAITS IS HOW SOLID IT COMES UP: along the fader's travel,
        as a DCA's trim is (WE) - and all of it at nought. */
    const auto& first = rig.liveRunOf (members[0])->id;
    const auto& second = rig.liveRunOf (members[1])->id;
    CHECK (pictures.factorOf (first) == doctest::Approx (1.0));
    CHECK (pictures.factorOf (second) == doctest::Approx (video::opacityForTrim (-6.0)));

    /*  AND A HAND ON THE FADER RIDES IT. */
    rig.send ("node.set", { osc::Value::string ("/godot/run/" + first + "/trim"), osc::Value::float64 (-20.0) },
              "window");
    rig.tickOnce();
    CHECK (pictures.factorOf (first) == doctest::Approx (video::opacityForTrim (-20.0)));

    rig.send ("node.set", { osc::Value::string ("/godot/run/" + first + "/trim"), osc::Value::float64 (-120.0) },
              "window");
    rig.tickOnce();
    CHECK (pictures.factorOf (first) == doctest::Approx (0.0));
}

TEST_CASE ("sampler: a hand landing on a picture's fader brings it up where the fader is, and velocity sets it (§49)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto touched = pictures.member (bank, "fill");
    const auto struck = pictures.member (bank, "fill");
    rig.set ("/godot/cue/" + touched + "/initialLevel", "-12");
    rig.set ("/godot/cue/" + struck + "/velocity", "true");

    rig.arm (bank);

    //  TOUCH-START, at the level the fader waits at.
    const auto trim = "/godot/run/" + rig.liveRunOf (touched)->id + "/trim";
    rig.send ("node.touch", { osc::Value::string (trim) }, "surface:DESK");

    REQUIRE (rig.tickUntil ([&rig, &touched]
                            {
                                const auto* run = rig.liveRunOf (touched);
                                return run != nullptr && run->state == cue::runState::playing;
                            }));
    rig.tickOnce();

    const auto touchedRun = rig.liveRunOf (touched)->id;
    REQUIRE (pictures.sink.shownFor (touchedRun) != nullptr);
    CHECK (pictures.factorOf (touchedRun) == doctest::Approx (video::opacityForTrim (-12.0)));

    //  A PAD STRUCK SOFTLY, its velocity the picture's level.
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]), osc::Value::int32 (40) }, "window");
    rig.tickOnce();

    const auto* run = rig.liveRunOf (struck);
    REQUIRE (run != nullptr);
    CHECK (run->trim < 0.0);
    CHECK (pictures.factorOf (run->id) == doctest::Approx (video::opacityForTrim (run->trim)));
}

TEST_CASE ("sampler: two pictures on one layer lie the later pressed on top, and a member fired by name is pressed (§49, AAY)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto a = pictures.member (bank, "fill");
    const auto b = pictures.member (bank, "fill");

    rig.arm (bank);

    //  B first, then A: A comes up later, so it lies on top of B on their one layer.
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) }, "window");
    rig.send ("cue.fire", { osc::Value::string (a) }, "window");
    rig.tickOnce();
    rig.tickOnce();

    const auto* shownA = pictures.sink.shownFor (rig.liveRunOf (a)->id);
    const auto* shownB = pictures.sink.shownFor (rig.liveRunOf (b)->id);
    REQUIRE (shownA != nullptr);
    REQUIRE (shownB != nullptr);
    CHECK (shownA->layer == shownB->layer);
    CHECK (shownA->order > shownB->order);

    //  Fired by name, it was a press of its strip: the same run, armed on it.
    CHECK (rig.runsOf (a) == 1u);
    CHECK (rig.holds (rig.liveRunOf (a), rig.strips[0]));
}

TEST_CASE ("sampler: a held picture let go goes down to black over its release fade, and is armed again (§49)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto still = pictures.member (bank, "picture", "still.png");
    rig.set ("/godot/cue/" + still + "/release", "hold");
    rig.set ("/godot/cue/" + still + "/releaseFade", "0.5");

    rig.arm (bank);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.tickOnce();
    rig.tickOnce();

    const auto runId = rig.liveRunOf (still)->id;
    REQUIRE (pictures.sink.shownFor (runId) != nullptr);

    rig.send ("strip.release", { osc::Value::string (rig.strips[0]) }, "window");
    rig.tickOnce();

    /*  NOT CUT: the next tick it is still going down, from where it was to
        black over half a second - 25 ticks of 960 samples - and taken off on
        the sample it gets there. */
    CHECK_FALSE (rig.runs.find (runId)->isFinished());

    const auto& points = pictures.sink.points[runId];
    REQUIRE (points.size() >= 2u);
    CHECK (points.back().value == doctest::Approx (0.0));
    CHECK (points.back().sample - points[points.size() - 2].sample == 25 * 960);

    REQUIRE_FALSE (pictures.sink.removed.empty());
    CHECK (pictures.sink.removed.back().first == runId);
    CHECK (pictures.sink.removed.back().second == points.back().sample);

    //  It ends when it is black, and the member is armed on its strip again.
    REQUIRE (rig.tickUntil ([&rig, &runId] { return rig.runs.find (runId)->isFinished(); }));
    REQUIRE (rig.tickUntil ([&rig, &still, &runId]
                            {
                                const auto* again = rig.liveRunOf (still);
                                return again != nullptr && again->id != runId;
                            }));
    CHECK (rig.holds (rig.liveRunOf (still), rig.strips[0]));
}

TEST_CASE ("sampler: a held picture's fader let go at the bottom takes it down; one playing out stays until MUTE (§49, AAX)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto held = pictures.member (bank, "fill");
    const auto still = pictures.member (bank, "picture", "still.png");
    rig.set ("/godot/cue/" + held + "/release", "hold");

    rig.arm (bank);

    //  FADER-STOP: touched, ridden to the bottom, let go there.
    const auto heldId = rig.liveRunOf (held)->id;
    const auto heldTrim = "/godot/run/" + heldId + "/trim";
    rig.send ("node.touch", { osc::Value::string (heldTrim) }, "surface:DESK");
    REQUIRE (rig.tickUntil ([&rig, &heldId] { return rig.runs.find (heldId)->state == cue::runState::playing; }));

    rig.send ("node.set", { osc::Value::string (heldTrim), osc::Value::float64 (-120.0) }, "surface:DESK");
    rig.tickOnce();
    CHECK (rig.runs.find (heldId)->state == cue::runState::playing);

    rig.send ("node.release", { osc::Value::string (heldTrim) }, "surface:DESK");
    REQUIRE (rig.tickUntil ([&rig, &heldId] { return rig.runs.find (heldId)->isFinished(); }));

    /*  A STILL PLAYING OUT never ends by itself: at the bottom of its fader it
        is unseen and still up, holding its strip, until its strip's MUTE - a
        kill - takes it away at once. */
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) }, "window");
    rig.tickOnce();
    rig.tickOnce();

    const auto stillId = rig.liveRunOf (still)->id;
    rig.send ("node.set", { osc::Value::string ("/godot/run/" + stillId + "/trim"), osc::Value::float64 (-120.0) },
              "window");

    for (int n = 0; n < 50; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (stillId)->state == cue::runState::playing);
    CHECK (pictures.factorOf (stillId) == doctest::Approx (0.0));
    CHECK (rig.holds (rig.runs.find (stillId), rig.strips[1]));

    rig.send ("run.kill", { osc::Value::string (stillId) }, "surface:DESK");
    REQUIRE (rig.tickUntil ([&rig, &stillId] { return rig.runs.find (stillId)->isFinished(); }));
    rig.tickOnce();

    const auto taken = std::find_if (pictures.sink.removed.begin(), pictures.sink.removed.end(),
                                     [&stillId] (const auto& removal) { return removal.first == stillId; });
    CHECK (taken != pictures.sink.removed.end());
}

TEST_CASE ("sampler: a second press restarts a movie and not a still, and stops a picture over its release fade (§49, ABD)")
{
    Rig rig;
    Pictures pictures { rig };

    const std::map<std::string, double> lengths { { "clip.mov", 10.0 } };
    rig.runner.setMediaDurations (&lengths);

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto still = pictures.member (bank, "picture", "still.png");
    const auto stopper = pictures.member (bank, "fill");
    rig.set ("/godot/cue/" + movie + "/startOffset", "2");
    rig.set ("/godot/cue/" + stopper + "/secondPress", "stop");

    rig.arm (bank);

    for (std::size_t n = 0; n < 3; ++n)
        rig.send ("strip.press", { osc::Value::string (rig.strips[n]) }, "window");

    //  The audio's clock moving, a tick a tick, so the movie's playhead does.
    for (int n = 0; n < 30; ++n)
    {
        rig.audio.elapsed += 960;
        rig.tickOnce();
    }

    //  THE MOVIE, from where GO starts it: its playhead put back to two seconds.
    const auto movieId = rig.liveRunOf (movie)->id;
    const auto& times = pictures.sink.geometry[movieId][video::Property::time];
    REQUIRE_FALSE (times.empty());
    CHECK (times.back().value > 2.0);

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.liveRunOf (movie)->id == movieId);
    CHECK (std::any_of (times.end() - 3, times.end(), [] (const video::Point& point)
                        { return std::abs (point.value - 2.0) < 0.05; }));

    //  THE STILL has no top to go back to: nothing happens.
    const auto stillId = rig.liveRunOf (still)->id;
    const auto shownBefore = pictures.sink.shown.size();
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) }, "window");
    rig.tickOnce();
    CHECK (rig.liveRunOf (still)->id == stillId);
    CHECK (pictures.sink.shown.size() == shownBefore);

    //  A SECOND PRESS THAT STOPS takes a picture down over its release fade.
    const auto stopperId = rig.liveRunOf (stopper)->id;
    rig.send ("strip.press", { osc::Value::string (rig.strips[2]) }, "window");
    rig.tickOnce();
    CHECK (rig.runs.find (stopperId)->state == cue::runState::stopping);
    REQUIRE (rig.tickUntil ([&rig, &stopperId] { return rig.runs.find (stopperId)->isFinished(); }));
}

TEST_CASE ("sampler: a still playing out keeps its strip when another bank takes over, and hands it on at MUTE (§49, ABJ)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto first = rig.group ("First", 3, 0);
    const auto still = pictures.member (first, "picture", "still.png");

    const auto second = rig.group ("Second", 4, 0);
    const auto next = pictures.member (second, "fill");

    rig.arm (first);
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.tickOnce();
    rig.tickOnce();

    const auto stillId = rig.liveRunOf (still)->id;
    REQUIRE (rig.runs.find (stillId)->state == cue::runState::playing);

    //  THE SECOND BANK ARMS AND TAKES OVER: the still plays on, on its strip.
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), second).ok);
    rig.tickOnce();
    rig.send ("go");

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (stillId)->state == cue::runState::playing);
    CHECK (rig.holds (rig.runs.find (stillId), rig.strips[0]));

    const auto* waiting = rig.liveRunOf (next);
    CHECK ((waiting == nullptr || ! rig.holds (waiting, rig.strips[0])));

    //  MUTE: the still is gone, and the second bank's picture lands on the strip.
    rig.send ("run.kill", { osc::Value::string (stillId) }, "surface:DESK");

    REQUIRE (rig.tickUntil ([&rig, &next]
                            {
                                const auto* landed = rig.liveRunOf (next);
                                return landed != nullptr && rig.holds (landed, rig.strips[0]);
                            }));
}

TEST_CASE ("sampler: an armed bank's pictures are read ahead after the focused standby and before another list's, until it closes (§49, ABF)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto still = pictures.member (bank, "picture", "still.png");
    pictures.member (bank, "movie", "clip.mov");
    pictures.member (bank, "fill");

    //  The cue after the bank, where GO leaves the standby: a still of its own.
    REQUIRE (rig.document.createCue (rig.listId, 4, "video", "Next", {},
                                     { { "source", "picture" }, { "file", "next.png" }, { "canvas", pictures.canvas } }).ok);

    //  And another list, its standby a still too.
    const auto other = rig.document.createList ("Other");
    REQUIRE (other.ok);
    const auto elsewhere = rig.document.createCue (other.id, 0, "video", "Elsewhere", {},
                                                   { { "source", "picture" }, { "file", "elsewhere.png" },
                                                     { "canvas", pictures.canvas } });
    REQUIRE (elsewhere.ok);
    rig.set ("/godot/list/" + other.id + "/standby", elsewhere.id);

    const auto paths = [&pictures]
    {
        std::vector<std::string> out;

        for (const auto& item : pictures.sink.preloads)
            out.push_back (item.path);

        return out;
    };

    rig.arm (bank);
    rig.tickOnce();

    /*  THE FOCUSED LIST'S STANDBY FIRST, which GO acts on; then the bank's still
        and movie - a fill has no file - then the other list's. */
    CHECK (paths() == std::vector<std::string> { "next.png", "still.png", "clip.mov", "elsewhere.png" });

    /*  BETWEEN PRESSES: a member's run ends and the next is armed a tick later,
        and the still is never let go in between. */
    const auto sentBefore = pictures.sink.preloadsSent.size();

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    rig.tickOnce();
    const auto stillId = rig.liveRunOf (still)->id;
    rig.send ("run.kill", { osc::Value::string (stillId) }, "surface:DESK");

    REQUIRE (rig.tickUntil ([&rig, &still, &stillId]
                            {
                                const auto* again = rig.liveRunOf (still);
                                return again != nullptr && again->id != stillId;
                            }));

    for (auto at = sentBefore; at < pictures.sink.preloadsSent.size(); ++at)
    {
        const auto& sent = pictures.sink.preloadsSent[at];
        CHECK (std::any_of (sent.begin(), sent.end(), [] (const video::Preload& item) { return item.path == "still.png"; }));
    }

    /*  ANOTHER BANK TAKES OVER: this one closes, and offers nothing more. */
    rig.arm (rig.bankA);

    REQUIRE (rig.tickUntil ([&paths]
                            {
                                const auto now = paths();
                                return std::find (now.begin(), now.end(), "still.png") == now.end();
                            }));
}

TEST_CASE ("sampler: a held picture pressed and let go in one tick comes up and goes down (§49)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto flash = pictures.member (bank, "fill");
    rig.set ("/godot/cue/" + flash + "/release", "hold");

    rig.arm (bank);
    const auto runId = rig.liveRunOf (flash)->id;

    REQUIRE (rig.engine.submit ("window", "strip.press", { osc::Value::string (rig.strips[0]) }));
    rig.send ("strip.release", { osc::Value::string (rig.strips[0]) }, "window");

    /*  IT ENDS, and its strip is free for the next press: the report that it
        came up, which reaches the run after the stop, does not hand it back to
        playing (RunCommands' `run.started`). */
    REQUIRE (rig.tickUntil ([&rig, &runId] { return rig.runs.find (runId)->isFinished(); }));
    CHECK (pictures.sink.shownFor (runId) != nullptr);

    const auto taken = std::find_if (pictures.sink.removed.begin(), pictures.sink.removed.end(),
                                     [&runId] (const auto& removal) { return removal.first == runId; });
    CHECK (taken != pictures.sink.removed.end());
}

//==============================================================================
/*  A MOVIE'S SOUND IN A BANK (namespace draft §49, ABA, ABH). */
namespace
{
    bool says (const std::vector<std::string>& pending, const char* word)
    {
        return std::find (pending.begin(), pending.end(), word) != pending.end();
    }
}

TEST_CASE ("sampler: a movie's sound is armed with its bank, under its movie, on a voice of its own (§49, ABA)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Movies", 3, 0);
    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    const auto movieId = rig.liveRunOf (movie)->id;

    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    /*  UNDER ITS MOVIE, ON A VOICE, ARMED AND NOT ASKED FOR - and on no strip:
        the movie holds the strip, the sound only comes with it. */
    const auto* voice = rig.liveRunOf (sound);
    REQUIRE (voice != nullptr);
    CHECK (voice->parent == movieId);
    CHECK (voice->waitsForVoice);
    CHECK (voice->track >= 0);
    CHECK (voice->armConfirmed);
    CHECK (voice->state == cue::runState::armed);
    CHECK_FALSE (voice->launchRequested);
    CHECK (voice->strip.empty());
    CHECK (rig.holds (rig.runs.find (movieId), rig.strips[0]));

    //  Made once: one run of each over a hundred ticks.
    for (int n = 0; n < 100; ++n)
        rig.tickOnce();

    CHECK (rig.runsOf (movie) == 1u);
    CHECK (rig.runsOf (sound) == 1u);
}

TEST_CASE ("sampler: a press on a movie waits until its sound is ready, and the two start on one sample (§49, ABH)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Movies", 3, 0);
    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));

    /*  THE DISK HAS NOT ANSWERED for the sound: the press is taken, and nothing
        comes up - a picture ahead of its sound would be out of step with it for
        as long as it played. */
    const auto movieId = rig.liveRunOf (movie)->id;
    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (pictures.sink.shownFor (movieId) == nullptr);
    CHECK (rig.runs.find (movieId)->state == cue::runState::armed);
    CHECK (rig.runs.find (movieId)->launchRequested);
    CHECK (rig.audio.launched.empty());
    CHECK (rig.published ("/godot/slot/" + rig.strips[0] + "/word") == "playing");

    //  IT ANSWERS: the movie comes up, and its sound - the one armed for it - with it.
    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&pictures, &movieId] { return pictures.sink.shownFor (movieId) != nullptr; }));
    rig.tickOnce();

    const auto* voice = rig.liveRunOf (sound);
    REQUIRE (voice != nullptr);
    CHECK (voice->parent == movieId);
    CHECK (rig.runsOf (sound) == 1u);
    REQUIRE (voice->launchedAtSample > 0);
    CHECK (rig.audio.launched == std::vector<int> { voice->track });

    //  ON ONE SAMPLE: the picture's first point, and the movie's clock, are the sound's launch.
    const auto& opacity = pictures.sink.points[movieId];
    const auto& times = pictures.sink.geometry[movieId][video::Property::time];
    REQUIRE_FALSE (opacity.empty());
    REQUIRE_FALSE (times.empty());
    CHECK (opacity.front().sample == voice->launchedAtSample);
    CHECK (times.front().sample == voice->launchedAtSample);
}

TEST_CASE ("sampler: a movie whose sound finds no voice reads pending, and its sound takes the next track that frees (§49, ABH)")
{
    Rig rig { 1 };
    Pictures pictures { rig };

    const auto bank = rig.group ("Mixed", 3, 0);
    const auto thunder = rig.document.createCue (bank, 0, "media", "Thunder").id;
    rig.set ("/godot/cue/" + thunder + "/file", "thunder.wav");
    rig.membersOf[bank].push_back (thunder);

    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));
    rig.tickOnce();

    /*  THE ONE TRACK IS THE SOUND MEMBER'S: the movie's sound waits for a
        voice, and the movie says so, on its row and on its strip. */
    const auto thunderId = rig.liveRunOf (thunder)->id;
    const auto movieId = rig.liveRunOf (movie)->id;
    CHECK (rig.runs.find (thunderId)->track == 0);
    CHECK (rig.liveRunOf (sound)->track < 0);
    CHECK (says (rig.liveRunOf (sound)->pending, "voice"));
    CHECK (says (rig.runs.find (movieId)->pending, "voice"));
    CHECK (rig.published ("/godot/slot/" + rig.strips[1] + "/word") == "pending");
    CHECK (rig.published ("/godot/run/" + movieId + "/pending") == "voice");

    //  PRESSED, it waits for the voice: nothing comes up.
    rig.send ("strip.press", { osc::Value::string (rig.strips[1]) }, "window");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (pictures.sink.shownFor (movieId) == nullptr);

    /*  MUTE ON THE SOUND MEMBER frees the track, and the movie's sound - waiting
        longest - takes it ahead of the member armed again. Then the movie comes
        up, its sound with it. */
    rig.send ("run.kill", { osc::Value::string (thunderId) }, "surface:DESK");

    REQUIRE (rig.tickUntil ([&rig, &sound]
                            {
                                const auto* voice = rig.liveRunOf (sound);
                                return voice != nullptr && voice->track == 0;
                            }));

    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&pictures, &movieId] { return pictures.sink.shownFor (movieId) != nullptr; }));

    CHECK_FALSE (says (rig.runs.find (movieId)->pending, "voice"));
    CHECK (rig.liveRunOf (thunder)->track < 0);
}

TEST_CASE ("sampler: a movie's fader moves its sound too, its meter is its sound's, and letting go fades both (§49, ABA, ABG)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Movies", 3, 0);
    const auto movie = pictures.member (bank, "movie", "clip.mov");
    rig.set ("/godot/cue/" + movie + "/release", "hold");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto movieId = rig.liveRunOf (movie)->id;
    const auto soundId = rig.liveRunOf (sound)->id;

    rig.send ("strip.press", { osc::Value::string (rig.strips[0]) }, "window");
    REQUIRE (rig.tickUntil ([&rig, &soundId] { return rig.runs.find (soundId)->launchedAtSample > 0; }));
    rig.tickOnce();

    const auto before = rig.runs.find (soundId)->level;

    //  TWENTY DB DOWN THE MOVIE'S FADER: the picture and its sound together.
    rig.send ("node.set", { osc::Value::string ("/godot/run/" + movieId + "/trim"), osc::Value::float64 (-20.0) },
              "window");
    rig.tickOnce();

    CHECK (rig.runs.find (soundId)->level == doctest::Approx (before - 20.0));
    CHECK (pictures.factorOf (movieId) == doctest::Approx (video::opacityForTrim (-20.0)));

    //  WHAT LEFT THE SOUND'S TRACK LIGHTS THE MOVIE'S METER.
    rig.audio.peaks[rig.runs.find (soundId)->track] = 0.5f;
    rig.tickOnce();
    CHECK (rig.runs.find (movieId)->meter == doctest::Approx (-6.0206).epsilon (1e-3));

    //  LET GO: the picture to black and its sound down with it, over the release fade.
    rig.send ("strip.release", { osc::Value::string (rig.strips[0]) }, "window");
    CHECK (rig.runs.find (movieId)->state == cue::runState::stopping);
    CHECK (rig.runs.find (soundId)->state == cue::runState::stopping);

    REQUIRE (rig.tickUntil ([&rig, &movieId, &soundId]
                            {
                                return rig.runs.find (movieId)->isFinished() && rig.runs.find (soundId)->isFinished();
                            }));
}

TEST_CASE ("sampler: a movie's sound lets its voice go when its idle movie is killed, and when its bank is taken over (§49)")
{
    Rig rig { 2 };
    Pictures pictures { rig };

    const auto bank = rig.group ("Movies", 3, 0);
    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));

    const auto firstMovie = rig.liveRunOf (movie)->id;
    const auto firstSound = rig.liveRunOf (sound)->id;
    CHECK (rig.runs.find (firstSound)->track >= 0);

    //  MUTE ON THE IDLE MOVIE: its sound goes with it, and both are armed again.
    rig.send ("run.kill", { osc::Value::string (firstMovie) }, "surface:DESK");
    REQUIRE (rig.tickUntil ([&rig, &firstSound] { return rig.runs.find (firstSound)->isFinished(); }));

    REQUIRE (rig.tickUntil ([&rig, &sound, &firstSound]
                            {
                                const auto* again = rig.liveRunOf (sound);
                                return again != nullptr && again->id != firstSound && again->track >= 0;
                            }));

    const auto secondSound = rig.liveRunOf (sound)->id;
    CHECK (rig.runs.find (secondSound)->parent == rig.liveRunOf (movie)->id);
    CHECK (rig.runs.find (secondSound)->parent != firstMovie);

    //  ANOTHER BANK TAKES OVER: this one closes, and the sound's voice is let go.
    rig.arm (rig.bankA);
    REQUIRE (rig.tickUntil ([&rig, &secondSound] { return rig.runs.find (secondSound)->isFinished(); }));
    CHECK (rig.liveRunOf (sound) == nullptr);
}

TEST_CASE ("sampler: M55a, a pad puts a picture up on the sample it would start a sound, a fader's touch and a movie with its sound one tick later (§49)")
{
    Rig rig;
    Pictures pictures { rig };

    const auto bank = rig.group ("Pictures", 3, 0);
    const auto padded = pictures.member (bank, "picture", "still.png");
    const auto touched = pictures.member (bank, "fill");

    const auto thunder = rig.document.createCue (bank, 2, "media", "Thunder").id;
    rig.set ("/godot/cue/" + thunder + "/file", "thunder.wav");
    rig.membersOf[bank].push_back (thunder);

    const auto movie = pictures.member (bank, "movie", "clip.mov");
    const auto sound = pictures.soundOf (bank, movie, "clip.wav");

    rig.arm (bank);
    REQUIRE (rig.tickUntil ([&rig, &sound] { return rig.liveRunOf (sound) != nullptr; }));
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    /*  THE AUDIO'S CLOCK MOVING A TICK A TICK, as the device's does, and the
        horizon a picture is put up on: a launch's, two ticks at the least. A
        tick's pass runs before its commands are applied (Console's
        `setBeforeTick`, and the rig's `tickOnce`), so a press applied in one
        tick is acted on in the next - for a sound as for a picture. */
    const auto tickOn = [&rig]
    {
        rig.audio.elapsed += 960;
        rig.tickOnce();
    };

    const auto horizon = static_cast<std::int64_t> (std::max (rig.runner.latencyTicks(), 2)) * 960;
    constexpr std::int64_t aTick = 960;

    const auto firstPoint = [&pictures] (const std::string& runId)
    {
        const auto& points = pictures.sink.points[runId];
        return points.empty() ? std::int64_t { -1 } : points.front().sample;
    };

    /*  A PAD, AND A SOUND'S PAD IN THE SAME TICK: the picture up on the very
        sample the sound starts on, a tick and a horizon from the clock the
        presses were applied at. */
    const auto padRun = rig.liveRunOf (padded)->id;
    const auto thunderRun = rig.liveRunOf (thunder)->id;
    rig.audio.elapsed += aTick;
    const auto padAt = rig.audio.elapsed;
    REQUIRE (rig.engine.submit ("window", "strip.press", { osc::Value::string (rig.strips[0]) }));
    rig.send ("strip.press", { osc::Value::string (rig.strips[2]) }, "window");

    for (int n = 0; n < 4; ++n)
        tickOn();

    CHECK (firstPoint (padRun) - padAt == aTick + horizon);
    CHECK (rig.runs.find (thunderRun)->launchedAtSample == firstPoint (padRun));

    //  A HAND LANDING ON A FADER: its press is the engine's record of the touch, a tick later.
    const auto touchRun = rig.liveRunOf (touched)->id;
    rig.audio.elapsed += aTick;
    const auto touchAt = rig.audio.elapsed;
    rig.send ("node.touch", { osc::Value::string ("/godot/run/" + touchRun + "/trim") }, "surface:DESK");

    for (int n = 0; n < 4; ++n)
        tickOn();

    CHECK (firstPoint (touchRun) - touchAt == 2 * aTick + horizon);

    /*  A MOVIE WITH ITS SOUND READY: let go by the bank's tick, a tick later,
        and its sound on the same sample. */
    const auto movieRun = rig.liveRunOf (movie)->id;
    rig.audio.elapsed += aTick;
    const auto movieAt = rig.audio.elapsed;
    rig.send ("strip.press", { osc::Value::string (rig.strips[3]) }, "window");

    for (int n = 0; n < 4; ++n)
        tickOn();

    CHECK (firstPoint (movieRun) - movieAt == 2 * aTick + horizon);

    const auto* voice = rig.liveRunOf (sound);
    REQUIRE (voice != nullptr);
    CHECK (voice->launchedAtSample == firstPoint (movieRun));

    MESSAGE ("M55a, from the clock a press is applied at to its picture's first sample: a pad ",
             static_cast<double> (aTick + horizon) / 48.0, " ms, as a sound's; a touch and a movie with its sound ",
             static_cast<double> (2 * aTick + horizon) / 48.0, " ms");
}
