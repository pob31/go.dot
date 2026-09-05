/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  GO, with a Player nobody can hear.

    The audio side reaches the cue layer through one abstract class, and a null
    implementation of it is a COMPLETE configuration rather than a degraded one.
    That is what makes `wfg replay` reproduce a performance on a laptop with no
    sound card, and it is why GO can be tested exhaustively in microseconds
    rather than through six seconds of Tracktion per case.

    The fake here is not a stub standing in for something real. It is the same
    interface a show uses, with the timing under the test's control - so the
    cases can ask what happens when GO arrives before the disk has answered,
    which on real hardware is a race nobody can schedule.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/cue/FadeJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>

using namespace wfg;

namespace
{
    /*  The audio side, with its timing in the test's hands.

        Arms do not complete on their own: `completeArms` is when the disk
        answers. That is the whole point - on real hardware the gap between GO
        and the media being ready is a race, and here it is a decision. */
    struct FakePlayer final : cue::Player
    {
        int trackCount() const override            { return tracks; }
        std::int64_t samplesElapsed() const override { return samples; }
        int blockSize() const override             { return block; }
        int channelsPerTrack() const override      { return channels; }

        void requestArm (const cue::ArmRequest& request) override
        {
            arms.push_back (request);
        }

        bool launchAtSample (int track, std::int64_t sample) override
        {
            launches.push_back ({ track, sample });
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            stopped.push_back (track);
            return true;
        }

        bool stopAtSample (int track, std::int64_t sample) override
        {
            stopsAt.push_back ({ track, sample });
            return true;
        }

        void setLevelDb (int track, double levelDb) override
        {
            levels.push_back ({ track, levelDb });
        }

        bool isPlaying (int track) const override
        {
            return playing.count (track) > 0;
        }

        bool isArmReady (int track) const override
        {
            return ready.count (track) > 0;
        }

        /** The disk answers: every outstanding arm is reported as ready. */
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId),
                                 osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        int tracks = 4;
        int block = 128;
        int channels = 2;
        std::int64_t samples = 0;

        std::vector<cue::ArmRequest> arms;
        std::vector<std::pair<int, std::int64_t>> launches;
        std::set<int> playing;
        std::set<int> ready;
        std::vector<int> stopped;
        std::vector<std::pair<int, std::int64_t>> stopsAt;
        std::vector<std::pair<int, double>> levels;
    };

    //==========================================================================
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
            runner.setSamplesPerTick (960);          // 48 kHz at 50 Hz

            listId = document.createList ("Sound").id;
            mediaId = document.createCue (listId, 0, "media", "Thunder").id;
            memoId = document.createCue (listId, 1, "memo", "House to half").id;

            document.setAttribute ("/godot/cue/" + mediaId + "/file", "thunder.wav");
        }

        /** One tick, with the Runner observing first as the tick thread does. */
        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult submitAndTick (const std::string& name,
                                          std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return tickOnce();
        }

        std::string standby() const
        {
            return document.findById (listId)[juce::Identifier ("standby")]
                       .toString().toStdString();
        }

        void setStandby (const std::string& cueId)
        {
            document.setAttribute (cue::standbyAddressOf (listId), cueId);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
        FakePlayer audio;

        std::string listId, mediaId, memoId;
        std::int64_t tick = 0;
    };
}

//==============================================================================
TEST_CASE ("launch latency: far enough ahead that a cue is never late, at every rate")
{
    /*  A launch placed at a beat that has already passed does not simply start
        late - Tracktion renders the block in hand from the head of the file and
        back-dates only the blocks after it, so the cue is late AND has a hole
        in it. The lead therefore has to clear: the tick observation error
        (blockSize - 1), one block that may be in flight, and one more for the
        block where the audio thread's try-lock misses the queue.

        What is asserted is that property, not the formula - a test that
        restated the formula would pass against any arithmetic including the
        wrong one. */
    const auto clears = [] (int blockSize, int samplesPerTick)
    {
        const auto ticks = cue::launchLatencyTicks (blockSize, samplesPerTick);
        const auto lead = static_cast<std::int64_t> (ticks) * samplesPerTick - (blockSize - 1);

        INFO ("block " << blockSize << ", samples/tick " << samplesPerTick
               << " -> " << ticks << " ticks, lead " << lead
               << " against " << (2 * blockSize) << " needed");

        return lead >= 2 * blockSize;
    };

    CHECK (clears (128, 960));      // 48 kHz
    CHECK (clears (64, 1920));      // 96 kHz
    CHECK (clears (512, 882));      // 44.1 kHz - the case the plan's rule barely passed
    CHECK (clears (512, 960));
    CHECK (clears (1024, 960));     // the plan's rule FAILED here
    CHECK (clears (1024, 882));
    CHECK (clears (2048, 960));     // and here
    CHECK (clears (2048, 882));

    /*  And it stays a latency somebody would accept. Two ticks at ordinary
        block sizes is 40 ms; a huge buffer costs more, which is the honest
        answer rather than a shorter one that would not clear. */
    CHECK (cue::launchLatencyTicks (128, 960) == 2);
    CHECK (cue::launchLatencyTicks (64, 1920) == 2);

    /*  Nonsense in, zero out, rather than an arithmetic exception on a thread
        that must not throw. */
    CHECK (cue::launchLatencyTicks (0, 960) == 0);
    CHECK (cue::launchLatencyTicks (128, 0) == 0);
}

//==============================================================================
TEST_CASE ("go: standby moves first, whatever the cue turns out to do")
{
    /*  PRD §3.5. The pointer advances on every GO - sounding cue, memo, or a
        cue that fails to find a voice - which is what lets an operator press GO
        down a list at speed without waiting to see what each one did. */
    Rig rig;
    rig.setStandby (rig.mediaId);

    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.standby() == rig.memoId);
}

TEST_CASE ("go: at the end of a list it is applied and stays put")
{
    Rig rig;
    rig.setStandby (rig.memoId);            // the last cue

    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.standby() == rig.memoId);
}

TEST_CASE ("go: with nothing in standby it is applied and does nothing")
{
    /*  An operator who has not armed a list has not made a mistake, and an R
        record every time would bury the rejections that matter. */
    Rig rig;

    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.runs.all().empty());
    CHECK (rig.audio.arms.empty());
}

TEST_CASE ("go: a memo cue advances standby and makes no run")
{
    Rig rig;
    rig.setStandby (rig.memoId);

    CHECK (rig.submitAndTick ("go").applied == 1);
    CHECK (rig.runs.all().empty());
}

//==============================================================================
TEST_CASE ("go: a media cue asks for a voice, and launches once the disk answers")
{
    Rig rig;
    rig.setStandby (rig.mediaId);

    CHECK (rig.submitAndTick ("go").applied == 1);

    /*  A voice is reserved and the media requested. Nothing has been launched:
        a launch placed before the disk has answered plays silence for as long
        as the disk takes, with the run reporting itself as playing throughout. */
    REQUIRE (rig.runs.all().size() == 1u);
    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms[0].mediaFile == "thunder.wav");
    CHECK (rig.audio.arms[0].track == 0);
    CHECK (rig.audio.launches.empty());

    const auto id = rig.runs.all().front().id;
    CHECK (rig.runs.find (id)->state == cue::runState::armed);

    /*  The disk answers. */
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    /*  Which is what the queued GO was waiting for - the next tick places it. */
    rig.tickOnce();

    REQUIRE (rig.audio.launches.size() == 1u);
    CHECK (rig.audio.launches[0].first == 0);

    /*  Placed a whole number of ticks ahead of where the counter is, so the
        instant is a function of the schedule and not of when this loop ran. */
    const auto lead = rig.audio.launches[0].second - rig.audio.samples;
    INFO ("lead " << lead << " samples");
    CHECK (lead == cue::launchLatencyTicks (rig.audio.block, 960) * 960);

    CHECK (rig.runs.find (id)->state == cue::runState::playing);
}

TEST_CASE ("go: a cue armed in advance launches on the tick GO arrives")
{
    /*  The arrangement the whole design is for: a cue reaching standby is armed
        while the operator reads the next line, so GO is only a placed instant.
        Here the arm has already completed before GO, and the launch happens on
        the very next tick rather than waiting for a disk. */
    Rig rig;
    rig.setStandby (rig.mediaId);

    rig.submitAndTick ("audio.arm", { osc::Value::string (rig.mediaId) });
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    CHECK (rig.audio.launches.empty());

    rig.submitAndTick ("go");
    rig.tickOnce();

    CHECK (rig.audio.launches.size() == 1u);
}

TEST_CASE ("go: every voice busy fails the run rather than stealing one")
{
    /*  A playing track is never stolen. The GO is APPLIED - it was a legal
        request the show could not honour - and the run says why. */
    Rig rig;
    rig.audio.tracks = 1;

    const auto second = rig.document.createCue (rig.listId, 2, "media", "Rain").id;
    rig.document.setAttribute ("/godot/cue/" + second + "/file", "rain.wav");

    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    rig.setStandby (second);
    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied >= 1);
    REQUIRE (rig.runs.all().size() == 2u);

    /*  The failure is SUBMITTED from inside the GO, so it is applied on the
        next tick like anything else the engine reports about itself. One path
        into the model, even for the engine's own bad news. */
    rig.tickOnce();

    const auto& failed = rig.runs.all().back();
    CHECK (failed.state == cue::runState::failed);
    CHECK (failed.error == cue::runError::noTrack);
}

TEST_CASE ("go: a cue with no file fails the run and says which failure it was")
{
    Rig rig;

    const auto silent = rig.document.createCue (rig.listId, 2, "media", "Nothing").id;
    rig.setStandby (silent);

    rig.submitAndTick ("go");
    rig.tickOnce();                 // the failure is applied like any report

    REQUIRE (rig.runs.all().size() == 1u);
    CHECK (rig.runs.all().front().state == cue::runState::failed);
    CHECK (rig.runs.all().front().error == cue::runError::mediaMissing);

    /*  And no voice was held for it. A failed run must not keep a track out of
        circulation for the rest of the show. */
    CHECK (rig.runs.lowestFreeTrack (4) == 0);
}

TEST_CASE ("go: a second GO on a running cue is applied, and there is still one run")
{
    /*  DECISION B, 2026-09-05, end to end this time: standby advances, the log
        says applied, and the sound carries on untouched. */
    Rig rig;
    rig.setStandby (rig.mediaId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.runs.all().size() == 1u);
    const auto launchesBefore = rig.audio.launches.size();

    rig.setStandby (rig.mediaId);
    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.runs.all().size() == 1u);
    CHECK (rig.audio.launches.size() == launchesBefore);
}

//==============================================================================
TEST_CASE ("go: a run ends when the sound does, on the tick it was observed")
{
    Rig rig;
    rig.setStandby (rig.mediaId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto id = rig.runs.all().front().id;
    REQUIRE (rig.runs.find (id)->state == cue::runState::playing);

    /*  The audio side starts sounding, then stops - which for a non-looping
        launcher clip is the ordinary end of a cue rather than a stop somebody
        asked for. */
    rig.audio.playing.insert (0);
    rig.tickOnce();
    CHECK (rig.runs.find (id)->state == cue::runState::playing);

    rig.audio.playing.erase (0);
    rig.tickOnce();

    CHECK (rig.runs.find (id)->state == cue::runState::done);

    /*  And the voice is back. */
    CHECK (rig.runs.lowestFreeTrack (4) == 0);
}

TEST_CASE ("go: the whole thing works with no audio side at all")
{
    /*  THE REPLAY CASE, and the reason the Player is an interface. A show
        replayed on a machine with no sound card must create the same runs,
        advance standby the same way and write the same log - only the sound is
        missing. A design where the cue layer talked to Tracktion directly could
        not do this, and the guarantee would be untestable. */
    Rig rig;
    rig.runner.setPlayer (nullptr);
    rig.setStandby (rig.mediaId);

    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.standby() == rig.memoId);

    REQUIRE (rig.runs.all().size() == 1u);
    CHECK (rig.runs.all().front().cue == rig.mediaId);
    CHECK (rig.runs.all().front().state == cue::runState::armed);

    /*  Ticking is harmless rather than a crash waiting for a null player. */
    rig.tickOnce();
    rig.tickOnce();
}

//==============================================================================
TEST_CASE ("cue.fire: fires a cue and leaves standby exactly where it was")
{
    /*  §4.11: every gesture-reachable action is a named command, and a button
        on a surface firing one cue is not the same gesture as GO. Only GO moves
        the pointer (§3.5). */
    Rig rig;
    rig.setStandby (rig.memoId);

    const auto outcome = rig.submitAndTick ("cue.fire",
                                            { osc::Value::string (rig.mediaId) });

    CHECK (outcome.applied == 1);
    CHECK (rig.standby() == rig.memoId);
    REQUIRE (rig.runs.all().size() == 1u);
    CHECK (rig.runs.all().front().cue == rig.mediaId);
}

TEST_CASE ("cue.fire: a cue nobody has is refused rather than invented")
{
    Rig rig;

    const auto outcome = rig.submitAndTick ("cue.fire",
                                            { osc::Value::string ("NOSUCHID") });

    CHECK (outcome.rejected == 1);
    CHECK (rig.runs.all().empty());
}

TEST_CASE ("go: the run identifier reaches the log, so a replay draws no numbers of its own")
{
    Rig rig;
    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto go = std::find_if (parsed.records.begin(), parsed.records.end(),
                                  [] (const auto& record) { return record.command == "go"; });

    REQUIRE (go != parsed.records.end());
    REQUIRE_FALSE (go->args.empty());

    const auto id = go->args[0].getString();
    INFO ("the go record carried run " << id);

    REQUIRE_FALSE (id.empty());
    CHECK (rig.runs.find (id) != nullptr);
}

//==============================================================================
/*  ROUTING: from what the designer wrote to what the matrix multiplies.

    The document says a destination in terms of a BUS, because a bus is where
    the author said a channel exists - and a show moved to another rig re-points
    its buses rather than every one of its cues. The Runner resolves that into
    absolute hardware channels, which is the only place that arithmetic lives,
    and the only place it could be wrong in a way that sends a cue somewhere
    nobody asked for.
*/
namespace
{
    /** A show with two buses, so a destination has somewhere to be resolved to. */
    struct RoutedRig : Rig
    {
        RoutedRig()
        {
            auto audioNode = document.root().getChildWithName ("Audio");
            audioNode.setProperty (juce::Identifier ("tracks"), 4, nullptr);

            main = addBus ("Main L/R", 0, 2);
            foldback = addBus ("Foldback", 4, 2);
        }

        std::string addBus (const char* name, int firstChannel, int width)
        {
            auto audioNode = document.root().getChildWithName ("Audio");

            juce::ValueTree bus { "Bus" };
            const auto id = runIds.generate();

            bus.setProperty (juce::Identifier ("id"), juce::String (id), nullptr);
            bus.setProperty (juce::Identifier ("name"), name, nullptr);
            bus.setProperty (juce::Identifier ("firstChannel"), firstChannel, nullptr);
            bus.setProperty (juce::Identifier ("width"), width, nullptr);

            audioNode.appendChild (bus, nullptr);
            return id;
        }

        void addRoute (const std::string& cueId, const std::string& busId,
                       const char* gains)
        {
            auto cue = document.findById (cueId);

            juce::ValueTree route { "Route" };
            route.setProperty (juce::Identifier ("id"),
                               juce::String (runIds.generate()), nullptr);
            route.setProperty (juce::Identifier ("bus"), juce::String (busId), nullptr);
            route.setProperty (juce::Identifier ("gains"), gains, nullptr);

            cue.appendChild (route, nullptr);
        }

        std::vector<cue::Coefficient> routingOf (const std::string& cueId,
                                                 std::string& problem)
        {
            return runner.resolveRouting (document.findById (cueId), 2, problem);
        }

        std::string main, foldback;
    };
}

TEST_CASE ("routing: a bus is a place on the rig, and the cue never names a channel")
{
    /*  Foldback starts at hardware channel 4, so a cue routed to it lands on 4
        and 5 - and the cue itself says nothing about either number. Re-point the
        bus and every cue feeding it moves, which is the whole reason a
        destination is a bus. */
    RoutedRig rig;

    rig.addRoute (rig.mediaId, rig.foldback, "1 0 0 1");

    std::string problem;
    const auto routing = rig.routingOf (rig.mediaId, problem);

    INFO (problem);
    CHECK (problem.empty());
    REQUIRE (routing.size() == 2u);

    CHECK (routing[0].input == 0);
    CHECK (routing[0].output == 4);
    CHECK (routing[0].gain == doctest::Approx (1.0f));

    CHECK (routing[1].input == 1);
    CHECK (routing[1].output == 5);
    CHECK (routing[1].gain == doctest::Approx (1.0f));
}

TEST_CASE ("routing: destinations are a list, and a cue reaches all of them")
{
    /*  PRD §3.9b. A source into a spatial processor AND a stereo feed to
        foldback is the ordinary case, not the exotic one. */
    RoutedRig rig;

    rig.addRoute (rig.mediaId, rig.main, "1 0 0 1");
    rig.addRoute (rig.mediaId, rig.foldback, "0.5 0 0 0.5");

    std::string problem;
    const auto routing = rig.routingOf (rig.mediaId, problem);

    CHECK (problem.empty());
    REQUIRE (routing.size() == 4u);

    CHECK (routing[0].output == 0);
    CHECK (routing[1].output == 1);
    CHECK (routing[2].output == 4);
    CHECK (routing[3].output == 5);
    CHECK (routing[3].gain == doctest::Approx (0.5f));
}

TEST_CASE ("routing: the gains are read row by row, input then channel")
{
    /*  A mono cue spread across a stereo bus at different gains: one input, two
        channels, and the order is the thing that would be silently wrong. */
    RoutedRig rig;

    rig.addRoute (rig.mediaId, rig.main, "0.25 0.75");

    std::string problem;
    const auto routing = rig.routingOf (rig.mediaId, problem);

    REQUIRE (routing.size() == 2u);

    CHECK (routing[0].input == 0);
    CHECK (routing[0].output == 0);
    CHECK (routing[0].gain == doctest::Approx (0.25f));

    CHECK (routing[1].input == 0);
    CHECK (routing[1].output == 1);
    CHECK (routing[1].gain == doctest::Approx (0.75f));
}

TEST_CASE ("routing: a zero coefficient is silence already, so it is not written")
{
    /*  The matrix starts silent, so a zero says nothing new - and a destination
        list of mostly-zero numbers across a wide rig would otherwise cost an
        atomic store per zero on every arm. */
    RoutedRig rig;

    rig.addRoute (rig.mediaId, rig.main, "1 0 0 0");

    std::string problem;
    const auto routing = rig.routingOf (rig.mediaId, problem);

    REQUIRE (routing.size() == 1u);
    CHECK (routing[0].input == 0);
    CHECK (routing[0].output == 0);
}

TEST_CASE ("routing: a cue routed nowhere yet is silent rather than wrong")
{
    /*  An ordinary state for a show being written. It resolves to nothing and
        says no problem, because there is none. */
    RoutedRig rig;

    std::string problem;
    CHECK (rig.routingOf (rig.mediaId, problem).empty());
    CHECK (problem.empty());

    rig.addRoute (rig.mediaId, rig.main, "");

    CHECK (rig.routingOf (rig.mediaId, problem).empty());
    CHECK (problem.empty());
}

TEST_CASE ("routing: a shape the rig cannot honour is refused, and says so")
{
    RoutedRig rig;

    SUBCASE ("a bus this show does not have")
    {
        rig.addRoute (rig.mediaId, "NOSUCHID", "1 0 0 1");

        std::string problem;
        CHECK (rig.routingOf (rig.mediaId, problem).empty());
        CHECK_FALSE (problem.empty());
    }

    SUBCASE ("gains that do not divide by the bus width")
    {
        /*  Three numbers across a stereo bus is not a shorter routing, it is a
            different one, and the client that wrote it meant something the show
            cannot do. */
        rig.addRoute (rig.mediaId, rig.main, "1 0 1");

        std::string problem;
        CHECK (rig.routingOf (rig.mediaId, problem).empty());
        CHECK_FALSE (problem.empty());
    }

    SUBCASE ("a cue wider than the track that would carry it")
    {
        /*  §3.9b: no silent up- or downmix. Four inputs across a stereo bus on
            a two-channel track is refused rather than folded down. */
        rig.addRoute (rig.mediaId, rig.main, "1 0 0 1 1 0 0 1");

        std::string problem;
        CHECK (rig.routingOf (rig.mediaId, problem).empty());
        CHECK_FALSE (problem.empty());
    }
}

TEST_CASE ("go: a cue that cannot be routed fails its run, never the load")
{
    /*  A show with one mis-pointed cue is still a show somebody has to run
        tonight. The GO is APPLIED - the request was legal - and the run says
        bad-route. */
    RoutedRig rig;

    rig.addRoute (rig.mediaId, "NOSUCHID", "1 0 0 1");
    rig.setStandby (rig.mediaId);

    const auto outcome = rig.submitAndTick ("go");
    rig.tickOnce();

    CHECK (outcome.applied == 1);
    REQUIRE (rig.runs.all().size() == 1u);
    CHECK (rig.runs.all().front().state == cue::runState::failed);
    CHECK (rig.runs.all().front().error == cue::runError::badRoute);
}

TEST_CASE ("go: the arm carries the level and the destinations the cue was written with")
{
    RoutedRig rig;

    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/level", "-6");
    rig.addRoute (rig.mediaId, rig.foldback, "1 0 0 1");
    rig.setStandby (rig.mediaId);

    rig.submitAndTick ("go");

    REQUIRE (rig.audio.arms.size() == 1u);

    const auto& arm = rig.audio.arms.front();
    CHECK (arm.mediaFile == "thunder.wav");
    CHECK (arm.levelDb == doctest::Approx (-6.0));
    REQUIRE (arm.routing.size() == 2u);
    CHECK (arm.routing[0].output == 4);

    /*  And the run carries the level it was armed at, which is what a fade will
        move - the cue's own level stays where the designer left it. */
    CHECK (rig.runs.all().front().level == doctest::Approx (-6.0));
}

//==============================================================================
/*  FADE AND STOP: what happens to a cue after it has started.

    A fade targets the CUE and finds its live run when it fires, because the
    show has to say which sound it means without knowing which instance is
    playing. It interpolates in the dB domain at control rate, and its per-tick
    values are never logged - §3.15 keeps continuous readouts out of the log,
    and a replay recomputes them from the GO and the document.
*/
namespace
{
    /*  The rig above, plus a fade cue and a stop cue pointed at the media one. */
    struct FadeRig : Rig
    {
        FadeRig()
        {
            fadeId = document.createCue (listId, 2, "fade", "Under").id;
            stopId = document.createCue (listId, 3, "stop", "Out").id;

            setCue (fadeId, "target", mediaId);
            setCue (fadeId, "level", "-20");
            setCue (fadeId, "duration", "1");

            setCue (stopId, "target", mediaId);
        }

        void setCue (const std::string& id, const char* name, const std::string& value)
        {
            document.setAttribute ("/godot/cue/" + id + "/" + name, value);
        }

        /** GO on one cue, without disturbing standby. */
        Engine::TickResult fire (const std::string& id)
        {
            return submitAndTick ("cue.fire", { osc::Value::string (id) });
        }

        /** Plays the media cue and leaves it sounding. */
        std::string startMedia()
        {
            fire (mediaId);
            audio.completeArms (engine);
            tickOnce();
            tickOnce();

            const auto id = runs.all().front().id;
            audio.playing.insert (runs.find (id)->track);
            tickOnce();

            return id;
        }

        std::string fadeId, stopId;
    };
}

TEST_CASE ("fade curve: dB, monotonic, and it arrives exactly")
{
    /*  The arithmetic on its own, because it is the part a rendered envelope
        cannot tell you is wrong - a curve that overshot by a hair would look
        like a fade in a plot and be a level nobody asked for. */
    using cue::fadeLevelDb;
    using cue::FadeCurve;

    for (const auto curve : { FadeCurve::linear, FadeCurve::sCurve })
    {
        INFO ("curve " << (curve == FadeCurve::linear ? "linear" : "sCurve"));

        /*  Both ends exactly. A fade that ended at -119.97 dB would leave a
            cue very slightly audible for the rest of the show. */
        CHECK (fadeLevelDb (0.0, -120.0, 0.0, curve) == doctest::Approx (0.0));
        CHECK (fadeLevelDb (0.0, -120.0, 1.0, curve) == doctest::Approx (-120.0));

        /*  Clamped, so a caller that overshot by a tick gets the destination
            rather than a level beyond it. */
        CHECK (fadeLevelDb (0.0, -120.0, 1.5, curve) == doctest::Approx (-120.0));
        CHECK (fadeLevelDb (0.0, -120.0, -0.5, curve) == doctest::Approx (0.0));

        /*  MONOTONIC ALL THE WAY DOWN, which is what stops a curve putting a
            level somewhere nobody asked for on its way past. */
        auto previous = fadeLevelDb (0.0, -120.0, 0.0, curve);

        for (int step = 1; step <= 100; ++step)
        {
            const auto now = fadeLevelDb (0.0, -120.0, step / 100.0, curve);
            REQUIRE (now <= previous);
            previous = now;
        }
    }

    /*  Linear is straight: halfway through is halfway down, in dB. */
    CHECK (fadeLevelDb (0.0, -20.0, 0.5, FadeCurve::linear) == doctest::Approx (-10.0));

    /*  And the sCurve is not, but agrees at the middle - smoothstep is
        symmetric, so it is slower at both ends and steeper in the middle. */
    CHECK (fadeLevelDb (0.0, -20.0, 0.5, FadeCurve::sCurve) == doctest::Approx (-10.0));
    CHECK (fadeLevelDb (0.0, -20.0, 0.25, FadeCurve::sCurve)
             > fadeLevelDb (0.0, -20.0, 0.25, FadeCurve::linear));
    CHECK (fadeLevelDb (0.0, -20.0, 0.75, FadeCurve::sCurve)
             < fadeLevelDb (0.0, -20.0, 0.75, FadeCurve::linear));

    /*  A word nobody recognises is linear rather than a refusal: this is read
        from a document the grammar already checked, and a cue that did nothing
        because of a hand edit would be a cue that fails on a show night. */
    CHECK (cue::fadeCurveFrom ("sCurve") == FadeCurve::sCurve);
    CHECK (cue::fadeCurveFrom ("linear") == FadeCurve::linear);
    CHECK (cue::fadeCurveFrom ("wobble") == FadeCurve::linear);
}

//==============================================================================
TEST_CASE ("fade: it moves the run's level, one value a tick, and arrives")
{
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (0.0));

    rig.fire (rig.fadeId);

    /*  A one-second fade at fifty ticks a second is fifty values. */
    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    const auto halfway = rig.runs.find (mediaRun)->level;
    INFO ("halfway: " << halfway << " dB");

    CHECK (halfway < -5.0);
    CHECK (halfway > -15.0);

    for (int i = 0; i < 30; ++i)
        rig.tickOnce();

    /*  Arrived exactly, and stopped there. */
    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (-20.0));

    /*  The MEDIA run is untouched - it is still playing, at a lower level. A
        fade changes what is happening, not what was decided. */
    CHECK (rig.runs.find (mediaRun)->state == cue::runState::playing);

    /*  And the audio side was told, once a tick rather than once. */
    INFO ("level writes: " << rig.audio.levels.size());
    CHECK (rig.audio.levels.size() >= 40u);
    CHECK (rig.audio.levels.back().second == doctest::Approx (-20.0));
}

TEST_CASE ("fade: the fade's own run finishes when the fade does")
{
    /*  A fade is a cue, so GO on it makes a run like any other - and a group
        will need that run to finish before it calls itself complete (§3.6). */
    FadeRig rig;

    rig.startMedia();
    rig.fire (rig.fadeId);

    REQUIRE (rig.runs.all().size() == 2u);
    const auto fadeRun = rig.runs.all().back().id;

    CHECK (rig.runs.find (fadeRun)->kind == "fade");
    CHECK_FALSE (rig.runs.find (fadeRun)->isFinished());

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    CHECK (rig.runs.find (fadeRun)->state == cue::runState::done);
}

TEST_CASE ("fade: a target that is not running is a no-op, applied rather than refused")
{
    /*  §3.8. Fading a cue that ended earlier than expected is what an operator
        does, not a mistake they made - there is simply nothing to fade. The
        fade's run reports done at once so a group waiting on it is not held up. */
    FadeRig rig;

    const auto outcome = rig.fire (rig.fadeId);

    CHECK (outcome.applied == 1);
    CHECK (outcome.rejected == 0);

    REQUIRE (rig.runs.all().size() == 1u);
    rig.tickOnce();

    CHECK (rig.runs.all().front().state == cue::runState::done);
    CHECK (rig.audio.levels.empty());
}

TEST_CASE ("fade: a fade takes over from where the level has got to")
{
    /*  Starting a second fade must not jump. It begins from where the level IS,
        not from where the first fade started - anything else is a click on a PA
        and a mistake nobody can account for afterwards. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();

    rig.fire (rig.fadeId);

    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    const auto interrupted = rig.runs.find (mediaRun)->level;
    INFO ("interrupted at " << interrupted << " dB");
    REQUIRE (interrupted < -1.0);

    /*  A second fade, back up to unity. */
    const auto second = rig.document.createCue (rig.listId, 4, "fade", "Back").id;
    rig.setCue (second, "target", rig.mediaId);
    rig.setCue (second, "level", "0");
    rig.setCue (second, "duration", "1");

    rig.fire (second);

    /*  Only one fade is in flight: the first was taken over, not left to fight
        the second over the same level. */
    REQUIRE (rig.runner.fades().size() == 1u);

    /*  THE ASSERTION THAT MATTERS, and it is about where the new fade STARTS
        rather than about the next value it produces. It begins from the level
        the first fade had reached - not from 0 dB, where that fade began, which
        would be a jump of ten decibels and a click.

        Read off the job rather than inferred from a sample, because a sample
        comparison would have to know how many ticks each fade had had, and that
        is arithmetic about the test rather than about the fade. */
    const auto& takeover = rig.runner.fades().front();

    INFO ("took over at " << takeover.fromDb << " dB, heading for " << takeover.toDb);

    CHECK (takeover.fromDb < -1.0);
    CHECK (takeover.fromDb == doctest::Approx (rig.runs.find (mediaRun)->level).epsilon (0.01));
    CHECK (takeover.toDb == doctest::Approx (0.0));

    /*  And it climbs from there. */
    const auto before = rig.runs.find (mediaRun)->level;
    rig.tickOnce();

    CHECK (rig.runs.find (mediaRun)->level > before);

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (0.0));
}

TEST_CASE ("fade: when the thing being faded ends, the fade stops writing to it")
{
    /*  A cue can finish on its own halfway through a fade. Writing levels into
        a voice that has moved on to another cue would be riding a fader that
        belongs to somebody else. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    rig.fire (rig.fadeId);

    for (int i = 0; i < 10; ++i)
        rig.tickOnce();

    const auto writesBefore = rig.audio.levels.size();

    rig.audio.playing.erase (0);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.runs.find (mediaRun)->isFinished());

    const auto writesAfter = rig.audio.levels.size();

    for (int i = 0; i < 20; ++i)
        rig.tickOnce();

    INFO ("writes before " << writesBefore << ", at the end " << writesAfter
           << ", now " << rig.audio.levels.size());

    CHECK (rig.audio.levels.size() == writesAfter);
    CHECK (rig.runner.fades().empty());
}

//==============================================================================
TEST_CASE ("stop: a hard stop says stopping at once and stops on the next tick")
{
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    rig.fire (rig.stopId);

    /*  Asked, and saying so. `done` here would publish a silence that has not
        happened. */
    CHECK (rig.runs.find (mediaRun)->state == cue::runState::stopping);
    CHECK (rig.audio.stopped.empty());

    rig.tickOnce();

    CHECK (rig.audio.stopped.size() == 1u);
    CHECK (rig.audio.stopped.front() == 0);
}

TEST_CASE ("stop: the fade verb reaches silence before it stops anything")
{
    /*  The whole reason a fade-and-stop is two things in that order: by the
        time the clip stops the level is already at silence, so Tracktion's own
        click suppression has nothing left to suppress. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "1");

    rig.fire (rig.stopId);

    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    /*  Halfway: on its way down, and NOT stopped. A stop that landed here would
        cut the sound off mid-fade. */
    INFO ("halfway down: " << rig.runs.find (mediaRun)->level << " dB");
    CHECK (rig.runs.find (mediaRun)->level < -20.0);
    CHECK (rig.audio.stopped.empty());

    for (int i = 0; i < 30; ++i)
        rig.tickOnce();

    /*  Silent, and only then stopped. */
    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (-120.0));
    REQUIRE (rig.audio.stopped.size() == 1u);

    /*  The level reached silence before the stop was issued, which is the
        ordering this case exists for. */
    REQUIRE_FALSE (rig.audio.levels.empty());
    CHECK (rig.audio.levels.back().second == doctest::Approx (-120.0));
}

TEST_CASE ("stop: a target that is not running is a no-op too")
{
    FadeRig rig;

    const auto outcome = rig.fire (rig.stopId);

    CHECK (outcome.applied == 1);
    CHECK (rig.audio.stopped.empty());

    rig.tickOnce();
    CHECK (rig.runs.all().front().state == cue::runState::done);
}

TEST_CASE ("stop: minus a hundred and twenty decibels is digital silence, not nearly")
{
    /*  A fade that left a cue at -119.9 dB would leave it in the sum for the
        rest of the show, sixty-four times over on a big rig. The floor is a
        real zero. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "0.2");

    rig.fire (rig.stopId);

    for (int i = 0; i < 20; ++i)
        rig.tickOnce();

    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (-120.0));

    audio::CueMatrix matrix;
    matrix.prepare (1, 1, 48000.0, 64);
    matrix.setGain (0, 0, 1.0f);
    matrix.setLevelDb (static_cast<float> (rig.runs.find (mediaRun)->level));
    matrix.snapToTargets();

    juce::AudioBuffer<float> in { 1, 64 }, out { 1, 64 };
    in.clear();
    out.clear();

    for (int n = 0; n < 64; ++n)
        in.setSample (0, n, 1.0f);

    matrix.process (in.getArrayOfReadPointers(), 1, out.getArrayOfWritePointers(), 1, 64);

    for (int n = 0; n < 64; ++n)
        REQUIRE (juce::exactlyEqual (out.getSample (0, n), 0.0f));
}
