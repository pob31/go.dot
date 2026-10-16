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

/*  A SEQUENCE'S JOIN (PRD §3.6; namespace draft §69): Let play, Gapless and
    Crossfade between two members, automatic and manual.

    The audio side here keeps the sample clock in the case's hands and plays a
    voice from the sample it was launched on until it is stopped or its file
    runs out, so a case can ask where a launch was placed, what level each
    voice was given and when, and what was stopped where.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/GroupJoin.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    constexpr int samplesPerTick = 960;     // 48 kHz at 50 Hz
    constexpr int sampleRate = 48000;

    struct JoinPlayer final : cue::Player
    {
        int trackCount() const override            { return 8; }
        std::int64_t samplesElapsed() const override { return samples; }
        int blockSize() const override             { return 128; }
        int channelsPerTrack() const override      { return 2; }
        int slotCount() const override             { return 1; }
        int sampleRate() const override            { return ::sampleRate; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t sample) override
        {
            launchedAt[track] = sample;
            launches.push_back ({ track, sample });
            return true;
        }

        bool stop (int track) override
        {
            launchedAt.erase (track);
            stopped.push_back (track);
            return true;
        }

        bool stopAtSample (int track, int, std::int64_t sample) override
        {
            stopsAt.push_back ({ track, sample });
            stopAt[track] = sample;
            return true;
        }

        void setLevelDb (int track, double levelDb) override
        {
            levels.push_back ({ track, levelDb });
            levelNow[track] = levelDb;
        }

        void setRouting (int, const std::vector<cue::Coefficient>&) override {}

        /*  A VOICE SOUNDS from its launch sample until a stop placed on it, or
            its file's length, is reached. */
        bool isPlaying (int track) const override
        {
            const auto launched = launchedAt.find (track);

            if (launched == launchedAt.end() || samples < launched->second)
                return false;

            if (const auto stops = stopAt.find (track); stops != stopAt.end() && samples >= stops->second)
                return false;

            if (const auto length = lengthOf.find (track); length != lengthOf.end()
                                                           && samples >= launched->second + length->second)
                return false;

            return true;
        }

        bool isArmReady (int track) const override { return ready.count (track) > 0; }

        /** The disk answers every outstanding arm. */
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
                armed.push_back (arm);
            }

            arms.clear();
        }

        std::int64_t samples = 0;
        std::vector<cue::ArmRequest> arms;
        std::vector<cue::ArmRequest> armed;
        std::set<int> ready;
        std::map<int, std::int64_t> launchedAt;
        std::map<int, std::int64_t> stopAt;
        std::map<int, std::int64_t> lengthOf;
        std::vector<std::pair<int, std::int64_t>> launches;
        std::vector<std::pair<int, std::int64_t>> stopsAt;
        std::vector<int> stopped;
        std::vector<std::pair<int, double>> levels;
        std::map<int, double> levelNow;
    };

    struct Rig;

    /*  A RUN, BY ITS IDENTIFIER, looked up at every use: the table moves its
        runs when it makes another, so a pointer held across a tick is one that
        can point at nothing. */
    struct RunRef
    {
        cue::RunTable* table = nullptr;
        std::string id;

        cue::Run* operator->() const { return table->find (id); }
        bool operator== (std::nullptr_t) const { return id.empty() || table->find (id) == nullptr; }
        bool operator!= (std::nullptr_t) const { return ! (*this == nullptr); }
    };

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (samplesPerTick);

            listId = document.createList ("Sound").id;
            document.setAttribute ("/godot/list/goDebounce", "0");
        }

        //  One tick, the sample clock where the tick is, the Runner observing first.
        Engine::TickResult tickOnce()
        {
            audio.samples = tick * samplesPerTick;
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        void apply (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            const auto result = tickOnce();
            REQUIRE_MESSAGE (result.applied >= 1, name << " was refused: " << engine.lastError());
        }

        bool tickUntil (const std::function<bool()>& ready, int bound = 400)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return true;

                tickOnce();
            }

            return ready();
        }

        std::string media (const std::string& parent, int index, const std::string& name)
        {
            const auto made = document.createCue (parent, index, "media", name);
            REQUIRE (made.ok);
            REQUIRE (document.setAttribute ("/godot/cue/" + made.id + "/file", name + ".wav").ok);
            return made.id;
        }

        RunRef runOf (const std::string& cueId)
        {
            RunRef newest { &runs, {} };

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    newest.id = run.id;

            return newest;
        }

        JoinPlayer audio;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (17) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        std::string listId;
        std::int64_t tick = 1;
    };

    std::int64_t samplesOf (double seconds)
    {
        return static_cast<std::int64_t> (std::llround (seconds * sampleRate));
    }

    /*  A MANUAL ACT OF TWO SOUNDS, the join it is asked for: the pointer on its
        first member, as a manual group has it. */
    struct Act
    {
        std::string group, first, second;
    };

    Act manualAct (Rig& rig, const std::string& joinWord, double seconds = 2.0)
    {
        Act act;
        const auto made = rig.document.createCue (rig.listId, 0, "group", "Act");
        REQUIRE (made.ok);
        act.group = made.id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + act.group + "/join", joinWord).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + act.group + "/joinLength", osc::formatDouble (seconds)).ok);

        act.first = rig.media (act.group, 0, "song");
        act.second = rig.media (act.group, 1, "next");
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (rig.listId), act.first).ok);
        rig.tickOnce();
        return act;
    }

    //  GO, the disk answering, and the member placed on its sample.
    RunRef goAndPlace (Rig& rig, const std::string& cueId)
    {
        rig.apply ("go");
        REQUIRE (rig.tickUntil ([&rig, &cueId] { return rig.runOf (cueId) != nullptr; }, 10));

        //  The disk answers whatever was asked of it, tick by tick, until the launch is placed.
        const auto run = rig.runOf (cueId);
        REQUIRE (rig.tickUntil ([&rig, run] { rig.audio.completeArms (rig.engine);
                                              return run->launchedAtSample > 0; }, 20));
        return run;
    }

    //  The clock on to `sample`, and one tick more, so the level for there has been sent.
    void playTo (Rig& rig, std::int64_t sample)
    {
        REQUIRE (rig.tickUntil ([&rig, sample] { return rig.audio.samples >= sample; }, 2000));
        rig.tickOnce();
    }
}

//==============================================================================
TEST_CASE ("join: the words, and the shape of a fade-in and a fade-out (namespace draft 69)")
{
    CHECK (cue::join::kindOf ("letPlay") == cue::join::Kind::letPlay);
    CHECK (cue::join::kindOf ("gapless") == cue::join::Kind::gapless);
    CHECK (cue::join::kindOf ("crossfade") == cue::join::Kind::crossfade);

    //  An absent row, or a word this build does not know, leaves the previous member alone.
    CHECK (cue::join::kindOf ("") == cue::join::Kind::letPlay);
    CHECK (cue::join::kindOf ("gap") == cue::join::Kind::letPlay);

    const std::int64_t from = 1000;
    const std::int64_t length = 4000;

    //  Nothing before the fade-in, all of it after; everything before a fade-out, nothing after.
    CHECK (cue::join::gainIn (from - 1, from, length, 0.0, false) == doctest::Approx (0.0));
    CHECK (cue::join::gainIn (from + length, from, length, 0.0, false) == doctest::Approx (1.0));
    CHECK (cue::join::gainOut (from - 1, from, length, 0.0, false) == doctest::Approx (1.0));
    CHECK (cue::join::gainOut (from + length, from, length, 0.0, false) == doctest::Approx (0.0));

    /*  AT NOUGHT, EQUAL POWER: a crossfade's two halves add to one in power all
        the way across, so the sum of two unlike sounds does not dip. */
    for (std::int64_t at = from; at <= from + length; at += 500)
    {
        INFO ("sample " << at);
        const auto in = cue::join::gainIn (at, from, length, 0.0, false);
        const auto out = cue::join::gainOut (at, from, length, 0.0, false);
        CHECK (in * in + out * out == doctest::Approx (1.0));
    }

    //  Halfway, each half is three decibels down.
    CHECK (cue::join::decibelsOf (cue::join::gainIn (from + length / 2, from, length, 0.0, false))
           == doctest::Approx (-3.0103).epsilon (0.001));

    //  A picture's is a straight line.
    CHECK (cue::join::gainIn (from + length / 4, from, length, 0.0, true) == doctest::Approx (0.25));

    /*  A POSITIVE CURVE LIFTS THE MIDDLE OF BOTH HALVES, a fuller crossing; a
        negative one dips it, and at -1 the two gains add to one, for material
        that is alike. */
    const auto middle = from + length / 2;
    CHECK (cue::join::gainIn (middle, from, length, 1.0, false) > cue::join::gainIn (middle, from, length, 0.0, false));
    CHECK (cue::join::gainOut (middle, from, length, 1.0, false) > cue::join::gainOut (middle, from, length, 0.0, false));
    CHECK (cue::join::gainIn (middle, from, length, -1.0, false) < cue::join::gainIn (middle, from, length, 0.0, false));
    CHECK (cue::join::gainIn (middle, from, length, -1.0, false) + cue::join::gainOut (middle, from, length, -1.0, false)
           == doctest::Approx (1.0));

    //  A length of nought is a step, and silence is the voice's floor.
    CHECK (cue::join::gainIn (from, from, 0, 0.0, false) == doctest::Approx (1.0));
    CHECK (cue::join::decibelsOf (0.0) == doctest::Approx (-120.0));
    CHECK (cue::join::decibelsOf (1.0) == doctest::Approx (0.0));
}

TEST_CASE ("join: a fade-in is silent until the launch and rises over its length, its silence sent once the arm lands")
{
    Rig rig;
    const auto sound = rig.media (rig.listId, 0, "rain");

    rig.apply ("cue.fire", { osc::Value::string (sound) });
    auto run = rig.runOf (sound);
    REQUIRE (run != nullptr);

    //  The hook that decides a join, stood in for: a two-second fade-in, set before the disk answered.
    run->joinIn = true;
    run->joinSamples = samplesOf (2.0);
    run->joinCurve = 0.0;
    rig.tickOnce();
    CHECK (run->joinDb == doctest::Approx (-120.0));

    /*  THE ARM LANDS, and its own level - the cue's nought - is on the voice.
        The silence worked out before there was a track is sent after it. */
    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.levelNow.empty(); }, 5));
    const auto track = rig.audio.levelNow.begin()->first;
    CHECK (rig.audio.levelNow[track] == doctest::Approx (-120.0));

    REQUIRE (rig.tickUntil ([run] { return run->launchedAtSample > 0; }, 20));
    const auto launched = run->launchedAtSample;

    //  Still silent up to the launch sample: the level was never above it before.
    for (const auto& level : rig.audio.levels)
        CHECK (level.second == doctest::Approx (-120.0));

    //  Halfway through the fade, three decibels down; at its end, the cue's own level.
    REQUIRE (rig.tickUntil ([&rig, launched] { return rig.audio.samples + samplesPerTick >= launched + samplesOf (1.0); }));
    rig.tickOnce();
    CHECK (rig.audio.levelNow[track] == doctest::Approx (-3.0).epsilon (0.15));

    REQUIRE (rig.tickUntil ([&rig, launched] { return rig.audio.samples >= launched + samplesOf (2.0); }));
    rig.tickOnce();
    CHECK (rig.audio.levelNow[track] == doctest::Approx (0.0));
    CHECK (run->joinDb == doctest::Approx (0.0));
}

TEST_CASE ("join: a fade-out ends where it was given, a group's term reaches its members, and a run on its way out keeps what it had")
{
    Rig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene", {}, { { "mode", "timeline" } });
    REQUIRE (scene.ok);
    const auto wind = rig.media (scene.id, 0, "wind");

    rig.apply ("cue.fire", { osc::Value::string (scene.id) });
    REQUIRE (rig.tickUntil ([&rig, &wind] { return rig.runOf (wind) != nullptr; }, 10));
    rig.audio.completeArms (rig.engine);

    auto member = rig.runOf (wind);
    REQUIRE (rig.tickUntil ([member] { return member->launchedAtSample > 0; }, 20));
    REQUIRE (rig.tickUntil ([&rig, member] { return rig.audio.samples > member->launchedAtSample; }, 20));

    /*  A FADE-OUT ON THE GROUP'S RUN, from now over a second: the member has
        no join of its own, and is taken down by its parent's - as a movie's
        locked sounds are by the movie's. */
    auto group = rig.runOf (scene.id);
    REQUIRE (group != nullptr);
    group->joinOut = true;
    group->joinOutFrom = rig.audio.samples;
    group->joinSamples = samplesOf (1.0);

    rig.tickOnce();
    CHECK (member->joinDb == doctest::Approx (0.0));
    CHECK (member->level < 0.0);
    CHECK (member->level > -3.0);

    REQUIRE (rig.tickUntil ([&rig, group] { return rig.audio.samples >= group->joinOutFrom + samplesOf (1.0); }));
    rig.tickOnce();
    CHECK (group->joinDb == doctest::Approx (-120.0));
    CHECK (member->level <= -120.0);

    //  A run with a fade-in, stopped halfway: its term holds there and the stop's fade takes it down.
    const auto rain = rig.media (rig.listId, 1, "rain");
    rig.apply ("cue.fire", { osc::Value::string (rain) });
    auto sounding = rig.runOf (rain);
    REQUIRE (sounding != nullptr);
    sounding->joinIn = true;
    sounding->joinSamples = samplesOf (2.0);
    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([sounding] { return sounding->launchedAtSample > 0; }, 20));
    const auto launched = sounding->launchedAtSample;
    REQUIRE (rig.tickUntil ([&rig, launched] { return rig.audio.samples >= launched + samplesOf (1.0); }));

    CHECK (sounding->joinDb < -1.0);
    CHECK (sounding->joinDb > -6.0);

    //  The stop lands in its tick's drain, after that tick's join pass: what it holds is read after.
    rig.apply ("run.stop", { osc::Value::string (sounding->id) });
    const auto halfway = sounding->joinDb;
    CHECK (halfway < 0.0);

    for (int n = 0; n < 10 && ! sounding->isFinished(); ++n)
    {
        CHECK (sounding->joinDb == doctest::Approx (halfway));
        rig.tickOnce();
    }
}

//==============================================================================
TEST_CASE ("join: a manual act's Let play leaves the song playing under the next")
{
    Rig rig;
    const auto act = manualAct (rig, "letPlay");

    auto song = goAndPlace (rig, act.first);
    playTo (rig, song->launchedAtSample + samplesOf (1.0));

    auto next = goAndPlace (rig, act.second);
    playTo (rig, next->launchedAtSample + samplesOf (3.0));

    CHECK_FALSE (song->joinOut);
    CHECK_FALSE (next->joinIn);
    CHECK_FALSE (song->isFinished());
    CHECK (song->joinDb == doctest::Approx (0.0));
}

TEST_CASE ("join: a manual act's Gapless cuts the song on the sample the next one starts on")
{
    Rig rig;
    const auto act = manualAct (rig, "gapless");

    auto song = goAndPlace (rig, act.first);
    playTo (rig, song->launchedAtSample + samplesOf (1.0));
    CHECK_FALSE (song->joinIn);         // the first member has nothing to come in over

    auto next = goAndPlace (rig, act.second);
    rig.tickOnce();
    const auto at = next->launchedAtSample;

    //  A tick's ramp that ends where the next one begins, and the song stopped there.
    CHECK (song->joinOut);
    CHECK (song->joinOutFrom == at - samplesPerTick);
    CHECK (song->joinStopAt == at);
    CHECK (song->joinOutBy == next->id);
    CHECK_FALSE (next->joinIn);

    playTo (rig, at);
    CHECK (song->joinDb == doctest::Approx (-120.0));
    REQUIRE (rig.tickUntil ([song] { return song->isFinished(); }, 20));
    CHECK_FALSE (next->isFinished());
    CHECK (next->joinDb == doctest::Approx (0.0));
}

TEST_CASE ("join: a manual act's Crossfade fades the song out as the next one fades in, from its launch, after its pre-wait")
{
    Rig rig;
    const auto act = manualAct (rig, "crossfade", 2.0);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + act.second + "/preWait", "0.5").ok);

    auto song = goAndPlace (rig, act.first);
    playTo (rig, song->launchedAtSample + samplesOf (1.0));

    //  The GO, and its half second of pre-wait: the song is left alone until the next one is placed.
    rig.apply ("go");
    REQUIRE (rig.tickUntil ([&rig, &act] { return rig.runOf (act.second) != nullptr; }, 10));
    auto next = rig.runOf (act.second);

    for (int n = 0; n < 40 && next->launchedAtSample <= 0; ++n)
    {
        CHECK_FALSE (song->joinOut);
        rig.audio.completeArms (rig.engine);
        rig.tickOnce();
    }

    REQUIRE (next->launchedAtSample > 0);
    rig.tickOnce();
    const auto at = next->launchedAtSample;

    CHECK (song->joinOut);
    CHECK (song->joinOutFrom == at);
    CHECK (song->joinStopAt == at + samplesOf (2.0));
    CHECK (next->joinIn);

    //  Halfway, each three decibels down: equal power.
    playTo (rig, at + samplesOf (1.0) - samplesPerTick);
    CHECK (song->joinDb == doctest::Approx (-3.0).epsilon (0.15));
    CHECK (next->joinDb == doctest::Approx (-3.0).epsilon (0.15));

    //  At its end the song is silent and stopped, and the next one is whole.
    playTo (rig, at + samplesOf (2.0));
    CHECK (song->joinDb == doctest::Approx (-120.0));
    REQUIRE (rig.tickUntil ([song] { return song->isFinished(); }, 20));
    CHECK (next->joinDb == doctest::Approx (0.0));
    CHECK_FALSE (next->isFinished());
}

TEST_CASE ("join: Esc across a manual crossfade stops both, each from where it was; Doh! gives the song its place back")
{
    SUBCASE ("Esc")
    {
        Rig rig;
        const auto act = manualAct (rig, "crossfade", 2.0);

        auto song = goAndPlace (rig, act.first);
        playTo (rig, song->launchedAtSample + samplesOf (1.0));
        auto next = goAndPlace (rig, act.second);
        playTo (rig, next->launchedAtSample + samplesOf (1.0));

        rig.apply ("run.stopAll");
        const auto songHeld = song->joinDb;
        const auto nextHeld = next->joinDb;
        CHECK (songHeld < -1.0);
        CHECK (nextHeld < -1.0);

        for (int n = 0; n < 10; ++n)
        {
            rig.tickOnce();
            CHECK (song->joinDb == doctest::Approx (songHeld));
            CHECK (next->joinDb == doctest::Approx (nextHeld));
        }
    }

    SUBCASE ("Doh!")
    {
        Rig rig;
        const auto act = manualAct (rig, "crossfade", 2.0);

        auto song = goAndPlace (rig, act.first);
        playTo (rig, song->launchedAtSample + samplesOf (1.0));
        auto next = goAndPlace (rig, act.second);
        const auto at = next->launchedAtSample;
        playTo (rig, at + samplesOf (0.5));

        const auto fallen = song->joinDb;
        CHECK (fallen < 0.0);

        rig.apply ("go.doh");
        REQUIRE (rig.tickUntil ([next] { return next->takenBack; }, 5));
        rig.tickOnce();

        //  It rises from where it was, over as long as it had fallen, and is never stopped.
        CHECK_FALSE (song->joinOut);
        CHECK (song->joinIn);
        CHECK (song->joinDb >= fallen - 0.5);
        playTo (rig, rig.audio.samples + samplesOf (0.6));
        CHECK (song->joinDb == doctest::Approx (0.0));
        playTo (rig, at + samplesOf (3.0));
        CHECK_FALSE (song->isFinished());
        CHECK_FALSE (song->joinStopSent);
    }
}
