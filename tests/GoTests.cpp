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

        /*  Ticks until a predicate holds, and answers whether it ever did.

            BOUNDED, and that is not caution. Every loop in these cases is
            waiting for a scheduler to do something, and a scheduler that has
            stopped is exactly what they are here to catch - so an unbounded
            wait would turn the most interesting failure into a suite that hangs
            and says nothing. */
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


        std::string standby() const
        {
            return document.findById (listId)[juce::Identifier ("standby")]
                       .toString().toStdString();
        }

        /*  Parks the standby and LETS THE ARM SETTLE, which is what a show
            does: the pointer reaches a cue while the operator reads the next
            line, and the voice and the file are made ready in that time. Since
            PR 3.3 the Runner asks for that arm from its hook, so a test that
            parked and pressed GO in the same tick would be measuring a race no
            operator can produce - and would see two commands applied on the GO
            tick rather than one. */
        void setStandby (const std::string& cueId)
        {
            document.setAttribute (cue::standbyAddressOf (listId), cueId);
            tickOnce();
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

TEST_CASE ("go: a memo cue advances standby, makes a run, and that run finishes")
{
    /*  THIS TEST USED TO ASSERT THE OPPOSITE, and the change is the point of
        PR 3.1 rather than a side effect of it.

        Phase 2 gave a memo no run, which was true to what a memo is - a line in
        the book - and made "is this cue done?" a question with as many answers
        as there were kinds: ask the fade jobs, ask the network jobs, poll the
        voice, and for a memo there was nobody to ask. §3.6 makes `done` the
        thing a sequence group advances on, so a group whose second member is a
        note to the operator has to know when to move to the third.

        So every kind gets a run and the run table is the one place that
        answers. A memo's run finishes on the tick AFTER it fired, exactly as a
        network cue with `wait: none` does, because that is when a report is
        allowed to leave. */
    Rig rig;
    rig.setStandby (rig.memoId);

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    CHECK (rig.runs.all().front().kind == "memo");
    CHECK (rig.runs.all().front().state == cue::runState::playing);
    CHECK (rig.audio.arms.empty());                 // nothing to make ready

    /*  The hook submits on the next tick and the handler applies it on the one
        after - the ordinary two-step every engine-origin report takes. */
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.runs.find (id) != nullptr);
    CHECK (rig.runs.find (id)->state == cue::runState::done);
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

    /*  The pointer reaching the second cue is what asks for the voice now, so
        the refusal arrives at standby rather than at GO - which is the whole
        argument for arming ahead: an operator finds out that the rig is full
        while there is still time to do something about it. */
    rig.setStandby (second);
    rig.tickOnce();

    REQUIRE (rig.runs.all().size() == 2u);

    const auto& failed = rig.runs.all().back();
    CHECK (failed.state == cue::runState::failed);
    CHECK (failed.error == cue::runError::noTrack);
}

TEST_CASE ("go: a cue with no file fails the run and says which failure it was")
{
    /*  AND IT SAYS SO AT STANDBY, which is what arming ahead is worth. Since
        PR 3.3 the pointer reaching a media cue asks for it to be armed, so a
        missing file is reported while the operator is reading the next line
        rather than at the moment their hand comes down. The CSV row has said
        this was the intention since PR 2.3 - "reported when the show loads and
        fails the cue when it is armed" - and nothing armed until now. */
    Rig rig;

    const auto silent = rig.document.createCue (rig.listId, 2, "media", "Nothing").id;
    rig.setStandby (silent);
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

    /*  Reported at standby, like the other two ways an arm can fail: parking on
        it is what asks for the routing to be resolved. */
    rig.setStandby (rig.mediaId);
    rig.tickOnce();

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

TEST_CASE ("run.kill: the sound stops, and not only the run's mind about itself")
{
    /*  FOUND BY THE BLACK-BOX DRIVER, and findable by nothing else that
        existed: `run.kill` marked a run `stopping` and went no further.

        Its own comment was right about why - it is a command on the model,
        registered with the run table alone so that it stays callable from
        `wfg replay` where there is no audio side - and it said "the audio side
        reports run.ended when it actually has", which was true of every path
        except the one nobody had written. Nothing told the audio side.

        So a killed cue read `stopping` and played on until its file ran out.
        Every model-level assertion passed, because the run DID reach done: the
        file ended first. Only an assertion on the samples could see the four
        seconds of audio in between, which is the argument for having a driver
        that listens to the render.

        The stop is issued from the tick hook rather than from the command, for
        the reason every report is: that is the thread which owns the Player,
        and a command handler is re-run by a replay. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    const auto track = rig.runs.find (mediaRun)->track;

    REQUIRE (track >= 0);
    REQUIRE (rig.audio.playing.count (track) == 1u);

    rig.submitAndTick ("run.kill", { osc::Value::string (mediaRun) });

    CHECK (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    rig.tickOnce();

    /*  THE VOICE IS ACTUALLY SILENT, which is the half that was missing. */
    CHECK (rig.audio.playing.count (track) == 0u);

    rig.tickOnce();

    /*  And the model catches up on its own, through the same edge that notices
        a cue reaching the end of its file. */
    CHECK (rig.runs.find (mediaRun)->isFinished());
}

TEST_CASE ("run.kill: a cue already on its way out by a fade is left to its fade")
{
    /*  THE ONE CASE THE FIX HAD TO BE CAREFUL OF. A stop cue with the `fade`
        verb also marks its target `stopping`, and it has a job counting down to
        a stop of its own. Stopping it the moment the state changed would land
        the cue at the instant the operator asked instead of at the end of the
        fade - which is the entire difference between the two verbs, and would
        have turned every fade-and-stop into a hard stop with extra steps. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    const auto track = rig.runs.find (mediaRun)->track;

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "1");
    rig.fire (rig.stopId);

    REQUIRE (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    /*  Halfway down and still sounding, which is what a fade-and-stop is. */
    CHECK (rig.audio.playing.count (track) == 1u);
    CHECK (rig.runs.find (mediaRun)->level < -1.0);

    for (int i = 0; i < 40; ++i)
        rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 0u);
}

TEST_CASE ("stop: a fade over the top of a fade-and-stop does not call the stop off")
{
    /*  AUTHOR, 2026-09-06: THE STOP HAPPENS WHEN IT SHOULD.

        An operator fires a slow stop and then changes their mind about the
        LEVEL - the scene ran long, the actor is still talking - and rides the
        cue back up. What they have not done is withdraw the stop. A cue that
        could be kept alive by touching a fader is a cue nobody can get rid of,
        and the operator who wanted it gone would have to find out during the
        show that it was not.

        I had this the other way round for one commit, on the reasoning that
        dropping the superseded job dropped its stop with it - which is a
        `remove_if` written for the level deciding a question about lifetime.

        WHAT THE LEVEL DOES IN BETWEEN IS STILL OPEN, and the author has named
        the case that will settle it: a fade on a GROUP over fades on its
        members, and relative fades composing on top of each other. Neither
        exists yet - `fade/@level` is a destination in dB and never an offset -
        so the new fade owns the level here, and this case does not pin that
        half. It pins the schedule, which is not the part that is open. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "2");
    rig.fire (rig.stopId);

    CHECK (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    /*  Halfway down, and the mind is changed about the level. */
    REQUIRE (rig.runs.find (mediaRun)->level < -1.0);

    rig.fire (rig.fadeId);

    /*  STILL STOPPING. The run says so because it is: what was taken over was
        the ramp, not the appointment. */
    CHECK (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    /*  The level follows the new fade - up, towards -20 dB from wherever the
        stop had got to, rather than on down to silence. */
    const auto atTakeover = rig.runs.find (mediaRun)->level;

    for (int i = 0; i < 10; ++i)
        rig.tickOnce();

    INFO ("took over at " << atTakeover << " dB, now "
           << rig.runs.find (mediaRun)->level << " dB");
    CHECK (rig.runs.find (mediaRun)->level > atTakeover);

    /*  AND IT STILL STOPS, on the tick the stop was always going to land on:
        two seconds is a hundred ticks from the stop cue, and twenty-six of them
        had gone when the fade took over. Checked from both sides, because "it
        stopped eventually" is not the claim - the claim is that the appointment
        did not move. */
    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    INFO ("just before: " << rig.runs.find (mediaRun)->state);
    CHECK_FALSE (rig.runs.find (mediaRun)->isFinished());

    for (int i = 0; i < 10; ++i)
        rig.tickOnce();

    INFO ("just after: " << rig.runs.find (mediaRun)->state);
    CHECK (rig.runs.find (mediaRun)->isFinished());
}

TEST_CASE ("stop: a fade shorter than the stop it took over waits for the stop")
{
    /*  The other order, and the one that would have hung. The new fade arrives
        at its level long before the stop is due, so a job that retired when its
        LEVEL finished would take the stop with it and the cue would play on for
        ever. That is why a fade job now has two ways of being over and only one
        of them is a counter running out. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "4");
    rig.fire (rig.stopId);

    for (int i = 0; i < 10; ++i)
        rig.tickOnce();

    /*  A one-second fade over the top of a four-second stop. */
    rig.fire (rig.fadeId);

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    /*  The fade has long since arrived and the cue is still playing, still on
        its way out. */
    INFO ("at " << rig.runs.find (mediaRun)->level << " dB, "
           << rig.runs.find (mediaRun)->state);
    CHECK (rig.runs.find (mediaRun)->state == cue::runState::stopping);
    CHECK (rig.runs.find (mediaRun)->level == doctest::Approx (-20.0).epsilon (0.01));
    REQUIRE (rig.runner.fades().size() == 1u);

    for (int i = 0; i < 160; ++i)
        rig.tickOnce();

    CHECK (rig.runs.find (mediaRun)->isFinished());
    CHECK (rig.runner.fades().empty());
}

TEST_CASE ("fade: a fade takes over from where the level has got to")
{
    /*  Starting a second fade must not jump. It begins from where the level IS,
        not from where the first fade started - anything else is a click on a PA
        and a mistake nobody can account for afterwards. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();

    rig.fire (rig.fadeId);

    /*  The fade's OWN run, which is a cue running like any other and which the
        rest of this case is about as much as the level is. */
    const auto firstFade = rig.runs.all().back().id;
    REQUIRE (rig.runs.find (firstFade)->cue == rig.fadeId);

    /*  PLAYING, not armed. A fade has nothing to arm - no voice to reserve, no
        file to make ready - so the state a run is born in is one a fade is
        never in, and a client watching this address while the level audibly
        moved would otherwise have read `armed` for the whole of it. */
    CHECK (rig.runs.find (firstFade)->state == cue::runState::playing);

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

    /*  AND THE FADE THAT WAS TAKEN OVER IS OVER, which is a separate claim from
        the level and a more consequential one. Its work is finished - somebody
        else is doing it now - so the run that reported that work has to end.

        A fade whose run never finished would be a cue that is still going for
        the rest of the show: a group waiting on it (Section 3.6) would wait for
        ever, a client watching /godot/run would show a fade that stopped moving
        an hour ago, and the table would grow one entry per fade nobody let
        finish. The rule is already written elsewhere in the Runner - a fade
        whose target has gone ends the same way - and this is the same
        situation.

        ONE TICK LATER, and that is the engine's shape rather than a delay: a
        tick drains a snapshot of its queue, so an event submitted from inside a
        command handler is applied on the tick after it. The same is true of
        every engine-origin report a command produces. */
    INFO ("straight after the takeover: " << rig.runs.find (firstFade)->state);
    rig.tickOnce();

    INFO ("a tick later: " << rig.runs.find (firstFade)->state);
    CHECK (rig.runs.find (firstFade)->isFinished());

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

//==============================================================================
/*  WAITS: the two ends of a cue, and what holds on them.

    §3.6 gives every cue a pre-wait and a post-wait, and the namespace draft
    §2.4 says they COMPOSE with a group's rather than being replaced by them.
    Groups arrive in PR 3.3; what is here is the pair working on a cue of its
    own, which is what a group will then wrap.

    THE DURATIONS ARE MEASURED RATHER THAN COUNTED. Every case below ticks until
    the state changes and asserts how many ticks that took, because a test that
    asserted "state at tick 6" restates the implementation's arithmetic and
    passes against any off-by-one it happens to share. What a wait promises is a
    LENGTH.
*/
namespace
{
    /*  Ticks until `run` leaves `state`, and answers how many ticks it took.
        Bounded, so a wait that never ends fails the test rather than hanging
        the suite. */
    template <typename R>
    int ticksSpentIn (R& rig, const std::string& run, const char* state, int bound = 400)
    {
        for (int n = 0; n < bound; ++n)
        {
            const auto* found = rig.runs.find (run);

            if (found == nullptr || found->state != state)
                return n;

            rig.tickOnce();
        }

        return bound;
    }
}

TEST_CASE ("ticksFor: seconds as the document spells them, ticks as the engine counts them")
{
    /*  ONE PLACE, because there were two and both were the literal 50 while
        TickClock::rateHz sat there being the definition. The rounding is to
        NEAREST rather than down, so a wait somebody typed as 0.02 is one tick
        rather than none - a wait that silently became no wait at all is the
        worst of the three possible answers. */
    CHECK (cue::ticksFor (0.0) == 0);
    CHECK (cue::ticksFor (1.0) == 50);
    CHECK (cue::ticksFor (0.02) == 1);
    CHECK (cue::ticksFor (0.5) == 25);
    CHECK (cue::ticksFor (2.5) == 125);

    // Not a wait. The schema refuses these; this is the second net.
    CHECK (cue::ticksFor (-1.0) == 0);
    CHECK (cue::ticksFor (0.001) == 0);
}

TEST_CASE ("pre-wait: the run exists from the GO and fires when the wait has elapsed")
{
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "0.2");   // 10 ticks

    const auto goTick = rig.tick;
    CHECK (rig.submitAndTick ("go").applied == 1);

    /*  THE RUN EXISTS FROM THE GO, which is what makes a pre-wait something an
        operator can watch rather than a gap where a cue should be. */
    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    CHECK (rig.runs.find (id)->state == cue::runState::waiting);
    CHECK (rig.runs.find (id)->dueTick == goTick + 10);

    /*  Standby did on the GO what it always does: it moved to the next sibling.
        The memo is the last cue in this list and there is no wrap (§3.5), so it
        stayed where it was - what matters is that it did not wait for the cue. */
    CHECK (rig.standby() == rig.memoId);

    CHECK (ticksSpentIn (rig, id, cue::runState::waiting) == 10);
    CHECK (rig.runs.find (id)->state == cue::runState::playing);
}

TEST_CASE ("pre-wait: a media cue arms during its wait, so the disk is paid for before it fires")
{
    /*  The reason a pre-wait is worth having on a sound cue rather than merely
        tolerable: the seconds it waits are seconds the disk spends getting
        ready. Arming during the wait is what turns a delay into preparation,
        and it is why the wait sits between the arm and the launch rather than
        in front of both. */
    Rig rig;

    /*  THE WAIT IS WRITTEN BEFORE THE POINTER ARRIVES, and the order matters
        now in a way it did not before PR 3.3: a run copies its waits when it is
        CREATED (§4.10), and standby arming is what creates this one. Setting
        the attribute afterwards would change the next run and not this one -
        which is the rule working, and would have made this test measure a cue
        with no pre-wait at all. */
    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/preWait", "0.2");   // 10 ticks
    rig.setStandby (rig.mediaId);

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    // The voice is reserved and the media requested, while the run still waits.
    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms[0].track == 0);
    CHECK (rig.runs.find (id)->track == 0);
    CHECK (rig.runs.find (id)->state == cue::runState::waiting);
    CHECK (rig.audio.launches.empty());

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    // Ready, and still not launched: armed is not fired.
    CHECK (rig.runs.find (id)->state == cue::runState::waiting);
    CHECK (rig.audio.launches.empty());

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (id)->state
                                           != cue::runState::waiting; }));

    /*  And once the wait is over the launch goes in on the next tick, because
        there is nothing left to make ready. That is the whole point. */
    rig.tickOnce();
    CHECK (rig.audio.launches.size() == 1u);
}

TEST_CASE ("post-wait: the run holds after its own work is over, and only then reports done")
{
    /*  §3.6: a post-wait is "how long after completion this cue reports done to
        its parent". So `postWait` is a PUBLISHED state and not a private timer:
        a group holding on this run has to keep holding, and a client watching it
        has to be told the same thing the group believes. */
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "0.2");   // 10 ticks

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    // The memo's own work ends on the next tick, and the post-wait begins there.
    CHECK (ticksSpentIn (rig, id, cue::runState::playing) == 1);
    CHECK (rig.runs.find (id)->state == cue::runState::postWait);

    CHECK (ticksSpentIn (rig, id, cue::runState::postWait) == 10);
    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

TEST_CASE ("waits: both ends of one cue, in order and without running into each other")
{
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "0.1");    // 5
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "0.3");   // 15

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    CHECK (ticksSpentIn (rig, id, cue::runState::waiting) == 5);
    CHECK (ticksSpentIn (rig, id, cue::runState::playing) == 1);
    CHECK (ticksSpentIn (rig, id, cue::runState::postWait) == 15);

    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

TEST_CASE ("waits: a run copies them when it is created, so editing the cue changes the next one")
{
    /*  §4.10's rule applied to a duration. The run instantiates what the cue
        said when it was fired; a designer lengthening a wait while it runs is
        writing the show, not steering tonight's performance. */
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "0.1");    // 5 ticks

    const auto goTick = rig.tick;
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto id = rig.runs.all().front().id;
    CHECK (rig.runs.find (id)->dueTick == goTick + 5);

    // Ten seconds from now on, and the run already in flight does not care.
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "10");

    CHECK (ticksSpentIn (rig, id, cue::runState::waiting) == 5);
    CHECK (rig.runs.find (id)->state == cue::runState::playing);
}

//==============================================================================
/*  run.kill ON A RUN THAT HOLDS NO VOICE.

    `enforceStops` stops a TRACK, and a fade, a network cue and a waiting run
    have none. Before this they were marked `stopping` and nothing acted on it:
    the fade went on writing levels for the rest of its duration and the run sat
    in `stopping` until the show closed - which a group would have waited on for
    ever.
*/

TEST_CASE ("run.kill: it reaches a fade, which holds a level rather than a voice")
{
    FadeRig rig;
    const auto media = rig.startMedia();

    rig.setCue (rig.fadeId, "duration", "10");        // long enough to catch in the act
    rig.setCue (rig.fadeId, "level", "-60");

    rig.fire (rig.fadeId);

    const auto fadeRun = rig.runs.all().back().id;
    REQUIRE (rig.runs.find (fadeRun)->kind == "fade");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto partWay = rig.runs.find (media)->level;
    CHECK (partWay < 0.0);
    CHECK (partWay > -60.0);

    REQUIRE (rig.submitAndTick ("run.kill",
                                { osc::Value::string (fadeRun) }).applied == 1);
    rig.tickOnce();

    CHECK (rig.runs.find (fadeRun)->state == cue::runState::done);

    /*  And the level it had reached is where it stays. A killed fade abandons
        its ramp; it does not snap the cue to a destination it never got to. */
    const auto after = rig.runs.find (media)->level;

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    /*  Approx rather than `==`, because the strict build treats a raw
        floating-point comparison as an error (-Wfloat-equal) - and rightly:
        what is being asserted is that the level did not MOVE, not that two
        doubles are bit-identical. */
    CHECK (rig.runs.find (media)->level == doctest::Approx (after));
    CHECK (rig.runs.find (media)->state == cue::runState::playing);
}

TEST_CASE ("run.kill: killing a fade-and-stop takes the stop with it")
{
    /*  The difference between a kill and a takeover, worth stating because they
        look alike and are opposite.

        A fade arriving over a fade-and-stop INHERITS the arrival (author,
        2026-09-06): riding a level back up does not withdraw the stop, because
        the operator who fired it has not changed their mind about the cue going
        away. `run.kill` is the other thing entirely - the immediate path, the
        one Esc and double-Esc will be built on, which asks nothing of the cue.
        Kill the run of a stop cue and the stop it was going to perform is what
        you killed. */
    FadeRig rig;
    rig.startMedia();

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "10");

    rig.fire (rig.stopId);

    const auto stopRun = rig.runs.all().back().id;
    REQUIRE (rig.runs.find (stopRun)->kind == "stop");

    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.kill",
                                { osc::Value::string (stopRun) }).applied == 1);
    rig.tickOnce();

    CHECK (rig.runs.find (stopRun)->state == cue::runState::done);

    /*  AND THE TARGET IS PLAYING AGAIN. The stop cue marked it `stopping` the
        moment it fired; killing the stop has to lift that mark, or
        `enforceStops` finds a run no job is holding and stops it - so killing a
        ten-second fade-and-stop would stop the cue instantly, which is the
        opposite of every reading of what was asked for. */
    const auto media = rig.runs.all().front().id;
    CHECK (rig.runs.find (media)->state == cue::runState::playing);

    // Well past where the stop would have landed, and nothing was ever stopped.
    for (int n = 0; n < 600; ++n)
        rig.tickOnce();

    CHECK (rig.audio.stopped.empty());
}

TEST_CASE ("run.kill: it reaches a run that is still in its pre-wait")
{
    /*  A waiting run has no voice, no job and no level, so nothing owned it -
        and `run.kill` writes `stopping` over the `waiting` that said who was
        looking after it. Without the ownership sweep it stayed `stopping` until
        the show closed. */
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "10");

    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto id = rig.runs.all().front().id;
    CHECK (rig.runs.find (id)->state == cue::runState::waiting);

    REQUIRE (rig.submitAndTick ("run.kill", { osc::Value::string (id) }).applied == 1);
    rig.tickOnce();

    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

//==============================================================================
/*  run.late, WHICH NOTHING PRODUCED UNTIL NOW.

    Registered, documented and tested since PR 2.3 and never once submitted,
    because the number is not visible from where the launch is placed:
    `launchIfDue` puts one a fixed number of ticks ahead of the tick it happens
    to run on, so by its own arithmetic it can never be late. What it did not
    know was the tick the launch was ASKED for on. The run knows.
*/

TEST_CASE ("run.late: a cue fired from cold reports the blocks the disk cost it")
{
    Rig rig;
    rig.setStandby (rig.mediaId);

    const auto goTick = rig.tick;
    CHECK (rig.submitAndTick ("go").applied == 1);
    const auto id = rig.runs.all().front().id;

    // The disk takes its time: twenty ticks before the arm comes back.
    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.audio.launches.empty());
    CHECK (rig.runs.find (id)->late == 0);

    rig.audio.completeArms (rig.engine);

    REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

    const auto launchTick = rig.tick - 1;             // tickOnce post-increments

    REQUIRE (rig.audio.launches.size() == 1u);
    CHECK (rig.runs.find (id)->late > 0);

    /*  The lateness is the EXCESS over the best case, in blocks. One tick is
        never late: a GO is applied in a tick's drain and the launch hook runs
        before the drain, so the earliest tick that can see the request is the
        one after it - for every cue, including one that was armed and ready.

        Derived here from what the test itself observed rather than restated
        from the implementation, so that changing either means changing both. */
    const auto lateTicks = launchTick - goTick - 1;
    CHECK (rig.runs.find (id)->late
             == static_cast<int> ((lateTicks * 960) / rig.audio.block));
}

TEST_CASE ("run.late: a cue armed at standby is not late, which is what arming is for")
{
    Rig rig;
    rig.setStandby (rig.mediaId);

    // Armed ahead, which is what standby will do from PR 3.3.
    REQUIRE (rig.submitAndTick ("audio.arm",
                                { osc::Value::string (rig.mediaId) }).applied == 1);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto id = rig.runs.all().front().id;
    CHECK (rig.runs.find (id)->state == cue::runState::armed);

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

    CHECK (rig.runs.find (id)->late == 0);
}

//==============================================================================
/*  GROUPS: time, order and lifetime, and nothing else.

    §4.12: containers describe behaviour, content describes output. A group run
    holds no voice and no level - what it holds is a position among its members
    and the answer to "are they finished yet". Which is what giving every kind a
    run bought in PR 3.1: there is ONE place to ask, and it answers the same way
    for a memo, a fade and a nested group.

    THE SCHEDULER REPORTS RATHER THAN ACTS, like every hook here. A member's
    `run.ended` is applied in one tick's drain, the scheduler sees it on the
    next and submits, and the launch goes in on the one after - two ticks plus
    the launch latency, published as sequenceGapTicks rather than left for
    somebody to find with a stopwatch. §3.6's sequence group is discrete
    children relaunched; the sample-accurate join is §3.24's range, a different
    mechanism on purpose.
*/
namespace
{
    /*  The rig, plus a group with three memo members. Memos because this is
        about ORDER and LIFETIME: a memo's run finishes on the tick after it
        fires, so a sequence of them advances as fast as the scheduler can, and
        what is being measured is the scheduler. */
    struct GroupRig : Rig
    {
        GroupRig()
        {
            groupId = document.createCue (listId, 2, "group", "Preshow").id;

            /*  AUTOMATIC, because `advance` defaults to MANUAL - which is the
                gentler default (a group somebody made and did not configure is
                one the operator drives) and the wrong one for the cases below,
                which are about the scheduler advancing a chain on its own.
                Manual groups have a rig and cases of their own. */
            document.setAttribute ("/godot/cue/" + groupId + "/advance", "auto");

            first = document.createCue (groupId, 0, "memo", "One").id;
            second = document.createCue (groupId, 1, "memo", "Two").id;
            third = document.createCue (groupId, 2, "memo", "Three").id;
        }

        void setCue (const std::string& id, const char* name, const std::string& value)
        {
            document.setAttribute ("/godot/cue/" + id + "/" + name, value);
        }

        /** Ticks until the group run finishes, bounded so a stuck group fails
            the test rather than hanging the suite. */
        int runToCompletion (const std::string& groupRun, int bound = 400)
        {
            for (int n = 0; n < bound; ++n)
            {
                const auto* found = runs.find (groupRun);

                if (found == nullptr || found->isFinished())
                    return n;

                tickOnce();
            }

            return bound;
        }

        /** The group's header or footer, made if it has none. */
        std::string roleOf (const std::string& group, const char* role)
        {
            const auto edit = document.createRole (group, role);
            REQUIRE (edit.ok);
            return edit.id;
        }

        /** The run of a cue, or empty. */
        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
        }

        std::string groupId, first, second, third;
    };
}

TEST_CASE ("group: a sequence runs its members one after another, in order")
{
    GroupRig rig;
    rig.setStandby (rig.groupId);

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto groupRun = rig.runs.all().front().id;

    CHECK (rig.runs.find (groupRun)->kind == "group");
    CHECK (rig.runs.find (groupRun)->state == cue::runState::playing);
    CHECK (rig.runs.find (groupRun)->track == -1);       // a group owns no output

    CHECK (rig.runToCompletion (groupRun) < 400);

    /*  Every member ran, each with a run of its own, and the group is the
        parent of all three. */
    CHECK (rig.runs.all().size() == 4u);

    for (const auto& cueId : { rig.first, rig.second, rig.third })
    {
        const auto id = rig.runOf (cueId);
        REQUIRE_MESSAGE (! id.empty(), "no run for " << cueId);
        CHECK (rig.runs.find (id)->parent == groupRun);
        CHECK (rig.runs.find (id)->state == cue::runState::done);
    }

    CHECK (rig.runs.find (groupRun)->children.size() == 3u);

    /*  IN ORDER, which for a sequence is the whole promise. Runs are created in
        the order they were spawned, so the table's own order is the answer. */
    std::vector<std::string> cuesInRunOrder;

    for (const auto& run : rig.runs.all())
        if (run.parent == groupRun)
            cuesInRunOrder.push_back (run.cue);

    CHECK (cuesInRunOrder == std::vector<std::string> { rig.first, rig.second, rig.third });
}

TEST_CASE ("group: a timeline schedules every member at entry, and each pre-wait is an offset")
{
    /*  §3.6: "Timeline group - all members scheduled at entry; pre-waits are
        offsets." Which is why raising the GROUP's pre-wait defers a whole scene
        without disturbing the relative timing somebody spent an afternoon
        getting right: the entry moves and every offset is measured from it. */
    GroupRig rig;
    rig.setCue (rig.groupId, "mode", "timeline");
    rig.setCue (rig.second, "preWait", "0.2");           // 10 ticks
    rig.setCue (rig.third, "preWait", "0.4");            // 20 ticks

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;

    // One tick for the scheduler to see the group, one for the spawns to apply.
    rig.tickOnce();
    rig.tickOnce();

    // All three exist at once, which a sequence would never do.
    CHECK (rig.runs.all().size() == 4u);

    const auto firstRun = rig.runOf (rig.first);
    const auto secondRun = rig.runOf (rig.second);
    const auto thirdRun = rig.runOf (rig.third);

    REQUIRE (! secondRun.empty());
    REQUIRE (! thirdRun.empty());

    CHECK (rig.runs.find (secondRun)->state == cue::runState::waiting);
    CHECK (rig.runs.find (thirdRun)->state == cue::runState::waiting);

    // And their offsets differ by exactly the difference in their pre-waits.
    CHECK (rig.runs.find (thirdRun)->dueTick - rig.runs.find (secondRun)->dueTick == 10);

    CHECK (rig.runToCompletion (groupRun) < 400);
    CHECK (rig.runs.find (firstRun)->state == cue::runState::done);
    CHECK (rig.runs.find (thirdRun)->state == cue::runState::done);
}

TEST_CASE ("group: it is not done until its last member is, and its post-wait runs on top")
{
    /*  §3.6's completion rule and §2.4's composition rule, in one case. A group
        is complete once every member is - each member's own post-wait included,
        since that is what done MEANS for a cue - and the group's post-wait then
        runs on top of that, before it reports done to whatever is waiting on
        it. Nested groups stack outward, one layer per level. */
    GroupRig rig;
    rig.setCue (rig.third, "postWait", "0.2");           // 10 ticks
    rig.setCue (rig.groupId, "postWait", "0.2");         // 10 more, on top

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;

    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.third).empty(); }));

    const auto lastMember = rig.runOf (rig.third);

    // The last member holds its own post-wait, and the group is still playing.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (lastMember)->state
                                           == cue::runState::postWait; }));

    CHECK (rig.runs.find (groupRun)->state == cue::runState::playing);

    // Then the group holds its own, and is still not done.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (groupRun)->state
                                           != cue::runState::playing; }));

    CHECK (rig.runs.find (lastMember)->state == cue::runState::done);
    CHECK (rig.runs.find (groupRun)->state == cue::runState::postWait);

    CHECK (rig.runToCompletion (groupRun) < 400);
}

TEST_CASE ("group: a disabled member is not spawned, and Phase 1's choice is now the other one")
{
    /*  Phase 1 asserted that a disabled cue is NOT skipped, and named the test
        for the choice rather than for a rule so that this moment would be
        visible: "Phase 3 revisits it when a GO that does nothing becomes a real
        failure rather than a hypothetical one."

        It has. A disabled member that a group spawned would be a run that plays
        nothing and is waited on for ever, which is the exact failure §3.6's
        completion table exists to avoid. So the scheduler skips it. It is still
        a row in the list, still addressable, still parkable - it is simply not
        run. */
    GroupRig rig;
    rig.setCue (rig.second, "enabled", "false");

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    CHECK (rig.runToCompletion (groupRun) < 400);

    CHECK (rig.runOf (rig.first) != "");
    CHECK (rig.runOf (rig.second) == "");                // never spawned
    CHECK (rig.runOf (rig.third) != "");
}

TEST_CASE ("group: an empty one completes rather than waiting for nothing")
{
    /*  §3.6 says an emptied round completes the group rather than spinning, and
        the same answer holds one level up: a group with no members - authored
        that way, or with every member disabled - is done. Anything else is a
        show that stops on a container somebody forgot to fill. */
    GroupRig rig;
    const auto empty = rig.document.createCue (rig.listId, 3, "group", "Nothing").id;

    rig.setStandby (empty);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    CHECK (rig.runToCompletion (groupRun) < 10);
    CHECK (rig.runs.find (groupRun)->state == cue::runState::done);
}

TEST_CASE ("group: groups nest, and the inner one completes before the outer")
{
    /*  §3.6: for a nested sequential parent, the child's completion is still
        "last member completes". Which means the tree of runs mirrors the tree
        of cues while it is running, and a parent waits on a child that is
        itself waiting on three of its own. */
    GroupRig rig;

    const auto outer = rig.document.createCue (rig.listId, 3, "group", "Scene").id;
    rig.setCue (outer, "advance", "auto");        // the default is manual
    rig.document.setAttribute ("/godot/cue/" + rig.groupId + "/enabled", "false");

    // Move the inner group inside the outer one, and give the outer a memo after it.
    REQUIRE (rig.document.move (rig.groupId, outer, 0).ok);
    rig.document.setAttribute ("/godot/cue/" + rig.groupId + "/enabled", "true");
    const auto after = rig.document.createCue (outer, 1, "memo", "After").id;

    rig.setStandby (outer);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto outerRun = rig.runs.all().front().id;
    CHECK (rig.runToCompletion (outerRun) < 400);

    const auto innerRun = rig.runOf (rig.groupId);
    REQUIRE (! innerRun.empty());

    CHECK (rig.runs.find (innerRun)->parent == outerRun);
    CHECK (rig.runs.find (rig.runOf (rig.first))->parent == innerRun);

    // The memo after the inner group ran, which means the outer waited for it.
    CHECK (rig.runOf (after) != "");
    CHECK (rig.runs.find (rig.runOf (after))->state == cue::runState::done);
}

TEST_CASE ("group: killing it takes its members with it")
{
    /*  A group's lifetime is one of the three things a group owns (§4.12), so
        ending one ends what it was organising. `run.kill` is the immediate
        path - it runs no footers and asks nothing of the cue - and this is what
        Phase 10's double-Esc will be built on. */
    GroupRig rig;
    rig.setCue (rig.second, "preWait", "10");            // long enough to be caught

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;

    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.second).empty(); }));

    const auto member = rig.runOf (rig.second);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (member)->state
                                           == cue::runState::waiting; }));

    REQUIRE (rig.submitAndTick ("run.kill",
                                { osc::Value::string (groupRun) }).applied == 1);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (member)->isFinished());
    CHECK (rig.runs.find (groupRun)->isFinished());

    // And the third member was never started.
    CHECK (rig.runOf (rig.third) == "");
}

//==============================================================================
/*  WHAT A CUE SAYS WHEN IT HAS NOT SAID ANYTHING.

    The canonical writer OMITS an attribute holding its default and the reader
    leaves it absent, so a cue that has never had a value written to it has no
    such property on its ValueTree at all. Reading one directly answers with the
    type's zero - an empty string, 0.0, false - and for most rows in the table
    that IS the default, so it looks like it works.

    For two of them it is not, and both are the kind of wrong that is quiet:
    `fade/@level` defaults to -120 and `osc/@timeout` to 5. A fade cue somebody
    created and did not fill in would have faded UP to 0 dB, which the table
    describes as the opposite of what a fresh fade cue is for ("a slow cut to
    nothing, which is a real thing to want and can only ever make the show
    quieter"). A verified network cue would have given up before it asked.

    Nothing caught it because every fixture and every other test sets these
    explicitly. The group scheduler found the same bug from the other end -
    reading `enabled` off the tree answered `false` for every cue in the show -
    which is what made it worth looking for the rest.
*/

TEST_CASE ("cue defaults: a fade nobody filled in goes to silence, not to unity")
{
    FadeRig rig;
    const auto media = rig.startMedia();

    /*  A fresh fade cue, pointed at the media and otherwise untouched: no
        level, no duration, no curve. */
    const auto bare = rig.document.createCue (rig.listId, 4, "fade", "Bare").id;
    rig.setCue (bare, "target", rig.mediaId);

    REQUIRE (rig.document.findById (bare).hasProperty (juce::Identifier ("level")) == false);
    CHECK (rig.document.getAttribute ("/godot/cue/" + bare + "/level").value_or ("?") == "-120");

    rig.fire (bare);
    rig.tickOnce();
    rig.tickOnce();

    // A duration of zero is a jump, so the destination is reached at once.
    CHECK (rig.runs.find (media)->level == doctest::Approx (-120.0));
}

//==============================================================================
/*  HEADERS AND FOOTERS: what runs before a group's members, and what runs
    after them and BLOCKS.

    §3.6 makes them independent of each other and of everything else: "the user
    decides whether to use either". A header is where §3.12's prepare/commit
    will live, extended from one row to a whole block. A footer is an ordinary
    cue list that runs at group exit - "kill LFOs and AutoMotion in WFS-DIY,
    stop effects processing, release audio interface channels" - and it is NOT
    an inverse of the header.

    FOOTERS BLOCK, which is the load-bearing half: the group is not done until
    its footer's cues report done, so a following scene that reallocates the
    same interface channels waits for the release rather than racing it.
*/

TEST_CASE ("group: a header runs before the members and a footer runs after them")
{
    GroupRig rig;

    const auto header = rig.roleOf (rig.groupId, "header");
    const auto footer = rig.roleOf (rig.groupId, "footer");

    const auto opening = rig.document.createCue (header, 0, "memo", "Pre-arm").id;
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    CHECK (rig.runToCompletion (groupRun) < 400);

    /*  Five runs: the group, one header cue, three members, one footer cue -
        and the order they were created in is the order they ran in, because a
        run is created when it is spawned. */
    std::vector<std::string> ran;

    for (const auto& run : rig.runs.all())
        if (run.parent == groupRun)
            ran.push_back (run.cue);

    CHECK (ran == std::vector<std::string> { opening, rig.first, rig.second, rig.third, closing });
}

TEST_CASE ("group: a header and a footer are each optional, and independent of the other")
{
    /*  §3.6: "Independent of each other; the user decides whether to use
        either." A group with only a footer must not spend a tick in a header
        it does not have. */
    GroupRig rig;

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    CHECK (rig.runToCompletion (groupRun) < 400);

    CHECK (rig.runOf (closing) != "");
    CHECK (rig.runs.find (rig.runOf (closing))->state == cue::runState::done);
}

TEST_CASE ("group: the footer blocks - the group is not done until its cues are")
{
    /*  The property a following scene depends on. A footer that released
        interface channels while the group reported done would be a race the
        next scene loses about one time in ten, which is the worst kind of
        show bug: it works in the tech and fails on a Friday. */
    GroupRig rig;

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;
    rig.setCue (closing, "postWait", "0.4");             // 20 ticks of holding on

    rig.setStandby (rig.groupId);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;

    // Wait until the footer cue is holding its post-wait...
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (closing);
        return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
    }));

    // ...and the group is still not done, because the footer is not.
    CHECK_FALSE (rig.runs.find (groupRun)->isFinished());

    CHECK (rig.runToCompletion (groupRun) < 400);
}

TEST_CASE ("group: a stop cue runs the footer, and run.kill does not")
{
    /*  §4.4's first two levels of stop, drawn now so that Phase 10 has only to
        bind keys to them.

        ESC IS GRACEFUL AND RUNS FOOTERS - "the same code path as normal
        completion, entered early: a group aborted at 04:12 releases its
        channels and kills its LFOs exactly as it would have at 06:00". The
        releasing is what a footer is FOR, so a graceful stop that skipped it
        would leave the channels held by a scene that has gone.

        DOUBLE ESC IS IMMEDIATE AND SKIPS THEM. "The world may be left in a
        state nobody declared; that is the price of an emergency."

        Both write `stopping` to the run, because both are true statements about
        it - so the state cannot say which was meant, and a flag does. */
    GroupRig rig;

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;
    rig.setCue (rig.first, "preWait", "10");             // hold the group in its members

    SUBCASE ("a stop cue is graceful")
    {
        const auto stopId = rig.document.createCue (rig.listId, 3, "stop", "Abort").id;
        rig.setCue (stopId, "target", rig.groupId);

        rig.setStandby (rig.groupId);
        CHECK (rig.submitAndTick ("go").applied == 1);

        const auto groupRun = rig.runs.all().front().id;
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) });
        CHECK (rig.runToCompletion (groupRun) < 400);

        // The footer ran on the way out.
        CHECK (rig.runOf (closing) != "");
    }

    SUBCASE ("run.kill is immediate")
    {
        rig.setStandby (rig.groupId);
        CHECK (rig.submitAndTick ("go").applied == 1);

        const auto groupRun = rig.runs.all().front().id;
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        rig.submitAndTick ("run.kill", { osc::Value::string (groupRun) });
        CHECK (rig.runToCompletion (groupRun) < 400);

        // And the footer did not.
        CHECK (rig.runOf (closing) == "");
    }
}

TEST_CASE ("group: a header's cues are published as cues, and are not members of the group")
{
    /*  A header is an ordinary cue list, so what is in it is ordinary cues with
        addresses of their own. What it must NOT be is a member: `order` is the
        group's cue list, and a header taking index 0 in it would have shifted
        every real member by one. */
    GroupRig rig;

    const auto header = rig.roleOf (rig.groupId, "header");
    const auto opening = rig.document.createCue (header, 0, "memo", "Pre-arm").id;

    /*  The header's cue is a cue: it exists, it is addressable, and it is
        reachable by its own identifier like any other. (That it is NOT in the
        group's `order` is asserted in TreeTests, where `order` lives - it is a
        derived value and the document does not store one.) */
    CHECK (rig.document.findById (opening).isValid());
    CHECK (rig.document.getAttribute ("/godot/cue/" + opening + "/name").value_or ("?")
             == "Pre-arm");

    /*  And asking for the same role twice answers with the one that exists,
        rather than making a second: a group has at most one of each. */
    CHECK (rig.roleOf (rig.groupId, "header") == header);
}

//==============================================================================
/*  A MANUAL SEQUENCE GROUP: the operator is the parent.

    §3.6: "manual - a member starts on GO. The standby pointer descends into the
    group; the operator is the parent. Toggleable during tech; a mid-run change
    takes effect at the next member boundary."

    So the scheduler does almost nothing here. It runs the header, and after
    that it spawns nothing: each GO on the member the pointer has reached
    creates that member's run as a child of the group's. What the group still
    owns is the things a group owns (§4.12) - lifetime, so killing it takes the
    members; order, so its footer runs after the last of them; and completion,
    so whatever is waiting on the group is told when it is over.

    Both attributes DEFAULT to this - `mode` to `sequence` and `advance` to
    `manual` - which is the gentler pair: a group somebody made and did not
    configure is one the operator drives rather than one that runs away.
*/
namespace
{
    struct ManualRig : Rig
    {
        ManualRig()
        {
            groupId = document.createCue (listId, 2, "group", "Scene").id;

            // Deliberately not configured: manual sequence is what both default to.
            first = document.createCue (groupId, 0, "memo", "One").id;
            second = document.createCue (groupId, 1, "memo", "Two").id;
            third = document.createCue (groupId, 2, "memo", "Three").id;
            /*  Somewhere for the pointer to go when it leaves the group. There
                is no wrap at the end of a list (§3.5), so without this the
                "it climbs out" case would be indistinguishable from "it is at
                the end and stays put". */
            after = document.createCue (listId, 3, "memo", "After").id;
        }

        void setCue (const std::string& id, const char* name, const std::string& value)
        {
            document.setAttribute ("/godot/cue/" + id + "/" + name, value);
        }

        std::string roleOf (const std::string& group, const char* role)
        {
            const auto edit = document.createRole (group, role);
            REQUIRE (edit.ok);
            return edit.id;
        }

        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
        }

        std::string groupId, first, second, third, after;
    };
}

TEST_CASE ("manual group: each GO fires one member, and the pointer walks through it")
{
    ManualRig rig;
    rig.setStandby (rig.first);           // the pointer descends here on its own

    /*  The FIRST GO enters the group: it creates the group's run - which is
        what the members will be children of - and the group fires member one
        after its header. */
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto groupRun = rig.runOf (rig.groupId);
    REQUIRE (! groupRun.empty());
    CHECK (rig.runs.find (rig.runOf (rig.first))->parent == groupRun);

    // And the pointer moved on, as it does on every GO, whatever the cue did.
    CHECK (rig.standby() == rig.second);

    /*  NOTHING ELSE HAPPENS ON ITS OWN. Twenty ticks after the first member has
        finished, the second has still not been fired: the operator is the
        parent, so the group is waiting for them. */
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (rig.first);
        return ! id.empty() && rig.runs.find (id)->isFinished();
    }));

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (rig.second) == "");
    CHECK_FALSE (rig.runs.find (groupRun)->isFinished());

    // The second GO fires the second member, into the same group run.
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.second).empty(); }));

    CHECK (rig.runs.find (rig.runOf (rig.second))->parent == groupRun);
    CHECK (rig.standby() == rig.third);

    /*  And the third takes the pointer OUT of the group, on the press that
        fires the last member - decision M, 2026-09-06: no GO is ever spent on
        leaving. (The count is not asserted: the same tick can apply a
        `run.launch` the scheduler asked for, which is the machinery working.) */
    CHECK (rig.submitAndTick ("go").rejected == 0);
    CHECK (rig.standby() == rig.after);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (groupRun)->isFinished(); }));
}

TEST_CASE ("manual group: it is over when its last member is, not when it is idle")
{
    /*  A manual group between GOs looks exactly like one that is over: no child
        is running either way. What tells them apart is whether the LAST member
        was ever started - so an idle group in the middle is still playing, and
        anything waiting on it keeps waiting. */
    ManualRig rig;
    rig.setStandby (rig.first);

    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    REQUIRE (rig.tickUntil ([&] { return rig.runs.allChildrenFinished (groupRun)
                                           && ! rig.runOf (rig.first).empty(); }));

    // Idle in the middle, and not done.
    CHECK_FALSE (rig.runs.find (groupRun)->isFinished());
}

TEST_CASE ("manual group: entering it runs the header first, then the member")
{
    /*  §3.6 puts the header before the members whatever the group's mode is -
        it is the group's own preparation, and the GO that enters the group is
        the same GO that fires the member at the far end of it. */
    ManualRig rig;

    const auto header = rig.roleOf (rig.groupId, "header");
    const auto opening = rig.document.createCue (header, 0, "memo", "Pre-arm").id;
    rig.setCue (opening, "postWait", "0.2");         // long enough to be caught

    rig.setStandby (rig.first);
    CHECK (rig.submitAndTick ("go").applied == 1);

    // The header cue runs, and the member has not.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (opening).empty(); }));
    CHECK (rig.runOf (rig.first) == "");

    // Then the member, once the header is done.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));
    CHECK (rig.runs.find (rig.runOf (opening))->isFinished());
}

TEST_CASE ("manual group: the pointer put in the middle enters there, not at the top")
{
    /*  The ordinary path descends to member one and GO there creates the group,
        so "enter at the first member" and "enter where the pointer is" are the
        same thing almost always. `standby.set` is where they differ - and
        starting a scene at a place nobody asked for is the wrong answer. */
    ManualRig rig;
    rig.setStandby (rig.second);

    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.second).empty(); }));

    CHECK (rig.runOf (rig.first) == "");             // never started
}

TEST_CASE ("manual group: killing it takes the members with it and skips the footer")
{
    ManualRig rig;

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;

    rig.setStandby (rig.first);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto groupRun = rig.runs.all().front().id;
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    rig.submitAndTick ("run.kill", { osc::Value::string (groupRun) });
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (groupRun)->isFinished(); }));

    CHECK (rig.runOf (closing) == "");               // no footer: it was killed
}

TEST_CASE ("manual group: a GO record carries every run it created")
{
    /*  One GO can create several runs - the groups between the pointer and the
        list, then the member - so the record is variadic. The guarantee is the
        one a single identifier gave, widened: a replay never draws a number of
        its own, so it needs to be handed all of them, in the order they were
        made. */
    ManualRig rig;

    // A manual group inside the manual group: two levels to create at once.
    const auto inner = rig.document.createCue (rig.groupId, 0, "group", "Inner").id;
    const auto deep = rig.document.createCue (inner, 0, "memo", "Deep").id;

    rig.setStandby (deep);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto go = std::find_if (parsed.records.begin(), parsed.records.end(),
                                  [] (const auto& record) { return record.command == "go"; });

    REQUIRE (go != parsed.records.end());

    /*  Two: the outer group's run and the inner group's. The member's own is
        spawned by the inner group's job after its header, so it carries its
        identifier in a `run.spawn` record instead. */
    CHECK (go->args.size() == 2u);

    for (const auto& arg : go->args)
        CHECK (rig.runs.find (arg.getString()) != nullptr);

    /*  AND THE SIGNATURE ACCEPTS WHAT THE HANDLER ANSWERED WITH, which is the
        half that makes the other half worth anything.

        `wfg replay` re-submits every record exactly as it was written, and the
        arity check is the first thing it meets. A command that answers with
        more identifiers than its signature accepts writes a record of its own
        that it would then refuse - so the session cannot reproduce itself, and
        the failure arrives as `arity` on a log nobody thought to replay.

        Asked of the registry rather than by submitting the record again,
        because submitting it would also RUN it: what is being checked is the
        signature, and the signature is where the rule lives. */
    const auto* command = rig.engine.commands().find ("go");
    REQUIRE (command != nullptr);

    const auto check = CommandRegistry::checkArgs (*command, go->args);
    CHECK (check.ok);
    CHECK (check.reason == "");
}

TEST_CASE ("manual group: a GO several levels down makes one run per group, and one member")
{
    /*  ONE PRESS, ONE RUN EACH, and this is the case that was wrong.

        A member inside a group inside a group needs both of those groups live
        before it can be their child, so the press creates both - and it used to
        create one of them TWICE: once on the way down, and once again when the
        outer group's job spawned its own first member, which is that same inner
        group. Two runs of one group, each with a job of its own, each spawning
        the member: the scene played twice, out of step with itself, under one
        GO the operator pressed once.

        Three separate mistakes made it: the entry point handed to every level
        was the pointer's own cue, which is a member of the innermost group and
        of nothing above it; every group on the path was fired immediately
        rather than left for its parent to start; and a phase looked at every
        child of the run rather than at the children of its own cues. */
    ManualRig rig;

    const auto inner = rig.document.createCue (rig.groupId, 0, "group", "Inner").id;
    const auto deepOne = rig.document.createCue (inner, 0, "memo", "Deep one").id;
    const auto deepTwo = rig.document.createCue (inner, 1, "memo", "Deep two").id;

    rig.setStandby (deepOne);
    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (deepOne).empty(); }));

    const auto runsFor = [&rig] (const std::string& cueId)
    {
        return std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                              [&cueId] (const cue::Run& run) { return run.cue == cueId; });
    };

    CHECK (runsFor (rig.groupId) == 1);
    CHECK (runsFor (inner) == 1);
    CHECK (runsFor (deepOne) == 1);

    /*  And the run tree is the document's shape: the member under the inner
        group, the inner group under the outer one. That is what makes killing
        the outer group take the whole scene. */
    const auto outerRun = rig.runOf (rig.groupId);
    const auto innerRun = rig.runOf (inner);

    REQUIRE (! outerRun.empty());
    REQUIRE (! innerRun.empty());

    CHECK (rig.runs.find (innerRun)->parent == outerRun);
    CHECK (rig.runs.find (rig.runOf (deepOne))->parent == innerRun);

    //  The pointer moved on inside the inner group, as it does on any GO.
    CHECK (rig.standby() == deepTwo);

    //  Nothing else runs on its own: the operator is still the parent.
    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (deepTwo) == "");
    CHECK_FALSE (rig.runs.find (outerRun)->isFinished());
}

TEST_CASE ("manual group: a descending GO still runs the outer header first")
{
    /*  §3.6 puts a group's header before its members whatever the pointer was
        on, and a GO that descends past that group is the case where it is
        easiest to lose: the member is several levels below and its group is
        created by the same press. Firing those groups on the way down did lose
        it - the innermost spawned its member on the next tick while the header
        above was still running, which is the scene starting before its own
        preparation. */
    ManualRig rig;

    const auto inner = rig.document.createCue (rig.groupId, 0, "group", "Inner").id;
    const auto deep = rig.document.createCue (inner, 0, "memo", "Deep").id;

    const auto header = rig.roleOf (rig.groupId, "header");
    const auto opening = rig.document.createCue (header.c_str(), 0, "memo", "Pre-arm").id;
    rig.setCue (opening, "postWait", "0.2");        // long enough to be caught in the act

    rig.setStandby (deep);
    CHECK (rig.submitAndTick ("go").applied == 1);

    //  The outer header runs, and nothing inside the inner group has started.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (opening).empty(); }));
    CHECK (rig.runOf (deep) == "");

    //  Then the member, once the header is done - one GO, nothing skipped.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (deep).empty(); }));
    CHECK (rig.runs.find (rig.runOf (opening))->isFinished());
}

TEST_CASE ("manual group: firing one by name is refused, because nobody would advance it")
{
    /*  §3.6 makes the OPERATOR the parent of a manual sequence group: its
        members start on GO, one press at a time, and the standby pointer is
        what says which. Fired from a surface there is nobody to press anything,
        so it would run its header, start its first member and then wait for a
        GO that is never coming - a scene stuck halfway with its voices held.

        Refused rather than quietly run as an automatic one, because "run this
        group without me" is a reasonable thing to want and is a different group
        from the one somebody wrote. From PR 3.7 a trigger is refused the same
        way and for the same reason. */
    ManualRig rig;

    const auto outcome = rig.submitAndTick ("cue.fire",
                                            { osc::Value::string (rig.groupId) });

    CHECK (outcome.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);
    CHECK (rig.runs.all().empty());

    /*  An AUTOMATIC group is a different thing entirely: it advances itself, so
        firing it by name is exactly what a surface button should do. */
    rig.setCue (rig.groupId, "advance", "auto");

    CHECK (rig.submitAndTick ("cue.fire",
                              { osc::Value::string (rig.groupId) }).applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));
}


//==============================================================================
/*  ROUNDS. §3.6 lets a group play its members several times over, in an order
    it chooses, with some of them left out - and every one of those is a
    decision the engine takes rather than something the document states. So each
    one is written down: `run.round` carries the seed and the members in the
    order they will play, and a replay reads that back rather than drawing it.

    The rig is an automatic sequence of memos, which is the shortest thing that
    has boundaries at all: a memo ends on the tick after it fires, so a round of
    three takes a dozen ticks rather than a file's length.
*/
namespace
{
    struct RoundRig : Rig
    {
        RoundRig()
        {
            groupId = document.createCue (listId, 2, "group", "Ambience").id;
            document.setAttribute ("/godot/cue/" + groupId + "/advance", "auto");

            first = document.createCue (groupId, 0, "memo", "One").id;
            second = document.createCue (groupId, 1, "memo", "Two").id;
            third = document.createCue (groupId, 2, "memo", "Three").id;
        }

        void setGroup (const char* name, const std::string& value)
        {
            REQUIRE (document.setAttribute ("/godot/cue/" + groupId + "/" + name, value).ok);
        }

        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
        }

        std::string groupRun() const
        {
            for (const auto& run : runs.all())
                if (run.cue == groupId)
                    return run.id;

            return {};
        }

        /** Every round this run has drawn, in order, as the log recorded them. */
        std::vector<std::vector<std::string>> rounds()
        {
            std::vector<std::vector<std::string>> out;

            for (const auto& record : LogFile::parse (engine.log().contents()).records)
            {
                if (record.command != "run.round")
                    continue;

                std::vector<std::string> round;

                for (std::size_t i = 2; i < record.args.size(); ++i)
                    round.push_back (record.args[i].getString());

                out.push_back (std::move (round));
            }

            return out;
        }

        /** The cues that have had a run, in the order they were created. */
        std::vector<std::string> played() const
        {
            std::vector<std::string> out;

            for (const auto& run : runs.all())
                if (run.cue != groupId && ! run.cue.empty())
                    out.push_back (run.cue);

            return out;
        }

        void goAndSettle (int ticks = 120)
        {
            REQUIRE (submitAndTick ("go").applied == 1);

            for (int n = 0; n < ticks; ++n)
                tickOnce();
        }

        std::string groupId, first, second, third;
    };
}

TEST_CASE ("rounds: a group with no loops plays its members once, and says which")
{
    /*  The ordinary group, and the round is still materialised. It costs one
        record and it is what makes every other case here readable: the order a
        group is going to play in is written down before it plays, whether or
        not anything chose it. */
    RoundRig rig;
    rig.setStandby (rig.groupId);
    rig.goAndSettle();

    const auto drawn = rig.rounds();
    REQUIRE (drawn.size() == 1u);
    CHECK (drawn[0] == std::vector<std::string> { rig.first, rig.second, rig.third });

    CHECK (rig.played() == std::vector<std::string> { rig.first, rig.second, rig.third });
    CHECK (rig.runs.find (rig.groupRun())->isFinished());
}

TEST_CASE ("rounds: loops plays the members again, and the count is of rounds")
{
    RoundRig rig;
    rig.setGroup ("loops", "3");
    rig.setStandby (rig.groupId);
    rig.goAndSettle (200);

    const auto drawn = rig.rounds();
    REQUIRE (drawn.size() == 3u);

    for (const auto& round : drawn)
        CHECK (round == std::vector<std::string> { rig.first, rig.second, rig.third });

    //  Nine cues, not three: the count is of ROUNDS (§3.6).
    CHECK (rig.played().size() == 9u);

    const auto* run = rig.runs.find (rig.groupRun());
    REQUIRE (run != nullptr);
    CHECK (run->iteration == 3);
    CHECK (run->iterations == 3);
    CHECK (run->isFinished());
}

TEST_CASE ("rounds: an infinite loop keeps going, and a boundary stop leaves it")
{
    /*  The ambience bed. Zero loops is for ever, and for ever has to be
        LEAVABLE without a cut - which is what the two graceful verbs are for:
        the scene reaches a boundary it was going to reach anyway and stops
        there. */
    RoundRig rig;
    rig.setGroup ("loops", "0");
    rig.setStandby (rig.groupId);
    rig.goAndSettle (200);

    const auto run = rig.groupRun();
    REQUIRE (! run.empty());
    CHECK_FALSE (rig.runs.find (run)->isFinished());
    CHECK (rig.rounds().size() > 3u);

    const auto roundsBefore = rig.rounds().size();

    CHECK (rig.submitAndTick ("run.stop", { osc::Value::string (run),
                                            osc::Value::string ("afterIteration") }).applied >= 1);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (run)->isFinished(); }));

    /*  It finished the round it was in and did not start another: the whole
        difference between this and `run.kill`, which would have cut it. */
    CHECK (rig.rounds().size() == roundsBefore);
}

TEST_CASE ("rounds: afterMember stops at the end of the one playing, not at the round's")
{
    RoundRig rig;
    rig.setGroup ("loops", "0");
    rig.setStandby (rig.groupId);

    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto run = rig.groupRun();
    const auto playedBefore = rig.played().size();

    CHECK (rig.submitAndTick ("run.stop", { osc::Value::string (run),
                                            osc::Value::string ("afterMember") }).applied >= 1);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (run)->isFinished(); }));

    /*  At most one more member than were already going: it stopped at the near
        boundary rather than finishing the round. */
    CHECK (rig.played().size() <= playedBefore + 1);
}

TEST_CASE ("rounds: shuffle draws a different order, and never repeats across a boundary")
{
    /*  §3.6's boundary constraint, which is the half of shuffling that is not
        obvious: a fresh draw is allowed to start with the member that just
        finished, and hearing the same ambience twice running is exactly what
        somebody asked for shuffling to avoid. */
    RoundRig rig;
    rig.setGroup ("selection", "shuffle");
    rig.setGroup ("loops", "6");
    rig.setStandby (rig.groupId);
    rig.goAndSettle (400);

    const auto drawn = rig.rounds();
    REQUIRE (drawn.size() == 6u);

    for (const auto& round : drawn)
    {
        //  Every member, once each: shuffling reorders, it does not select.
        auto sorted = round;
        std::sort (sorted.begin(), sorted.end());

        auto members = std::vector<std::string> { rig.first, rig.second, rig.third };
        std::sort (members.begin(), members.end());

        CHECK (sorted == members);
    }

    for (std::size_t i = 1; i < drawn.size(); ++i)
        CHECK (drawn[i].front() != drawn[i - 1].back());

    //  And it is a shuffle rather than the same order six times.
    const auto allSame = std::all_of (drawn.begin(), drawn.end(),
                                      [&drawn] (const std::vector<std::string>& round)
                                      {
                                          return round == drawn.front();
                                      });
    CHECK_FALSE (allSame);
}

TEST_CASE ("rounds: a seeded shuffle draws the SAME orders on every platform")
{
    /*  A GOLDEN, and it is not pedantry. The shuffle is written out in the
        Runner - SplitMix64 and Fisher-Yates, eight lines - rather than reached
        for in <random>, because that header's ENGINES are specified down to the
        bit and its DISTRIBUTIONS are not: `std::shuffle` with one seed gives
        different orders on different standard libraries.

        Which would mean a show rehearsed on this machine playing a different
        order in the theatre, and a fixture drawn here failing on the CI
        runners. This case is what would notice - it runs on three platforms and
        two locales, and the numbers below came off one of them. */
    RoundRig rig;
    rig.setGroup ("selection", "shuffle");
    rig.setGroup ("loops", "5");
    rig.setGroup ("seed", "7");
    rig.setStandby (rig.groupId);
    rig.goAndSettle (400);

    std::vector<std::vector<int>> byPosition;

    for (const auto& round : rig.rounds())
    {
        std::vector<int> positions;

        for (const auto& cueId : round)
            positions.push_back (cueId == rig.first ? 0 : cueId == rig.second ? 1 : 2);

        byPosition.push_back (std::move (positions));
    }

    const std::vector<std::vector<int>> expected { { 0, 1, 2 }, { 0, 1, 2 }, { 0, 2, 1 },
                                                   { 2, 1, 0 }, { 2, 1, 0 } };
    CHECK (byPosition == expected);
}

TEST_CASE ("rounds: a seed makes a shuffled group play the same order every night")
{
    /*  Which is how a shuffled scene gets rehearsed. Two runs of the same show
        with the same seed draw the same rounds; the seed is what a designer
        writes down after a night they liked. */
    const auto ordersFor = [] (const std::string& seed)
    {
        RoundRig rig;
        rig.setGroup ("selection", "shuffle");
        rig.setGroup ("loops", "4");
        rig.setGroup ("seed", seed);
        rig.setStandby (rig.groupId);
        rig.goAndSettle (300);

        //  By POSITION rather than by identifier: two rigs have different
        //  documents, so the cues are the same three members with other names.
        std::vector<std::vector<int>> out;

        for (const auto& round : rig.rounds())
        {
            std::vector<int> byIndex;

            for (const auto& cueId : round)
                byIndex.push_back (cueId == rig.first ? 0 : cueId == rig.second ? 1 : 2);

            out.push_back (std::move (byIndex));
        }

        return out;
    };

    CHECK (ordersFor ("12345") == ordersFor ("12345"));
    CHECK (ordersFor ("12345") != ordersFor ("999"));
}

TEST_CASE ("rounds: play N of M plays N, and a different N each round when shuffled")
{
    RoundRig rig;
    rig.setGroup ("selection", "shuffle");
    rig.setGroup ("play", "2");
    rig.setGroup ("loops", "5");
    rig.setStandby (rig.groupId);
    rig.goAndSettle (300);

    const auto drawn = rig.rounds();
    REQUIRE (drawn.size() == 5u);

    for (const auto& round : drawn)
        CHECK (round.size() == 2u);

    CHECK (rig.played().size() == 10u);
}

TEST_CASE ("rounds: a pruned member sits out the round, or the whole run")
{
    /*  What an operator does at 22:40 (§3.6). It is not an edit: the show is
        untouched, and tomorrow the cue is back - which is why it lives on the
        run and evaporates with it. */
    RoundRig rig;
    rig.setGroup ("loops", "3");
    rig.setStandby (rig.groupId);

    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.groupRun().empty(); }));

    const auto run = rig.groupRun();

    CHECK (rig.submitAndTick ("run.prune", { osc::Value::string (run),
                                             osc::Value::string (rig.second),
                                             osc::Value::string ("group") }).applied >= 1);

    const auto timesPlayed = [&rig] (const std::string& cueId)
    {
        const auto all = rig.played();
        return std::count (all.begin(), all.end(), cueId);
    };

    const auto atPrune = timesPlayed (rig.second);

    for (int n = 0; n < 250; ++n)
        rig.tickOnce();

    //  It never played again, while its neighbours played their three rounds.
    CHECK (timesPlayed (rig.second) == atPrune);
    CHECK (timesPlayed (rig.first) == 3);
    CHECK (timesPlayed (rig.third) == 3);

    //  And the document did not change: the cue is still in the group, enabled.
    CHECK (rig.document.getAttribute ("/godot/cue/" + rig.second + "/enabled") == "true");
    CHECK (rig.document.findById (rig.second).isValid());
}

TEST_CASE ("rounds: pruning every member completes the group rather than spinning")
{
    RoundRig rig;
    rig.setGroup ("loops", "0");
    rig.setStandby (rig.groupId);

    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.groupRun().empty(); }));

    const auto run = rig.groupRun();

    for (const auto& cueId : { rig.first, rig.second, rig.third })
        CHECK (rig.submitAndTick ("run.prune", { osc::Value::string (run),
                                                 osc::Value::string (cueId),
                                                 osc::Value::string ("group") }).applied >= 1);

    /*  §3.6: an emptied round completes the group. An infinite loop with
        nothing left to play would otherwise draw an empty round for ever. */
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (run)->isFinished(); }));
}

TEST_CASE ("rounds: an unpruned member is back from the NEXT round, not this one")
{
    RoundRig rig;
    rig.setGroup ("loops", "4");
    rig.setStandby (rig.groupId);

    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.groupRun().empty(); }));

    const auto run = rig.groupRun();

    CHECK (rig.submitAndTick ("run.prune", { osc::Value::string (run),
                                             osc::Value::string (rig.third),
                                             osc::Value::string ("group") }).applied >= 1);

    REQUIRE (rig.tickUntil ([&] { return rig.rounds().size() >= 2u; }));

    const auto timesPlayed = [&rig] (const std::string& cueId)
    {
        const auto all = rig.played();
        return std::count (all.begin(), all.end(), cueId);
    };

    //  Two rounds drawn and it has played in neither.
    CHECK (timesPlayed (rig.third) == 0);

    CHECK (rig.submitAndTick ("run.unprune", { osc::Value::string (run),
                                               osc::Value::string (rig.third) }).applied >= 1);

    for (int n = 0; n < 250; ++n)
        rig.tickOnce();

    /*  Back, and from the NEXT round: four rounds in all, so it plays in fewer
        than four of them - putting it into an order already drawn, and possibly
        already passed, would be a cue arriving somewhere nobody chose. */
    CHECK (timesPlayed (rig.third) > 0);
    CHECK (timesPlayed (rig.third) < 4);
    CHECK (timesPlayed (rig.first) == 4);
}

TEST_CASE ("rounds: a manual group loops, and the pointer wraps rather than leaving")
{
    /*  The half of looping that belongs to the operator. §3.6 makes them the
        parent of a manual group, so the group only advances when they press GO
        - and if the pointer left on the last member of round one, their next
        press would fire whatever follows a group that has two thirds of itself
        still to play.

        The document cannot answer this on its own: it says the group loops
        three times, and only the RUN knows which round it is on. This is the
        one question the cursor asks about what is running. */
    ManualRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.groupId + "/loops", "2").ok);

    rig.setStandby (rig.first);

    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.standby() == rig.second);

    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.standby() == rig.third);

    //  The last member of round one, and the pointer WRAPS instead of leaving.
    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.standby() == rig.first);

    /*  Round two, which only begins once round one's members are done - so the
        presses are spaced the way an operator's are, waiting for the group to
        get there rather than racing it. */
    REQUIRE (rig.tickUntil ([&]
    {
        const auto* run = rig.runs.find (rig.runOf (rig.groupId));
        return run != nullptr && run->iteration == 2;
    }));

    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.standby() == rig.second);

    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.standby() == rig.third);

    //  And now it leaves: there is no round three.
    CHECK (rig.submitAndTick ("go").applied >= 1);

    CHECK (rig.standby() != rig.first);
    CHECK (rig.standby() != rig.second);
    CHECK (rig.standby() != rig.third);
}

TEST_CASE ("rounds: a manual group ignores shuffle, because the operator is choosing")
{
    /*  The pointer walks the list in document order and cannot be made to jump
        about (§3.5), so a shuffled manual group would have the group finishing
        at whichever member the draw put last - at a moment the operator has no
        way to see coming. Ignored rather than refused at load, because the
        setting means something the moment somebody makes the group automatic. */
    ManualRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.groupId + "/selection",
                                        "shuffle").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.groupId + "/play", "2").ok);

    rig.setStandby (rig.first);
    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    std::vector<std::string> round;

    for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
        if (record.command == "run.round")
            for (std::size_t i = 2; i < record.args.size(); ++i)
                round.push_back (record.args[i].getString());

    CHECK (round == std::vector<std::string> { rig.first, rig.second, rig.third });
}
