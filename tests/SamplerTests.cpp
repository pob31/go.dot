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

        /*  A FADER STRIP PARKS THE RUN: its trim starts at silence, so lifting
            the fader is what starts it. */
        CHECK (run->trim <= -119.0);
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

    /*  AND THE MEMBER IS BACK ON ITS STRIP, a fresh run, parked: a clip plays
        any number of times. */
    REQUIRE (rig.tickUntil ([&rig, &members, &heldId]
                            {
                                const auto* again = rig.liveRunOf (members[0]);
                                return again != nullptr && again->id != heldId;
                            }));

    const auto* again = rig.liveRunOf (members[0]);
    CHECK (rig.holds (again, rig.strips[0]));
    CHECK (again->trim <= -119.0);
    CHECK_FALSE (again->held);
}

TEST_CASE ("sampler: a pad let go before its clip sounded leaves the fader up, and starts nothing")
{
    /*  FOUND RECORDING THE FIXTURE. A pad pressed on a fader strip lifts the
        trim to unity with no start edge; let go before the launch was placed,
        the member stays armed at that level. The fader-edge rule had only ever
        cleared `parked` on a start, so the next tick read a parked fader at
        unity and started the clip nobody was pressing. */
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

TEST_CASE ("sampler: a fader lifted from the bottom starts the clip, and let go there stops a hold clip")
{
    Rig rig;
    const auto& members = rig.membersOf[rig.bankA];
    rig.set ("/godot/cue/" + members[0] + "/release", "hold");
    rig.set ("/godot/cue/" + members[1] + "/release", "playOut");
    rig.arm (rig.bankA);

    const auto trimOf = [&rig, &members] (std::size_t i)
    {
        return "/godot/run/" + rig.liveRunOf (members[i])->id + "/trim";
    };

    /*  A HAND ON THE FADER, lifting it past the start threshold: fader-start. */
    const auto hold = trimOf (0);
    rig.send ("node.touch", { osc::Value::string (hold) }, "surface:DESK");
    rig.send ("node.set", { osc::Value::string (hold), osc::Value::float64 (-100.0) }, "surface:DESK");
    rig.tickOnce();

    CHECK (rig.liveRunOf (members[0])->launchRequested);
    CHECK (std::abs (rig.liveRunOf (members[0])->trim - (-100.0)) < 1.0e-9);

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

    /*  A PLAY-OUT CLIP taken to the bottom and let go is muted, not stopped. */
    const auto playOut = trimOf (1);
    rig.send ("node.touch", { osc::Value::string (playOut) }, "surface:DESK");
    rig.send ("node.set", { osc::Value::string (playOut), osc::Value::float64 (-50.0) }, "surface:DESK");
    rig.tickOnce();
    rig.sound (members[1]);
    rig.send ("node.set", { osc::Value::string (playOut), osc::Value::float64 (-120.0) }, "surface:DESK");
    rig.send ("node.release", { osc::Value::string (playOut) }, "surface:DESK");
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.liveRunOf (members[1])->state == cue::runState::playing);
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
