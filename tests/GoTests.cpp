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
            return true;
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
