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
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/tree/TreeSnapshot.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/FxRows.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/cue/FadeJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/Solver.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/FadePoints.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/Replay.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
            lastArmEq = request.eq;
            lastArmFx = request.fx;
        }

        /** The EQ the last arm carried (Phase 9a); `completeArms` clears `arms`. */
        audio::EqSettings lastArmEq;
        std::vector<cue::FxSetting> lastArmFx;

        /*  A sounding cue's width at one insert, changed under it (2026-09-26). */
        struct FxShape { int track, slot, feed, back; };
        std::vector<FxShape> fxShapes;

        void setFxShape (int track, int slot, int feed, int back) override
        {
            fxShapes.push_back ({ track, slot, feed, back });
        }

        int slotCount() const override             { return slots; }
        int sampleRate() const override            { return rate; }

        bool launchAtSample (int track, int slot, std::int64_t sample) override
        {
            launches.push_back ({ track, sample });
            launchedSlots.push_back (slot);
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            stopped.push_back (track);
            return true;
        }

        /*  A KILL, kept beside the stop it is here (namespace draft §23.6): on
            a real voice it also empties the chain, and what a case can see of
            that is the Runner asking for it. */
        bool kill (int track) override
        {
            kills.push_back (track);
            return stop (track);
        }

        /*  A DOUBLE ESC'S SWEEP, with the voices it was told the press leaves
            ready (namespace draft §23.6, FY), one entry a press. */
        void resetEffects (const std::vector<int>& readyTracks) override
        {
            sweeps.push_back (readyTracks);
        }

        bool stopAtSample (int track, int slot, std::int64_t sample) override
        {
            stopsAt.push_back ({ track, sample });
            stoppedSlots.push_back (slot);
            return true;
        }

        void setLevelDb (int track, double levelDb) override
        {
            levels.push_back ({ track, levelDb });
        }

        /*  A voice's speed (namespace draft §22.4): every breakpoint placed, in
            the order it was placed. */
        struct RatePoint { int track; std::int64_t sample; double speed; };
        std::vector<RatePoint> ratePoints;
        double speedLimit = 20.0;

        bool placeRate (int track, std::int64_t sample, double speed) override
        {
            ratePoints.push_back ({ track, sample, speed });
            return true;
        }

        double stretchSpeedLimit() const override { return speedLimit; }

        /*  Kept per track and COUNTED, because the two things worth asserting
            about a live routing change are what it became and that it did not
            also disturb the level. */
        void setRouting (int track, const std::vector<cue::Coefficient>& coefficients) override
        {
            routings[track] = coefficients;
            ++routingPushes;
        }

        std::map<int, std::vector<cue::Coefficient>> routings;
        int routingPushes = 0;

        /*  The EQ a voice was last given while sounding (Phase 9a), counted
            for the same reason the routing is: an edit must reach it once,
            and a quiet tick must not reach it at all. */
        void setEq (int track, const audio::EqSettings& settings) override
        {
            eqs[track] = settings;
            ++eqPushes;
        }

        std::map<int, audio::EqSettings> eqs;
        int eqPushes = 0;

        /*  The inserts, for the EQ's reason (Phase 9a, PR 9a.8): what was
            switched and what moved, in order, so a test can say an edit
            reached the voice as exactly the pushes it should. */
        struct FxEnable { int track; int slot; bool enabled; };
        struct FxValue { int track; int slot; int parameter; float value; };

        void setFxEnabled (int track, int slot, bool enabled) override
        {
            fxEnables.push_back ({ track, slot, enabled });
        }

        void setFxParameter (int track, int slot, int parameter, float value) override
        {
            fxValues.push_back ({ track, slot, parameter, value });
        }

        std::vector<FxEnable> fxEnables;
        std::vector<FxValue> fxValues;

        /*  A whole state asked for after the arm (2026-09-25), in order: what
            the voice loads before the cue may launch. Empty path = the preset's own. */
        struct FxState { int track; int slot; std::string path; };
        std::vector<FxState> fxStates;

        void requestFxState (int track, int slot, const std::string& path) override
        {
            fxStates.push_back ({ track, slot, path });
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

        /*  One slot a track, which is a show whose widest media cue has one
            range or none - every fixture there is, until a case says otherwise.
            A test that wants to be refused for `no-slot` sets it. */
        int slots = 1;

        /*  48 kHz, so that a range of N seconds is N times this many samples
            and a test can write the number it means. */
        int rate = 48000;

        std::int64_t samples = 0;

        std::vector<cue::ArmRequest> arms;
        std::vector<std::pair<int, std::int64_t>> launches;

        /** Which slot each launch and each placed stop was aimed at. */
        std::vector<int> launchedSlots;
        std::vector<int> stoppedSlots;
        std::set<int> playing;
        std::set<int> ready;
        std::vector<int> stopped;
        std::vector<int> kills;
        std::vector<std::vector<int>> sweeps;
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

            /*  A SCRIPT PRESSES GO, NOT A HAND: these cases press it as fast as
                the ticks come, to see what the scheduler does with each press,
                and a show that says nothing now refuses a GO inside half a
                second of the last one (2026-09-28). The cases about that window
                set it back themselves. */
            document.setAttribute ("/godot/list/goDebounce", "0");
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

        /** The first run of a cue, or empty. */
        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
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

TEST_CASE ("go: at the end of a list it is applied and clears the pointer")
{
    /*  FIRING THE LAST CUE LEAVES THE POINTER NOWHERE (author, 2026-09-18),
        which is the resting state a list carries before anybody arms it (§3.5,
        §4.6) - so a show that has been run through ends where it began.

        The pointer used to stay on the cue that had just gone, which meant a
        second GO FIRED IT AGAIN. Now the second GO is applied and does
        nothing, which is what the handler's own comment always claimed.

        THE ARROWS ARE UNCHANGED: `standby.next` off the end still stays put,
        because looking is not firing. */
    Rig rig;
    rig.setStandby (rig.memoId);            // the last cue

    const auto outcome = rig.submitAndTick ("go");

    CHECK (outcome.applied == 1);
    CHECK (rig.standby().empty());

    /*  And a second press has nothing to fire rather than the same cue again -
        which is the whole of what clearing buys. The applied count is not
        asserted, because the memo's own run ends in that tick and the engine
        submits for itself; what matters is that no SECOND run was made. */
    rig.submitAndTick ("go");

    CHECK (rig.standby().empty());
    CHECK (rig.runs.all().size() == 1u);
}

TEST_CASE ("standby.next: at the end of a list it still stays put, because looking is not firing")
{
    /*  THE OTHER HALF OF THE RULE ABOVE, and the reason a GO's walk is a
        second function rather than an edit to the arrows': a pointer that
        vanished under a keypress would be the machine taking somebody's place
        away while they were reading. Firing off the end is the show being
        over; walking off it is not. */
    Rig rig;
    rig.setStandby (rig.memoId);            // the last cue

    CHECK (rig.submitAndTick ("standby.next").applied == 1);
    CHECK (rig.standby() == rig.memoId);

    //  And nothing was fired by looking.
    CHECK (rig.runs.all().empty());
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

        /*  What the file is, which is a fact the cue carries rather than one
            the disk is asked for: the file travels between machines and may be
            absent tonight, and a replay must not depend on it at all. */
        void setMedia (const std::string& cueId, int channels, bool fold = false)
        {
            auto cue = document.findById (cueId);
            cue.setProperty (juce::Identifier ("channels"), channels, nullptr);

            if (fold)
                cue.setProperty (juce::Identifier ("stereoToMono"), true, nullptr);
        }

        void aimAt (const std::string& cueId, const std::string& busId)
        {
            document.findById (cueId)
                .setProperty (juce::Identifier ("directOut"), juce::String (busId), nullptr);
        }

        std::string addSend (const std::string& cueId, const std::string& busId, double levelDb)
        {
            auto cue = document.findById (cueId);

            juce::ValueTree send { "Send" };
            const auto id = runIds.generate();

            send.setProperty (juce::Identifier ("id"), juce::String (id), nullptr);
            send.setProperty (juce::Identifier ("bus"), juce::String (busId), nullptr);
            send.setProperty (juce::Identifier ("level"), levelDb, nullptr);

            cue.appendChild (send, nullptr);
            return id;
        }

        /** "input>output@gain", in emission order, for a readable assertion. */
        std::string spreadOf (const std::string& cueId, std::string& problem)
        {
            std::string out;

            for (const auto& one : routingOf (cueId, problem))
            {
                if (! out.empty()) out += ' ';
                out += std::to_string (one.input) + ">" + std::to_string (one.output) + "@"
                         + juce::String (one.gain, 3).toStdString();
            }

            return out;
        }

        /*  Plays the media cue and leaves it sounding, which is the state the
            live-routing pass is about. The same sequence `FadeRig::startMedia`
            uses; it is spelled again here rather than shared because the two
            rigs declare different shows and the shape is four lines. */
        std::string play()
        {
            submitAndTick ("cue.fire", { osc::Value::string (mediaId) });
            audio.completeArms (engine);
            tickOnce();
            tickOnce();

            const auto id = runs.all().front().id;
            audio.playing.insert (runs.find (id)->track);
            tickOnce();

            return id;
        }

        /** The same spelling, for what was actually pushed at a playing track. */
        std::string pushedOn (int track) const
        {
            std::string out;

            const auto found = audio.routings.find (track);

            if (found == audio.routings.end())
                return out;

            for (const auto& one : found->second)
            {
                if (! out.empty()) out += ' ';
                out += std::to_string (one.input) + ">" + std::to_string (one.output) + "@"
                         + juce::String (one.gain, 3).toStdString();
            }

            return out;
        }

        std::vector<cue::Coefficient> routingOf (const std::string& cueId,
                                                 std::string& problem, int chainChannels = 0)
        {
            return runner.resolveRouting (document.findById (cueId), 2, problem, chainChannels);
        }

        /** The spelling above, for a cue its inserts made `chainChannels` wide. */
        std::string widenedSpreadOf (const std::string& cueId, int chainChannels, std::string& problem)
        {
            std::string out;

            for (const auto& one : routingOf (cueId, problem, chainChannels))
            {
                if (! out.empty()) out += ' ';
                out += std::to_string (one.input) + ">" + std::to_string (one.output) + "@"
                         + juce::String (one.gain, 3).toStdString();
            }

            return out;
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

TEST_CASE ("direct out: a cue's channels spread across the output it is aimed at")
{
    /*  A Route says its coefficients one by one, because a designer placing a
        source among twelve processor inputs means something no rule could
        guess. A DIRECT OUT is the ordinary case and derives them, and this is
        the whole of the derivation. Foldback starts at hardware 4, so every
        output below is 4 or 5. */
    RoutedRig rig;
    std::string problem;

    SUBCASE ("as wide as the output, channel to channel")
    {
        rig.setMedia (rig.mediaId, 2);
        rig.aimAt (rig.mediaId, rig.foldback);
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>4@1.000 1>5@1.000");
        CHECK (problem.empty());
    }

    SUBCASE ("a mono cue feeds every channel of a stereo out")
    {
        rig.setMedia (rig.mediaId, 1);
        rig.aimAt (rig.mediaId, rig.foldback);
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>4@1.000 0>5@1.000");
        CHECK (problem.empty());
    }

    SUBCASE ("a fold is a downmix somebody asked for: both sides at half")
    {
        /*  BOTH ROWS, which is what makes it a fold rather than a left
            channel at -6 dB: folding is summing the two sides, so each needs
            its own row into the destination. */
        rig.setMedia (rig.mediaId, 2, true);
        rig.aimAt (rig.mediaId, rig.addBus ("Voice", 8, 1));
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>8@0.500 1>8@0.500");
        CHECK (problem.empty());
    }

    SUBCASE ("and a silent one is refused rather than performed")
    {
        /*  PRD 3.9b: width is explicit and a quiet downmix is not on offer.
            The toggle above is the way to ask for one. */
        rig.setMedia (rig.mediaId, 2);
        rig.aimAt (rig.mediaId, rig.addBus ("Voice", 8, 1));
        rig.routingOf (rig.mediaId, problem);
        CHECK (problem == "the cue is wider than its direct out");
    }

    SUBCASE ("a cue whose channel count nobody has written cannot be placed")
    {
        rig.aimAt (rig.mediaId, rig.foldback);
        rig.routingOf (rig.mediaId, problem);
        CHECK (problem == "the cue's channel count is not known");
    }

    SUBCASE ("and an output that has gone fails the run rather than the load")
    {
        rig.setMedia (rig.mediaId, 2);
        rig.aimAt (rig.mediaId, "NOSUCHBS");
        rig.routingOf (rig.mediaId, problem);
        CHECK (problem == "the cue's direct out is no bus of this show");
    }
}

TEST_CASE ("sends: a mix channel is a destination with a level, and silence costs nothing")
{
    RoutedRig rig;
    std::string problem;

    rig.setMedia (rig.mediaId, 2);

    SUBCASE ("the level scales the same spread a direct out would get")
    {
        rig.addSend (rig.mediaId, rig.foldback, -6.0);
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>4@0.501 1>5@0.501");
        CHECK (problem.empty());
    }

    SUBCASE ("silence contributes no coefficient at all")
    {
        /*  -120 dB is how this document spells silence everywhere else, and a
            track with nothing to add should cost nothing to mix. */
        rig.addSend (rig.mediaId, rig.foldback, -120.0);
        CHECK (rig.spreadOf (rig.mediaId, problem).empty());
        CHECK (problem.empty());
    }

    SUBCASE ("a send switched off contributes nothing, and keeps its level for when it comes back")
    {
        /*  send/on (author, 2026-09-25): the press of a rotary on a surface's
            Send page. */
        const auto send = rig.addSend (rig.mediaId, rig.foldback, -6.0);
        REQUIRE (rig.document.setAttribute ("/godot/send/" + send + "/on", "false").ok);

        CHECK (rig.spreadOf (rig.mediaId, problem).empty());
        CHECK (problem.empty());
        CHECK (rig.document.getAttribute ("/godot/send/" + send + "/level").value_or ("?") == "-6");

        REQUIRE (rig.document.setAttribute ("/godot/send/" + send + "/on", "true").ok);
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>4@0.501 1>5@0.501");
    }

    SUBCASE ("a cue may hold a direct out and several sends at once")
    {
        /*  PRD 3.9b: a cue's destinations are a LIST and not a choice - a
            source into the processor plus a stereo feed to foldback is
            ordinary, and so is a direct out plus a mix. */
        rig.aimAt (rig.mediaId, rig.main);
        rig.addSend (rig.mediaId, rig.foldback, 0.0);
        CHECK (rig.spreadOf (rig.mediaId, problem)
                 == "0>0@1.000 1>1@1.000 0>4@1.000 1>5@1.000");
        CHECK (problem.empty());
    }

    SUBCASE ("and a send to an output that has gone fails the run")
    {
        rig.addSend (rig.mediaId, "NOSUCHBS", 0.0);
        rig.routingOf (rig.mediaId, problem);
        CHECK (problem == "send names no bus of this show");
    }
}

TEST_CASE ("sends: a fader moved while the cue is sounding is heard, and moves no level")
{
    /*  The author, 2026-09-22, asked for a send mixer, and a mixer whose
        faders only take effect at the next GO is a mixer nobody can mix on.

        THE LEVEL IS THE THING THAT MUST NOT MOVE. An arm snaps every smoother
        to its target because the voice is silent; doing that here would take a
        fade that is halfway down and put it back wherever the document says,
        in the middle of the sound. So the coefficients go through their own
        door and the level goes through none. */
    RoutedRig rig;

    rig.setMedia (rig.mediaId, 2);
    rig.aimAt (rig.mediaId, rig.main);

    const auto run = rig.play();
    REQUIRE (rig.runs.find (run) != nullptr);

    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);

    /*  NOTHING HAS BEEN PUSHED THROUGH THIS DOOR YET, and that is the design
        rather than a gap: a cue that has just been armed got its coefficients
        from the arm, which snapped them while the voice was silent. This pass
        exists only for what changes AFTER that. */
    rig.tickOnce();
    CHECK (rig.pushedOn (track).empty());

    const auto levelWrites = rig.audio.levels.size();

    //  A send raised to unity on the cue that is already playing.
    const auto send = rig.addSend (rig.mediaId, rig.foldback, 0.0);
    juce::ignoreUnused (send);

    rig.tickOnce();

    CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>4@1.000 1>5@1.000");

    /*  AND NOTHING TOUCHED THE LEVEL. `applyLevels` writes only when the
        number changes, so a routing edit that disturbed it would show up here
        as a write that should not exist. */
    CHECK (rig.audio.levels.size() == levelWrites);

    SUBCASE ("and a tick with nothing edited pushes nothing at all")
    {
        /*  Gated on the show's revision, so the ordinary case - fifty ticks a
            second with nobody typing - costs one comparison. */
        const auto pushes = rig.audio.routingPushes;

        for (int i = 0; i < 10; ++i)
            rig.tickOnce();

        CHECK (rig.audio.routingPushes == pushes);
    }
}

TEST_CASE ("feed: a slot and a bus written without their default widths still route")
{
    /*  A SAVED SHOW IS WRITTEN WITHOUT ITS DEFAULTS. The canonical writer
        omits an attribute that equals its default - a slot one channel wide
        carries no `width` at all, a bus starting at channel nought no
        `firstChannel` - and the document's getter hands the default back,
        which is the round trip the omission depends on. `resolveRouting` read
        those four attributes straight off the tree, where an absent width is
        nought, so every feed in a show that had been saved and reopened was
        refused `bad-route`. The author found it on a recovered show, which is
        the same writer (2026-09-18). This is that show, made by hand: the
        attributes the writer would have dropped are never set. */
    RoutedRig rig;

    const auto wide = rig.addBus ("WFS send", 8, 12);

    const auto mountEdit = rig.document.createMount ("/wfs", "namespaces/wfs.json");
    REQUIRE (mountEdit.ok);

    const auto slot = rig.document.createSlot (mountEdit.id, "/wfs/input/1");
    REQUIRE (slot.ok);

    //  The bus, and nothing else: no width, no first channel - as written.
    rig.document.findById (slot.id).setProperty (juce::Identifier ("bus"), juce::String (wide), nullptr);

    const auto feed = rig.document.createFeed (rig.mediaId, slot.id);
    REQUIRE (feed.ok);
    rig.document.findById (feed.id).setProperty (juce::Identifier ("gains"), "1", nullptr);

    std::string problem;
    auto routing = rig.routingOf (rig.mediaId, problem);

    INFO (problem);
    CHECK (problem.empty());
    REQUIRE (routing.size() == 1u);

    //  One channel wide at the first input of the bus: the two defaults, applied.
    CHECK (routing[0].output == 8);

    /*  AND A BUS THE SAME WAY. Its width is the one attribute a bus always
        carries in the fixtures, so the case has to be made: a one-wide bus
        whose width the writer dropped. */
    const auto mono = rig.addBus ("Mono send", 20, 1);
    rig.document.findById (mono).removeProperty (juce::Identifier ("width"), nullptr);

    const auto route = rig.document.createRoute (rig.mediaId, mono);
    REQUIRE (route.ok);
    rig.document.findById (route.id).setProperty (juce::Identifier ("gains"), "1", nullptr);

    problem.clear();
    routing = rig.routingOf (rig.mediaId, problem);

    INFO (problem);
    CHECK (problem.empty());
    REQUIRE (routing.size() == 2u);
    CHECK (routing[1].output == 20);
}

TEST_CASE ("feed: a slot is a routing and a claim in one object")
{
    /*  PR 4.2. A `Route` sends a cue to a bus; a `Feed` sends it to a PROCESSOR
        INPUT, which means to that slot's own channels of the slot's bus - the
        same coefficients landing at one more offset - and claims the slot.

        THE TWO HALVES ARE ONE OBJECT DELIBERATELY. The audio reaches the
        processor through an ordinary bus, and the claim that keeps a second cue
        out of the position, the trajectory and the LFO state behind that input
        (§3.9b) is the same row that carries it there. Two separate objects
        would let a show route a cue somewhere it had not claimed. The claim
        itself is PR 4.3's; that this routes is here.

        `firstChannel` on the slot is the attribute that makes one wide send
        ordinary: a rig feeding a twelve-input processor has ONE twelve-channel
        bus, and the third input is channel two of it. Exactly the bus's own
        idea one level down. */
    RoutedRig rig;

    const auto wide = rig.addBus ("WFS send", 8, 12);

    const auto mountEdit = rig.document.createMount ("/wfs", "namespaces/wfs.json");
    REQUIRE (mountEdit.ok);

    const auto slot = rig.document.createSlot (mountEdit.id, "/wfs/input/3");
    REQUIRE (slot.ok);

    auto slotNode = rig.document.findById (slot.id);
    slotNode.setProperty (juce::Identifier ("bus"), juce::String (wide), nullptr);
    slotNode.setProperty (juce::Identifier ("width"), 1, nullptr);
    slotNode.setProperty (juce::Identifier ("firstChannel"), 2, nullptr);

    const auto feed = rig.document.createFeed (rig.mediaId, slot.id);
    REQUIRE (feed.ok);

    rig.document.findById (feed.id)
       .setProperty (juce::Identifier ("gains"), "1", nullptr);

    std::string problem;
    auto routing = rig.routingOf (rig.mediaId, problem);

    INFO (problem);
    CHECK (problem.empty());
    REQUIRE (routing.size() == 1u);

    /*  Hardware channel 10: the bus starts at 8 and this input is two channels
        into it. The cue said neither number. */
    CHECK (routing[0].input == 0);
    CHECK (routing[0].output == 10);
    CHECK (routing[0].gain == doctest::Approx (1.0f));

    /*  AND IT SITS BESIDE A ROUTE RATHER THAN INSTEAD OF ONE (§3.9b): "a source
        into WFS plus a stereo feed to foldback is ordinary". */
    rig.addRoute (rig.mediaId, rig.foldback, "1 0");

    routing = rig.routingOf (rig.mediaId, problem);

    INFO (problem);
    CHECK (problem.empty());
    REQUIRE (routing.size() == 2u);

    std::set<int> outputs;

    for (const auto& coefficient : routing)
        outputs.insert (coefficient.output);

    CHECK (outputs == std::set<int> { 4, 10 });
}

//==============================================================================
/*  CLAIMS: the half of a slot that is not a routing.

    PRD §3.9e's two shared rules, and they are the whole of what the four slot
    kinds have in common. A claim on a busy slot is neither a failure nor a
    race: it lands when the holder's run ends, and the claimant shows *pending*
    meanwhile. And the failure policy is the SLOT'S rather than the claim's - a
    voice fails at entry, a processor input waits, a rack channel degrades.

    There is no record of any of it, and that is a conclusion rather than an
    omission: a claim is issued in an arm, released in the handler that ends the
    run, and handed to the head of a queue in that same handler. Every step is
    already inside a command a replay has, and handing a slot to the head of a
    queue decides nothing.
*/
namespace
{
    /*  The routed rig, plus a processor input and a second media cue to fight
        over it with. */
    struct ClaimRig : RoutedRig
    {
        ClaimRig()
        {
            wide = addBus ("WFS send", 8, 12);

            const auto mountEdit = document.createMount ("/wfs", "namespaces/wfs.json");
            REQUIRE (mountEdit.ok);

            slotId = document.createSlot (mountEdit.id, "/wfs/input/3").id;
            REQUIRE_FALSE (slotId.empty());

            auto slot = document.findById (slotId);
            slot.setProperty (juce::Identifier ("bus"), juce::String (wide), nullptr);
            slot.setProperty (juce::Identifier ("width"), 1, nullptr);
            slot.setProperty (juce::Identifier ("firstChannel"), 2, nullptr);

            secondId = document.createCue (listId, 3, "media", "Second").id;
            document.setAttribute ("/godot/cue/" + secondId + "/file", "thunder.wav");

            feedTo (mediaId);
            feedTo (secondId);
        }

        void feedTo (const std::string& cueId)
        {
            const auto feed = document.createFeed (cueId, slotId);
            REQUIRE (feed.ok);

            document.findById (feed.id)
                    .setProperty (juce::Identifier ("gains"), "1", nullptr);
        }

        /** Arms whatever standby is on, the way the pointer does. */
        std::string armAt (const std::string& cueId)
        {
            setStandby (cueId);
            audio.completeArms (engine);
            tickOnce();

            const auto* live = runs.liveRunOf (cueId);
            return live != nullptr ? live->id : std::string {};
        }

        /*  A HOLDER THAT IS ACTUALLY SOUNDING, which is what a claim has to
            wait for and what an arm at standby is not.

            The pointer moving off a media cue now gives its voice and its slots
            back - a cue nobody fired holding a processor input for the rest of
            the show was the leak PR 4.11's driver found - so a case that needs
            a slot HELD while the pointer is somewhere else has to fire the cue
            rather than park on it and walk away. Which is the honest scenario
            anyway: what a second cue waits for is a first cue that is playing.
        */
        std::string playing (const std::string& cueId)
        {
            submitAndTick ("cue.fire", { osc::Value::string (cueId) });
            audio.completeArms (engine);
            tickOnce();
            tickOnce();

            const auto* live = runs.liveRunOf (cueId);
            return live != nullptr ? live->id : std::string {};
        }

        std::string wide, slotId, secondId;
    };
}

TEST_CASE ("claim: a feed takes its slot at the arm, and gives it back at the end")
{
    ClaimRig rig;

    const auto first = rig.armAt (rig.mediaId);
    REQUIRE_FALSE (first.empty());

    /*  HELD FROM THE ARM, not from the launch: the whole point of arming ahead
        is that everything scarce is settled before the operator's hand comes
        down. */
    CHECK (rig.runs.find (first)->claims == std::vector<std::string> { rig.slotId });
    CHECK (rig.runs.find (first)->pending.empty());

    REQUIRE (rig.runs.holderOf (rig.slotId) != nullptr);
    CHECK (rig.runs.holderOf (rig.slotId)->id == first);

    /*  And back at the end, which is the same moment the voice comes back. */
    rig.submitAndTick ("run.kill", { osc::Value::string (first) });
    rig.tickOnce();

    REQUIRE (rig.runs.find (first)->isFinished());
    CHECK (rig.runs.find (first)->claims.empty());
    CHECK (rig.runs.holderOf (rig.slotId) == nullptr);
}

TEST_CASE ("claim: a second cue waits, in words, and does not sound meanwhile")
{
    /*  §3.9e: "A claim on a busy slot is neither a failure nor a race: it lands
        when the holder's run ends. The claimant shows *pending* - in words,
        never colour alone - and GO has already returned." */
    ClaimRig rig;

    //  FIRED RATHER THAN PARKED ON: the holder has to be a cue that is going,
    //  because a cue merely armed at the pointer lets go the moment the pointer
    //  moves - see `playing` above.
    const auto first = rig.playing (rig.mediaId);
    REQUIRE_FALSE (first.empty());

    rig.setStandby (rig.secondId);
    CHECK (rig.submitAndTick ("go").applied == 1);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto* second = rig.runs.liveRunOf (rig.secondId);
    REQUIRE (second != nullptr);

    /*  It has the slot in `pending` and not in `claims`, and the first still
        holds it. */
    CHECK (second->pending == std::vector<std::string> { rig.slotId });
    CHECK (second->claims.empty());
    CHECK (rig.runs.holderOf (rig.slotId)->id == first);

    /*  AND IT MAKES NO SOUND. A pending claim holds the whole cue: half a cue
        is not a cue, and a cue sent into a slot somebody else holds is the
        fighting the claim exists to prevent. */
    const auto launchesBefore = rig.audio.launches.size();

    for (int n = 0; n < 8; ++n)
        rig.tickOnce();

    CHECK (rig.audio.launches.size() == launchesBefore);

    /*  THE HOLDER ENDS, AND IT LANDS - in the handler that ended the run,
        without a record of its own. */
    rig.submitAndTick ("run.kill", { osc::Value::string (first) });
    rig.tickOnce();

    const auto secondId = rig.runs.liveRunOf (rig.secondId)->id;

    CHECK (rig.runs.find (secondId)->pending.empty());
    CHECK (rig.runs.find (secondId)->claims == std::vector<std::string> { rig.slotId });
    CHECK (rig.runs.holderOf (rig.slotId)->id == secondId);

    /*  And now it can sound. */
    for (int n = 0; n < 8; ++n)
        rig.tickOnce();

    CHECK (rig.audio.launches.size() > launchesBefore);
}

TEST_CASE ("claim: a run that FAILS gives its slots back, and nothing sends run.ended")
{
    /*  The exit a reading misses. `run.failed` sets `failed` and `endedAtTick`
        and sends no `run.ended` at all, so a media cue that dies after taking
        its claims would hold them for the session - and the symptom would
        arrive on a later cue, as a wait that never ends. */
    ClaimRig rig;

    const auto first = rig.armAt (rig.mediaId);
    REQUIRE_FALSE (first.empty());
    REQUIRE (rig.runs.holderOf (rig.slotId) != nullptr);

    rig.submitAndTick ("run.failed", { osc::Value::string (first),
                                       osc::Value::string (cue::runError::mediaMissing) });

    CHECK (rig.runs.find (first)->state == cue::runState::failed);
    CHECK (rig.runs.find (first)->claims.empty());
    CHECK (rig.runs.holderOf (rig.slotId) == nullptr);
}

TEST_CASE ("claim: with no audio side, a cue armed twice does not wait on itself")
{
    /*  SUSPECTED IN PR 5.6 (namespace §14.5), AND THIS IS THE TEST THAT WAS
        OWED BEFORE ANY FIX. With no audio side a run's track stays -1, so a
        cue with a pre-wait is armed when it is entered and again when the wait
        elapses and the fire path finds no track. The second arm reaches
        `claimSlotsFor`, which asks `holderOf` - and the holder of every slot
        the run took at its first arm is the run itself. Read as busy, that
        would put the run in its own queue for a Feed, `pending` on a slot it
        holds, and warn `no-channel` against itself for an Insert.

        That configuration is `wfg replay`'s and every `wfg serve` without
        `--hosted` - the black-box drivers, a laptop with no interface - while
        a hosted session reserves a track at the first arm and never arms
        twice. The two would disagree about what the slot rows say. */
    ClaimRig rig;
    rig.runner.setPlayer (nullptr);

    const auto channel = rig.document.createRackChannel ("mono");
    REQUIRE (channel.ok);
    REQUIRE (rig.document.createInsert (rig.mediaId, channel.id).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/preWait", "0.1").ok);

    rig.submitAndTick ("cue.fire", { osc::Value::string (rig.mediaId) });

    const auto* entered = rig.runs.liveRunOf (rig.mediaId);
    REQUIRE (entered != nullptr);
    REQUIRE (entered->state == std::string (cue::runState::waiting));

    const auto id = entered->id;
    REQUIRE (rig.runs.find (id)->claims.size() == 2u);

    /*  THE SECOND ARM IS SEEN TO HAPPEN, as PR 5.6's own case insists: the run
        leaves `waiting`, which is the path that arms again. */
    bool left = false;

    for (int n = 0; n < 50 && ! left; ++n)
    {
        rig.tickOnce();
        left = rig.runs.find (id)->state != std::string (cue::runState::waiting);
    }

    REQUIRE (left);

    /*  The same run, still holding both, waiting on neither, and warning of
        nothing: a slot it holds is not busy to it. */
    CHECK (rig.runs.find (id)->claims.size() == 2u);
    CHECK (rig.runs.find (id)->pending.empty());
    CHECK (rig.runs.find (id)->warning.empty());
    CHECK (rig.runs.waitersFor (rig.slotId).empty());
}

TEST_CASE ("claim: with no audio side, a cue armed twice behind another waits in the queue once")
{
    /*  The other half of the same double arm: a slot somebody else holds.
        The first arm queues the run, and the second must not queue it again -
        a run in the queue twice is one the head of the queue would be handed
        twice. */
    ClaimRig rig;
    rig.runner.setPlayer (nullptr);

    //  With no audio side nothing launches, so a fired cue stays armed and
    //  holds its slot for as long as the case needs it to.
    rig.submitAndTick ("cue.fire", { osc::Value::string (rig.mediaId) });

    const auto* holder = rig.runs.liveRunOf (rig.mediaId);
    REQUIRE (holder != nullptr);
    REQUIRE (rig.runs.holderOf (rig.slotId) == holder);

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.secondId + "/preWait", "0.1").ok);
    rig.submitAndTick ("cue.fire", { osc::Value::string (rig.secondId) });

    const auto* entered = rig.runs.liveRunOf (rig.secondId);
    REQUIRE (entered != nullptr);

    const auto id = entered->id;
    REQUIRE (rig.runs.find (id)->pending == std::vector<std::string> { rig.slotId });

    bool left = false;

    for (int n = 0; n < 50 && ! left; ++n)
    {
        rig.tickOnce();
        left = rig.runs.find (id)->state != std::string (cue::runState::waiting);
    }

    REQUIRE (left);

    CHECK (rig.runs.find (id)->pending == std::vector<std::string> { rig.slotId });
    CHECK (rig.runs.waitersFor (rig.slotId).size() == 1u);
}

TEST_CASE ("claim: a rack channel degrades rather than waits")
{
    /*  §3.9e gives each slot kind a failure policy of its own, and the rack's is
        DEGRADE: the cue plays dry and says so. A scene that stopped because a
        reverb was busy would be worse than a dry scene, and §3.9c's edit-time
        analysis is what keeps it from happening on the night at all. */
    ClaimRig rig;

    const auto channel = rig.document.createRackChannel ("mono");
    REQUIRE (channel.ok);

    for (const auto& cueId : { rig.mediaId, rig.secondId })
    {
        const auto insert = rig.document.createInsert (cueId, channel.id);
        REQUIRE (insert.ok);
    }

    const auto first = rig.playing (rig.mediaId);
    REQUIRE_FALSE (first.empty());
    CHECK (rig.runs.find (first)->warning.empty());
    REQUIRE (rig.runs.holderOf (channel.id) != nullptr);

    /*  The second cue finds it busy. It does NOT wait - the channel is not in
        its pending list - and it carries a warning saying what it lost. */
    rig.setStandby (rig.secondId);
    CHECK (rig.submitAndTick ("go").applied == 1);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto* second = rig.runs.liveRunOf (rig.secondId);
    REQUIRE (second != nullptr);

    CHECK (second->warning == cue::runWarning::noChannel);
    CHECK (std::find (second->pending.begin(), second->pending.end(), channel.id)
             == second->pending.end());

    /*  A WARNING IS NOT AN ERROR. The run is going: it did something smaller
        than it meant to, and reporting that as a failure would either stop the
        scene or teach an operator to ignore the state that means stopped. */
    CHECK (second->error.empty());
    CHECK_FALSE (second->isFinished());
}

TEST_CASE ("claim: a shared rack channel is declared and never claimed")
{
    /*  §3.9e: a reverb many cues send into is a bus with a chain, and a bus is
        shared by construction with nothing to allocate. */
    ClaimRig rig;

    const auto channel = rig.document.createRackChannel ("stereo");
    REQUIRE (channel.ok);
    rig.document.findById (channel.id)
       .setProperty (juce::Identifier ("access"), "shared", nullptr);

    for (const auto& cueId : { rig.mediaId, rig.secondId })
        REQUIRE (rig.document.createInsert (cueId, channel.id).ok);

    const auto first = rig.armAt (rig.mediaId);
    REQUIRE_FALSE (first.empty());

    CHECK (rig.runs.holderOf (channel.id) == nullptr);
    CHECK (std::find (rig.runs.find (first)->claims.begin(),
                      rig.runs.find (first)->claims.end(), channel.id)
             == rig.runs.find (first)->claims.end());
    CHECK (rig.runs.find (first)->warning.empty());
}

TEST_CASE ("claim: a replay takes the same claims, with no audio at all")
{
    /*  THE ARGUMENT FOR HAVING NO RECORD, made executable. A claim is derived
        from the document alone - the cue's `Feed` children against the declared
        pool - so it is issued ABOVE `armMedia`'s null-player return and a
        session with no audio side takes it exactly as a hosted one does.

        Were it issued below, every claim would be absent from every replay and
        from every `wfg serve` without `--hosted` - which is precisely the
        sessions the log is supposed to reproduce. */
    ClaimRig rig;
    rig.runner.setPlayer (nullptr);

    rig.setStandby (rig.mediaId);
    rig.tickOnce();

    const auto* live = rig.runs.liveRunOf (rig.mediaId);

    if (live != nullptr)
    {
        CHECK (live->claims == std::vector<std::string> { rig.slotId });
        CHECK (live->track == -1);          // no voice, because there is no audio
    }
    else
    {
        /*  Standby does not arm without a Player at all, which is the older
            behaviour and equally fine: what must not happen is a run that
            exists and holds nothing. */
        CHECK (rig.runs.all().empty());
    }
}

TEST_CASE ("feed: a slot that does not fit its bus fails the arm rather than the load")
{
    /*  `validate()` refuses this shape when the show is read. Asked again here
        for the reason every arm-time check exists: this is the moment the thing
        is actually used, and a document edited since it was read is a document
        nobody validated. */
    RoutedRig rig;

    const auto mountEdit = rig.document.createMount ("/wfs", "namespaces/wfs.json");
    REQUIRE (mountEdit.ok);

    const auto slot = rig.document.createSlot (mountEdit.id, "/wfs/input/1");
    REQUIRE (slot.ok);

    auto slotNode = rig.document.findById (slot.id);
    slotNode.setProperty (juce::Identifier ("bus"), juce::String (rig.foldback), nullptr);
    slotNode.setProperty (juce::Identifier ("width"), 2, nullptr);
    slotNode.setProperty (juce::Identifier ("firstChannel"), 1, nullptr);   // 1 and 2 of two

    const auto feed = rig.document.createFeed (rig.mediaId, slot.id);
    REQUIRE (feed.ok);
    rig.document.findById (feed.id)
       .setProperty (juce::Identifier ("gains"), "1 0", nullptr);

    std::string problem;
    const auto routing = rig.routingOf (rig.mediaId, problem);

    CHECK (routing.empty());
    INFO (problem);
    CHECK (problem.find ("does not fit") != std::string::npos);
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
            stopId = document.createCue (listId, 3, "transport", "Out").id;

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

    /*  The same double, for a level that has to arrive EXACTLY rather than
        nearly. Spelled as two comparisons rather than `==` so the strict
        preset's -Wfloat-equal has nothing to say about a comparison that is
        meant. */
    bool same (double a, double b) noexcept
    {
        return ! (a < b) && ! (a > b);
    }
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

TEST_CASE ("fade curve: a drawn curve meets every breakpoint after the first, and leaves from where the run is")
{
    /*  The arithmetic of `Fade/@points` on its own (PR 5.16a, §14.6). */
    using cue::fadeLevelDb;

    const auto drawn = doc::readFadePoints ("0 0 0.5 -30 1 -10");
    REQUIRE (drawn.problem.empty());
    REQUIRE (drawn.points.size() == 3u);

    const auto& points = drawn.points;

    /*  EXACTLY AT EACH BREAKPOINT AFTER THE FIRST, with nothing but the
        breakpoint's own level - a dip to -30 that bottomed out at -29.99 would
        be a drawing the fade did not quite follow. */
    CHECK (same (fadeLevelDb (0.0, points, 0.5), -30.0));
    CHECK (same (fadeLevelDb (0.0, points, 1.0), -10.0));

    /*  Straight in dB between them, which is what `linear` means. */
    CHECK (fadeLevelDb (0.0, points, 0.25) == doctest::Approx (-15.0));
    CHECK (fadeLevelDb (0.0, points, 0.75) == doctest::Approx (-20.0));

    /*  FROM WHERE THE RUN IS, NOT FROM THE FIRST BREAKPOINT. A run sitting at
        -6 dB starts the drawing at -6 and joins it at the second breakpoint;
        starting at the drawn 0 would be a six-decibel jump, which on a PA is a
        click. */
    CHECK (same (fadeLevelDb (-6.0, points, 0.0), -6.0));
    CHECK (fadeLevelDb (-6.0, points, 0.25) == doctest::Approx (-18.0));
    CHECK (same (fadeLevelDb (-6.0, points, 0.5), -30.0));

    /*  Clamped, like the two words. */
    CHECK (same (fadeLevelDb (0.0, points, 1.5), -10.0));
    CHECK (same (fadeLevelDb (-6.0, points, -0.5), -6.0));

    /*  And a drawing too short to be one does not move the level rather than
        read past its end. The door and the loader never let one through. */
    CHECK (same (fadeLevelDb (-6.0, {}, 0.5), -6.0));
    CHECK (same (fadeLevelDb (-6.0, { doc::FadePoint { 0.0, -20.0 } }, 0.5), -6.0));
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

TEST_CASE ("fade: a drawn curve lands on each breakpoint's level at its time")
{
    /*  PR 5.16a. Two seconds - a hundred ticks - dipping to -30 dB at the
        halfway point and coming back up to -10. The fade's own `level` and
        `curve` say something else entirely, and are not read: where points
        exist they are the whole of the shape (§14.6). */
    FadeRig rig;

    rig.setCue (rig.fadeId, "duration", "2");
    rig.setCue (rig.fadeId, "level", "-120");
    rig.setCue (rig.fadeId, "curve", "sCurve");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.fadeId + "/points",
                                        "0 0 0.5 -30 1 -10").ok);

    const auto mediaRun = rig.startMedia();
    REQUIRE (rig.runs.find (mediaRun)->level == doctest::Approx (0.0));

    rig.fire (rig.fadeId);

    /*  The level after every tick, so the case can find the dip wherever the
        first tick of the fade happens to fall rather than assume it. */
    std::vector<double> levels;

    for (int i = 0; i < 120; ++i)
    {
        rig.tickOnce();
        levels.push_back (rig.runs.find (mediaRun)->level);
    }

    const auto bottom = std::min_element (levels.begin(), levels.end());
    const auto dip = static_cast<std::size_t> (std::distance (levels.begin(), bottom));

    /*  THE DIP IS EXACTLY THE BREAKPOINT, and it is the only sample there. */
    CHECK (same (*bottom, -30.0));
    CHECK (std::count_if (levels.begin(), levels.end(),
                          [] (double level) { return same (level, -30.0); }) == 1);

    /*  Halfway to the dip, halfway down in dB - linear, and not the sCurve the
        fade's `curve` names, which would still be near the top at a quarter. */
    REQUIRE (dip >= 25u);
    CHECK (levels[dip - 25] == doctest::Approx (-15.0));

    /*  Fifty ticks - one second, the other half of the fade - after the dip, it
        arrives at the last breakpoint: -10, not the -120 `level` says. And it
        stays there. */
    REQUIRE (dip + 50 < levels.size());
    CHECK (same (levels[dip + 50], -10.0));
    CHECK (levels[dip + 49] < -10.0);
    CHECK (same (levels.back(), -10.0));

    /*  The media run is still playing - a fade is not a stop. */
    CHECK (rig.runs.find (mediaRun)->state == cue::runState::playing);
}

TEST_CASE ("fade: a drawn curve leaves from the run's level, not from its first breakpoint")
{
    /*  The media cue plays at -6 dB and the drawing starts at 0. Following the
        drawing literally would jump six decibels up on the first tick of the
        fade, and a jump on a PA is a click - so the fade leaves from -6, for
        the reason a fade over a fade leaves from where the level has got to. */
    FadeRig rig;

    rig.setCue (rig.mediaId, "level", "-6");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.fadeId + "/points",
                                        "0 0 1 -20").ok);

    const auto mediaRun = rig.startMedia();
    REQUIRE (rig.runs.find (mediaRun)->level == doctest::Approx (-6.0));

    rig.fire (rig.fadeId);
    rig.tickOnce();

    const auto first = rig.runs.find (mediaRun)->level;
    INFO ("one tick into the fade: " << first << " dB");

    /*  One or two ticks of a one-second fade from -6 to -20: a little below -6,
        and nowhere near the drawn 0. */
    CHECK (first < -6.0);
    CHECK (first > -7.0);

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    CHECK (same (rig.runs.find (mediaRun)->level, -20.0));
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

TEST_CASE ("fade: stopWhenDone stops the target when the fade arrives, and its default does not")
{
    /*  The author's tick box (2026-09-18: "a tick box to stop a media file
        once a fade has completed"). Until it existed a fade never stopped
        anything, even at silence: the case above pins that a fade to -20
        leaves the run PLAYING, and this one pins the box. The path is the stop
        cue's own fade verb, so nothing new can drift from it. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.fadeId + "/stopWhenDone", "true").ok);

    rig.fire (rig.fadeId);

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    //  Arrived, and the arrival was the stop.
    CHECK (rig.runs.find (mediaRun)->state != cue::runState::playing);
}

TEST_CASE ("fade: its own playhead moves, so a bar drawn over it has something to draw")
{
    /*  A FADE HOLDS NO VOICE, and `updatePositions` measured every playhead
        from the SAMPLE a launch was placed at - so a fade's position was the
        literal nought for its whole life and a client drawing a bar over one
        had nothing to measure from. The author found it by looking: the bar
        sat at the start however long the fade was, which reads as a duration
        that does not take (2026-09-18).

        The stamp it is measured from is set in `fireKind`, which is the one
        place EVERY kind passes: a cue with a pre-wait arrives through
        `fireNow`, and a cue without one is fired straight from the GO without
        going near it. */
    FadeRig rig;

    rig.startMedia();
    rig.fire (rig.fadeId);

    REQUIRE (rig.runs.all().size() == 2u);
    const auto fadeRun = rig.runs.all().back().id;

    REQUIRE (rig.runs.find (fadeRun)->kind == "fade");
    CHECK (rig.runs.find (fadeRun)->launchRequestedAtTick > 0);

    const auto atStart = rig.runs.find (fadeRun)->position;

    for (int i = 0; i < 10; ++i)
        rig.tickOnce();

    const auto later = rig.runs.find (fadeRun)->position;

    CHECK (later > atStart);

    //  And it is measured in seconds, so ten ticks is a fifth of one.
    CHECK (later - atStart > 0.15);
    CHECK (later - atStart < 0.25);
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

    /*  Standby did on the GO what it always does: it moved. The memo is the
        last cue in this list and there is no wrap (§3.5), so it moved to
        nowhere - what matters here is that it did not wait for the cue. */
    CHECK (rig.standby().empty());

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
    REQUIRE (rig.runs.find (stopRun)->kind == "transport");

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
        const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Abort").id;
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

TEST_CASE ("stop levels: run.stopAll runs every footer, run.killAll runs none, and both take every root")
{
    /*  §4.4's Esc and double Esc as the commands they became (2026-09-18,
        "Panic is missing and Esc key is not bound"). Each is its single-run
        command over every ROOT run: the group is told to stop and brings its
        members down itself, which is what keeps the footer on the graceful
        path and off the immediate one. A second, unrelated run is stopped by
        the same press. */
    GroupRig rig;

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;
    rig.setCue (rig.first, "preWait", "10");             // hold the group in its members

    //  Something else running beside the group: an osc cue waiting on nothing.
    const auto lone = rig.document.createCue (rig.listId, 3, "memo", "Lone").id;
    rig.setCue (lone, "preWait", "10");

    const auto start = [&]
    {
        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").applied == 1);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        rig.submitAndTick ("cue.fire", { osc::Value::string (lone) });
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (lone).empty(); }));
    };

    SUBCASE ("stopAll is graceful")
    {
        start();
        const auto groupRun = rig.runOf (rig.groupId);
        const auto loneRun = rig.runOf (lone);

        REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);
        CHECK (rig.runToCompletion (groupRun) < 400);
        CHECK (rig.runToCompletion (loneRun) < 400);

        CHECK (rig.runOf (closing) != "");           // the footer ran on the way out
    }

    SUBCASE ("killAll is immediate")
    {
        start();
        const auto groupRun = rig.runOf (rig.groupId);
        const auto loneRun = rig.runOf (lone);

        REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);
        CHECK (rig.runToCompletion (groupRun) < 400);
        CHECK (rig.runToCompletion (loneRun) < 400);

        CHECK (rig.runOf (closing) == "");           // and the footer did not
    }

    SUBCASE ("a silent show is applied and nothing is said")
    {
        REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);
        REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);
        CHECK (rig.engine.lastError().empty());
    }
}

//==============================================================================
/*  THE PANIC FADE (author, 2026-09-28: "there should be ... a 'Panic' fade
    duration that fades out all playing cues. It seems the Panic cuts
    everything with no fade time").

    Esc is §4.4's graceful level, and a cut was the least graceful thing it
    could do to a cue that was sounding. It now fades every sounding run to
    silence over the show's `audio/panicFade` and stops it there - the job a
    stop cue's fade verb runs - and a double Esc still cuts at once. */
TEST_CASE ("panic fade: Esc fades what is sounding over the show's number, and stops it there")
{
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    const auto media = rig.startMedia();
    const auto track = rig.runs.find (media)->track;
    REQUIRE (rig.audio.playing.count (track) == 1u);

    REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);
    CHECK (rig.runs.find (media)->state == cue::runState::stopping);

    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    /*  Halfway down and still sounding: a fade, not a cut. */
    CHECK (rig.audio.playing.count (track) == 1u);
    CHECK (rig.runs.find (media)->level < -1.0);
    CHECK (rig.runs.find (media)->level > -120.0);

    for (int n = 0; n < 30; ++n)
        rig.tickOnce();

    /*  At silence, and then stopped - the order a fade verb keeps, so the
        clip's own click suppression has nothing left to do. */
    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (media)->isFinished(); }, 5));
}

TEST_CASE ("panic fade: nought is the cut Esc always was, and a show that says nothing fades for a second")
{
    SUBCASE ("nought")
    {
        FadeRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

        const auto media = rig.startMedia();
        const auto track = rig.runs.find (media)->track;

        rig.submitAndTick ("run.stopAll");
        rig.tickOnce();

        CHECK (rig.audio.playing.count (track) == 0u);
        CHECK (rig.runner.fades().empty());
    }

    SUBCASE ("the default")
    {
        FadeRig rig;
        CHECK (rig.document.getAttribute ("/godot/audio/panicFade") == std::optional<std::string> ("1"));

        const auto media = rig.startMedia();
        rig.submitAndTick ("run.stopAll");

        REQUIRE (rig.runner.fades().size() == 1u);
        CHECK (rig.runner.fades().front().ticksTotal == TickClock::rateHz);
        CHECK (rig.runner.fades().front().target == media);
    }
}

TEST_CASE ("panic fade: a double Esc in the middle of it cuts at once")
{
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "2").ok);

    const auto media = rig.startMedia();
    const auto track = rig.runs.find (media)->track;

    rig.submitAndTick ("run.stopAll");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.audio.playing.count (track) == 1u);

    /*  §4.4: "drops all actions". The fade is an action, and it is dropped -
        waiting out the rest of two seconds is not immediate. */
    REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);
    rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (media)->isFinished(); }, 5));
}

TEST_CASE ("double Esc: a cue a stop cue is fading out is cut, not handed back its level")
{
    /*  FOUND WHILE BUILDING THE PANIC FADE, and older than it. A double Esc
        marks the stop cue's run and its target alike; the killed fade then
        put its target back to `playing` - the rule for a stop cue killed on
        its own, where the operator asked nothing of the cue - and nothing was
        left to stop it. The cue played on, at whatever level the fade had
        reached, after the one press that promises everything is dropped. */
    FadeRig rig;
    const auto media = rig.startMedia();
    const auto track = rig.runs.find (media)->track;

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "10");
    rig.fire (rig.stopId);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.audio.playing.count (track) == 1u);

    REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);
    rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (media)->isFinished(); }, 5));
}

TEST_CASE ("Esc: a cue a stop cue is fading out is stopped with everything else, never handed back")
{
    /*  THE SINGLE-ESC TWIN of the case above, and just as old: Esc marked the
        stop cue's run with the other roots, the fade whose run was stopped gave
        its target back its level, and the cue played on. With a panic fade of
        nought Esc is a cut, and the cue is cut with the rest. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

    const auto media = rig.startMedia();
    const auto track = rig.runs.find (media)->track;

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "10");
    rig.fire (rig.stopId);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);
    rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (media)->isFinished(); }, 5));
    CHECK (rig.tickUntil ([&] { return rig.runs.all().back().isFinished(); }, 5));   // and the stop cue's run
}

TEST_CASE ("panic fade: a cue already fading out keeps whichever stop lands first")
{
    SUBCASE ("a long fade-and-stop is brought down with everything else")
    {
        FadeRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

        const auto media = rig.startMedia();
        const auto track = rig.runs.find (media)->track;

        rig.setCue (rig.stopId, "verb", "fade");
        rig.setCue (rig.stopId, "duration", "10");
        rig.fire (rig.stopId);
        rig.tickOnce();

        rig.submitAndTick ("run.stopAll");

        for (int n = 0; n < 60; ++n)
            rig.tickOnce();

        CHECK (rig.audio.playing.count (track) == 0u);
    }

    SUBCASE ("a short one is left to land where it was going to")
    {
        FadeRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "5").ok);

        const auto media = rig.startMedia();
        const auto track = rig.runs.find (media)->track;

        rig.setCue (rig.stopId, "verb", "fade");
        rig.setCue (rig.stopId, "duration", "0.4");
        rig.fire (rig.stopId);
        rig.tickOnce();

        rig.submitAndTick ("run.stopAll");

        for (int n = 0; n < 25; ++n)
            rig.tickOnce();

        CHECK (rig.audio.playing.count (track) == 0u);
    }
}

TEST_CASE ("panic fade: a group's footer runs once its members have faded out")
{
    /*  §4.4: "same code path as normal completion, entered early". The group
        is asked to stop as it always was; its sounding member is already
        fading, so the footer - the releasing - waits for the fade rather than
        cutting underneath it. */
    GroupRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    const auto rain = rig.document.createCue (rig.groupId, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "rain.wav");

    const auto footer = rig.roleOf (rig.groupId, "footer");
    const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rain).empty(); }));

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto rainRun = rig.runOf (rain);
    const auto track = rig.runs.find (rainRun)->track;
    REQUIRE (track >= 0);
    rig.audio.playing.insert (track);
    rig.tickOnce();

    const auto groupRun = rig.runOf (rig.groupId);
    REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 1u);             // still fading
    CHECK (rig.runOf (closing).empty());                       // and the footer waits for it
    CHECK_FALSE (rig.runs.find (groupRun)->isFinished());

    CHECK (rig.runToCompletion (groupRun) < 100);
    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK_FALSE (rig.runOf (closing).empty());                 // the footer ran on the way out
}

TEST_CASE ("panic fade: a cue whose file ends during it gives its voice back, and the stop does not follow it")
{
    /*  A finished run still names the track it held. A fade that ends in a stop
        used to stop that track at its tick whether or not the cue had already
        ended - so a cue that ran out during Esc's fade, and a GO that took its
        voice a moment later, had the new cue cut when the old fade arrived. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    const auto old = rig.startMedia();
    const auto track = rig.runs.find (old)->track;

    rig.submitAndTick ("run.stopAll");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    //  The file runs out, a fifth of the way down.
    rig.audio.playing.erase (track);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (old)->isFinished(); }, 5));

    //  GO on the next thing, which takes the voice that was given back.
    rig.fire (rig.mediaId);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto again = rig.runs.all().back().id;
    REQUIRE (again != old);
    REQUIRE (rig.runs.find (again)->track == track);
    rig.audio.playing.insert (track);

    for (int n = 0; n < 60; ++n)
        rig.tickOnce();

    CHECK (rig.audio.playing.count (track) == 1u);
    CHECK_FALSE (rig.runs.find (again)->isFinished());
}

TEST_CASE ("Esc: a group a stop cue is fading out stops with everything else, and its footer runs")
{
    /*  The group-sized twin of the stop cue's hole, which turned out not to be
        one: a stop cue fading a GROUP holds no voice, so the panic fade does not
        take it over, and Esc stops the group as the root it is - its sequence
        does not carry on, and its footer runs. Kept because the voice-sized
        case above was broken, and this is where the same rule would break
        next. */
    for (const auto* seconds : { "1", "0" })
    {
        INFO ("panicFade " << seconds);

        GroupRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", seconds).ok);

        const auto footer = rig.roleOf (rig.groupId, "footer");
        const auto closing = rig.document.createCue (footer, 0, "memo", "Release").id;
        rig.setCue (rig.first, "preWait", "10");             // hold the group in its members

        const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Preshow out").id;
        rig.setCue (stopId, "target", rig.groupId);
        rig.setCue (stopId, "verb", "fade");
        rig.setCue (stopId, "duration", "20");

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").applied == 1);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));
        const auto groupRun = rig.runOf (rig.groupId);

        rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) });
        rig.tickOnce();
        REQUIRE (rig.runs.find (groupRun)->state == cue::runState::stopping);

        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);

        //  Well inside the stop cue's twenty seconds: Esc has brought it down.
        CHECK (rig.runToCompletion (groupRun) < 100);
        CHECK_FALSE (rig.runOf (closing).empty());
        CHECK (rig.runOf (rig.second).empty());               // and the sequence did not carry on
    }
}

TEST_CASE ("panic fade: a GO while a manual group fades out starts the scene again")
{
    /*  Before the fade, the group was gone a tick after Esc, so a GO a moment
        later entered it afresh. A group that takes a second to leave must not
        swallow that GO: the member it fires would be spawned into a group
        that kills it on the next tick - a GO that made no sound. */
    GroupRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);
    rig.setCue (rig.groupId, "advance", "manual");

    const auto rain = rig.document.createCue (rig.groupId, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "rain.wav");

    rig.setStandby (rain);                // the pointer on the member, as a manual group has it
    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rain).empty(); }));

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto track = rig.runs.find (rig.runOf (rain))->track;
    REQUIRE (track >= 0);
    rig.audio.playing.insert (track);
    rig.tickOnce();

    const auto oldGroup = rig.runOf (rig.groupId);
    REQUIRE (rig.standby() == rig.first);

    rig.submitAndTick ("run.stopAll");
    rig.tickOnce();
    REQUIRE (rig.runs.find (oldGroup)->state == cue::runState::stopping);

    //  Rejections, not applications: the same tick applies the machine's own reports.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }, 20));

    const auto* one = rig.runs.find (rig.runOf (rig.first));
    REQUIRE (one != nullptr);
    CHECK (one->parent != oldGroup);
    CHECK_FALSE (one->killed);
    CHECK_FALSE (one->skipFooter);
}

//==============================================================================
/*  THE STOP LEVELS HELD TO THEIR LAW (namespace draft §23, 2026-09-30).

    ESC BRINGS A SCENE DOWN THE WAY IT WOULD HAVE ENDED ANYWAY. A group asked
    to stop ended its members with `run.kill`, which marks each of them "skip
    your footer" - so a scene inside a scene, aborted by Esc, never gave back
    what it held, and PRD §4.4's "Esc runs footers" was true of the outermost
    level only. Now a graceful stop stops the members and only a kill kills
    them; the parent already waits for every member before its own footer, so
    the innermost comes down first (PRD: "nested scopes tear down
    innermost-first"). */
namespace
{
    /*  The rig's group with a group inside it, ahead of its memos, each with a
        footer of its own. The inner group's one member waits out a long
        pre-wait, which holds the whole scene in its members until somebody
        stops it. */
    struct NestedRig : GroupRig
    {
        NestedRig()
        {
            inner = document.createCue (groupId, 0, "group", "Inner").id;
            setCue (inner, "advance", "auto");

            deep = document.createCue (inner, 0, "memo", "Deep").id;
            setCue (deep, "preWait", "10");

            innerRelease = document.createCue (roleOf (inner, "footer"), 0, "memo", "Inner release").id;
            outerRelease = document.createCue (roleOf (groupId, "footer"), 0, "memo", "Outer release").id;
        }

        /** GO on the outer group, ticked on until the inner group's member exists. */
        void start()
        {
            setStandby (groupId);
            REQUIRE (submitAndTick ("go").rejected == 0);
            REQUIRE (tickUntil ([this] { return ! runOf (deep).empty(); }));

            outerRun = runOf (groupId);
            innerRun = runOf (inner);

            REQUIRE_FALSE (outerRun.empty());
            REQUIRE_FALSE (innerRun.empty());
            REQUIRE (runs.find (innerRun)->parent == outerRun);
        }

        std::string inner, deep, innerRelease, outerRelease, outerRun, innerRun;
    };
}

TEST_CASE ("stop levels: Esc brings a nested group down innermost-first, and both footers run")
{
    NestedRig rig;
    rig.start();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    CHECK (rig.runToCompletion (rig.outerRun) < 400);

    /*  BOTH FOOTERS RAN, the inner scene's as well as the outer's: §4.4's "Esc
        runs footers" at every level, not only the one Esc reached. */
    const auto innerReleaseRun = rig.runOf (rig.innerRelease);
    const auto outerReleaseRun = rig.runOf (rig.outerRelease);

    CHECK_FALSE (innerReleaseRun.empty());
    REQUIRE_FALSE (outerReleaseRun.empty());

    /*  INNERMOST FIRST: the inner scene had ended before the outer one's
        footer began, because the outer waits for every member it has. */
    CHECK (rig.runs.find (rig.innerRun)->endedAtTick
             <= rig.runs.find (outerReleaseRun)->launchRequestedAtTick);

    /*  STOPPED, NOT KILLED: neither the inner group nor its member carries a
        kill's marks - so nothing reads this as the operator's kill, and a
        persistent cue down here would not be suspended by it. */
    for (const auto& id : { rig.innerRun, rig.runOf (rig.deep) })
    {
        INFO ("run " << id);
        CHECK_FALSE (rig.runs.find (id)->killed);
        CHECK_FALSE (rig.runs.find (id)->skipFooter);
    }

    /*  AND NOTHING WAS KILLED AT ALL: a graceful stop, all the way down. */
    const auto records = LogFile::parse (rig.engine.log().contents()).records;

    CHECK (std::none_of (records.begin(), records.end(),
                         [] (const auto& record) { return record.command == "run.kill"; }));
}

TEST_CASE ("stop levels: a double Esc on a nested group runs neither footer, and a stop cue aimed at the outer group runs both")
{
    NestedRig rig;

    SUBCASE ("double Esc: every level killed, no footer anywhere")
    {
        rig.start();

        REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
        CHECK (rig.runToCompletion (rig.outerRun) < 400);

        CHECK (rig.runOf (rig.innerRelease).empty());
        CHECK (rig.runOf (rig.outerRelease).empty());

        /*  The immediate path is the one it always was: a group that skips its
            footer kills its members, and they skip theirs. */
        CHECK (rig.runs.find (rig.innerRun)->skipFooter);

        /*  AND IT CARRIES `killed`, WHICH IS A KNOWN GAP AND NOT A RULE (namespace
            draft §23.2, gap c): a member killed by its group is marked as the
            running pane's kill, and a persistent cue sounding as one would be
            suspended by a double Esc, against §18.8. Pinned so that the day the
            gap is ruled on, this line is the one that says so. */
        CHECK (rig.runs.find (rig.innerRun)->killed);
    }

    SUBCASE ("a stop cue aimed at the outer group is graceful, as Esc is")
    {
        const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Abort").id;
        rig.setCue (stopId, "target", rig.groupId);

        rig.start();

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) }).rejected == 0);
        CHECK (rig.runToCompletion (rig.outerRun) < 400);

        CHECK_FALSE (rig.runOf (rig.innerRelease).empty());
        CHECK_FALSE (rig.runOf (rig.outerRelease).empty());
        CHECK_FALSE (rig.runs.find (rig.innerRun)->killed);
    }
}

TEST_CASE ("stop levels: a nested group whose member holds its post-wait still comes down")
{
    /*  ONE STOP A MEMBER, NOT ONE A TICK. A stopping group sent its stop to every
        member still running on every tick. A member holding its post-wait
        answers a stop with `run.ended`, which starts its post-wait again - and
        the next tick stopped it again, for ever: the scene never came down.
        Graceful teardown of nested groups makes that an ordinary show, so a
        member is now asked once, and one already on its way out is left to
        finish. (2026-10-02, K2, namespace draft §23.13: one holding its
        post-wait is no longer left to run it out - the stop ends it.) */
    NestedRig rig;
    rig.setCue (rig.deep, "preWait", "0");
    rig.setCue (rig.deep, "postWait", "0.2");            // ten ticks

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (rig.deep);
        return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
    }));

    const auto outerRun = rig.runOf (rig.groupId);

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    CHECK (rig.runToCompletion (outerRun) < 400);
    CHECK_FALSE (rig.runOf (rig.innerRelease).empty());
}

TEST_CASE ("stop levels: a double Esc cuts a footer that is already running")
{
    /*  §4.4: a double Esc is immediate and skips footers - a footer Esc had
        already started among them. A group's stop was only acted on before its
        footer began, so a footer holding a pre-wait played on to its end
        through the press that promises everything is dropped. */
    GroupRig rig;
    rig.setCue (rig.first, "preWait", "10");             // hold the group in its members

    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
    rig.setCue (closing, "preWait", "10");               // and in its footer, once it gets there

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto groupRun = rig.runOf (rig.groupId);

    //  Esc: the member comes down, and the footer begins and holds.
    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (closing);
        return ! id.empty() && rig.runs.find (id)->state == cue::runState::waiting;
    }, 50));

    //  Double Esc: the footer is cut, and the scene is over within a few ticks.
    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
    CHECK (rig.runToCompletion (groupRun, 10) < 10);
    CHECK (rig.runs.find (rig.runOf (closing))->isFinished());
}

TEST_CASE ("Esc: a group a stop cue is fading out is never handed back, however long its member takes")
{
    /*  A FADE WHOSE RUN IS STOPPED GIVES ITS TARGET BACK - the rule for a stop
        cue killed on its own, where the operator asked nothing of the cue - and
        Esc stops a stop cue's run with every other root. For a VOICE that is
        §21.4's hole, closed by the panic fade. For a GROUP it put the scene
        back to `playing` while its member was still ending, and once the member
        had gone the scene played its next one, after Esc. A group's `stopping`
        belongs to its own job, which has already asked its members to stop. */
    GroupRig rig;
    rig.setCue (rig.first, "postWait", "1");             // fifty ticks to finish, once it has fired

    const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Preshow out").id;
    rig.setCue (stopId, "target", rig.groupId);
    rig.setCue (stopId, "verb", "fade");
    rig.setCue (stopId, "duration", "20");

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (rig.first);
        return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
    }));

    const auto groupRun = rig.runOf (rig.groupId);

    rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) });
    rig.tickOnce();
    REQUIRE (rig.runs.find (groupRun)->state == cue::runState::stopping);

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);

    CHECK (rig.runToCompletion (groupRun) < 400);
    CHECK (rig.runOf (rig.second).empty());               // the sequence never carried on
}

TEST_CASE ("stop levels: a member a killed stop cue hands back is killed again, not left playing")
{
    /*  ONCE A MEMBER, UNLESS IT CAME BACK. A killed group kills its members once
        each - and one of them here is a stop cue fading another member out. A
        stop cue whose own run is killed hands its target back its level and
        `playing`, so the member it was fading is playing again after the kill
        reached it. Asked only once, it would play on to the end of its file
        with the scene waiting for it; it is killed again. (Passes on the code
        before the once-a-member rule, which killed every member every tick: it
        is here so that the rule does not take that away.) */
    GroupRig rig;
    rig.setCue (rig.groupId, "mode", "timeline");

    const auto bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
    rig.setCue (bed, "file", "bed.wav");

    const auto out = rig.document.createCue (rig.groupId, 1, "transport", "Bed out").id;
    rig.setCue (out, "target", bed);
    rig.setCue (out, "verb", "fade");
    rig.setCue (out, "duration", "10");
    rig.setCue (out, "preWait", "0.2");                  // once the bed is sounding

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto bedRun = rig.runOf (bed);
    const auto track = rig.runs.find (bedRun)->track;
    REQUIRE (track >= 0);
    rig.audio.playing.insert (track);

    //  The stop cue fires, and the bed is on its way out over ten seconds.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (bedRun)->state == cue::runState::stopping; }));
    REQUIRE (rig.audio.playing.count (track) == 1u);

    const auto groupRun = rig.runOf (rig.groupId);
    REQUIRE (rig.submitAndTick ("run.kill", { osc::Value::string (groupRun) }).rejected == 0);

    CHECK (rig.runToCompletion (groupRun, 20) < 20);
    CHECK (rig.audio.playing.count (track) == 0u);
}

TEST_CASE ("stop levels: a member the double Esc has cut is given no level by its lane, from the press to its end")
{
    /*  H3 (namespace draft §23.6). A double Esc takes every voice to silence
        in the press's own tick - the sweep - and a member of a scene is killed
        later, by its group's job. A lane moves the member's level every tick
        until its run ends, and the level written over that silence brought
        the voice back for the ticks before its kill. This player's sweep only
        counts the press, and its stop ends the sound at once: what is checked
        is that the Runner writes nothing to the voice from the press to the
        run's end.
        The write after a kill, while a real voice still reads as playing, is
        the AudioTests case's to hear. */
    GroupRig rig;

    const auto rain = rig.document.createCue (rig.groupId, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "rain.wav");
    rig.setCue (rain, "levelLane", "0 0 8 -24");         // all the way down: a level that moves every tick

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rain).empty(); }));

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto rainRun = rig.runOf (rain);
    const auto track = rig.runs.find (rainRun)->track;
    REQUIRE (track >= 0);
    rig.audio.playing.insert (track);

    const auto writesTo = [&rig, track]
    {
        return std::count_if (rig.audio.levels.begin(), rig.audio.levels.end(),
                              [track] (const std::pair<int, double>& write) { return write.first == track; });
    };

    //  Time moves, as the sound does.
    const auto onTick = [&rig]
    {
        rig.audio.samples += 960;
        rig.tickOnce();
    };

    const auto killed = [&rig, track]
    {
        return std::find (rig.audio.stopped.begin(), rig.audio.stopped.end(), track) != rig.audio.stopped.end();
    };

    //  While it plays, its voice follows the lane.
    for (int n = 0; n < 5; ++n)
        onTick();

    const auto playingWrites = writesTo();

    for (int n = 0; n < 5; ++n)
        onTick();

    REQUIRE (writesTo() > playingWrites);

    //  The press: the scene marked, and from here on nothing reaches the voice.
    REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
    onTick();

    const auto atThePress = writesTo();
    REQUIRE_FALSE (killed());

    //  Its group's job kills it a tick or two on (this player's kill is its stop).
    for (int n = 0; n < 5 && ! killed(); ++n)
        onTick();

    REQUIRE (killed());

    for (int n = 0; n < 5; ++n)
        onTick();

    CHECK (rig.runs.find (rainRun)->isFinished());
    CHECK (writesTo() == atThePress);
}

TEST_CASE ("stop levels: killing a stop cue and the cue it is fading in the same drain still kills the cue")
{
    /*  FOUND BY H3'S FINAL CHECK (2026-10-01, namespace draft §23.6). A stop
        cue whose own run is ended hands the cue it was fading back to
        `playing` (`advanceFades`: the fade killed on its own gives its target
        back its level), and the hand-back asked only whether the target's stop
        had been issued - not whether the target itself had been killed. So the
        running pane's kill of both, landing in one drain, lost the second: the
        stop cue's end handed the killed cue back to `playing`, `enforceStops`
        found nothing stopping, and the cue played on. A killed target is
        never handed back. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    const auto track = rig.runs.find (mediaRun)->track;
    REQUIRE (track >= 0);

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "10");
    rig.fire (rig.stopId);
    REQUIRE (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    const auto stopRun = rig.runOf (rig.stopId);
    REQUIRE_FALSE (stopRun.empty());

    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    REQUIRE (rig.audio.playing.count (track) == 1u);
    REQUIRE (rig.audio.kills.empty());

    //  Both in one drain, the stop cue first, as a pane listing them would.
    REQUIRE (rig.engine.submit ("cli", "run.kill", { osc::Value::string (stopRun) }));
    REQUIRE (rig.engine.submit ("cli", "run.kill", { osc::Value::string (mediaRun) }));
    rig.tickOnce();

    CHECK (rig.tickUntil ([&rig, track] { return ! rig.audio.kills.empty(); }, 5));
    CHECK (rig.audio.kills == std::vector<int> { track });
    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (mediaRun)->isFinished(); }, 5));
}

TEST_CASE ("stop levels: the pane's kill of a cue a fade-and-stop is holding cuts it at once, not at the fade's end")
{
    /*  H3 (namespace draft §23.6, GC). A stop cue's fade holds its target back
        from `enforceStops` until the stop it carries is due - which is the
        whole of the fade verb - and the hold made no exception for a kill. So
        the running pane's kill of a cue half a second into a ten-second
        fade-out reached the voice only when the fade would have ended; and a
        run that is cut is given no level (FZ), so the cue hung at the level the
        kill found it at for the rest of the fade, and was then stopped there,
        its tail ringing. A kill asks nothing of the cue (§4.4's immediate
        level): it lands at once, and the fade, nothing left to stop, runs out
        its own run. */
    FadeRig rig;

    const auto mediaRun = rig.startMedia();
    const auto track = rig.runs.find (mediaRun)->track;
    REQUIRE (track >= 0);

    rig.setCue (rig.stopId, "verb", "fade");
    rig.setCue (rig.stopId, "duration", "10");
    rig.fire (rig.stopId);
    REQUIRE (rig.runs.find (mediaRun)->state == cue::runState::stopping);

    //  Half a second into ten: on its way down, and still sounding.
    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    REQUIRE (rig.audio.playing.count (track) == 1u);
    REQUIRE (rig.audio.kills.empty());
    REQUIRE (rig.audio.stopped.empty());

    const auto writesTo = [&rig, track]
    {
        return std::count_if (rig.audio.levels.begin(), rig.audio.levels.end(),
                              [track] (const std::pair<int, double>& write) { return write.first == track; });
    };

    REQUIRE (rig.submitAndTick ("run.kill", { osc::Value::string (mediaRun) }).rejected == 0);
    const auto atThePress = writesTo();
    rig.tickOnce();

    //  The kill reaches the voice in the tick after the press...
    CHECK (rig.audio.kills == std::vector<int> { track });
    CHECK (rig.audio.playing.count (track) == 0u);

    //  ...the run ends with the sound, given no level on the way...
    CHECK (rig.tickUntil ([&] { return rig.runs.find (mediaRun)->isFinished(); }, 5));
    CHECK (writesTo() == atThePress);

    //  ...and the fade, nothing left to stop, stops nothing when it is due.
    for (int n = 0; n < 500; ++n)
        rig.tickOnce();

    CHECK (rig.audio.kills == std::vector<int> { track });
    CHECK (rig.audio.stopped == std::vector<int> { track });
}

TEST_CASE ("stop levels: a kill that finds a member holding its post-wait kills its voice, as well as ending it")
{
    /*  H3 (namespace draft §23.6, GD). A killed group ends a member that holds
        its post-wait with `run.done`, on the spot (ES): its sound is over, and
        the wait - its voice with it - is all it holds. But it is the clip that
        is over, not the chain: the voice's EQ and inserts still ring what the
        file's last moments put into them, at the cue's level, and `run.done`
        reaches no Player. So the pane's kill of a scene left a member's reverb
        ringing until it died away, or until the next arm on that voice cleared
        it. A double Esc's sweep reached it; the pane's kill had nothing that
        did. */
    GroupRig rig;

    const auto bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
    rig.setCue (bed, "file", "bed.wav");
    rig.setCue (bed, "postWait", "30");

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

    const auto bedRun = rig.runOf (bed);
    const auto voice = rig.runs.find (bedRun)->track;
    REQUIRE (voice >= 0);

    rig.audio.playing.insert (voice);                 // it sounds...
    rig.tickOnce();
    rig.tickOnce();
    rig.audio.playing.erase (voice);                  // ...and its file reaches its end

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (bedRun)->state == cue::runState::postWait; }));
    REQUIRE (rig.audio.kills.empty());

    //  The running pane's kill of the scene.
    const auto groupRun = rig.runOf (rig.groupId);
    REQUIRE (rig.submitAndTick ("run.kill", { osc::Value::string (groupRun) }).rejected == 0);
    CHECK (rig.runToCompletion (groupRun, 10) < 10);

    CHECK (rig.runs.find (bedRun)->isFinished());
    CHECK (rig.audio.kills == std::vector<int> { voice });
    CHECK_FALSE (rig.runs.isTrackBusy (voice));
}

TEST_CASE ("stop levels: with no audio side, a stop cue aimed at a group still runs its footer")
{
    /*  A FADE THAT ENDS IN A STOP SAYS THE END ITSELF when there is no audio
        side to report it - for a sound. It said it for a group too, and so
        ended the scene on the spot: no member stopped in order, no footer. A
        `wfg serve` without `--hosted` runs shows of memos and network cues, and
        its footers are exactly where a scene gives the desk back. */
    GroupRig rig;
    rig.runner.setPlayer (nullptr);

    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
    rig.setCue (rig.first, "preWait", "10");             // hold the group in its members

    const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Abort").id;
    rig.setCue (stopId, "target", rig.groupId);

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto groupRun = rig.runOf (rig.groupId);

    REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) }).rejected == 0);
    CHECK (rig.runToCompletion (groupRun) < 400);
    CHECK_FALSE (rig.runOf (closing).empty());
}

TEST_CASE ("stop levels: a double Esc during Esc's teardown cuts the inner scene's footer, and runs no outer one")
{
    /*  EVERY REAL DOUBLE ESC IS TWO PRESSES: the client and the D700 send
        `run.stopAll` on the first and `run.killAll` on the second, inside 750 ms
        - so the kill always lands on a teardown Esc has started. Here the inner
        scene's footer has begun and holds, and the second press has to cut it
        and leave the outer footer unrun. (A guard: it passes on the code before
        the kill was read from above too. A rule that passed over every member
        already on its way out would fail it.) */
    NestedRig rig;
    rig.setCue (rig.innerRelease, "preWait", "10");      // the inner footer holds once it begins
    rig.start();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&]
    {
        const auto id = rig.runOf (rig.innerRelease);
        return ! id.empty() && rig.runs.find (id)->state == cue::runState::waiting;
    }, 50));

    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
    CHECK (rig.runToCompletion (rig.outerRun, 20) < 20);
    CHECK (rig.runOf (rig.outerRelease).empty());
    CHECK (rig.runs.find (rig.runOf (rig.innerRelease))->isFinished());
}

TEST_CASE ("stop levels: a scene whose parent the second press kills does not begin its footer in that tick")
{
    /*  THE KILL IS READ FROM ABOVE (namespace draft §23.2). A killed group
        reaches its members through its own job, a tick after the press - and a
        scene inside it that read only its OWN mark in that tick was still being
        stopped gracefully. Its last member ending in the same drain as the
        second press, it began its footer, and the footer's cues were spawned and
        launched under a double Esc that promises none runs. */
    NestedRig rig;
    rig.start();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);

    //  Esc reaches the inner scene's member...
    REQUIRE (rig.tickUntil ([&]
    {
        return rig.runs.find (rig.runOf (rig.deep))->state == cue::runState::stopping;
    }, 20));

    //  ...and the second press lands in the drain that ends it.
    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
    CHECK (rig.runToCompletion (rig.outerRun, 20) < 20);

    CHECK (rig.runOf (rig.innerRelease).empty());
    CHECK (rig.runOf (rig.outerRelease).empty());
}

TEST_CASE ("stop levels: a double Esc never waits out a post-wait")
{
    /*  §4.4: the double Esc is immediate. A kill wrote `stopping` over a
        post-wait, and the `run.ended` that followed began the post-wait again
        from nought - so a member thirty seconds into a thirty-second post-wait
        held its voice, its slots and its scene for thirty more, and a cue killed
        in its pre-wait began a post-wait nothing had reached. A kill asks
        nothing of the cue: a post-wait under way is ended, and none is begun
        (namespace draft §23.2). */
    SUBCASE ("a member holding its post-wait is ended, and its voice given back")
    {
        GroupRig rig;

        const auto bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
        rig.setCue (bed, "file", "bed.wav");
        rig.setCue (bed, "postWait", "30");

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

        rig.audio.completeArms (rig.engine);
        REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

        const auto bedRun = rig.runOf (bed);
        const auto voice = rig.runs.find (bedRun)->track;
        REQUIRE (voice >= 0);

        rig.audio.playing.insert (voice);                 // it sounds...
        rig.tickOnce();
        rig.tickOnce();
        rig.audio.playing.erase (voice);                  // ...and reaches its end

        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (bedRun)->state == cue::runState::postWait; }));
        REQUIRE (rig.runs.isTrackBusy (voice));           // a post-wait holds its voice

        const auto groupRun = rig.runOf (rig.groupId);

        REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
        CHECK (rig.runToCompletion (groupRun, 10) < 10);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));
    }

    SUBCASE ("a member killed in its pre-wait begins no post-wait")
    {
        GroupRig rig;
        rig.setCue (rig.first, "preWait", "10");
        rig.setCue (rig.first, "postWait", "30");

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (rig.first);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::waiting;
        }));

        const auto groupRun = rig.runOf (rig.groupId);

        REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
        CHECK (rig.runToCompletion (groupRun, 10) < 10);
    }

    SUBCASE ("a cue on its own, killed in its pre-wait, begins no post-wait")
    {
        Rig rig;
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "10");
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "30");

        rig.setStandby (rig.memoId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto memoRun = rig.runOf (rig.memoId);
        REQUIRE_FALSE (memoRun.empty());
        REQUIRE (rig.runs.find (memoRun)->state == cue::runState::waiting);

        REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (memoRun)->isFinished(); }, 10));
    }

    SUBCASE ("a footer's cue holding its post-wait, cut by the second press")
    {
        GroupRig rig;
        rig.setCue (rig.first, "preWait", "10");         // hold the group in its members

        const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
        rig.setCue (closing, "postWait", "30");

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        const auto groupRun = rig.runOf (rig.groupId);

        //  Esc: the member comes down, and the footer's cue fires and holds its post-wait.
        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (closing);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
        }, 50));

        REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);
        CHECK (rig.runToCompletion (groupRun, 10) < 10);
        CHECK (rig.runs.find (rig.runOf (closing))->isFinished());
    }
}

TEST_CASE ("stop levels: Esc ends a post-wait too, begins none, and the footer still runs")
{
    /*  THE AUTHOR'S RULING (2026-10-02, K2, namespace draft §23.13: "Esc ends
        them too"). Under Esc a member's post-wait ran out before its scene's
        footer: a member that had just begun a thirty-second post-wait kept its
        voice, its slots and its scene for that half-minute, and a member Esc
        stopped in its PRE-wait - never fired - began its whole post-wait when
        it ended. Now a stop asked of a run ends a post-wait under way and
        begins none, as a kill already did (ES); the footers still run, which
        is what Esc keeps of normal completion. A scene that ends on its own
        still waits out its members' post-waits - the guard at the end. */
    SUBCASE ("a member holding its post-wait is ended, its voice given back, and the footer runs within the fade")
    {
        GroupRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

        const auto bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
        rig.setCue (bed, "file", "bed.wav");
        rig.setCue (bed, "postWait", "30");

        const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

        rig.audio.completeArms (rig.engine);
        REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

        const auto bedRun = rig.runOf (bed);
        const auto voice = rig.runs.find (bedRun)->track;
        REQUIRE (voice >= 0);

        rig.audio.playing.insert (voice);                 // it sounds...
        rig.tickOnce();
        rig.tickOnce();
        rig.audio.playing.erase (voice);                  // ...and reaches its end

        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (bedRun)->state == cue::runState::postWait; }));
        REQUIRE (rig.runs.isTrackBusy (voice));           // a post-wait holds its voice

        const auto groupRun = rig.runOf (rig.groupId);

        //  Esc: the footer within the panic fade and a few ticks, not thirty seconds on.
        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        CHECK (rig.tickUntil ([&] { return ! rig.runOf (closing).empty(); }, 60));
        CHECK (rig.runs.find (bedRun)->state == cue::runState::done);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));
        CHECK (rig.audio.kills.empty());                  // Esc cuts nothing
        CHECK (rig.runToCompletion (groupRun, 70) < 70);
    }

    SUBCASE ("a member Esc stops in its pre-wait begins no post-wait")
    {
        GroupRig rig;
        rig.setCue (rig.first, "preWait", "10");
        rig.setCue (rig.first, "postWait", "30");

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (rig.first);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::waiting;
        }));

        const auto groupRun = rig.runOf (rig.groupId);
        const auto memberRun = rig.runOf (rig.first);

        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (memberRun)->isFinished(); }, 10));
        CHECK (rig.runToCompletion (groupRun, 70) < 70);
    }

    SUBCASE ("a scene's own post-wait is not begun after the footer Esc ran")
    {
        GroupRig rig;
        rig.setCue (rig.first, "preWait", "10");          // hold the group in its members
        rig.setCue (rig.groupId, "postWait", "30");

        const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        const auto groupRun = rig.runOf (rig.groupId);

        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        CHECK (rig.runToCompletion (groupRun, 70) < 70);
        CHECK_FALSE (rig.runOf (closing).empty());
    }

    SUBCASE ("a cue on its own, stopped in its pre-wait, begins no post-wait")
    {
        Rig rig;
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/preWait", "10");
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "30");

        rig.setStandby (rig.memoId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto memoRun = rig.runOf (rig.memoId);
        REQUIRE_FALSE (memoRun.empty());
        REQUIRE (rig.runs.find (memoRun)->state == cue::runState::waiting);

        REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (memoRun)->isFinished(); }, 10));
    }

    SUBCASE ("a stop cue aimed at the scene ends its member's post-wait, and the footer runs")
    {
        GroupRig rig;
        rig.setCue (rig.first, "postWait", "30");

        const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
        const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Abort").id;
        rig.setCue (stopId, "target", rig.groupId);

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (rig.first);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
        }));

        const auto groupRun = rig.runOf (rig.groupId);

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) }).rejected == 0);
        CHECK (rig.runToCompletion (groupRun, 70) < 70);
        CHECK_FALSE (rig.runOf (closing).empty());
        CHECK (rig.runOf (rig.second).empty());           // and nothing after it was fired
    }

    SUBCASE ("guard: a scene ending on its own still waits out its member's post-wait before its footer")
    {
        GroupRig rig;
        rig.setCue (rig.third, "postWait", "0.4");        // twenty ticks

        const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (rig.third);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
        }));

        int ticks = 0;

        while (rig.runOf (closing).empty() && ticks < 100)
        {
            rig.tickOnce();
            ++ticks;
        }

        CHECK (ticks >= 20);
        CHECK (ticks < 100);
    }
}

TEST_CASE ("stop levels: a cue on its own holding its post-wait is ended at once by Esc or the pane's stop")
{
    /*  A ROOT IN ITS POST-WAIT, reached by Esc's own stop, and by the pane's
        stop aimed at it (2026-10-02, K2, namespace draft §23.13): the stop
        wrote `stopping` over the wait, and the `run.ended` that followed began
        it again from nought - every stop of a cue holding its post-wait cost it
        the whole post-wait once more. */
    for (const auto* how : { "run.stopAll", "run.stop" })
    {
        INFO (std::string (how));
        Rig rig;
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "30");

        rig.setStandby (rig.memoId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto memoRun = rig.runOf (rig.memoId);
        REQUIRE_FALSE (memoRun.empty());
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (memoRun)->state == cue::runState::postWait; }, 10));

        if (std::string (how) == "run.stop")
            REQUIRE (rig.submitAndTick (how, { osc::Value::string (memoRun) }).rejected == 0);
        else
            REQUIRE (rig.submitAndTick (how).rejected == 0);

        CHECK (rig.tickUntil ([&] { return rig.runs.find (memoRun)->isFinished(); }, 10));
    }
}

TEST_CASE ("stop levels: a cue's authored ending keeps its target's post-wait, and an abort ends it")
{
    /*  THE LINE BETWEEN THE TWO (2026-10-02, K2's review, namespace draft
        §23.13, JX): an ABORT - Esc, a double Esc, the pane's stop, and the
        stop a stopping scene's job sends each member - ends a post-wait and
        begins none; a cue's AUTHORED ENDING - a stop cue aimed at a cue, a
        fade that ends in a stop - is how that cue ends in the show, and its
        post-wait is still the gap the designer wrote after it (PRD §3.6:
        "pre-wait and post-wait win"). And whatever the stop, a post-wait that
        had begun is never begun again from nought. */
    SUBCASE ("a stop cue fading a sequence member: the next member follows its post-wait after the fade's end")
    {
        GroupRig rig;

        const auto bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
        rig.setCue (bed, "file", "bed.wav");
        rig.setCue (bed, "postWait", "3");                // 150 ticks

        const auto fadeOut = rig.document.createCue (rig.listId, 3, "transport", "Fade the bed").id;
        rig.setCue (fadeOut, "target", bed);
        rig.setCue (fadeOut, "verb", "fade");
        rig.setCue (fadeOut, "duration", "0.2");          // ten ticks

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

        rig.audio.completeArms (rig.engine);
        REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

        const auto bedRun = rig.runOf (bed);
        rig.audio.playing.insert (rig.runs.find (bedRun)->track);
        rig.tickOnce();
        rig.tickOnce();

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (fadeOut) }).rejected == 0);

        //  The fade lands its stop, and the bed holds its post-wait.
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (bedRun)->state == cue::runState::postWait; }, 40));

        int ticks = 0;

        while (rig.runOf (rig.first).empty() && ticks < 300)
        {
            rig.tickOnce();
            ++ticks;
        }

        CHECK (ticks >= 145);
        CHECK (ticks < 170);
    }

    SUBCASE ("a stop cue aimed at a scene in an act: its members' waits end, its footer runs, its own post-wait spaces the next")
    {
        GroupRig rig;

        const auto act = rig.document.createCue (rig.listId, 3, "group", "Act").id;
        rig.setCue (act, "advance", "auto");

        const auto scene = rig.document.createCue (act, 0, "group", "Scene one").id;
        rig.setCue (scene, "advance", "auto");
        rig.setCue (scene, "postWait", "1");              // fifty ticks before the next scene

        const auto hold = rig.document.createCue (scene, 0, "memo", "Hold").id;
        rig.setCue (hold, "preWait", "10");
        rig.setCue (hold, "postWait", "30");

        const auto release = rig.document.createCue (rig.roleOf (scene, "footer"), 0, "memo", "Release").id;
        const auto next = rig.document.createCue (act, 1, "memo", "Scene two").id;

        const auto abort = rig.document.createCue (rig.listId, 4, "transport", "Leave scene one").id;
        rig.setCue (abort, "target", scene);

        rig.setStandby (act);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (hold).empty(); }));

        const auto holdRun = rig.runOf (hold);

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (abort) }).rejected == 0);

        //  The member's wait goes and the footer runs at once...
        CHECK (rig.tickUntil ([&] { return rig.runs.find (holdRun)->isFinished(); }, 20));
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (release).empty(); }, 20));

        //  ...and the scene's own post-wait still spaces the next scene.
        int ticks = 0;

        while (rig.runOf (next).empty() && ticks < 200)
        {
            rig.tickOnce();
            ++ticks;
        }

        CHECK (ticks >= 45);
        CHECK (ticks < 70);
    }

    SUBCASE ("a stop cue aimed at a cue holding its post-wait leaves the wait where it was")
    {
        Rig rig;
        rig.document.setAttribute ("/godot/cue/" + rig.memoId + "/postWait", "1");   // fifty ticks

        const auto stopId = rig.document.createCue (rig.listId, 2, "transport", "Stop it").id;
        rig.document.setAttribute ("/godot/cue/" + stopId + "/target", rig.memoId);

        rig.setStandby (rig.memoId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto memoRun = rig.runOf (rig.memoId);
        REQUIRE_FALSE (memoRun.empty());
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (memoRun)->state == cue::runState::postWait; }, 10));

        for (int n = 0; n < 20; ++n)
            rig.tickOnce();

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) }).rejected == 0);

        //  Thirty ticks of the fifty were left: not ended now, not fifty more.
        CHECK_FALSE (rig.tickUntil ([&] { return rig.runs.find (memoRun)->isFinished(); }, 20));
        CHECK (rig.tickUntil ([&] { return rig.runs.find (memoRun)->isFinished(); }, 20));
    }

    SUBCASE ("guard: in an automatic sequence a member's post-wait spaces the next member")
    {
        GroupRig rig;
        rig.setCue (rig.first, "postWait", "0.4");        // twenty ticks

        rig.setStandby (rig.groupId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&]
        {
            const auto id = rig.runOf (rig.first);
            return ! id.empty() && rig.runs.find (id)->state == cue::runState::postWait;
        }));

        int ticks = 0;

        while (rig.runOf (rig.second).empty() && ticks < 100)
        {
            rig.tickOnce();
            ++ticks;
        }

        CHECK (ticks >= 18);
        CHECK (ticks < 30);
    }
}

TEST_CASE ("stop levels: a fade-and-stop on a scene is a fade, and the scene stops at its end")
{
    /*  THE AUTHOR'S RULING (2026-10-02, K3, namespace draft §23.14): "Fade and
        stop is a fade behaviour, not a stop feature that just stops (microfade
        to prevent a click only)." A stop cue with the `fade` verb aimed at a
        scene made the scene `stopping` from its first tick, and the scene's
        job stopped its members on the next: only the scene's own level moved
        over the duration, under members that had already gone and a footer
        already running. Now the scene plays on as it would - members
        sounding, its sequence carrying on - while its level fades, and at the
        fade's end it is stopped the way a stop of the scene stops it: members
        brought down, footer run once. `hard` stays a stop with only the
        anti-click. Esc, a double Esc and the pane's stop are aborts and do not
        wait for the fade.

        THE SCENE: an automatic sequence [Bed (a sounding media cue), One, Two,
        Three] with a footer [Release], and a stop cue aimed at it. */
    struct Scene
    {
        GroupRig rig;
        std::string bed, release, stopId, groupRun, bedRun;
        int voice = -1;

        Scene (const char* verb, const char* seconds)
        {
            bed = rig.document.createCue (rig.groupId, 0, "media", "Bed").id;
            rig.setCue (bed, "file", "bed.wav");

            release = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

            stopId = rig.document.createCue (rig.listId, 3, "transport", "Preshow out").id;
            rig.setCue (stopId, "target", rig.groupId);
            rig.setCue (stopId, "verb", verb);
            rig.setCue (stopId, "duration", seconds);
        }

        /** GO on the scene, ticked on until the bed sounds. */
        void start()
        {
            rig.setStandby (rig.groupId);
            REQUIRE (rig.submitAndTick ("go").rejected == 0);
            REQUIRE (rig.tickUntil ([this] { return ! rig.runOf (bed).empty(); }));

            rig.audio.completeArms (rig.engine);
            REQUIRE (rig.tickUntil ([this] { return ! rig.audio.launches.empty(); }));

            bedRun = rig.runOf (bed);
            voice = rig.runs.find (bedRun)->track;
            REQUIRE (voice >= 0);

            rig.audio.playing.insert (voice);
            rig.tickOnce();
            rig.tickOnce();

            groupRun = rig.runOf (rig.groupId);
            REQUIRE (rig.runs.find (bedRun)->state == cue::runState::playing);
        }

        void fire (const std::string& cueId)
        {
            REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (cueId) }).rejected == 0);
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                rig.tickOnce();
        }

        int runsOf (const std::string& cueId) const
        {
            return static_cast<int> (std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                                    [&cueId] (const cue::Run& run) { return run.cue == cueId; }));
        }

        bool bedStopped() const
        {
            return std::find (rig.audio.stopped.begin(), rig.audio.stopped.end(), voice) != rig.audio.stopped.end();
        }

        bool bedFinished() const
        {
            return rig.runs.find (bedRun)->isFinished();
        }
    };

    SUBCASE ("over two seconds: at one second the scene still plays, its level half way down; at the end it stops, the footer once")
    {
        Scene scene ("fade", "2");
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (50);

        const auto* group = scene.rig.runs.find (scene.groupRun);
        INFO ("the scene's level at one second: " << group->ownLevel << " dB");

        CHECK (scene.rig.runs.find (scene.bedRun)->state == cue::runState::playing);
        CHECK_FALSE (scene.bedStopped());
        CHECK (group->ownLevel < -50.0);
        CHECK (group->ownLevel > -70.0);
        CHECK (scene.runsOf (scene.release) == 0);

        //  The second half: stopped at the fade's end, not before, and silent first.
        int ticks = 0;

        while (! scene.bedFinished() && ticks < 120)
        {
            scene.rig.tickOnce();
            ++ticks;
        }

        CHECK (ticks >= 45);
        CHECK (ticks < 60);
        CHECK (scene.bedStopped());
        CHECK (scene.rig.audio.kills.empty());
        CHECK (scene.rig.runs.find (scene.groupRun)->ownLevel < -119.0);

        CHECK (scene.rig.runToCompletion (scene.groupRun, 40) < 40);
        CHECK (scene.runsOf (scene.release) == 1);
        CHECK (scene.rig.runOf (scene.rig.first).empty());    // the sequence ends where the fade did
    }

    SUBCASE ("guard: a hard stop on the same scene still stops it at once")
    {
        Scene scene ("hard", "2");
        scene.start();
        scene.fire (scene.stopId);

        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 5));
        CHECK (scene.bedStopped());
        CHECK (scene.rig.runToCompletion (scene.groupRun, 10) < 10);
        CHECK (scene.runsOf (scene.release) == 1);
        CHECK (scene.rig.runOf (scene.rig.first).empty());
    }

    SUBCASE ("Esc during the fade brings the scene down the Esc way, without waiting for the fade")
    {
        Scene scene ("fade", "4");
        REQUIRE (scene.rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (50);

        CHECK (scene.rig.runs.find (scene.bedRun)->state == cue::runState::playing);

        REQUIRE (scene.rig.submitAndTick ("run.stopAll").rejected == 0);

        //  The bed is Esc's: faded over the panic fade, an abort, stopped at its end.
        CHECK (scene.rig.runs.find (scene.bedRun)->stopEndsWait);
        CHECK_FALSE (scene.bedStopped());
        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 60));
        CHECK (scene.rig.audio.kills.empty());

        //  The scene down and its footer run, well inside the stop cue's four seconds.
        CHECK (scene.rig.runToCompletion (scene.groupRun, 20) < 20);
        CHECK (scene.runsOf (scene.release) == 1);
        CHECK (scene.rig.runOf (scene.rig.first).empty());
    }

    SUBCASE ("the scene finishing on its own during the fade ends the way it would have")
    {
        Scene scene ("fade", "4");
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (20);

        scene.rig.audio.playing.erase (scene.voice);      // the bed reaches its end

        //  The sequence carries on - One, Two, Three - and the footer runs once.
        CHECK (scene.rig.runToCompletion (scene.groupRun, 100) < 100);
        CHECK_FALSE (scene.rig.runOf (scene.rig.first).empty());
        CHECK_FALSE (scene.rig.runOf (scene.rig.third).empty());
        CHECK (scene.runsOf (scene.release) == 1);
        CHECK_FALSE (scene.bedStopped());                 // it ended; nothing stopped it

        //  And the stop cue's own run ends when its fade does.
        const auto stopRun = scene.rig.runOf (scene.stopId);
        REQUIRE_FALSE (stopRun.empty());
        CHECK (scene.rig.tickUntil ([&] { return scene.rig.runs.find (stopRun)->isFinished(); }, 200));
        CHECK (scene.runsOf (scene.release) == 1);
    }

    SUBCASE ("a fade cue that stops when done treats a scene the same way")
    {
        Scene scene ("fade", "2");
        const auto fadeId = scene.rig.document.createCue (scene.rig.listId, 4, "fade", "Fade it out").id;
        scene.rig.setCue (fadeId, "target", scene.rig.groupId);
        scene.rig.setCue (fadeId, "level", "-120");
        scene.rig.setCue (fadeId, "duration", "2");
        scene.rig.setCue (fadeId, "stopWhenDone", "true");

        scene.start();
        scene.fire (fadeId);
        scene.ticks (50);

        CHECK (scene.rig.runs.find (scene.bedRun)->state == cue::runState::playing);
        CHECK_FALSE (scene.bedStopped());

        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 60));
        CHECK (scene.rig.runToCompletion (scene.groupRun, 40) < 40);
        CHECK (scene.runsOf (scene.release) == 1);
    }

    SUBCASE ("a double Esc during the fade cuts the scene at once, and runs no footer")
    {
        Scene scene ("fade", "4");
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (50);

        REQUIRE (scene.rig.submitAndTick ("run.killAll").rejected == 0);

        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 5));
        CHECK (std::find (scene.rig.audio.kills.begin(), scene.rig.audio.kills.end(), scene.voice)
                 != scene.rig.audio.kills.end());
        CHECK (scene.rig.runToCompletion (scene.groupRun, 10) < 10);
        CHECK (scene.runsOf (scene.release) == 0);
    }

    SUBCASE ("the pane's stop on the scene during the fade stops it at once, footer and all")
    {
        Scene scene ("fade", "4");
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (50);

        REQUIRE (scene.rig.submitAndTick ("run.stop", { osc::Value::string (scene.groupRun) }).rejected == 0);

        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 5));
        CHECK (scene.rig.runToCompletion (scene.groupRun, 10) < 10);
        CHECK (scene.runsOf (scene.release) == 1);
    }

    SUBCASE ("pinned: the stop cue's own run killed during the fade lands the scene's stop at once")
    {
        /*  A fade-and-stop whose own run is killed hands a CUE back to
            `playing` (the rule in `advanceFades`); a scene it never hands
            back (EK), so the stop it was asked lands now, gracefully. */
        Scene scene ("fade", "4");
        scene.start();
        scene.fire (scene.stopId);
        scene.ticks (50);

        const auto stopRun = scene.rig.runOf (scene.stopId);
        REQUIRE (scene.rig.submitAndTick ("run.kill", { osc::Value::string (stopRun) }).rejected == 0);

        CHECK (scene.rig.tickUntil ([&] { return scene.bedFinished(); }, 5));
        CHECK (scene.rig.runToCompletion (scene.groupRun, 10) < 10);
        CHECK (scene.runsOf (scene.release) == 1);
    }

    SUBCASE ("an abort on an outer scene reaches a scene a stop cue is fading inside it")
    {
        /*  The fade's hold lets go under ANY abort above the faded scene, not
            only on it. Esc stops the stop cue's own run too, which lets the
            hold go by itself; the pane's stop on the outer scene leaves the
            stop cue running, and the outer scene's job leaves a member already
            on its way out alone - so without the walk up, the inner scene
            played on to the fade's end, its sequence launching members after
            the press. */
        for (const auto* press : { "run.stopAll", "run.stop" })
        {
            INFO (press);

            GroupRig rig;
            REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

            const auto inner = rig.document.createCue (rig.groupId, 0, "group", "Inner").id;
            rig.setCue (inner, "advance", "auto");

            const auto bed = rig.document.createCue (inner, 0, "media", "Bed").id;
            rig.setCue (bed, "file", "bed.wav");
            const auto after = rig.document.createCue (inner, 1, "memo", "After").id;

            const auto innerRelease = rig.document.createCue (rig.roleOf (inner, "footer"), 0, "memo", "Inner release").id;
            const auto outerRelease = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Outer release").id;

            const auto stopId = rig.document.createCue (rig.listId, 3, "transport", "Inner out").id;
            rig.setCue (stopId, "target", inner);
            rig.setCue (stopId, "verb", "fade");
            rig.setCue (stopId, "duration", "4");

            rig.setStandby (rig.groupId);
            REQUIRE (rig.submitAndTick ("go").rejected == 0);
            REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bed).empty(); }));

            rig.audio.completeArms (rig.engine);
            REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

            const auto bedRun = rig.runOf (bed);
            const auto voice = rig.runs.find (bedRun)->track;
            REQUIRE (voice >= 0);
            rig.audio.playing.insert (voice);
            rig.tickOnce();
            rig.tickOnce();

            const auto outerRun = rig.runOf (rig.groupId);

            REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopId) }).rejected == 0);

            for (int n = 0; n < 50; ++n)
                rig.tickOnce();

            CHECK (rig.runs.find (bedRun)->state == cue::runState::playing);

            const auto outcome = std::string (press) == "run.stop"
                                   ? rig.submitAndTick ("run.stop", { osc::Value::string (outerRun) })
                                   : rig.submitAndTick ("run.stopAll");
            REQUIRE (outcome.rejected == 0);

            CHECK (rig.tickUntil ([&] { return rig.runs.find (bedRun)->isFinished(); }, 60));
            CHECK (rig.runToCompletion (outerRun, 30) < 30);
            CHECK (rig.runOf (after).empty());                // the inner sequence did not carry on
            CHECK_FALSE (rig.runOf (innerRelease).empty());
            CHECK_FALSE (rig.runOf (outerRelease).empty());
            CHECK (rig.audio.kills.empty());
        }
    }
}

//==============================================================================
/*  DOUBLE ESC DROPS WHAT WAS STILL TO COME (2026-10-02, H4, namespace draft
    §23.10): nothing a killed run had in hand starts after the press. */
TEST_CASE ("double Esc: a cue whose disk answers as the press lands is never launched")
{
    /*  The disk's answer is a record queued ahead of the press, so the run is
        ready in the very drain that kills it - and the launch loop, which runs
        on the next tick before the kill reaches the voice, asked only whether
        the run was finished. It launched a cue the press had dropped, placed
        its speed, and only then was the voice cut. */
    FadeRig rig;
    rig.fire (rig.mediaId);
    REQUIRE_FALSE (rig.audio.arms.empty());
    const auto run = rig.runs.all().front().id;

    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);

    REQUIRE (rig.runs.find (run)->state == cue::runState::stopping);
    REQUIRE (rig.runs.find (run)->armConfirmed);
    REQUIRE (rig.runs.find (run)->skipFooter);

    const auto launches = rig.audio.launches.size();
    const auto ratePoints = rig.audio.ratePoints.size();

    rig.tickOnce();

    CHECK (rig.audio.launches.size() == launches);
    CHECK (rig.audio.ratePoints.size() == ratePoints);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (run)->isFinished(); }, 5));
}

TEST_CASE ("double Esc: a start cue's target fires nothing after the press")
{
    /*  A start cue's fire of its target is submitted by the next tick's hook.
        So a start cue fired in the press's drain - before the press - fired its
        target a tick after the press that drops every action; and one fired the
        tick before had its fire submitted by the press's own tick's hook, which
        drained behind the press and made a fresh run nothing had marked (the
        review, 2026-10-02). */
    Rig rig;
    const auto start = rig.document.createCue (rig.listId, 2, "start", "Go the memo").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + start + "/target", rig.memoId).ok);

    auto pressed = true;
    auto tickBefore = false;

    SUBCASE ("fired in the press's own drain") {}
    SUBCASE ("fired the tick before: its fire drains behind the press") { tickBefore = true; }
    SUBCASE ("without the press, the target fires - the case is not empty") { pressed = false; }

    REQUIRE (rig.engine.submit ("cli", "cue.fire", { osc::Value::string (start) }));

    if (tickBefore)
        rig.tickOnce();

    if (pressed)
        REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));

    rig.tickOnce();
    REQUIRE_FALSE (rig.runOf (start).empty());

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (rig.memoId).empty() == pressed);
}

//==============================================================================
/*  THE LEAST TIME BETWEEN TWO GOs (PRD §3.7's GO debounce, a show setting
    since 2026-09-28, author: "a 'time between' Go's"). */
TEST_CASE ("go: inside the least time between two GOs a GO is refused, and the standby does not move")
{
    Rig rig;
    const auto third = rig.document.createCue (rig.listId, 2, "memo", "Blackout").id;
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0.5").ok);

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.standby() == rig.memoId);

    /*  A bounce, two ticks later: refused in words, and nothing moved. */
    rig.tickOnce();
    const auto bounced = rig.submitAndTick ("go");
    CHECK (bounced.applied == 0);
    CHECK (rig.engine.lastError().find ("too-soon") != std::string::npos);
    CHECK (rig.standby() == rig.memoId);
    CHECK (rig.runOf (rig.memoId).empty());

    /*  Measured from the GO that FIRED, not from the one refused: half a
        second after the first press, the next one fires what the bounce
        would have. */
    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").applied == 1);
    CHECK_FALSE (rig.runOf (rig.memoId).empty());
    CHECK (rig.standby() == third);
}

TEST_CASE ("go: a show that says nothing waits half a second between GOs, and nought lets two on one tick both fire")
{
    //  The author's default (2026-09-28): a show that says nothing is protected.
    const doc::ShowDocument fresh;
    CHECK (fresh.getAttribute ("/godot/list/goDebounce") == std::optional<std::string> ("0.5"));

    Rig rig;                               // which says nought, as a script would
    rig.setStandby (rig.mediaId);
    REQUIRE (rig.engine.submit ("cli", "go", {}));
    REQUIRE (rig.engine.submit ("cli", "go", {}));
    CHECK (rig.tickOnce().applied == 2);
    CHECK_FALSE (rig.runOf (rig.memoId).empty());
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

TEST_CASE ("go: from inside a group the machine parents, it fires the one cue and not the scene")
{
    /*  WHAT THE AUTHOR WILL DO FIRST after 2026-09-16, and therefore the case
        that has to be right before anything else about that day is.

        They asked, with the page open, to be able to "select a cue within a
        group individually to start from this level" - and named timeline groups
        specifically: "even start all cues timelines should move the standby
        pointer from one cue to the next to try each individual cue it
        contains". Asked what GO should then do, they decided: GO fires the ONE
        cue the pointer is on, and starting the whole group FROM there is a
        second named gesture, which is a later round's.

        So this is the try-one-cue gesture, end to end: park on the middle
        member of a scene the machine parents - which the write door refused
        outright until that day - press GO, and get that cue and nothing else.

        THE FAILURE IT IS GUARDING AGAINST IS THE SCENE STARTING. Entering a
        timeline group schedules every member at entry (§3.6), so an
        implementation that let the press descend into the group would give the
        operator the whole scene when they asked for one line of it - and would
        do it while they were trying to check that one line in a tech rehearsal,
        which is the worst possible moment to be handed a cue they did not ask
        for. The automatic sequence is the same mistake read one member at a
        time. */
    Rig rig;

    /*  Three memos, because this is about WHAT RAN and not about sound, and a
        memo's run finishes on the tick after it fires. A cue after the group so
        that "the pointer stayed inside the scene" is distinguishable from "the
        pointer left it". */
    const auto scene = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
    const auto one = rig.document.createCue (scene, 0, "memo", "One").id;
    const auto two = rig.document.createCue (scene, 1, "memo", "Two").id;
    const auto three = rig.document.createCue (scene, 2, "memo", "Three").id;

    rig.document.createCue (rig.listId, 3, "memo", "After");

    SUBCASE ("a timeline group")
    {
        rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline");
    }

    SUBCASE ("an automatic sequence")
    {
        rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto");
    }

    /*  PARKED ON THE MIDDLE MEMBER. This line is the whole of what changed:
        `setStandby` writes through the document's own door, and until
        2026-09-16 that door refused this outright. */
    rig.setStandby (two);
    REQUIRE (rig.standby() == two);

    CHECK (rig.submitAndTick ("go").applied >= 1);

    /*  THE SCENE DID NOT ENTER, asked the moment the press lands, because a
        group that entered is `playing` from that tick.

        `liveRunOf` and not "has no run at all": the pointer sitting inside a
        scene is a horizon (§3.12), so the block may well have been PREPARED,
        and a preparation is a promise rather than a performance - which is
        exactly the distinction `liveRunOf` is there to draw. What must not have
        happened is that promise being adopted and the scene started. */
    CHECK (rig.runs.liveRunOf (scene) == nullptr);

    /*  And the pointer moved to the next MEMBER, not past the whole chain.
        §3.5's "positionally after the automated chain" is what GO on the
        group's own row does; from inside, the next press is the next line the
        operator wants to hear. */
    CHECK (rig.standby() == three);

    // The cue the pointer was on ran.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (two).empty(); }));

    /*  AND NOTHING ELSE DID, given long enough that it would have. A timeline
        that entered would have scheduled the third member at entry; an
        automatic sequence would have advanced to it as soon as the second was
        done. Forty ticks is most of a second at this rate, and both memos
        finish in one. */
    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (one) == "");
    CHECK (rig.runOf (three) == "");
    CHECK (rig.runs.liveRunOf (scene) == nullptr);
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

TEST_CASE ("manual group: every run a press creates is in a record, and the horizon draws them first")
{
    /*  ONE PRESS CAN CREATE SEVERAL RUNS - the groups between the pointer and
        the list, then the member - so the guarantee is not "one identifier per
        record" but "no identifier a replay has to invent". Both the records
        that make runs are variadic for that reason.

        AND THE HORIZON DRAWS THEM FIRST, which is PR 4.5's change to this and
        is why the test says what it says. The pointer landing inside a scene
        prepares it: both groups are created THEN, by `run.prepare`, which
        carries their identifiers - and GO, arriving at a block that is already
        there, adopts rather than creates and carries none. The same runs, the
        same order, in the earlier record. */
    ManualRig rig;

    // A manual group inside the manual group: two levels to prepare at once.
    const auto inner = rig.document.createCue (rig.groupId, 0, "group", "Inner").id;
    const auto deep = rig.document.createCue (inner, 0, "memo", "Deep").id;

    rig.setStandby (deep);
    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto found = [&parsed] (const char* name)
    {
        return std::find_if (parsed.records.begin(), parsed.records.end(),
                             [name] (const auto& record) { return record.command == name; });
    };

    const auto prepare = found ("run.prepare");
    const auto go = found ("go");

    REQUIRE (prepare != parsed.records.end());
    REQUIRE (go != parsed.records.end());

    /*  The cue the horizon was asked about, then the two group runs it made:
        the outer one and the inner one, outermost first. The member's own run
        is spawned by the inner group's job after its header, so it carries its
        identifier in a `run.spawn` record instead. */
    REQUIRE (prepare->args.size() == 3u);
    CHECK (prepare->args[0].getString() == deep);

    for (std::size_t n = 1; n < prepare->args.size(); ++n)
        CHECK (rig.runs.find (prepare->args[n].getString()) != nullptr);

    CHECK (rig.runs.find (prepare->args[1].getString())->cue == rig.groupId);
    CHECK (rig.runs.find (prepare->args[2].getString())->cue == inner);

    /*  AND GO CREATED NOTHING, because there was nothing left to create. */
    CHECK (go->args.empty());

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
    for (const auto* pair : { &*go, &*prepare })
    {
        const auto* command = rig.engine.commands().find (pair->command);
        REQUIRE (command != nullptr);

        const auto check = CommandRegistry::checkArgs (*command, pair->args);
        CHECK (check.ok);
        CHECK (check.reason == "");
    }
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

//==============================================================================
/*  THE HORIZON: getting a scene ready before anybody presses anything.

    PRD §3.12. The pointer landing on a group, or on a member inside one, is the
    moment to prepare the whole block - its headers, its slots, its first
    sounds - rather than the one row the pointer is on. What that buys is the
    thing arming ahead has always bought, at the scale of a scene: the disk is
    paid while the operator reads the next line rather than after their hand
    comes down.

    AND IT IS ONLY AS GOOD AS ITS REVOCATION (§13.1), which is why the two are
    one PR and one set of cases. A horizon that could not give a scene back
    would hold voices and processor inputs for every scene the pointer passed
    over, in a mechanism whose entire subject is that those are scarce.

    A PREPARED RUN IS NOT LIVE, and most of what follows is that sentence tested
    from a different side each time: the pointer does not wrap into it, a stop
    does not act on it, GO adopts it rather than starting a second one, and its
    footer does not run because it has not finished anything.
*/
namespace
{
    /*  The manual scene, with a media member to hold a voice, a header cue that
        cannot be prepared, and a footer to prove it does not run. */
    struct PrepareRig : ManualRig
    {
        PrepareRig()
        {
            sound = document.createCue (groupId, 0, "media", "Thunder").id;
            document.setAttribute ("/godot/cue/" + sound + "/file", "thunder.wav");

            const auto header = roleOf (groupId, "header");
            opening = document.createCue (header, 0, "memo", "House to half").id;

            const auto footer = roleOf (groupId, "footer");
            closing = document.createCue (footer, 0, "memo", "Release").id;
        }

        const cue::Run* prepared (const std::string& cueId) const
        {
            return runs.preparedRunOf (cueId);
        }

        std::string sound, opening, closing;
    };
}

TEST_CASE ("prepare: the pointer landing in a scene gets it ready, and the scene is not running")
{
    /*  §3.12's horizon, and the sentence the rest of these depend on. */
    PrepareRig rig;

    rig.setStandby (rig.sound);
    rig.tickOnce();                       // the hook submits; the handler applies

    const auto* ready = rig.prepared (rig.groupId);
    REQUIRE (ready != nullptr);

    CHECK (ready->state == cue::runState::preparing);
    CHECK (ready->parent.empty());

    /*  AND NOTHING IS RUNNING. Five callers ask `liveRunOf` whether a cue is
        going, and every one of them would do the wrong thing if a promise
        looked like a performance. */
    CHECK (rig.runs.liveRunOf (rig.groupId) == nullptr);

    /*  THE ROW SAYS HOW FAR AHEAD IT HAS BEEN GOT, and here it says `partial`,
        which is the honest answer rather than a shortfall. §3.12 decides
        preparability per PARAMETER and not per cue: this scene's header is a
        memo, a memo cannot be got ready ahead of anything, and it will run at
        entry like any other header cue. A block that is partly anticipatable
        says so instead of pretending - which is §3.6's own word for it. */
    CHECK (std::string (ready->prepare) == cue::preparedness::partial);
}

TEST_CASE ("prepare: the horizon arms the first member under the block it prepared")
{
    /*  `armablesFor` is the lookahead standby has done since PR 3.13, asked of
        the pointer's own cue and PARENTED. Under the block, so that a
        revocation reaches it by following `children` rather than by going
        looking for it. */
    PrepareRig rig;

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto* member = rig.runs.find (rig.runOf (rig.sound));
    REQUIRE (member != nullptr);
    REQUIRE (rig.prepared (rig.groupId) != nullptr);

    CHECK (member->parent == rig.prepared (rig.groupId)->id);
    CHECK (member->state == cue::runState::armed);
    CHECK (member->track >= 0);                 // a voice, held ahead
    CHECK_FALSE (member->launchRequested);      // and no sound
}

TEST_CASE ("prepare: a prepared group holds, and runs no footer")
{
    /*  THE HOLD PHASE, AND WHY IT IS NOT DECORATION. `finishPhase` moves a
        group on to the next phase with anything in it and ends the run when
        none has; there is no branch in it that waits. A prepared job falling
        through it would run the group's footer and end the scene before
        anybody pressed GO. */
    PrepareRig rig;

    rig.setStandby (rig.sound);

    for (int n = 0; n < 60; ++n)
        rig.tickOnce();

    const auto* ready = rig.prepared (rig.groupId);
    REQUIRE (ready != nullptr);

    CHECK_FALSE (ready->isFinished());
    CHECK (rig.runOf (rig.closing) == "");
    CHECK (rig.runOf (rig.opening) == "");      // the header waits for entry too
}

TEST_CASE ("prepare: GO adopts the block rather than starting a second one")
{
    /*  The failure this prevents is a scene with two schedulers: one run
        holding the voices and another launching every member beside it, a tick
        apart, under one press. */
    PrepareRig rig;

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto preparedId = rig.prepared (rig.groupId)->id;
    const auto armedId = rig.runOf (rig.sound);

    CHECK (rig.submitAndTick ("go").applied == 1);

    const auto runsFor = [&rig] (const std::string& cueId)
    {
        return std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                              [&cueId] (const cue::Run& run) { return run.cue == cueId; });
    };

    CHECK (runsFor (rig.groupId) == 1);

    const auto* live = rig.runs.liveRunOf (rig.groupId);
    REQUIRE (live != nullptr);
    CHECK (live->id == preparedId);             // the same run, now playing
    CHECK (live->state == cue::runState::playing);

    /*  AND THE HEADER STILL RUNS AT ENTRY, because a memo is not anticipatable
        and its preparation was never its execution. */
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.opening).empty(); }));
    CHECK (runsFor (rig.opening) == 1);

    /*  Then the member, on the run the horizon armed - not a second one. */
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armedId)->launchRequested; }));
    CHECK (runsFor (rig.sound) == 1);
}

TEST_CASE ("prepare: the pointer moving away gives the voices and the slots back")
{
    /*  §13.1: anticipation is only as good as its revocation. A scene got ready
        and then left behind holds a voice for a cue nobody is about to fire,
        and before this nothing ended an armed, never-launched run at all. */
    PrepareRig rig;

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto preparedId = rig.prepared (rig.groupId)->id;
    const auto armedId = rig.runOf (rig.sound);
    const auto voice = rig.runs.find (armedId)->track;

    REQUIRE (voice >= 0);
    REQUIRE (rig.runs.isTrackBusy (voice));

    /*  Away, to a cue outside the block. */
    rig.setStandby (rig.after);
    rig.tickOnce();

    CHECK (rig.runs.find (preparedId)->isFinished());
    CHECK (rig.runs.find (preparedId)->warning == cue::runWarning::revoked);

    /*  AND THE MEMBER WITH IT, children before parents. */
    CHECK (rig.runs.find (armedId)->isFinished());
    CHECK (rig.runs.find (armedId)->warning == cue::runWarning::revoked);

    /*  THE VOICE IS FREE, which is the whole point: `holdsTrack()` is a track
        and an UNFINISHED run, so ending it is the whole of letting go. */
    CHECK_FALSE (rig.runs.isTrackBusy (voice));

    /*  A REVOCATION IS NOT A FAILURE. Nothing went wrong: a scene was got ready
        and then not wanted, which is what anticipation is allowed to cost. */
    CHECK (rig.runs.find (preparedId)->error.empty());
    CHECK (rig.runs.find (preparedId)->state == cue::runState::done);
}

TEST_CASE ("prepare: moving inside the same block prepares nothing twice and revokes nothing")
{
    /*  The pointer walking down a scene's members is inside one block the whole
        time. A horizon that rebuilt itself on every step would re-arm every
        voice in the scene each time the operator moved, and one that revoked on
        every step would give them all back a tick later. */
    PrepareRig rig;

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto preparedId = rig.prepared (rig.groupId)->id;

    rig.setStandby (rig.first);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.prepared (rig.groupId) != nullptr);
    CHECK (rig.prepared (rig.groupId)->id == preparedId);
    CHECK_FALSE (rig.runs.find (preparedId)->isFinished());
}

TEST_CASE ("prepare: a stop aimed at a prepared scene does nothing, because it is not running")
{
    /*  PRD §3.8 makes a stop aimed at something that is not running a silent
        no-op, and a prepared run must not turn that into an act. It is one of
        the five readings `liveRunOf` carries. */
    PrepareRig rig;

    const auto halt = rig.document.createCue (rig.listId, 4, "transport", "Halt").id;
    rig.setCue (halt, "target", rig.groupId);

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto preparedId = rig.prepared (rig.groupId)->id;

    CHECK (rig.submitAndTick ("cue.fire", { osc::Value::string (halt) }).applied == 1);
    rig.tickOnce();

    /*  Still preparing: the stop found nothing running and said nothing. */
    CHECK (rig.runs.find (preparedId)->state == cue::runState::preparing);
}

TEST_CASE ("prepare: a media cue at standby says armed on its own row")
{
    /*  The smallest horizon there is, and it has been here since PR 2.3 without
        a word for itself. `armed` is §13.6's vocabulary for a preparation with
        nothing to verify, and saying it on the row is the difference between an
        operator seeing that the next cue is ready and having to know that it
        always is. */
    Rig rig;

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));

    CHECK (std::string (rig.runs.find (rig.runOf (rig.mediaId))->prepare)
             == cue::preparedness::armed);

    /*  And it stops being a preparation when it is fired: `prepare` answers a
        question about the future, and a cue that is playing has none left. */
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (rig.runOf (rig.mediaId))
                                              ->launchRequested; }));

    CHECK (rig.runs.find (rig.runOf (rig.mediaId))->prepare.empty());
}

TEST_CASE ("prepare: a member armed ahead inside a running scene still fires when GO reaches it")
{
    /*  THE CASE WHERE ARMING AHEAD AND DECISION N MEET.

        Decision N ignores a GO on a media cue that is already sounding: the
        press is applied, the pointer has advanced, and the playing instance
        carries on. `armed` is NOT sounding, and the difference is the whole
        point of arming ahead - a cue armed at standby is sitting on a reserved
        voice with its file ready and no sound coming out, and GO is what turns
        that into a launch.

        `armInternal` has drawn that distinction since PR 3.3, for a cue at the
        top of a list. This is the same question one level in: the pointer is on
        member two of a scene that is already running, so the horizon armed it
        under the group's run, and the GO that reaches it has to launch THAT run
        rather than ignore it or start a second beside it. */
    ManualRig rig;

    const auto earlier = rig.document.createCue (rig.groupId, 0, "media", "First").id;
    const auto later = rig.document.createCue (rig.groupId, 1, "media", "Second").id;

    for (const auto& id : { earlier, later })
        rig.document.setAttribute ("/godot/cue/" + id + "/file", "thunder.wav");

    rig.setStandby (earlier);
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (earlier).empty()
                                          && rig.runs.find (rig.runOf (earlier))
                                                 ->launchRequested; }));

    //  The pointer moved to the second member and the horizon armed it, inside
    //  the scene.
    CHECK (rig.standby() == later);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (later).empty(); }));

    const auto armed = rig.runOf (later);
    CHECK (rig.runs.find (armed)->state == cue::runState::armed);
    CHECK_FALSE (rig.runs.find (armed)->launchRequested);

    //  And GO launches THAT run, not a second one.
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }));

    const auto runsFor = std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                        [&later] (const cue::Run& run)
                                        {
                                            return run.cue == later;
                                        });
    CHECK (runsFor == 1);
}

//==============================================================================
/*  A SCENE'S ROW INSIDE A RUNNING ACT (2026-10-01, namespace draft §23.9, J2).

    The case above, one level up. The pointer stands on the row of a scene that
    plays itself - a timeline, an automatic sequence - inside an act already
    running, and the horizon has made the scene ready under the act: its header
    run ahead, its first sounds armed. A GO on that row went down the member
    path, which spawned a SECOND run of the scene beside the block - none of the
    adoption doors was on that road - so the scene entered cold, its sounds armed
    again with the operator's hand already down, while the horizon's copy held
    its voices until the act ended, and after: the pointer moving away gave back
    only what stood at the top of a list. */
TEST_CASE ("prepare: a GO on a scene's row inside a running act adopts the scene made ready there, its sound armed once")
{
    ManualRig rig;

    const auto scene = rig.document.createCue (rig.groupId, 1, "group", "Storm").id;
    rig.setCue (scene, "mode", "timeline");
    const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "thunder.wav");

    //  Parked through the command, so the replay at the end is given the pointer too.
    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (rig.first) }).rejected == 0);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);              // into the act, on its first line
    REQUIRE (rig.standby() == scene);                              // the walk stands on a timeline's row

    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                          && ! rig.runOf (rain).empty(); }));

    const auto actRun = rig.runOf (rig.groupId);
    const auto block = rig.runs.preparedRunOf (scene)->id;
    const auto armed = rig.runOf (rain);

    REQUIRE (rig.runs.find (block)->parent == actRun);
    REQUIRE (rig.runs.find (armed)->parent == block);
    REQUIRE (rig.runs.find (armed)->track >= 0);                   // a voice, held ahead
    REQUIRE (rig.audio.arms.size() == 1u);

    const auto goTick = rig.tick;
    REQUIRE (rig.submitAndTick ("go").rejected == 0);              // the scene's row

    //  The sound the horizon armed is the one that launches...
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }, 20));

    /*  ...on the tick the cold road asked for its launch - the act's job
        launching the scene, the scene its sound - with the arm made already:
        the disk's 0.4 s is what the adoption saves, not a tick. */
    CHECK (rig.runs.find (armed)->launchRequestedAtTick - goTick <= 3);

    //  ...in the block it was armed in, which the act now plays: one scene, one sound, one arm.
    const auto* live = rig.runs.liveRunOf (scene);
    REQUIRE (live != nullptr);
    CHECK (live->id == block);
    CHECK (live->parent == actRun);

    const auto runsFor = [&rig] (const std::string& cueId)
    {
        return std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                              [&cueId] (const cue::Run& run) { return run.cue == cueId; });
    };

    CHECK (runsFor (scene) == 1);
    CHECK (runsFor (rain) == 1);
    CHECK (rig.audio.arms.size() == 1u);

    /*  AND THE SESSION REPLAYS RECORD FOR RECORD, with no audio side: the
        adoption is the `go` handler's, decided on what the log's own
        `run.prepare` made, so a replay adopts the same block and the record
        names nothing drawn for it. A net - the cold road replayed as faithfully. */
    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    ManualRig fresh;
    fresh.runner.setPlayer (nullptr);
    REQUIRE (doc::CanonicalXml::read (show, fresh.document).ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);
    REQUIRE (fresh.runs.find (block) != nullptr);
    CHECK (fresh.runs.find (block)->state == rig.runs.find (block)->state);
    CHECK (fresh.runs.find (block)->parent == actRun);
}

TEST_CASE ("prepare: the pointer leaving a scene made ready under a running act gives its voice back, and moving inside one keeps it")
{
    /*  §13.1's bargain one level down: a scene got ready under an act and then
        left behind holds a voice for a GO that is not coming. The give-back
        looked only at blocks with no parent, so this one kept its voice for as
        long as the act ran. What the pointer is still inside - a manual scene
        of the act, walked line by line - is wanted, and is kept. */
    SUBCASE ("away from a timeline's row: to a later line of the act, and out of the act")
    {
        for (const auto* where : { "a later line of the act", "a cue after the act" })
        {
            INFO (std::string (where));
            ManualRig rig;

            const auto scene = rig.document.createCue (rig.groupId, 1, "group", "Storm").id;
            rig.setCue (scene, "mode", "timeline");
            const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
            rig.setCue (rain, "file", "thunder.wav");

            rig.setStandby (rig.first);
            REQUIRE (rig.submitAndTick ("go").rejected == 0);
            REQUIRE (rig.standby() == scene);
            REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                                  && ! rig.runOf (rain).empty(); }));

            const auto block = rig.runs.preparedRunOf (scene)->id;
            const auto armed = rig.runOf (rain);
            const auto voice = rig.runs.find (armed)->track;
            REQUIRE (voice >= 0);

            rig.setStandby (std::string (where) == "a cue after the act" ? rig.after : rig.second);
            rig.tickOnce();

            CHECK (rig.runs.find (block)->isFinished());
            CHECK (rig.runs.find (block)->warning == cue::runWarning::revoked);
            CHECK (rig.runs.find (armed)->warning == cue::runWarning::revoked);
            CHECK_FALSE (rig.runs.isTrackBusy (voice));

            //  The act itself is left alone: it is running, and waits for its next GO.
            CHECK_FALSE (rig.runs.find (rig.runOf (rig.groupId))->isFinished());
        }
    }

    SUBCASE ("inside a manual scene of the act: from one of its lines to the next")
    {
        ManualRig rig;
        const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inner").id;
        const auto one = rig.document.createCue (inner, 0, "memo", "Line one").id;
        const auto two = rig.document.createCue (inner, 1, "memo", "Line two").id;

        rig.setStandby (rig.first);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == one);                            // the walk descends into a manual scene
        REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (inner) != nullptr; }));

        const auto block = rig.runs.preparedRunOf (inner)->id;
        REQUIRE (rig.runs.find (block)->parent == rig.runOf (rig.groupId));

        rig.setStandby (two);
        rig.tickOnce();

        REQUIRE (rig.runs.preparedRunOf (inner) != nullptr);
        CHECK (rig.runs.preparedRunOf (inner)->id == block);
        CHECK_FALSE (rig.runs.find (block)->isFinished());
    }
}

/*  THE GO'S OWN SCENE, WHEN THE SAME GO ENTERS ITS PARENT (2026-10-01, namespace
    draft §23.9, IC). The pointer on the row of a scene made ready inside a
    group no GO has entered yet - an act's first scene, or a manual scene's first
    line inside a running act - and the GO enters that group by adopting the
    block the horizon made of it. The scene's own block was left marked for the
    parent's job to ask for when its members begin, and the pointer, moved on by
    the GO, found it on the next tick: a scene nobody had asked for, given back
    with its sound before the parent reached it. The parent then launched a
    revoked run, and the scene the operator GO'd never played. */
TEST_CASE ("prepare: a GO that enters a scene's parent on the scene's row plays the scene made ready there")
{
    const auto runsFor = [] (const ManualRig& rig, const std::string& cueId)
    {
        return std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                              [&cueId] (const cue::Run& run) { return run.cue == cueId; });
    };

    const auto revokesOf = [] (ManualRig& rig, const std::string& runId)
    {
        const auto records = LogFile::parse (rig.engine.log().contents()).records;

        return std::count_if (records.begin(), records.end(),
                              [&runId] (const auto& record)
                              {
                                  return record.command == "run.revoke" && ! record.args.empty()
                                           && record.args[0].isString() && record.args[0].getString() == runId;
                              });
    };

    SUBCASE ("an act nobody had entered, the scene its first line")
    {
        /*  With a line in the act's header the horizon could not take ahead,
            the act spends the GO's next tick in its header and its members
            begin later; with none, they begin at once. Only the first held the
            fault, and the second is a net on the same road. */
        for (const auto withHeader : { true, false })
        {
            INFO (std::string (withHeader ? "a line in the act's header" : "nothing in the act's header"));
            ManualRig rig;

            std::string opening;

            if (withHeader)
                opening = rig.document.createCue (rig.roleOf (rig.groupId, "header"), 0, "memo", "House to half").id;

            const auto scene = rig.document.createCue (rig.groupId, 0, "group", "Storm").id;
            rig.setCue (scene, "mode", "timeline");
            const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
            rig.setCue (rain, "file", "thunder.wav");

            rig.setStandby (scene);
            REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                                  && ! rig.runOf (rain).empty(); }));

            const auto* act = rig.runs.preparedRunOf (rig.groupId);
            REQUIRE (act != nullptr);
            const auto actBlock = act->id;
            const auto block = rig.runs.preparedRunOf (scene)->id;
            const auto armed = rig.runOf (rain);
            REQUIRE (rig.runs.find (block)->parent == actBlock);
            REQUIRE (rig.runs.find (armed)->parent == block);

            REQUIRE (rig.submitAndTick ("go").rejected == 0);      // enters the act, on the scene's row
            REQUIRE (rig.standby() == rig.first);                  // and the walk goes on to the act's next line

            CHECK (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }));

            const auto* live = rig.runs.liveRunOf (scene);
            REQUIRE (live != nullptr);
            CHECK (live->id == block);
            CHECK (live->parent == actBlock);
            CHECK (rig.runs.find (block)->warning != cue::runWarning::revoked);
            CHECK (revokesOf (rig, block) == 0);
            CHECK (runsFor (rig, scene) == 1);
            CHECK (runsFor (rig, rain) == 1);
            CHECK (rig.audio.arms.size() == 1u);

            if (withHeader)
                CHECK (runsFor (rig, opening) == 1);               // the act's header ran first, once
        }
    }

    SUBCASE ("a manual scene inside a running act, a timeline its first line")
    {
        ManualRig rig;
        const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inner").id;
        const auto scene = rig.document.createCue (inner, 0, "group", "Storm").id;
        rig.setCue (scene, "mode", "timeline");
        const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
        rig.setCue (rain, "file", "thunder.wav");
        const auto line = rig.document.createCue (inner, 1, "memo", "Line").id;

        rig.setStandby (rig.first);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);          // into the act, on its first line
        REQUIRE (rig.standby() == scene);                          // the walk descends into the manual scene
        REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                              && ! rig.runOf (rain).empty(); }));

        const auto actRun = rig.runOf (rig.groupId);
        REQUIRE (rig.runs.preparedRunOf (inner) != nullptr);
        const auto innerBlock = rig.runs.preparedRunOf (inner)->id;
        const auto block = rig.runs.preparedRunOf (scene)->id;
        const auto armed = rig.runOf (rain);
        REQUIRE (rig.runs.find (innerBlock)->parent == actRun);
        REQUIRE (rig.runs.find (block)->parent == innerBlock);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);          // the timeline's row: enters the manual scene
        REQUIRE (rig.standby() == line);

        CHECK (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }));

        REQUIRE (rig.runs.liveRunOf (inner) != nullptr);
        CHECK (rig.runs.liveRunOf (inner)->id == innerBlock);

        const auto* live = rig.runs.liveRunOf (scene);
        REQUIRE (live != nullptr);
        CHECK (live->id == block);
        CHECK (live->parent == innerBlock);
        CHECK (rig.runs.find (block)->warning != cue::runWarning::revoked);
        CHECK (revokesOf (rig, block) == 0);
        CHECK (runsFor (rig, scene) == 1);
        CHECK (runsFor (rig, rain) == 1);
        CHECK (rig.audio.arms.size() == 1u);
    }
}

/*  ITS OWN PRE-WAIT, ONCE (2026-10-01, namespace draft §23.9, HY). A scene
    adopted on its row inside a running act is left for the act's job to
    launch, as every member is, and that launch is what runs a scene's
    pre-wait. Entered at the GO instead, the scene would start its members at
    once and the act's launch, arriving a tick later, would set it waiting over
    a scene already playing. */
TEST_CASE ("prepare: a scene with a pre-wait, adopted on its row inside a running act, waits it before it sounds")
{
    ManualRig rig;

    const auto scene = rig.document.createCue (rig.groupId, 1, "group", "Storm").id;
    rig.setCue (scene, "mode", "timeline");
    rig.setCue (scene, "preWait", "1");
    const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "thunder.wav");

    rig.setStandby (rig.first);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.standby() == scene);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                          && ! rig.runOf (rain).empty(); }));

    const auto block = rig.runs.preparedRunOf (scene)->id;
    const auto armed = rig.runOf (rain);

    const auto goTick = rig.tick;
    REQUIRE (rig.submitAndTick ("go").rejected == 0);              // the scene's row

    //  Nothing sounds while the scene waits its second, and the block is what waits.
    auto waited = false;

    for (int n = 0; n < 45; ++n)
    {
        rig.tickOnce();
        waited = waited || rig.runs.find (block)->state == cue::runState::waiting;
    }

    CHECK (waited);
    CHECK_FALSE (rig.runs.find (armed)->launchRequested);

    //  Then the sound the horizon armed, a second after the GO and not before.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }, 40));

    const auto after = rig.runs.find (armed)->launchRequestedAtTick - goTick;
    INFO ("launch requested " << after << " ticks after the GO");
    CHECK (after >= 50);
    CHECK (after <= 55);

    REQUIRE (rig.runs.liveRunOf (scene) != nullptr);
    CHECK (rig.runs.liveRunOf (scene)->id == block);
    CHECK (std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                          [&scene] (const cue::Run& run) { return run.cue == scene; }) == 1);
    CHECK (rig.audio.arms.size() == 1u);
}

/*  A SCENE INSIDE A BLOCK THAT IS ONLY MADE READY (2026-10-01, namespace draft
    §23.9, HZ). The give-back reaches a block under any parent, not only a
    running act: the pointer moving from one manual scene of an act nobody has
    entered to another gives the first scene back, the act's own block kept for
    the scene it is on now; and moving inside the scene keeps it. Until J2 such
    a scene was held until the act itself was given back. */
TEST_CASE ("prepare: the pointer moving between the scenes of an act nobody has entered gives back the scene it left")
{
    ManualRig rig;

    const auto sceneA = rig.document.createCue (rig.groupId, 1, "group", "Scene A").id;
    const auto soundA = rig.document.createCue (sceneA, 0, "media", "A one").id;
    rig.setCue (soundA, "file", "thunder.wav");
    const auto lineA = rig.document.createCue (sceneA, 1, "memo", "A two").id;

    const auto sceneB = rig.document.createCue (rig.groupId, 2, "group", "Scene B").id;
    const auto soundB = rig.document.createCue (sceneB, 0, "media", "B one").id;
    rig.setCue (soundB, "file", "thunder.wav");

    rig.setStandby (soundA);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (sceneA) != nullptr
                                          && ! rig.runOf (soundA).empty(); }));

    REQUIRE (rig.runs.preparedRunOf (rig.groupId) != nullptr);
    const auto actBlock = rig.runs.preparedRunOf (rig.groupId)->id;
    const auto blockA = rig.runs.preparedRunOf (sceneA)->id;
    const auto armA = rig.runOf (soundA);
    const auto voice = rig.runs.find (armA)->track;
    REQUIRE (rig.runs.find (blockA)->parent == actBlock);
    REQUIRE (voice >= 0);

    SUBCASE ("to the other scene's first line")
    {
        rig.setStandby (soundB);
        REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (sceneB) != nullptr; }));
        rig.tickOnce();

        CHECK (rig.runs.find (blockA)->isFinished());
        CHECK (rig.runs.find (blockA)->warning == cue::runWarning::revoked);
        CHECK (rig.runs.find (armA)->isFinished());
        CHECK (rig.runs.find (armA)->warning == cue::runWarning::revoked);

        //  The voice it let go of is free for the scene the pointer is on now, which arms on it.
        REQUIRE_FALSE (rig.runOf (soundB).empty());
        CHECK (rig.runs.find (rig.runOf (soundB))->track >= 0);

        //  The act's block is the one the pointer is still in, and the new scene is made ready inside it.
        REQUIRE (rig.runs.preparedRunOf (rig.groupId) != nullptr);
        CHECK (rig.runs.preparedRunOf (rig.groupId)->id == actBlock);
        CHECK (rig.runs.find (actBlock)->state == cue::runState::preparing);
        CHECK (rig.runs.preparedRunOf (sceneB)->parent == actBlock);
    }

    SUBCASE ("to the next line of the same scene")
    {
        rig.setStandby (lineA);
        rig.tickOnce();

        REQUIRE (rig.runs.preparedRunOf (sceneA) != nullptr);
        CHECK (rig.runs.preparedRunOf (sceneA)->id == blockA);
        CHECK_FALSE (rig.runs.find (blockA)->isFinished());
        CHECK (rig.runs.preparedRunOf (rig.groupId)->id == actBlock);
    }
}

/*  A SOUND ARMED IN A RUNNING ACT AND PASSED OVER (2026-10-01, namespace draft
    §23.9, ID). The pointer reaching a media line of an act that is running arms
    it under the act, marked as a promise; a manual act takes only the members a
    GO asks for, so one the operator walked past was never taken - and the give-
    back left every arm under another run alone, so its voice was held while the
    act ran, and after: §13.1's scrolling that emptied the rack, one level in. */
TEST_CASE ("prepare: the pointer passing over a sound in a running act gives its voice back")
{
    SUBCASE ("to a later line of the act, and out of the act")
    {
        for (const auto* where : { "a later line of the act", "a cue after the act" })
        {
            INFO (std::string (where));
            ManualRig rig;
            const auto sound = rig.document.createCue (rig.groupId, 1, "media", "Thunder").id;
            rig.setCue (sound, "file", "thunder.wav");

            rig.setStandby (rig.first);
            REQUIRE (rig.submitAndTick ("go").rejected == 0);      // into the act, on its first line
            REQUIRE (rig.standby() == sound);
            REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (sound).empty(); }));

            const auto actRun = rig.runOf (rig.groupId);
            const auto armed = rig.runOf (sound);
            const auto voice = rig.runs.find (armed)->track;
            REQUIRE (rig.runs.find (armed)->parent == actRun);
            REQUIRE_FALSE (rig.runs.find (armed)->prepare.empty());
            REQUIRE (voice >= 0);

            rig.setStandby (std::string (where) == "a cue after the act" ? rig.after : rig.second);
            rig.tickOnce();

            CHECK (rig.runs.find (armed)->isFinished());
            CHECK (rig.runs.find (armed)->warning == cue::runWarning::revoked);
            CHECK_FALSE (rig.runs.isTrackBusy (voice));
            CHECK_FALSE (rig.runs.find (actRun)->isFinished());
        }
    }

    SUBCASE ("but not the sound a GO entered the act on, while the act's header runs")
    {
        /*  The arm under an act whose members have not begun is the one the GO
            entered the act at, waiting for them: a net, passing before and
            after. */
        ManualRig rig;
        const auto opening = rig.document.createCue (rig.roleOf (rig.groupId, "header"), 0, "memo", "House to half").id;
        const auto sound = rig.document.createCue (rig.groupId, 0, "media", "Thunder").id;
        rig.setCue (sound, "file", "thunder.wav");

        rig.setStandby (sound);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (sound).empty(); }));
        const auto armed = rig.runOf (sound);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == rig.first);

        CHECK (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }));
        CHECK (rig.runs.find (armed)->warning != cue::runWarning::revoked);
        CHECK_FALSE (rig.runOf (opening).empty());
    }
}

/*  A SCENE INSIDE AN ACT NOBODY HAS ENTERED, WHILE THE ACT'S HEADER IS GOT READY
    (2026-10-01, namespace draft §23.9, IE). A header the horizon can take ahead
    puts the act's block in its preparing phase, which launches what it pre-sends
    - and it launched every child that was not a sound, the scene's own block
    among them: the scene let out of its hold and played, with no GO pressed. */
TEST_CASE ("prepare: a scene made ready inside an act nobody has entered waits for a GO while the act's header is got ready")
{
    ManualRig rig;
    const auto bed = rig.document.createCue (rig.roleOf (rig.groupId, "header"), 0, "media", "Bed").id;
    rig.setCue (bed, "file", "thunder.wav");

    const auto scene = rig.document.createCue (rig.groupId, 0, "group", "Storm").id;
    rig.setCue (scene, "mode", "timeline");
    const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "thunder.wav");

    rig.setStandby (scene);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                          && ! rig.runOf (rain).empty() && ! rig.runOf (bed).empty(); }));

    const auto block = rig.runs.preparedRunOf (scene)->id;
    const auto armed = rig.runOf (rain);

    for (int n = 0; n < 30; ++n)
        rig.tickOnce();

    //  Still only made ready, the act's and the scene's blocks both, and nothing asked to sound.
    CHECK (rig.runs.find (block)->state == cue::runState::preparing);
    CHECK (rig.runs.liveRunOf (scene) == nullptr);
    CHECK (rig.runs.preparedRunOf (rig.groupId) != nullptr);
    CHECK (rig.runs.liveRunOf (rig.groupId) == nullptr);
    CHECK_FALSE (rig.runs.find (armed)->launchRequested);
    CHECK_FALSE (rig.runs.find (rig.runOf (bed))->launchRequested);
}

//==============================================================================
/*  A SCENE THAT NEVER STARTED HAS NO FOOTER, AND ESC LEAVES THE STANDBY ALONE
    (namespace draft §23, 2026-09-30).

    A footer is where a scene gives back what it took, so a group stopped while
    it was only being made ready ran a footer that released things it never
    took - and, being `stopping`, was no longer found by the path that puts back
    what the horizon pre-sent. A group stopped while prepared is now given back
    the way the pointer moving away gives one back: what it pre-sent put back,
    then revoked, no footer.

    AND ESC STOPS WHAT IS RUNNING. The standby's preparation - its arm, its
    prepared block - is not running and the pointer has not moved, so it is
    still wanted: Esc and a double Esc leave it, and the next GO is as instant as
    it would have been. */
TEST_CASE ("prepare: Esc and a double Esc leave a prepared scene standing, and GO still adopts it")
{
    for (const auto* level : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (level));
        PrepareRig rig;

        rig.setStandby (rig.sound);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));
        REQUIRE (rig.prepared (rig.groupId) != nullptr);

        const auto preparedId = rig.prepared (rig.groupId)->id;
        const auto armedId = rig.runOf (rig.sound);
        const auto voice = rig.runs.find (armedId)->track;
        REQUIRE (voice >= 0);

        REQUIRE (rig.submitAndTick (level).rejected == 0);

        for (int n = 0; n < 60; ++n)                     // past a panic fade of a second
            rig.tickOnce();

        //  STILL READY: the block, its member's voice, and nothing of its own exit.
        REQUIRE (rig.prepared (rig.groupId) != nullptr);
        CHECK (rig.prepared (rig.groupId)->id == preparedId);
        CHECK (rig.runs.find (armedId)->state == cue::runState::armed);
        CHECK (rig.runs.isTrackBusy (voice));
        CHECK (rig.runOf (rig.closing).empty());
        CHECK (rig.runOf (rig.opening).empty());

        //  AND THE NEXT GO TAKES THAT BLOCK, and launches the member it armed.
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto* live = rig.runs.liveRunOf (rig.groupId);
        REQUIRE (live != nullptr);
        CHECK (live->id == preparedId);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (armedId)->launchRequested; }));
    }
}

TEST_CASE ("prepare: a double Esc's sweep is told the voice a prepared scene armed, not a sounding cue's, and GO launches it unarmed again")
{
    /*  H3 (namespace draft §23.6, FY). The press spares what was only made
        ready (§23.3), and the GO after it launches that with no arm in between
        - so the sweep must leave those voices at the level their arm set, and
        it is told which they are: every voice of a run only made ready. That
        is the standby's own arm, and as here the member a prepared scene's
        horizon armed, which the press spares through its block. A list of the
        roots alone - the rule the press itself stops by - would hand the sweep
        none of a scene's members, and the scene would launch at silence. */
    PrepareRig rig;

    //  The scene made ready, its member's voice held ahead.
    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto armedId = rig.runOf (rig.sound);
    const auto voice = rig.runs.find (armedId)->track;
    REQUIRE (voice >= 0);
    REQUIRE (rig.runs.find (armedId)->onlyPrepared());

    //  And the list's own cue, fired by name and left sounding.
    REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (rig.mediaId) }).rejected == 0);
    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&] { return ! rig.audio.launches.empty(); }));

    const auto* fired = rig.runs.liveRunOf (rig.mediaId);
    REQUIRE (fired != nullptr);

    const auto firedId = fired->id;
    const auto sounding = fired->track;
    REQUIRE (sounding >= 0);
    REQUIRE (sounding != voice);
    rig.audio.playing.insert (sounding);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);

    REQUIRE (rig.audio.sweeps.size() == 1u);
    const auto spared = rig.audio.sweeps.back();
    CHECK (std::find (spared.begin(), spared.end(), voice) != spared.end());
    CHECK (std::find (spared.begin(), spared.end(), sounding) == spared.end());

    //  The sounding cue is killed; the member stays as its arm left it.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (firedId)->isFinished(); }, 10));
    REQUIRE (rig.runs.find (armedId)->state == cue::runState::armed);

    //  AND THE GO LAUNCHES IT WHERE THE HORIZON ARMED IT, asking for no arm.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    CHECK (rig.tickUntil ([&] { return rig.runs.find (armedId)->launchRequested; }));
    CHECK (std::none_of (rig.audio.arms.begin(), rig.audio.arms.end(),
                         [&armedId] (const cue::ArmRequest& arm) { return arm.runId == armedId; }));
    CHECK (rig.audio.sweeps.size() == 1u);
}

TEST_CASE ("Esc: the standby's armed cue stays armed, and GO launches that very run")
{
    for (const auto* level : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (level));
        Rig rig;

        rig.setStandby (rig.mediaId);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));

        const auto armedId = rig.runOf (rig.mediaId);
        const auto voice = rig.runs.find (armedId)->track;
        REQUIRE (voice >= 0);

        REQUIRE (rig.submitAndTick (level).rejected == 0);

        for (int n = 0; n < 60; ++n)
            rig.tickOnce();

        CHECK (rig.runs.find (armedId)->state == cue::runState::armed);
        CHECK (rig.runs.isTrackBusy (voice));

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (armedId)->launchRequested; }));

        const auto runsFor = std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                            [&rig] (const cue::Run& run) { return run.cue == rig.mediaId; });
        CHECK (runsFor == 1);
    }
}

TEST_CASE ("prepare: a scene prepared inside a running one is given back when Esc stops the running one, and runs no footer of its own")
{
    /*  The running scene is stopped the Esc way, and stops its members - one of
        which is the next scene, prepared under it by the horizon and never
        entered. That one is revoked, its armed member with it; the running
        scene's own footer runs. */
    ManualRig rig;

    const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inner").id;     // manual, the default
    const auto rain = rig.document.createCue (inner, 0, "media", "Rain").id;
    rig.setCue (rain, "file", "rain.wav");

    const auto innerRelease = rig.document.createCue (rig.roleOf (inner, "footer"), 0, "memo", "Inner release").id;
    const auto outerRelease = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Outer release").id;

    rig.setStandby (rig.first);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto outerRun = rig.runOf (rig.groupId);

    //  The pointer walked into the inner scene, and the horizon prepared it under the running one.
    REQUIRE (rig.standby() == rain);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (inner) != nullptr
                                          && ! rig.runOf (rain).empty(); }));

    const auto innerRun = rig.runs.preparedRunOf (inner)->id;
    REQUIRE (rig.runs.find (innerRun)->parent == outerRun);

    const auto armed = rig.runOf (rain);
    const auto voice = rig.runs.find (armed)->track;
    REQUIRE (voice >= 0);

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (outerRun)->isFinished(); }));

    CHECK (rig.runOf (innerRelease).empty());             // never started, so nothing to release
    CHECK_FALSE (rig.runOf (outerRelease).empty());       // the running scene's footer ran

    for (const auto& id : { innerRun, armed })
    {
        INFO ("run " << id);
        CHECK (rig.runs.find (id)->state == cue::runState::done);
        CHECK (rig.runs.find (id)->warning == cue::runWarning::revoked);
    }

    CHECK_FALSE (rig.runs.isTrackBusy (voice));
}

TEST_CASE ("prepare: a stop aimed at a prepared scene's run gives it back, and runs no footer")
{
    /*  By its run, since a stop cue aimed at the CUE finds nothing running and
        is §3.8's silent no-op. A stop and a kill end it alike: a revocation is
        not a footer, and what was made ready is let go of either way. */
    for (const auto* verb : { "run.stop", "run.kill" })
    {
        INFO (std::string (verb));
        PrepareRig rig;

        rig.setStandby (rig.sound);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));
        REQUIRE (rig.prepared (rig.groupId) != nullptr);

        const auto preparedId = rig.prepared (rig.groupId)->id;
        const auto armedId = rig.runOf (rig.sound);
        const auto voice = rig.runs.find (armedId)->track;
        REQUIRE (voice >= 0);

        REQUIRE (rig.submitAndTick (verb, { osc::Value::string (preparedId) }).rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (preparedId)->isFinished(); }, 20));

        CHECK (rig.runs.find (preparedId)->warning == cue::runWarning::revoked);
        CHECK (rig.runs.find (armedId)->warning == cue::runWarning::revoked);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));
        CHECK (rig.runOf (rig.closing).empty());
        CHECK (rig.runOf (rig.opening).empty());
    }
}

//==============================================================================
/*  A BLOCK CAN HOLD A SOUND, and then it is not only made ready (namespace draft
    §23.3). The horizon arms a scene's first member under the block it prepares,
    and a cue fired by name - a surface's button, a start cue, a trigger - that
    finds its armed run launches it where it stands: under a block no GO has
    entered. Esc and a double Esc spared the block, and a stop reaching it gave
    it back, the sounding member with it, on the model alone. */
namespace
{
    /*  A member the horizon armed, fired by name and let sound: the armed run
        launched where it stands, and the voice it holds. */
    int soundByName (Rig& rig, const std::string& cueId)
    {
        const auto armedId = rig.runOf (cueId);
        REQUIRE_FALSE (armedId.empty());

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (cueId) }).rejected == 0);
        REQUIRE (rig.runOf (cueId) == armedId);            // that run, not a second

        rig.audio.completeArms (rig.engine);
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armedId)->state == cue::runState::playing; }));

        const auto voice = rig.runs.find (armedId)->track;
        REQUIRE (voice >= 0);

        rig.audio.playing.insert (voice);
        rig.tickOnce();
        return voice;
    }
}

TEST_CASE ("prepare: a member fired by name out of a prepared scene is stopped by Esc and a double Esc")
{
    /*  The scene at standby, prepared and holding; one member of it fired by
        name and sounding. Esc stops what is running, so the block is not left
        standing around it: the member is stopped the way the press stops, on
        its voice, and then the block is given back. A cut, so that what reaches
        the voice is the press itself - a panic fade reaches a sounding voice
        whoever holds it. */
    for (const auto* level : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (level));
        PrepareRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

        rig.setStandby (rig.sound);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));
        REQUIRE (rig.prepared (rig.groupId) != nullptr);

        const auto blockId = rig.prepared (rig.groupId)->id;
        const auto soundRun = rig.runOf (rig.sound);
        const auto voice = soundByName (rig, rig.sound);
        REQUIRE (rig.runs.find (soundRun)->parent == blockId);

        REQUIRE (rig.submitAndTick (level).rejected == 0);
        CHECK (rig.tickUntil ([&] { return rig.runs.find (soundRun)->isFinished(); }, 20));

        //  STOPPED ON ITS VOICE, and not given back behind the voice's back.
        CHECK (std::count (rig.audio.stopped.begin(), rig.audio.stopped.end(), voice) >= 1);
        CHECK (rig.runs.find (soundRun)->warning != cue::runWarning::revoked);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));

        //  And then the block, which never started: given back, with no footer.
        CHECK (rig.tickUntil ([&] { return rig.runs.find (blockId)->isFinished(); }, 20));
        CHECK (rig.runs.find (blockId)->warning == cue::runWarning::revoked);
        CHECK (rig.runOf (rig.closing).empty());
    }
}

TEST_CASE ("prepare: Esc on a running scene stops a member fired by name out of the scene prepared inside it")
{
    /*  THE SAME ONE LEVEL IN, where it was worse. The running scene stopped the
        prepared one, which gave itself back - its sounding member revoked with
        it, on the model and nowhere else. The panic fade bringing the member
        down then found its target over and stopped nothing, and the voice
        played on to the end of its file with no run owning it, out of reach of
        any later Esc. What was asked for inside the block is now stopped first,
        the way its scene was, and the block is given back once it has gone. */
    for (const auto* level : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (level));
        ManualRig rig;

        const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inner").id;     // manual, the default
        const auto rain = rig.document.createCue (inner, 0, "media", "Rain").id;
        rig.setCue (rain, "file", "rain.wav");

        const auto innerRelease = rig.document.createCue (rig.roleOf (inner, "footer"), 0, "memo", "Inner release").id;

        rig.setStandby (rig.first);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

        const auto outerRun = rig.runOf (rig.groupId);

        REQUIRE (rig.standby() == rain);
        REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (inner) != nullptr
                                              && ! rig.runOf (rain).empty(); }));

        const auto innerRun = rig.runs.preparedRunOf (inner)->id;
        const auto rainRun = rig.runOf (rain);
        const auto voice = soundByName (rig, rain);
        REQUIRE (rig.runs.find (rainRun)->parent == innerRun);

        REQUIRE (rig.submitAndTick (level).rejected == 0);                // the panic fade: a second
        CHECK (rig.tickUntil ([&] { return rig.runs.find (rainRun)->isFinished(); }, 100));

        CHECK (std::count (rig.audio.stopped.begin(), rig.audio.stopped.end(), voice) >= 1);
        CHECK (rig.runs.find (rainRun)->warning != cue::runWarning::revoked);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));

        //  The scene prepared inside is given back once its member has gone, with no footer of its own.
        CHECK (rig.tickUntil ([&] { return rig.runs.find (outerRun)->isFinished(); }));
        CHECK (rig.runs.find (innerRun)->warning == cue::runWarning::revoked);
        CHECK (rig.runOf (innerRelease).empty());
    }
}

TEST_CASE ("prepare: the pointer moving away leaves a member fired by name playing, and gives its scene back once it has gone")
{
    /*  The pointer moving away gives back what was only made ready, and a
        member fired by name is not that: somebody asked for it. Revoked with its
        block, as it was, it ended on the model alone - the voice played on with
        no run owning it, and no Esc could reach it. The block now waits for what
        was asked for inside it and is given back once that has gone, whether it
        ends on its own or Esc ends it. */
    for (const auto* how : { "on its own", "Esc" })
    {
        INFO (std::string (how));
        PrepareRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

        rig.setStandby (rig.sound);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));
        REQUIRE (rig.prepared (rig.groupId) != nullptr);

        const auto blockId = rig.prepared (rig.groupId)->id;
        const auto soundRun = rig.runOf (rig.sound);
        const auto voice = soundByName (rig, rig.sound);

        //  Away, to a cue outside the block: the member plays on, and the block waits for it.
        rig.setStandby (rig.after);
        rig.tickOnce();
        rig.tickOnce();

        CHECK_FALSE (rig.runs.find (soundRun)->isFinished());
        CHECK (rig.runs.isTrackBusy (voice));
        CHECK_FALSE (rig.runs.find (blockId)->isFinished());

        if (std::string (how) == "Esc")
            REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
        else
            rig.audio.playing.erase (voice);

        CHECK (rig.tickUntil ([&] { return rig.runs.find (soundRun)->isFinished(); }, 20));
        CHECK (rig.runs.find (soundRun)->warning != cue::runWarning::revoked);
        CHECK_FALSE (rig.runs.isTrackBusy (voice));

        CHECK (rig.tickUntil ([&] { return rig.runs.find (blockId)->isFinished(); }, 20));
        CHECK (rig.runs.find (blockId)->warning == cue::runWarning::revoked);
        CHECK (rig.runOf (rig.closing).empty());
    }
}

TEST_CASE ("prepare: a seek on the standby's armed cue asks for it")
{
    /*  A SEEK LAUNCHES, so it is asking. It was the one road to a launch that
        left the `prepare` mark on: the standby's arm, scrubbed, played while
        still reading as made ready in case - and whether Esc spared it then
        turned on a flag the launching hook clears, which a replay never
        clears. */
    Rig rig;

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));

    const auto armedId = rig.runOf (rig.mediaId);
    REQUIRE_FALSE (rig.runs.find (armedId)->prepare.empty());

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (armedId),
                                              osc::Value::float64 (1.0) }).rejected == 0);

    CHECK (rig.runs.find (armedId)->prepare.empty());
    CHECK_FALSE (rig.runs.find (armedId)->onlyPrepared());
}

//==============================================================================
/*  THE HEADER AS A PRESET SHEET: a mark on the member, and a line nobody wrote.

    The author, looking at the first web client: "since this is something that
    preloads and prepares OSC parameters ahead of time, the parameters of the
    groups could have a preload/preset tickbox to add them in the header... The
    preloaded or preset lines would appear in italics showing they are set from
    a cue from the group." And on where the gesture points: "we could drag a cue
    to be preloaded to a header from one level or another of the nested groups
    it's in."

    THE MARK IS THE DECISION AND THE LINE IS A READING OF IT (§4.10). Nothing is
    copied into the `Header` element, so editing the line is editing the member,
    deleting the member removes the line with no repair rule to write, and the
    two can never come to disagree because there is only one of them.

    IT NAMES AN ANCESTOR RATHER THAN BEING A TICKBOX, which is what the drag
    means: a cue three groups deep can be got ready by its own group, by the
    act, or by the opening scene, and which one is a decision about HOW EARLY.
*/
TEST_CASE ("preset: a member marked for its group's header is prepared with that header")
{
    PrepareRig rig;

    /*  The scene's own media member, marked to be got ready by the scene rather
        than when its turn comes. It is already a member of `groupId`, so the
        scene IS an ancestor. */
    rig.setCue (rig.sound, "preset", rig.groupId);

    rig.setStandby (rig.first);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto* ready = rig.prepared (rig.groupId);
    REQUIRE (ready != nullptr);

    /*  ARMED UNDER THE BLOCK THAT PREPARED IT, which is what makes a revocation
        reach it - and the voice reserved early is the voice that will sound. */
    const auto* member = rig.runs.find (rig.runOf (rig.sound));
    REQUIRE (member != nullptr);
    CHECK (member->parent == ready->id);
    CHECK (member->state == cue::runState::armed);
    CHECK_FALSE (member->launchRequested);
}

TEST_CASE ("preset: the derived line is a reading of the mark, and goes with the member")
{
    /*  §13.7 and open questions §5's first: nothing is written into the Header
        element, so there is one object and not two. */
    PrepareRig rig;

    Engine tree;
    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, tree.commands(), mounts, rig.runs };

    const auto derived = [&]
    {
        parameters.markStale();

        tree::EngineState state;
        state.version = "test";

        const auto snapshot = parameters.publish (0, state);
        const auto* node = snapshot->find ("/godot/cue/" + rig.groupId + "/headerDerived");

        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        return node->soleValue()->getString();
    };

    CHECK (derived().empty());

    rig.setCue (rig.sound, "preset", rig.groupId);
    CHECK (derived() == rig.sound);

    /*  AND THE HEADER ELEMENT IS UNTOUCHED. The line is derived; the written
        header is what somebody wrote, and the two lists are separate. */
    const auto header = rig.document.findById (rig.groupId)
                            .getChildWithName (juce::Identifier ("Header"));
    REQUIRE (header.isValid());
    CHECK (header.getNumChildren() == 1);         // the memo, and nothing else

    /*  DELETING THE MEMBER REMOVES THE LINE, with no repair rule to write. */
    REQUIRE (rig.document.remove (rig.sound).ok);
    CHECK (derived().empty());
}

TEST_CASE ("preset: a member marked for its GRANDPARENT is prepared when the grandparent is reached")
{
    /*  The author's own picture: a cue is got ready by one level or another of
        the nested groups it is in, and which one is a decision about how early.

        The horizon reaches the outermost group the pointer is inside, so a mark
        naming the grandparent is honoured from outside the inner group - which
        is exactly the point of naming a level rather than ticking a box. */
    PrepareRig rig;

    const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inner").id;
    const auto deep = rig.document.createCue (inner, 0, "media", "Deep").id;
    rig.document.setAttribute ("/godot/cue/" + deep + "/file", "thunder.wav");

    rig.setCue (deep, "preset", rig.groupId);

    /*  The pointer lands on the outer scene's first member: the horizon
        prepares the whole block, and the mark says this one comes with it. */
    rig.setStandby (rig.first);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (deep).empty(); }));

    const auto* ready = rig.prepared (rig.groupId);
    REQUIRE (ready != nullptr);

    const auto* member = rig.runs.find (rig.runOf (deep));
    REQUIRE (member != nullptr);
    CHECK (member->parent == ready->id);
    CHECK (member->state == cue::runState::armed);
}

TEST_CASE ("preset: the member still runs where it sits, on the run its ancestor armed")
{
    /*  THE WHOLE DIFFERENCE BETWEEN THIS AND MOVING THE CUE. The header line
        says *this is got ready here*; the cue list still says *this happens
        there*. So the group the member actually belongs to adopts the arm the
        ancestor made rather than making a second one beside it - and the voice
        reserved early is the voice that sounds. */
    PrepareRig rig;

    rig.setCue (rig.sound, "preset", rig.groupId);

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.sound).empty(); }));

    const auto armed = rig.runOf (rig.sound);
    REQUIRE (! armed.empty());

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (armed)->launchRequested; }));

    const auto howMany = std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                        [&rig] (const cue::Run& run)
                                        {
                                            return run.cue == rig.sound;
                                        });

    CHECK (howMany == 1);
}

TEST_CASE ("preset: a scene whose header is entirely derived is prepared, not partial")
{
    /*  FOUND BY THE PHASE 4 DRIVER, which parked on a scene with one preset
        member and no written header and read `partial` for ever.

        `partial` means "something in this block could not be got ready", and
        what it was counted against was the WRITTEN header's members - so a
        block made only of derived lines was one preparable cue against nought
        written ones, and the two numbers disagreed by construction. Both sides
        come from `blockCuesIn` now, which is the whole block: derived first,
        written after, each cue once.

        The shape matters because the preset design encourages it. A designer
        who marks three members for their scene's header and writes none has a
        scene whose whole preparation is derived, and that is the ordinary case
        rather than a corner. */
    PrepareRig rig;

    //  The written header's memo goes - it is the cue that cannot be prepared
    //  and the reason this rig's other cases read `partial` correctly. What is
    //  left is a block made only of the derived line.
    REQUIRE (rig.submitAndTick ("object.delete",
                                { osc::Value::string (rig.opening) }).applied == 1);

    rig.setCue (rig.sound, "preset", rig.groupId);
    rig.setStandby (rig.groupId);

    const auto word = [&rig]
    {
        const auto* ready = rig.prepared (rig.groupId);
        return ready != nullptr ? std::string (ready->prepare) : std::string {};
    };

    REQUIRE (rig.tickUntil ([&word] { return word() == cue::preparedness::armed
                                             || word() == cue::preparedness::verified
                                             || word() == cue::preparedness::partial; }));

    INFO ("prepare says " << word());
    CHECK (word() != cue::preparedness::partial);
}

TEST_CASE ("preset: a mark on a group the cue is not inside warns and does nothing")
{
    /*  A WARNING AND NOT A REFUSAL: the repair is somebody dragging it
        somewhere sensible, and yesterday's saved show has to open tomorrow.
        What it must not do is quietly look like it worked. */
    PrepareRig rig;

    /*  A second scene, which is nobody's ancestor here. */
    const auto elsewhere = rig.document.createCue (rig.listId, 4, "group", "Elsewhere").id;
    rig.setCue (rig.sound, "preset", elsewhere);

    const auto said = [&rig]
    {
        for (const auto& problem : rig.document.warnings())
            if (problem.find ("not a group this cue is inside") != std::string::npos)
                return true;

        return false;
    };

    CHECK (said());

    /*  And nothing acts on it: the pointer reaching `Elsewhere` prepares a
        block that does not contain the cue, so the cue is not armed. */
    rig.setStandby (elsewhere);

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (rig.sound) == "");

    /*  Pointed at a group it IS inside, the warning goes. */
    rig.setCue (rig.sound, "preset", rig.groupId);
    CHECK_FALSE (said());
}

//==============================================================================
/*  THE JUMP: making a position true rather than only describing it.

    PRD §3.13. `list.aim` asks what the show WOULD be somewhere; this makes it
    so. One record, whose applied arguments carry every run identifier it drew,
    because a replay never draws one of its own - and everything the jump does
    happens inside the handler, in one drain, because a jump made of six records
    would be a jump a replay could interleave differently.

    THE THING TO KEEP HOLD OF while reading these: the scheduler is not told
    about the jump. It finds a run tree that looks exactly like one it built
    itself and carries on from the next tick - which is why the fields the tree
    builder writes by hand are the ones the scheduler RE-READS, and why getting
    one of them wrong is a group that ends after one round or draws a shuffle
    from a seed the show never used.
*/
namespace
{
    /*  A scene worth jumping into: a timeline group whose three members sound
        at nought, two and ten seconds, so a position picks out a different set
        of them each time. */
    struct JumpRig : Rig
    {
        JumpRig()
        {
            scene = document.createCue (listId, 2, "group", "Scene").id;
            document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline");

            early = memberOf (0, "One", "0");
            middle = memberOf (1, "Two", "2");
            late = memberOf (2, "Three", "10");

            after = document.createCue (listId, 3, "memo", "After").id;

            durations["thunder.wav"] = 4.0;
            runner.setMediaDurations (&durations);
        }

        std::string memberOf (int index, const char* name, const char* preWait)
        {
            const auto id = document.createCue (scene, index, "media", name).id;
            document.setAttribute ("/godot/cue/" + id + "/file", "thunder.wav");
            document.setAttribute ("/godot/cue/" + id + "/preWait", preWait);
            return id;
        }

        Engine::TickResult jumpTo (const std::string& cueId, double offset)
        {
            REQUIRE (engine.submit ("cli", "list.aim",
                                    { osc::Value::string (listId),
                                      osc::Value::string (cueId),
                                      osc::Value::float64 (offset) }));
            tickOnce();

            return submitAndTick ("list.loadToTime", { osc::Value::string (listId) });
        }

        const cue::Run* liveRunOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId && ! run.isFinished())
                    return &run;

            return nullptr;
        }

        std::map<std::string, double> durations;
        std::string scene, early, middle, late, after;
    };
}

//==============================================================================
namespace
{
    /*  RUNS THE SCHEDULER FORWARD, PLAYING THE AUDIO SIDE'S OWN PART.

        `FakePlayer` is a script rather than a simulation: it reports a track as
        playing only when a case says so, because every other case in this file
        decides for itself when a cue starts and stops - which is right for a
        case about a stop and useless for a case about a show running by itself.

        So this plays the two halves the audio side would: a launched run's
        track starts sounding, and a run whose material has run out stops. Both
        are what the engine LOOKS for - `observeEdges` ends a run on the falling
        edge of `isPlaying` - so the scheduler learns it the way it always does
        and no test-only path into the engine is opened.

        THE ERASES COME BEFORE THE INSERTS, in two passes over one tick, because
        a sequence hands the next member the track the last one just gave back:
        one pass would silence the new run on the tick it started. */
    void runOn (JumpRig& rig, std::int64_t until, double material,
                std::map<std::string, std::int64_t>& since)
    {
        const auto ticksOfMaterial = static_cast<std::int64_t> (material * 50.0);

        while (rig.tick < until)
        {
            rig.audio.completeArms (rig.engine);

            for (const auto& run : rig.runs.all())
            {
                if (run.kind != "media" || run.isFinished() || run.track < 0)
                    continue;

                const auto found = since.find (run.id);

                if (found != since.end() && rig.tick - found->second >= ticksOfMaterial)
                    rig.audio.playing.erase (run.track);
            }

            for (const auto& run : rig.runs.all())
            {
                if (run.kind != "media" || run.isFinished() || run.track < 0
                     || run.state != cue::runState::playing)
                    continue;

                if (since.find (run.id) == since.end())
                {
                    since.emplace (run.id, rig.tick);
                    rig.audio.playing.insert (run.track);
                }
            }

            rig.tickOnce();
        }
    }
}

//==============================================================================
/*  THE EQUIVALENCE TEST: the solver against the scheduler, on one show.

    §13.8's own sentence, and the stated reason to trust any of this: "a solver
    that disagrees with the scheduler is wrong by definition". Everything else
    in this file asks the solver what it thinks; this asks whether what it
    thinks is what actually happens.

    HOW IT AVOIDS BEING CIRCULAR, which is the whole difficulty. The moment is
    named by the DOCUMENT's own arithmetic - a timeline group whose members sit
    at nought, two and ten seconds, each four seconds long, so "three seconds
    into the scene" is a fact about the show and not about either implementation.
    Then two independent things are asked about that moment: the SCHEDULER is
    run forward to it and its live runs read off the run table, and the SOLVER is
    asked what should be sounding there. Neither is derived from the other.

    WHY A TIMELINE GROUP. Its members overlap, so the answer at three seconds is
    two cues rather than one - and a solver that simply reported "the cue you
    aimed at" would pass a sequence and fail this. Two of the three moments below
    have more than one thing sounding for exactly that reason.
*/
TEST_CASE ("equivalence: what the solver says is sounding is what the scheduler sounds")
{
    struct Moment
    {
        const char* what;
        double sceneSeconds;      ///< where the show is, in the scene's own time
        const char* aimAt;        ///< the member to aim at
        double offset;            ///< how far into that member
    };

    JumpRig rig;

    /*  The scene is a timeline group: `early` at nought, `middle` at two,
        `late` at ten, each four seconds of `thunder.wav`. The three moments are
        chosen a whole second clear of every boundary, so that neither answer
        depends on which side of a tick a launch landed. */
    const Moment moments[] {
        { "one second in: the first member alone",         1.0,  nullptr, 1.0 },
        { "three seconds in: the first two overlap",       3.0,  nullptr, 1.0 },
        { "eleven seconds in: only the last is left",     11.0,  nullptr, 1.0 },
    };

    const std::string members[] { rig.early, rig.middle, rig.late };
    const double starts[] { 0.0, 2.0, 10.0 };
    constexpr double material = 4.0;

    rig.setStandby (rig.scene);
    REQUIRE (rig.submitAndTick ("go").applied >= 1);

    const auto enteredAt = rig.tick;

    /*  HOW LONG EACH RUN HAS BEEN SOUNDING, kept by the CASE and not by one
        window of it: a run first seen at one and a half seconds is four seconds
        old at five and a half, and a map that started again each time would
        have every cue for ever young and nothing would ever end. */
    std::map<std::string, std::int64_t> playedSince;

    for (const auto& moment : moments)
    {
        INFO (moment.what);

        /*  THE SCHEDULER, RUN FORWARD. Arms are completed as the disk would,
            every tick, because a run that never armed never launches and the
            question here is about a show that is running rather than about a
            rig that is stuck. */
        const auto until = enteredAt
                             + static_cast<std::int64_t> (moment.sceneSeconds * 50.0);

        runOn (rig, until, material, playedSince);

        std::set<std::string> scheduled;

        for (const auto& run : rig.runs.all())
            if (run.kind == "media" && ! run.isFinished()
                 && run.state == cue::runState::playing)
                scheduled.insert (run.cue);

        /*  THE SOLVER, ASKED ABOUT THE SAME MOMENT. The aim is whichever member
            the document says is sounding then, and how far into it - arithmetic
            over the show's own offsets, which is what an operator dragging the
            aim is doing by hand. */
        std::string aimCue;
        auto aimOffset = 0.0;

        for (std::size_t n = 0; n < 3; ++n)
            if (moment.sceneSeconds >= starts[n]
                 && moment.sceneSeconds < starts[n] + material)
            {
                aimCue = members[n];
                aimOffset = moment.sceneSeconds - starts[n];
            }

        REQUIRE_FALSE (aimCue.empty());

        const auto plan = cue::solve (rig.document, &rig.durations, nullptr,
                                      { rig.listId, aimCue, aimOffset });

        REQUIRE (plan.ok);

        /*  MEDIA AGAINST MEDIA. The plan carries the target's GROUPS as well,
            outermost first, because a member sounds as part of its scene - so
            the two sides are made comparable by asking each for the same thing
            rather than by quietly dropping something from one of them. That the
            scene is in the plan is checked below, where it means something. */
        std::set<std::string> solved;

        for (const auto& planned : plan.runs)
            if (planned.when == cue::planned::sounding
                 && rig.document.findById (planned.cue).getType().toString() == "Media")
                solved.insert (planned.cue);

        const auto spell = [] (const std::set<std::string>& set)
        {
            std::string out;

            for (const auto& cueId : set)
                out += (out.empty() ? "" : " ") + cueId;

            return out.empty() ? std::string ("nothing") : out;
        };

        INFO (moment.what << " | the scheduler sounds " << spell (scheduled)
               << " | the solver says " << spell (solved));

        CHECK (solved == scheduled);

        /*  AND THE SCENE COMES WITH ITS MEMBER. A jump into a scene has to
            build the scene, or the scheduler it hands the tree to would spawn
            a second one over the top. */
        const auto carriesTheScene =
            std::any_of (plan.runs.begin(), plan.runs.end(),
                         [&rig] (const cue::PlannedRun& planned)
                         { return planned.cue == rig.scene; });

        CHECK (carriesTheScene);
    }
}

TEST_CASE ("equivalence: the solver agrees about an automatic sequence too")
{
    /*  The other chain kind, and it fails differently: a sequence's members
        follow one another rather than overlapping, so what has to agree is
        WHICH one - and getting the arithmetic one member out is the mistake
        that would not show up in a timeline at all.

        A sequence's member starts when the one before it ENDS, which the
        document knows: four seconds of material each, so member n starts at
        4n seconds. Two ticks of scheduler latency sit between a member's end
        and the next one's launch (§11.5's `sequenceGapTicks`), which is why the
        moments below are a second clear of every join. */
    JumpRig rig;

    //  The same three cues, re-made as an automatic SEQUENCE rather than a
    //  timeline: the pre-waits go, and the mode with them.
    rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence");
    rig.document.setAttribute ("/godot/cue/" + rig.scene + "/advance", "auto");

    for (const auto& member : { rig.early, rig.middle, rig.late })
        rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0");

    rig.setStandby (rig.scene);
    REQUIRE (rig.submitAndTick ("go").applied >= 1);

    const auto enteredAt = rig.tick;
    const std::string members[] { rig.early, rig.middle, rig.late };
    std::map<std::string, std::int64_t> playedSince;

    for (std::size_t n = 0; n < 3; ++n)
    {
        //  A second and a half into member n, which is 4n + 1.5 in scene time.
        const auto seconds = 4.0 * static_cast<double> (n) + 1.5;
        const auto until = enteredAt + static_cast<std::int64_t> (seconds * 50.0);

        runOn (rig, until, 4.0, playedSince);

        std::set<std::string> scheduled;

        for (const auto& run : rig.runs.all())
            if (run.kind == "media" && ! run.isFinished()
                 && run.state == cue::runState::playing)
                scheduled.insert (run.cue);

        const auto plan = cue::solve (rig.document, &rig.durations, nullptr,
                                      { rig.listId, members[n], 1.5 });

        REQUIRE (plan.ok);

        /*  MEDIA AGAINST MEDIA. The plan carries the target's GROUPS as well,
            outermost first, because a member sounds as part of its scene - so
            the two sides are made comparable by asking each for the same thing
            rather than by quietly dropping something from one of them. That the
            scene is in the plan is checked below, where it means something. */
        std::set<std::string> solved;

        for (const auto& planned : plan.runs)
            if (planned.when == cue::planned::sounding
                 && rig.document.findById (planned.cue).getType().toString() == "Media")
                solved.insert (planned.cue);

        const auto spell = [] (const std::set<std::string>& set)
        {
            std::string out;

            for (const auto& cueId : set)
                out += (out.empty() ? "" : " ") + cueId;

            return out.empty() ? std::string ("nothing") : out;
        };

        INFO ("member " << n << ", " << seconds << " s in"
               << " | the scheduler sounds " << spell (scheduled)
               << " | the solver says " << spell (solved));

        CHECK (solved == scheduled);
    }
}

TEST_CASE ("jump: the scene is rebuilt mid-way, with every member accounted for")
{
    /*  A member that is missing is a member the group spawns a second time; one
        missing from the FINISHED end is a group that thinks it has not started.
        So a jump five seconds into the scene has to produce all three. */
    JumpRig rig;

    CHECK (rig.jumpTo (rig.middle, 3.0).applied == 1);      // five seconds into the scene

    /*  The group is playing, with the round and the loop count the scheduler
        will re-read every tick. */
    const auto* group = rig.liveRunOf (rig.scene);
    REQUIRE (group != nullptr);
    CHECK (group->state == cue::runState::playing);
    CHECK (group->iterations == 1);
    CHECK (group->round.size() == 3u);

    /*  The first member is over, the second is sounding, the third is waiting
        for the five seconds it has left. */
    const auto* first = rig.runs.find (rig.runOf (rig.early));
    REQUIRE (first != nullptr);
    CHECK (first->isFinished());

    const auto* second = rig.liveRunOf (rig.middle);
    REQUIRE (second != nullptr);
    CHECK (second->launchRequested);
    CHECK (second->startOffset == doctest::Approx (3.0));

    const auto* third = rig.liveRunOf (rig.late);
    REQUIRE (third != nullptr);
    CHECK (third->state == cue::runState::waiting);

    /*  Five seconds at fifty ticks a second, from the tick the jump was
        applied on. */
    CHECK (third->dueTick > rig.runs.find (rig.runOf (rig.middle))->launchRequestedAtTick);

    /*  And every one of them is inside the scene. */
    for (const auto* run : { first, second, third })
        CHECK (run->parent == group->id);
}

TEST_CASE ("jump: the pointer lands after the target and the state position agrees")
{
    /*  §3.5 for the first, §3.13 for the second: after a jump the two pointers
        agree, and the divergence afterwards is what a running view shows. */
    JumpRig rig;

    rig.jumpTo (rig.middle, 1.0);

    /*  AFTER THE SCENE, AND NOT ON ITS THIRD MEMBER, which is the interesting
        half. "Positionally after the target" has to mean the next place a GO
        would mean something, and a jump has just set the whole scene running:
        the third member is already scheduled, so a pointer left on it would arm
        the operator's next press to fire a cue the scene is about to fire
        itself.

        THE REASON THIS USED TO GIVE WAS A DIFFERENT ONE, and it stopped being
        true on 2026-09-16. It said the pointer may sit at the top of a list or
        inside a manual sequence group "and nowhere else" - which was the rule
        until the author asked for the pointer to stand inside every group, and
        is the rule the solver's own copy of the walk (`ShowWalk.h`,
        `Placed::mayLandHere`) still applies. The behaviour here is right for
        the reason above and the assertion has not moved; what has moved is that
        the cursor and the solver now answer "where may the pointer be"
        differently, and this is the case that would notice if somebody made the
        solver agree. */
    CHECK (rig.standby() == rig.after);

    const auto landed = rig.runner.listState().positionOf (rig.listId);
    CHECK (landed.cue == rig.middle);
    CHECK (landed.offset == doctest::Approx (1.0));
}

TEST_CASE ("jump: what it abandons is ended, and its voices come back")
{
    /*  A jump that left the old scene running would be two shows at once. Ended
        the way `run.kill` ends a run - the whole descent - and with NO FOOTER,
        because a footer is arbitrary and need not be an inverse: running one
        would be arbitrary work fighting the values the jump is about to send. */
    JumpRig rig;

    //  Something playing that the jump does not want: the plain media cue at
    //  the top of the list, fired from cold.
    rig.setStandby (rig.mediaId);
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));

    const auto abandoned = rig.runOf (rig.mediaId);
    REQUIRE (! abandoned.empty());
    REQUIRE_FALSE (rig.runs.find (abandoned)->isFinished());

    rig.jumpTo (rig.middle, 1.0);

    CHECK (rig.runs.find (abandoned)->isFinished());

    /*  AND IT IS NOT HOLDING A VOICE. `holdsTrack()` is a track and an
        unfinished run, so ending it is the whole of letting go - and the track
        itself is busy again a moment later, because the jump's own members took
        it. Asking about the TRACK would therefore be asking the wrong question:
        what matters is that this run has let go of it. */
    CHECK_FALSE (rig.runs.find (abandoned)->holdsTrack());
    CHECK (rig.runs.find (abandoned)->claims.empty());
}

TEST_CASE ("jump: a planned cue that is already running is relaunched at the offset, never doubled")
{
    /*  PRD §3.25: load-to-time "stops and relaunches a cue already playing at
        the wrong offset". Until 2026-09-26 the sweep passed over every run of a
        cue the plan names, and the build - which makes every planned cue
        afresh - made a second one beside it: a playing run sounding on under
        its own relaunch, or an armed standby run holding a voice the relaunch
        needed. The persistent section keeping its voices through a jump (the
        same day) is what made the second of those fail `no-track`. */
    JumpRig rig;

    rig.setStandby (rig.mediaId);
    CHECK (rig.submitAndTick ("go").applied == 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));

    const auto before = rig.runOf (rig.mediaId);
    REQUIRE_FALSE (rig.runs.find (before)->isFinished());

    REQUIRE (rig.jumpTo (rig.mediaId, 1.0).applied == 1);

    CHECK (rig.runs.find (before)->isFinished());
    CHECK_FALSE (rig.runs.find (before)->holdsTrack());

    auto live = 0;

    for (const auto& run : rig.runs.all())
        if (run.cue == rig.mediaId && ! run.isFinished())
            ++live;

    CHECK (live == 1);

    const auto* relaunched = rig.liveRunOf (rig.mediaId);
    REQUIRE (relaunched != nullptr);
    CHECK (relaunched->id != before);
    CHECK (relaunched->startOffset == doctest::Approx (1.0));
}

TEST_CASE ("jump: the record carries every run it drew, and the signature accepts them")
{
    /*  The `go` guarantee, widened to a jump: a replay never draws a number of
        its own, so every identifier the handler produced is in the record - and
        the record it writes is one the arity check will let back in, or the
        session could not reproduce itself. */
    JumpRig rig;

    rig.jumpTo (rig.middle, 1.0);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto jump = std::find_if (parsed.records.begin(), parsed.records.end(),
                                    [] (const auto& record)
                                    {
                                        return record.command == "list.loadToTime";
                                    });

    REQUIRE (jump != parsed.records.end());

    /*  The list, then the group and its three members. */
    REQUIRE (jump->args.size() == 5u);
    CHECK (jump->args[0].getString() == rig.listId);

    for (std::size_t n = 1; n < jump->args.size(); ++n)
        CHECK (rig.runs.find (jump->args[n].getString()) != nullptr);

    const auto* command = rig.engine.commands().find ("list.loadToTime");
    REQUIRE (command != nullptr);

    const auto check = CommandRegistry::checkArgs (*command, jump->args);
    CHECK (check.ok);
    CHECK (check.reason == "");
}

TEST_CASE ("jump: the scheduler carries the scene on from where the jump left it")
{
    /*  The whole point of building a tree the scheduler recognises. Nothing is
        told about the jump: the third member's wait expires on its own and the
        group launches it, because a group job re-reads the round from the run
        every tick rather than keeping a copy. */
    JumpRig rig;

    rig.jumpTo (rig.middle, 1.0);           // three seconds into the scene

    const auto third = rig.runOf (rig.late);
    REQUIRE (! third.empty());
    REQUIRE (rig.runs.find (third)->state == cue::runState::waiting);

    /*  Seven seconds of ticks, and the member that was still to come has been
        asked for. */
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (third)->state
                                          != cue::runState::waiting; }, 500));

    CHECK (rig.runs.find (third)->state != cue::runState::waiting);
}

TEST_CASE ("jump: with no aim there is nothing to make true")
{
    /*  A jump is `list.aim`'s question answered, so without a question it is
        applied and does nothing - rather than jumping somewhere nobody asked
        for. */
    JumpRig rig;

    const auto result = rig.submitAndTick ("list.loadToTime",
                                           { osc::Value::string (rig.listId) });

    CHECK (result.applied == 1);
    CHECK (rig.runs.all().empty());
}

//==============================================================================
/*  A SEAT IS IN THE ROUND ITS PLAN IS IN (J1, 2026-10-01, namespace draft
    §23.8; the author: "fix it, own commit").

    A jump builds the scene it lands in, and `seatPlan` gave every group it made
    `iteration` nought - no round begun - beside `iterations` from the group's
    `loops`. So the round the jump landed in was never counted. When it ended,
    `endOfRound` found nought below one and drew ANOTHER round of a scene that
    plays once, and every member played again. And the GO handler reads the same
    count, through `standbyAfterFiring`: on a manual group's last member,
    `heldForAnotherRound` saw a round still to play and sent the pointer back to
    the group's first member instead of on past the group.

    The solver gives no round for a group - a looping one is `unknown-round`,
    and the round it says it took is the first - so a run the seat makes is in
    round one. A run already standing, the scene a seek re-seats at a second of
    itself, is in the round it was in, and keeps it.

    AND A SEEK SEATS ONLY WHAT THE WALK CAN PLACE (HX, the J1 review). A scene
    whose members the walk gives no seconds - one that loops, a timeline with a
    header, a manual group - has nothing to re-seat. The seek ended every member
    all the same and seated the scene alone, which with its round kept ended a
    scene in its last round where the hand had put it, footer and all. It is now
    applied and changes nothing. */
namespace
{
    /** How many rounds the log says a group run drew. */
    std::size_t roundsDrawn (JumpRig& rig, const std::string& groupRun)
    {
        std::size_t count = 0;

        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.kind == LogRecord::Kind::applied && record.command == "run.round"
                  && ! record.args.empty() && record.args[0].getString() == groupRun)
                ++count;

        return count;
    }

    /** The seed of the newest round a group run drew, or nought when none. */
    std::int32_t newestRoundSeed (JumpRig& rig, const std::string& groupRun)
    {
        std::int32_t seed = 0;

        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.kind == LogRecord::Kind::applied && record.command == "run.round"
                  && record.args.size() > 1 && record.args[0].getString() == groupRun)
                seed = record.args[1].getInt32();

        return seed;
    }

    /** How many runs a cue has had. */
    std::size_t runsOfCue (const JumpRig& rig, const std::string& cueId)
    {
        std::size_t count = 0;

        for (const auto& run : rig.runs.all())
            if (run.cue == cueId)
                ++count;

        return count;
    }

    /** The unfinished runs under a group run, in the order they were made. */
    std::vector<std::string> unfinishedUnder (const JumpRig& rig, const std::string& groupRun)
    {
        std::vector<std::string> out;

        for (const auto* child : rig.runs.childrenOf (groupRun))
            if (! child->isFinished())
                out.push_back (child->id);

        return out;
    }

    /** The first record of a command applied to a run, or nothing. */
    std::optional<LogRecord> firstApplied (JumpRig& rig, const std::string& command,
                                           const std::string& runId)
    {
        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.kind == LogRecord::Kind::applied && record.command == command
                  && ! record.args.empty() && record.args[0].getString() == runId)
                return record;

        return std::nullopt;
    }

    /*  `runOn` a tick at a time until a predicate holds - the audio side playing
        its part, a launched run sounding and one whose material has run out
        stopping. Bounded, so a scene that never ends fails the case rather than
        hanging the suite. */
    template <typename Predicate>
    bool playOn (JumpRig& rig, Predicate done, std::map<std::string, std::int64_t>& since,
                 int bound = 2500)
    {
        for (int n = 0; n < bound; ++n)
        {
            if (done())
                return true;

            runOn (rig, rig.tick + 1, 4.0, since);
        }

        return done();
    }

    /*  The session replayed record for record into a fresh rig with no audio
        side, and the pointer read where the replay left it. A handler decision
        is only a decision if a replay takes it the same way. Read on the
        session's own list: the show read in replaces the fresh rig's. */
    std::string replayedStandby (JumpRig& rig)
    {
        const auto show = doc::CanonicalXml::write (rig.document);
        const auto original = LogFile::parse (rig.engine.log().contents());
        REQUIRE (original.errors.empty());

        JumpRig fresh;
        fresh.runner.setPlayer (nullptr);

        const auto loaded = doc::CanonicalXml::read (show, fresh.document);
        REQUIRE (loaded.ok);

        const auto result = replay (fresh.engine, original);

        for (const auto& mismatch : result.mismatches)
            INFO (mismatch);

        CHECK (result.ok);
        return fresh.document.findById (rig.listId)[juce::Identifier ("standby")].toString().toStdString();
    }
}

TEST_CASE ("jump: a scene jumped into ends after the round it was in, and does not play it again")
{
    /*  The scene plays once, `loops` being one unless a show says otherwise.
        Seated at round nought, it played the round the jump had put it in, drew
        a second, and played every member again. */
    JumpRig rig;
    auto aimed = rig.middle;

    SUBCASE ("a timeline")
    {
        //  JumpRig's own scene, three seconds in: one member sounding at three
        //  seconds, one at one second, the last due in seven.
    }

    SUBCASE ("an automatic sequence, jumped into its last member")
    {
        /*  The LAST member, so that nothing is seated still to come: a
            sequence spawns its next member itself, and one the seat has
            already made waiting beside it is another fault than this one. */
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/advance", "auto").ok);
        aimed = rig.late;
    }

    REQUIRE (rig.jumpTo (aimed, 1.0).rejected == 0);

    const auto* scene = rig.liveRunOf (rig.scene);
    REQUIRE (scene != nullptr);
    const auto sceneRun = scene->id;

    //  In the round the plan is in: the first, which is the only one.
    CHECK (scene->iteration == 1);
    CHECK (scene->iterations == 1);

    std::map<std::string, std::int64_t> since;
    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));

    //  It ended after that round: no second round drawn, every member played once.
    CHECK (roundsDrawn (rig, sceneRun) == 0u);

    for (const auto& member : { rig.early, rig.middle, rig.late })
    {
        INFO ("member " << member);
        CHECK (runsOfCue (rig, member) == 1u);
    }
}

TEST_CASE ("jump: into a manual group, and the GO on its last member walks on rather than wrapping")
{
    /*  The handler's half, the one a replay reads. A manual group's last member
        keeps the pointer only while the group has a round still to play, and a
        group seated at round nought always had one: the GO sent the pointer back
        to the group's first member, and the group, ending its round, drew another
        and started that member on its own. */
    JumpRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);

    std::map<std::string, std::int64_t> since;

    SUBCASE ("read from the order")
    {
        //  Nothing fired yet: the jump is read from the list's order.
    }

    SUBCASE ("read from the history")
    {
        /*  The scene entered by GO and its second member fired, then the jump
            back into that member: what it is read from is the list's history.
            Parked through the command, so the replay is given the pointer. The
            second member waits two seconds before it sounds. */
        REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (rig.early) }).applied == 1);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (playOn (rig, [&] { return rig.liveRunOf (rig.early) != nullptr
                                             && rig.liveRunOf (rig.early)->state == cue::runState::playing; },
                         since, 50));

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (playOn (rig, [&] { return rig.liveRunOf (rig.middle) != nullptr
                                             && rig.liveRunOf (rig.middle)->state == cue::runState::playing; },
                         since, 200));
    }

    REQUIRE (rig.jumpTo (rig.middle, 1.0).rejected == 0);
    REQUIRE (rig.standby() == rig.late);

    const auto* scene = rig.liveRunOf (rig.scene);
    REQUIRE (scene != nullptr);
    const auto sceneRun = scene->id;
    CHECK (scene->iteration == 1);

    //  The last member: the group has no round left, so the pointer leaves it.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    CHECK (rig.standby() == rig.after);

    //  And the group ends after that member, without starting its first again.
    const auto firstsBefore = runsOfCue (rig, rig.early);

    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
    CHECK (roundsDrawn (rig, sceneRun) == 0u);
    CHECK (runsOfCue (rig, rig.early) == firstsBefore);

    CHECK (replayedStandby (rig) == rig.standby());
}

TEST_CASE ("rounds: a scene a jump seats in the first of two rounds plays exactly one round more")
{
    /*  `loops` two. A jump's seat is always in the first round - the solver
        cannot tell a looping scene's rounds apart - so the round the jump landed
        in is counted and exactly one is left to play, whole. Seated at round
        nought, the scene played two more. */
    JumpRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/loops", "2").ok);

    //  All three members at nought, so that a round is four seconds.
    for (const auto& member : { rig.middle, rig.late })
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0").ok);

    std::map<std::string, std::int64_t> since;

    SUBCASE ("an automatic sequence")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/advance", "auto").ok);

        REQUIRE (rig.jumpTo (rig.late, 1.0).rejected == 0);

        const auto* scene = rig.liveRunOf (rig.scene);
        REQUIRE (scene != nullptr);
        const auto sceneRun = scene->id;
        CHECK (scene->iteration == 1);
        CHECK (scene->iterations == 2);

        /*  COUNTED FROM THE SEAT, NOT FROM NOTHING. What the seat makes beside
            the member it landed on is the walk's to say, and today it says
            nothing for a chain that loops (§13.8) - a walk taught to time round
            one of it would seat the two members before as over, and the round
            still to play is the same either way. */
        std::map<std::string, std::size_t> seated;

        for (const auto& member : { rig.early, rig.middle, rig.late })
            seated[member] = runsOfCue (rig, member);

        CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));

        //  One round more, whole: every member once more, and nothing else.
        CHECK (roundsDrawn (rig, sceneRun) == 1u);

        for (const auto& member : { rig.early, rig.middle, rig.late })
        {
            INFO ("member " << member);
            CHECK (runsOfCue (rig, member) == seated[member] + 1u);
        }
    }

    SUBCASE ("a manual group: the pointer wraps once, then walks on")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);

        REQUIRE (rig.jumpTo (rig.middle, 1.0).rejected == 0);
        REQUIRE (rig.standby() == rig.late);

        const auto* scene = rig.liveRunOf (rig.scene);
        REQUIRE (scene != nullptr);
        const auto sceneRun = scene->id;

        //  Round one's last member: a round is still to play, so back to the top.
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.standby() == rig.early);

        //  Round two, once round one's members are done; then its three members.
        REQUIRE (playOn (rig, [&] { return roundsDrawn (rig, sceneRun) == 1u; }, since));

        for (const auto* expected : { &rig.middle, &rig.late, &rig.after })
        {
            REQUIRE (rig.submitAndTick ("go").rejected == 0);
            CHECK (rig.standby() == *expected);
        }

        CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
        CHECK (roundsDrawn (rig, sceneRun) == 1u);
    }
}

TEST_CASE ("rounds: a shuffled scene jumped into draws its next round from a seed of its own")
{
    /*  Nobody seeded the shuffle, so each run draws a seed of its own and
        writes it into the log (`group,seed`: "zero means a fresh one per run").
        A run the seat makes holds the group's seed, nought - "no seed" - and,
        seated in round one, it takes its own seed from round two on: drawn from
        nought, every jump into the scene would shuffle the same way, from a
        seed the show never used. It draws one at round two, as a fired run
        does at round one. */
    JumpRig rig;

    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>> {
             { "mode", "sequence" }, { "advance", "auto" }, { "selection", "shuffle" }, { "loops", "2" } })
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/" + name, value).ok);

    REQUIRE (rig.jumpTo (rig.late, 1.0).rejected == 0);

    const auto* scene = rig.liveRunOf (rig.scene);
    REQUIRE (scene != nullptr);
    const auto sceneRun = scene->id;

    std::map<std::string, std::int64_t> since;
    REQUIRE (playOn (rig, [&] { return roundsDrawn (rig, sceneRun) == 1u; }, since));

    CHECK (newestRoundSeed (rig, sceneRun) != 0);
}

TEST_CASE ("seek: a scene that plays once, scrubbed, plays the rest of its round from there and ends")
{
    /*  The everyday scrub. JumpRig's scene plays once and the walk times it, so
        a seek three seconds in re-seats its members under the scene's own run at
        their seconds - the first three seconds in, the second one second in, the
        last due in seven - and the scene keeps the round it is in (HV). Seated
        at round nought, as every seat was before J1, it played that round out
        from the second asked for, then drew another and played every member
        again from the top. */
    JumpRig rig;
    std::map<std::string, std::int64_t> since;
    std::string sceneRun;
    std::size_t rounds = 0;
    std::size_t runsEach = 0;
    auto firstIn = 3.0;                 // where the seek puts the first member

    SUBCASE ("scrubbed while it plays its round")
    {
        rig.setStandby (rig.scene);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto* scene = rig.liveRunOf (rig.scene);
        REQUIRE (scene != nullptr);
        sceneRun = scene->id;

        //  Its round drawn, and its first member a second in.
        REQUIRE (playOn (rig, [&] { return roundsDrawn (rig, sceneRun) == 1u; }, since));
        playOn (rig, [] { return false; }, since, 50);
        REQUIRE (rig.runs.find (sceneRun)->iteration == 1);

        rounds = 1;
        runsEach = 2;       // the one its round made, and the one the seek made
    }

    SUBCASE ("scrubbed in its own pre-wait, before its round has begun")
    {
        /*  HV's other half: a standing run that has begun no round is seated as
            a new one is, in round one. Fired by name with a second of pre-wait,
            and sought while it waits. */
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/preWait", "1").ok);
        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (rig.scene) }).rejected == 0);

        const auto* scene = rig.liveRunOf (rig.scene);
        REQUIRE (scene != nullptr);
        REQUIRE (scene->state == cue::runState::waiting);
        REQUIRE (scene->iteration == 0);
        sceneRun = scene->id;

        rounds = 0;         // the seat counts the round it is in, and nothing draws one
        runsEach = 1;       // the seek's

        //  A scene's seconds count from its entry, its own pre-wait among them
        //  (§3.6), so three seconds in is its first member's second.
        firstIn = 2.0;
    }

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (sceneRun),
                                              osc::Value::float64 (3.0) }).rejected == 0);

    //  In the first round: the one it was in, or the one it is seated in.
    CHECK (rig.runs.find (sceneRun)->iteration == 1);

    //  Its members seated at their seconds, under its own run.
    const auto* first = rig.liveRunOf (rig.early);
    REQUIRE (first != nullptr);
    CHECK (first->parent == sceneRun);
    CHECK (first->startOffset == doctest::Approx (firstIn));

    //  The rest of that round, and no round more.
    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
    CHECK (rig.runs.find (sceneRun)->iteration == 1);
    CHECK (roundsDrawn (rig, sceneRun) == rounds);

    for (const auto& member : { rig.early, rig.middle, rig.late })
    {
        INFO ("member " << member);
        CHECK (runsOfCue (rig, member) == runsEach);
    }
}

TEST_CASE ("seek: a manual group sought keeps its round, and the GO on its last member walks on")
{
    /*  The `go` HANDLER after a seek, the half a replay reads. A manual group
        has an operator between its members and no second to seek to - the
        clients offer it no drag - but `run.seek` takes any group run. Before J1
        the seek's seat wrote the group's round back to nought, so the GO on its
        last member sent the pointer back to its first, and the group, ending
        that member, drew another round, started its first member by itself and
        waited for GOs for ever. The walk places no member of a manual group, so
        the seek now leaves it as it is (HX): its round, its members, and so the
        pointer, which walks on past it. */
    JumpRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);

    std::map<std::string, std::int64_t> since;
    std::string sceneRun;
    std::size_t rounds = 0;
    std::vector<std::string> walk;      // where each GO after the seek puts the pointer

    SUBCASE ("in its only round, two of its members fired")
    {
        /*  Parked through the command, so the replay is given the pointer. The
            second member waits two seconds before it sounds. */
        REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (rig.early) }).applied == 1);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (playOn (rig, [&] { return rig.liveRunOf (rig.early) != nullptr
                                             && rig.liveRunOf (rig.early)->state == cue::runState::playing; },
                         since, 50));

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (playOn (rig, [&] { return rig.liveRunOf (rig.middle) != nullptr
                                             && rig.liveRunOf (rig.middle)->state == cue::runState::playing; },
                         since, 200));

        REQUIRE (rig.liveRunOf (rig.scene) != nullptr);
        sceneRun = rig.liveRunOf (rig.scene)->id;

        rounds = 1;                     // the GO that entered it drew its round
        walk = { rig.after };
    }

    SUBCASE ("in the second of two rounds, its first member fired")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/loops", "2").ok);

        for (const auto& member : { rig.middle, rig.late })
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0").ok);

        //  Jumped into round one and its last member GO'd: back to the top, and
        //  round two drawn once round one is done.
        REQUIRE (rig.jumpTo (rig.middle, 1.0).rejected == 0);
        REQUIRE (rig.liveRunOf (rig.scene) != nullptr);
        sceneRun = rig.liveRunOf (rig.scene)->id;

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == rig.early);
        REQUIRE (playOn (rig, [&] { return roundsDrawn (rig, sceneRun) == 1u; }, since));

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == rig.middle);
        REQUIRE (rig.runs.find (sceneRun)->iteration == 2);

        rounds = 1;                     // round two's: the jump's seat drew none
        walk = { rig.late, rig.after };
    }

    const auto iterationBefore = rig.runs.find (sceneRun)->iteration;
    const auto unfinishedBefore = unfinishedUnder (rig, sceneRun);
    REQUIRE (! unfinishedBefore.empty());

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (sceneRun),
                                              osc::Value::float64 (1.0) }).rejected == 0);

    //  Left as it was: in the same round, with the same members.
    CHECK (rig.runs.find (sceneRun)->iteration == iterationBefore);
    CHECK (unfinishedUnder (rig, sceneRun) == unfinishedBefore);

    //  And the GO on its last member takes the pointer past it.
    for (const auto& expected : walk)
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.standby() == expected);
    }

    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
    CHECK (roundsDrawn (rig, sceneRun) == rounds);

    CHECK (replayedStandby (rig) == rig.standby());
}

TEST_CASE ("seek: a scene the walk cannot time is left as it is, playing where it was")
{
    /*  HX (namespace draft §23.8). The walk gives a scene's members their
        seconds only when nothing in the way of the arithmetic is unknown - one
        round, every member, in the written order, no header, the machine pacing
        it (§13.8) - so for any other scene the solver places none of them, and a
        seek has nothing to seat. It ended every member all the same and seated
        the scene alone, over a job with nothing left to wait for. With the round
        kept (HV), a timeline in its last round - where one with a header that
        plays once always is - ran its footer and ended where the hand had put
        it; before J1 it began its round again from the top. An automatic sequence
        that loops awaited nothing for ever, either way. Now the seek is applied
        and changes nothing. The clients offer the drag on all of these. */
    JumpRig rig;
    std::map<std::string, std::int64_t> since;
    std::string sceneRun;
    std::size_t rounds = 1;             // the rounds the scene draws, all told

    const auto loopTwice = [&rig]
    {
        //  Two rounds of four seconds: every member at nought.
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/loops", "2").ok);

        for (const auto& member : { rig.middle, rig.late })
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0").ok);
    };

    const auto goInto = [&rig, &since, &sceneRun] (std::size_t roundToReach)
    {
        rig.setStandby (rig.scene);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto* scene = rig.liveRunOf (rig.scene);
        REQUIRE (scene != nullptr);
        sceneRun = scene->id;

        //  Into that round, and half a second of it played.
        REQUIRE (playOn (rig, [&] { return roundsDrawn (rig, sceneRun) == roundToReach; }, since));
        playOn (rig, [] { return false; }, since, 25);
        REQUIRE (rig.runs.find (sceneRun)->iteration == static_cast<int> (roundToReach));
    };

    SUBCASE ("a timeline with a header, in the one round it plays")
    {
        const auto header = rig.document.createRole (rig.scene, "header");
        REQUIRE (header.ok);
        REQUIRE (rig.document.createCue (header.id, 0, "memo", "House to half").ok);

        goInto (1);
    }

    SUBCASE ("a timeline in the first of two rounds")
    {
        loopTwice();
        goInto (1);
        rounds = 2;
    }

    SUBCASE ("a timeline in the second of two rounds")
    {
        loopTwice();
        goInto (2);
        rounds = 2;
    }

    SUBCASE ("an automatic sequence that loops, in its first round")
    {
        loopTwice();
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/mode", "sequence").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/advance", "auto").ok);
        goInto (1);
        rounds = 2;
    }

    const auto iterationBefore = rig.runs.find (sceneRun)->iteration;
    const auto unfinishedBefore = unfinishedUnder (rig, sceneRun);
    const auto madeBefore = rig.runs.childrenOf (sceneRun).size();
    REQUIRE (! unfinishedBefore.empty());

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (sceneRun),
                                              osc::Value::float64 (3.0) }).rejected == 0);

    //  A few ticks on: still playing, in the same round, with the same members -
    //  none ended, none made.
    playOn (rig, [] { return false; }, since, 10);

    CHECK_FALSE (rig.runs.find (sceneRun)->isFinished());
    CHECK (rig.runs.find (sceneRun)->iteration == iterationBefore);
    CHECK (unfinishedUnder (rig, sceneRun) == unfinishedBefore);
    CHECK (rig.runs.childrenOf (sceneRun).size() == madeBefore);

    //  And on to its own end, with no round more than it has.
    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
    CHECK (roundsDrawn (rig, sceneRun) == rounds);
}

TEST_CASE ("seek: one landing in the drain where a looping scene draws its first round changes nothing either")
{
    /*  A client's record goes ahead of the hook's in a drain, and the tick a
        scene leaves `entering` is the one in which its job draws its first
        round. A seek queued for that tick found the scene with no round begun,
        and the seat put it in round one (J1) - and the `run.round` behind it,
        already on its way, counted round two: a scene that loops twice played
        one round, and not even that, since the seek had ended its members.
        Before J1 the seat wrote nought, the record made it one, and the second
        round played. A scene that loops is one the walk cannot time, so the
        seek now changes nothing, whatever drain it lands in (HX). */
    JumpRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.scene + "/loops", "2").ok);

    for (const auto& member : { rig.middle, rig.late })
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0").ok);

    //  Made ready at standby, its three members armed under it, and entered.
    rig.setStandby (rig.scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto* scene = rig.liveRunOf (rig.scene);
    REQUIRE (scene != nullptr);
    const auto sceneRun = scene->id;
    REQUIRE (roundsDrawn (rig, sceneRun) == 0u);

    const auto armedBefore = unfinishedUnder (rig, sceneRun);
    REQUIRE (! armedBefore.empty());

    //  Queued ahead of the tick in which its job draws round one.
    REQUIRE (rig.engine.submit ("cli", "run.seek", { osc::Value::string (sceneRun),
                                                     osc::Value::float64 (1.0) }));
    rig.tickOnce();

    //  The race, really run: the seek and the first round in one drain, the seek first.
    const auto seekRecord = firstApplied (rig, "run.seek", sceneRun);
    const auto roundRecord = firstApplied (rig, "run.round", sceneRun);
    REQUIRE (seekRecord.has_value());
    REQUIRE (roundRecord.has_value());
    REQUIRE (seekRecord->tick == roundRecord->tick);
    REQUIRE (seekRecord->seq < roundRecord->seq);

    //  Round one counted once, and nothing it held ended.
    CHECK (rig.runs.find (sceneRun)->iteration == 1);
    CHECK (unfinishedUnder (rig, sceneRun) == armedBefore);

    //  And both rounds play, whole.
    std::map<std::string, std::int64_t> since;
    CHECK (playOn (rig, [&] { return rig.runs.find (sceneRun)->isFinished(); }, since));
    CHECK (roundsDrawn (rig, sceneRun) == 2u);

    for (const auto& member : { rig.early, rig.middle, rig.late })
    {
        INFO ("member " << member);
        CHECK (runsOfCue (rig, member) == 2u);
    }
}

//==============================================================================
/*  THE SEEK: scrubbing a running cue or a running scene (author, 2026-09-18).

    `run.seek` is one record per position the hand settles on. For a media run
    it is the voice stopped and asked for again at the new second - the SAME
    run, so the row, the identifier and any fade aimed at it are untouched. For
    a group it is the scene re-seated at a second of its own timeline under the
    same group run, which is what brings a member already over back.
*/
TEST_CASE ("seek: a media run moves to a second of its file and stays the run it was")
{
    Rig rig;
    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    const auto id = rig.runs.all().front().id;
    auto* run = rig.runs.find (id);
    REQUIRE (run != nullptr);
    REQUIRE (run->state == cue::runState::playing);
    REQUIRE (rig.audio.launches.size() == 1u);

    const auto track = run->track;
    rig.audio.playing.insert (track);
    rig.tickOnce();

    /*  A fade has brought it down to -12 dB; the seek must keep that. */
    run->ownLevel = -12.0;
    run->level = -12.0;

    const auto result = rig.submitAndTick ("run.seek", { osc::Value::string (id),
                                                         osc::Value::float64 (12.5) });
    CHECK (result.applied == 1);

    /*  The voice was stopped and asked for again at the second, on the same
        track, at the faded level - and the run is still playing, still the
        same identifier, with its head where the hand put it. */
    REQUIRE (rig.audio.stopped.size() == 1u);
    CHECK (rig.audio.stopped.front() == track);
    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms.front().track == track);
    CHECK (rig.audio.arms.front().startOffset == doctest::Approx (12.5));
    CHECK (rig.audio.arms.front().levelDb == doctest::Approx (-12.0));

    run = rig.runs.find (id);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::playing);
    CHECK (run->positionOrigin == doctest::Approx (12.5));
    CHECK (run->launchRequested);
    CHECK (rig.runs.all().size() == 1u);

    /*  The silence between the stop and the new launch is not the cue ending:
        the edge watcher was told to forget it had heard the sound. */
    rig.tickOnce();
    CHECK (rig.runs.find (id)->state == cue::runState::playing);

    /*  The disk answers, the launch is placed again, and the sound resumes. */
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.audio.launches.size() == 2u);

    rig.audio.playing.insert (track);
    rig.tickOnce();
    CHECK (rig.runs.find (id)->state == cue::runState::playing);

    /*  And from there the ordinary end still ends it. */
    rig.audio.playing.erase (track);
    rig.tickOnce();
    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

TEST_CASE ("seek: a scene is re-seated at a second of itself, under the run it already has")
{
    /*  The scene's three members sound at nought, two and ten seconds and last
        four each. Fired, and then scrubbed to three seconds in: the first is
        three seconds in, the second one second in, the third due in seven -
        all under the group run GO made, which keeps its identifier. */
    JumpRig rig;
    rig.setStandby (rig.scene);
    rig.submitAndTick ("go");
    rig.tickOnce();

    const auto* group = rig.liveRunOf (rig.scene);
    REQUIRE (group != nullptr);
    const auto groupId = group->id;

    /*  Let the scene get going: the first member launches, the others wait. */
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    std::vector<std::string> before;

    for (const auto& run : rig.runs.all())
        if (run.id != groupId)
            before.push_back (run.id);

    REQUIRE (! before.empty());

    const auto at = rig.tick;
    const auto result = rig.submitAndTick ("run.seek", { osc::Value::string (groupId),
                                                         osc::Value::float64 (3.0) });
    CHECK (result.applied == 1);

    /*  The same group run, playing, and reading three seconds in. */
    group = rig.liveRunOf (rig.scene);
    REQUIRE (group != nullptr);
    CHECK (group->id == groupId);
    CHECK (group->state == cue::runState::playing);
    CHECK (group->launchRequestedAtTick == at - 150);

    /*  What it held before is over. */
    for (const auto& id : before)
    {
        const auto* old = rig.runs.find (id);
        REQUIRE (old != nullptr);
        CHECK (old->isFinished());
    }

    const auto* first = rig.liveRunOf (rig.early);
    REQUIRE (first != nullptr);
    CHECK (first->launchRequested);
    CHECK (first->startOffset == doctest::Approx (3.0));

    const auto* second = rig.liveRunOf (rig.middle);
    REQUIRE (second != nullptr);
    CHECK (second->launchRequested);
    CHECK (second->startOffset == doctest::Approx (1.0));

    const auto* third = rig.liveRunOf (rig.late);
    REQUIRE (third != nullptr);
    CHECK (third->state == cue::runState::waiting);
    CHECK (third->dueTick == at + 350);

    for (const auto* run : { first, second, third })
        CHECK (run->parent == groupId);

    /*  One job carries the scene on, and it is the group's. */
    auto jobs = 0;

    for (const auto& job : rig.runner.groups())
        if (job.run == groupId && ! job.retired)
            ++jobs;

    CHECK (jobs == 1);

    /*  AND NOTHING LAUNCHES THEM A SECOND TIME. The first live scrub had the
        retired job take the fresh members and launch them again, which for
        the waiting one began its wait afresh: its due tick is the test. */
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.liveRunOf (rig.late) != nullptr);
    CHECK (rig.liveRunOf (rig.late)->dueTick == at + 350);
    CHECK (rig.liveRunOf (rig.late)->state == cue::runState::waiting);

    /*  And scrubbing back before the first member is over brings a finished
        one back: at eleven seconds the first two are over and the third is
        one second in; at nought all three are ahead again. */
    rig.submitAndTick ("run.seek", { osc::Value::string (groupId), osc::Value::float64 (11.0) });
    REQUIRE (rig.liveRunOf (rig.late) != nullptr);
    CHECK (rig.liveRunOf (rig.late)->startOffset == doctest::Approx (1.0));
    CHECK (rig.liveRunOf (rig.early) == nullptr);

    rig.submitAndTick ("run.seek", { osc::Value::string (groupId), osc::Value::float64 (0.0) });
    REQUIRE (rig.liveRunOf (rig.early) != nullptr);
    CHECK (rig.liveRunOf (rig.early)->launchRequested);
    REQUIRE (rig.liveRunOf (rig.middle) != nullptr);
    CHECK (rig.liveRunOf (rig.middle)->state == cue::runState::waiting);
}

TEST_CASE ("seek: what has no material to seek in is refused, and a run that is over is left")
{
    Rig rig;
    rig.setStandby (rig.memoId);
    rig.submitAndTick ("go");

    const auto memoRun = rig.runOf (rig.memoId);
    REQUIRE (! memoRun.empty());

    /*  A memo has nothing to seek in. */
    auto result = rig.submitAndTick ("run.seek", { osc::Value::string (memoRun),
                                                   osc::Value::float64 (1.0) });
    CHECK (result.rejected == 1);

    result = rig.submitAndTick ("run.seek", { osc::Value::string ("NOTARUN1"),
                                              osc::Value::float64 (1.0) });
    CHECK (result.rejected == 1);

    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");
    const auto mediaRun = rig.runOf (rig.mediaId);

    result = rig.submitAndTick ("run.seek", { osc::Value::string (mediaRun),
                                              osc::Value::float64 (-1.0) });
    CHECK (result.rejected == 1);

    /*  Over: applied and nothing, since a hand still dragging when the sound
        ends is not a mistake. */
    rig.submitAndTick ("run.kill", { osc::Value::string (mediaRun) });
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.runs.find (mediaRun)->isFinished());

    const auto armsBefore = rig.audio.arms.size();
    result = rig.submitAndTick ("run.seek", { osc::Value::string (mediaRun),
                                              osc::Value::float64 (2.0) });
    CHECK (result.applied == 1);
    CHECK (rig.audio.arms.size() == armsBefore);
}

//==============================================================================
/*  THE HISTORY READING (2026-09-19): a load to time as a position in what the
    list actually did. The author's reframing - "scrubbing through the load to
    time history" - is what makes the solver read the steps: a cue fired three
    GOs ago is three GOs of real time in, not "over" because it comes earlier in
    the list. */
namespace
{
    struct ClockRig : Rig
    {
        ClockRig()
        {
            //  Thunder is the base rig's first cue; a second sound after it, a
            //  stop aimed at the first, and a memo to land on.
            rain = document.createCue (listId, 1, "media", "Rain").id;
            document.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav");
            stopThunder = document.createCue (listId, 2, "transport", "Cut the thunder").id;
            document.setAttribute ("/godot/cue/" + stopThunder + "/target", mediaId);
            after = document.createCue (listId, 3, "memo", "After").id;

            durations["thunder.wav"] = 10.0;
            durations["rain.wav"] = 10.0;
            runner.setMediaDurations (&durations);
        }

        /** GO on the pointer, then let the arm settle a tick. */
        std::int64_t goNow()
        {
            const auto at = tick;
            submitAndTick ("go");
            audio.completeArms (engine);
            tickOnce();
            return at;
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        cue::Plan planFor (const std::string& cueId, double offset) const
        {
            return cue::solveAim (document, &durations, nullptr, { listId, cueId, offset },
                                  &runner.listState().historyOf (listId));
        }

        const cue::PlannedRun* planned (const cue::Plan& plan, const std::string& cueId) const
        {
            for (const auto& run : plan.runs)
                if (run.cue == cueId)
                    return &run;

            return nullptr;
        }

        std::map<std::string, double> durations;
        std::string rain, stopThunder, after;
    };
}

TEST_CASE ("history: a cue fired earlier is placed by the clock, not read as over")
{
    ClockRig rig;
    rig.setStandby (rig.mediaId);

    const auto thunderAt = rig.goNow();          // thunder, standby moves to rain
    rig.ticks (100 - static_cast<int> (rig.tick - thunderAt));
    const auto rainAt = rig.goNow();             // two seconds later
    CHECK (rainAt - thunderAt == 100);

    //  One second into the rain: the thunder is three seconds in and sounding.
    const auto plan = rig.planFor (rig.rain, 1.0);
    REQUIRE (plan.ok);
    CHECK (plan.how == "history");
    CHECK (plan.instant == rainAt + 50);

    const auto* thunder = rig.planned (plan, rig.mediaId);
    REQUIRE (thunder != nullptr);
    CHECK (thunder->when == cue::planned::sounding);
    CHECK (thunder->offset == doctest::Approx (3.0));

    const auto* rainRun = rig.planned (plan, rig.rain);
    REQUIRE (rainRun != nullptr);
    CHECK (rainRun->offset == doctest::Approx (1.0));

    //  The pointer lands after the last GO, which was the rain.
    CHECK (plan.standby == rig.stopThunder);

    //  Nine seconds into the rain the thunder has run out.
    const auto later = rig.planFor (rig.rain, 9.0);
    CHECK (rig.planned (later, rig.mediaId) == nullptr);
    REQUIRE (rig.planned (later, rig.rain) != nullptr);

    //  Before the rain fired: only the thunder, two seconds in.
    const auto before = rig.planFor (rig.rain, -1.0);
    CHECK (before.instant == rainAt - 1);
    REQUIRE (rig.planned (before, rig.mediaId) != nullptr);
    CHECK (rig.planned (before, rig.mediaId)->offset == doctest::Approx ((rainAt - 1 - thunderAt) / 50.0));
    CHECK (rig.planned (before, rig.rain) == nullptr);

    //  The order reading is what a cue with no step gets, and says so.
    const auto never = rig.planFor (rig.after, -1.0);
    CHECK (never.how == "order");
    CHECK (never.instant == -1);
}

TEST_CASE ("history: a stop step ends its target from then on, and a fire after it is a new one")
{
    ClockRig rig;
    rig.setStandby (rig.mediaId);
    rig.goNow();                                 // thunder
    rig.ticks (40);
    rig.setStandby (rig.stopThunder);
    rig.goNow();                                 // the stop, one second in
    rig.ticks (40);
    rig.setStandby (rig.rain);
    const auto rainAt = rig.goNow();

    const auto plan = rig.planFor (rig.rain, 1.0);
    CHECK (plan.how == "history");
    CHECK (plan.instant == rainAt + 50);
    CHECK (rig.planned (plan, rig.mediaId) == nullptr);
    REQUIRE (rig.planned (plan, rig.rain) != nullptr);

    //  Fired again after the stop, the thunder is back.
    rig.ticks (40);
    rig.setStandby (rig.mediaId);
    const auto again = rig.goNow();
    CHECK (again > rainAt);

    const auto replan = rig.planFor (rig.mediaId, 0.5);
    REQUIRE (rig.planned (replan, rig.mediaId) != nullptr);
    CHECK (rig.planned (replan, rig.mediaId)->offset == doctest::Approx (0.5));
}

TEST_CASE ("history: the jump retimes the steps, and drops the ones it undid")
{
    ClockRig rig;
    rig.setStandby (rig.mediaId);
    const auto thunderAt = rig.goNow();
    rig.ticks (100 - static_cast<int> (rig.tick - thunderAt));
    rig.goNow();                                 // the rain, two seconds later
    rig.ticks (100);
    rig.setStandby (rig.after);
    const auto afterAt = rig.goNow();            // a step the jump will undo

    REQUIRE (rig.runner.listState().historyOf (rig.listId).size() == 3u);

    //  Aim one second into the rain and make it true.
    REQUIRE (rig.engine.submit ("cli", "list.aim",
                                { osc::Value::string (rig.listId), osc::Value::string (rig.rain),
                                  osc::Value::float64 (1.0) }));
    rig.tickOnce();
    const auto jumpAt = rig.tick;
    CHECK (rig.submitAndTick ("list.loadToTime", { osc::Value::string (rig.listId) }).applied == 1);

    //  Both sounds are rebuilt where the clock put them.
    const auto* thunder = rig.runs.liveRunOf (rig.mediaId);
    REQUIRE (thunder != nullptr);
    CHECK (thunder->startOffset == doctest::Approx (3.0));
    const auto* rainRun = rig.runs.liveRunOf (rig.rain);
    REQUIRE (rainRun != nullptr);
    CHECK (rainRun->startOffset == doctest::Approx (1.0));

    /*  The history is on the new clock: the thunder reads as fired 150 ticks
        before the jump, the rain 50 - and the memo, fired after the instant,
        is gone. */
    const auto& steps = rig.runner.listState().historyOf (rig.listId);
    REQUIRE (steps.size() == 2u);
    CHECK (steps[0].cue == rig.mediaId);
    CHECK (steps[0].tick == jumpAt - 150);
    CHECK (steps[1].cue == rig.rain);
    CHECK (steps[1].tick == jumpAt - 50);
    juce::ignoreUnused (afterAt);

    //  So a later aim reads right: three seconds into the rain, thunder at five.
    const auto replan = rig.planFor (rig.rain, 3.0);
    REQUIRE (rig.planned (replan, rig.mediaId) != nullptr);
    CHECK (rig.planned (replan, rig.mediaId)->offset == doctest::Approx (5.0));
    CHECK (rig.planned (replan, rig.after) == nullptr);

    //  And the published solve says which reading it is.
    CHECK (replan.toJson().find ("\"how\": \"history\"") != std::string::npos);
}

//==============================================================================
/*  THE START CUE AND THE LIVE RECORDER (author, 2026-09-18; built 2026-09-19).

    A start cue presses a button: it fires another cue by name and is done.
    The live recorder is the history kept from a tick and written, on stop,
    into a take - a timeline group of start cues at the seconds they were
    pressed - which is the author's own reframing: "4 is like dumping the
    load to time history to a group for replay."
*/
TEST_CASE ("start: a start cue fires its target by name on the next tick, and is done")
{
    Rig rig;

    const auto starter = rig.document.createCue (rig.listId, 2, "start", "Press thunder").id;
    REQUIRE (! starter.empty());
    rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.mediaId);

    rig.setStandby (starter);
    const auto at = rig.tick;
    CHECK (rig.submitAndTick ("go").applied == 1);

    //  Its own run is playing, then done the next tick, as a memo's is.
    const auto own = rig.runOf (starter);
    REQUIRE (! own.empty());

    //  The hook fires the target by name on the next tick: a record of its own.
    rig.tickOnce();
    rig.tickOnce();

    auto fired = false;

    for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
        if (record.command == "cue.fire" && ! record.args.empty()
              && record.args.front().getString() == rig.mediaId)
            fired = true;

    CHECK (fired);
    CHECK (! rig.runOf (rig.mediaId).empty());
    CHECK (rig.runs.find (own)->isFinished());

    //  Standby was not moved by the fire: GO moved it past the start cue.
    CHECK (rig.standby() != starter);
    juce::ignoreUnused (at);
}

TEST_CASE ("record: what was pressed between start and stop becomes a take of start cues at their seconds")
{
    Rig rig;

    //  Nothing kept: refused.
    CHECK (rig.submitAndTick ("record.stop").rejected == 1);

    CHECK (rig.submitAndTick ("record.start").applied == 1);
    const auto since = rig.tick - 1;

    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");                            // thunder, at since + 1
    const auto thunderAt = rig.tick - 1;

    for (int n = 0; n < 99; ++n)
        rig.tickOnce();

    rig.submitAndTick ("cue.fire", { osc::Value::string (rig.memoId) });   // two seconds later
    const auto memoAt = rig.tick - 1;

    //  The stop, and whatever else that tick applied - the memo's own ending.
    const auto stopped = rig.submitAndTick ("record.stop");
    CHECK (stopped.applied >= 1);
    CHECK (stopped.rejected == 0);

    //  A list named Live recorder, with one take: a timeline group.
    std::string recorder;

    for (const auto& child : rig.document.root().getChildWithName (juce::Identifier ("Lists")))
        if (child.getType().toString() == "List"
             && rig.document.getAttribute ("/godot/list/" + child[juce::Identifier ("id")].toString().toStdString()
                                           + "/name").value_or ("") == "Live recorder")
            recorder = child[juce::Identifier ("id")].toString().toStdString();

    REQUIRE (! recorder.empty());

    const auto list = rig.document.findById (recorder);
    REQUIRE (list.getNumChildren() == 1);

    const auto take = list.getChild (0);
    CHECK (take.getType().toString() == "Group");
    CHECK (rig.document.getAttribute ("/godot/cue/" + take[juce::Identifier ("id")].toString().toStdString() + "/name").value_or ("") == "Take 1");
    CHECK (rig.document.getAttribute ("/godot/cue/" + take[juce::Identifier ("id")].toString().toStdString() + "/mode").value_or ("") == "timeline");
    REQUIRE (take.getNumChildren() == 2);

    const auto first = take.getChild (0);
    const auto second = take.getChild (1);
    CHECK (first.getType().toString() == "Start");
    CHECK (second.getType().toString() == "Start");

    const auto attribute = [&rig] (const juce::ValueTree& node, const char* name)
    {
        return rig.document.getAttribute ("/godot/cue/" + node[juce::Identifier ("id")].toString().toStdString()
                                          + "/" + name).value_or ("");
    };

    CHECK (attribute (first, "target") == rig.mediaId);
    CHECK (attribute (first, "name") == "Start Thunder");
    CHECK (osc::parseDouble (attribute (first, "preWait")).value_or (-1.0)
             == doctest::Approx ((thunderAt - since) / 50.0));
    CHECK (attribute (second, "target") == rig.memoId);
    CHECK (osc::parseDouble (attribute (second, "preWait")).value_or (-1.0)
             == doctest::Approx ((memoAt - since) / 50.0));

    //  The record carries every identifier the take drew, list first.
    const auto* command = rig.engine.commands().find ("record.stop");
    REQUIRE (command != nullptr);

    for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
        if (record.command == "record.stop" && record.kind == LogRecord::Kind::applied)
            CHECK (record.args.size() == 4u);       // the list, the take, two start cues

    //  A second take lands beside the first, and the list is reused.
    rig.submitAndTick ("record.start");
    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");
    CHECK (rig.submitAndTick ("record.stop").applied == 1);
    CHECK (rig.document.findById (recorder).getNumChildren() == 2);
    CHECK (rig.document.getAttribute ("/godot/cue/"
                                      + rig.document.findById (recorder).getChild (1)[juce::Identifier ("id")]
                                            .toString().toStdString() + "/name").value_or ("") == "Take 2");

    //  And the recorder reads as off.
    CHECK_FALSE (rig.runner.listState().isRecording());
}

TEST_CASE ("record: a locked show keeps recording but refuses to write the take")
{
    Rig rig;
    rig.submitAndTick ("record.start");
    rig.setStandby (rig.mediaId);
    rig.submitAndTick ("go");

    rig.document.setAttribute ("/godot/document/locked", "true");
    CHECK (rig.submitAndTick ("record.stop").rejected == 1);
    CHECK (rig.runner.listState().isRecording());

    rig.document.setAttribute ("/godot/document/locked", "false");
    CHECK (rig.submitAndTick ("record.stop").applied == 1);
}

TEST_CASE ("M19: what the horizon costs, in ticks from the pointer landing")
{
    /*  MEASUREMENT M19 (§13.14), the half of it this PR can take.

        §13.14 asks for a prepared header of twenty ANTICIPATABLE OSC cues
        against the mock target, counted from the pointer landing to `verified`.
        A network cue is not pre-sent yet - §13.1 forbids sending a value to a
        target nobody can ask what it held, and the read-before-write that fixes
        that arrives with it - so what is measured here is the arm half: twenty
        media cues in one scene, from the pointer landing to the block being
        ready.

        REPORTED AND NOT GATED, like every measurement in this suite. What it is
        for is that a designer building a scene on the horizon knows what it
        costs before they rely on it. */
    PrepareRig rig;

    constexpr int members = 20;

    for (int n = 0; n < members; ++n)
    {
        const auto id = rig.document.createCue (rig.groupId, n + 1, "media",
                                                "Voice " + std::to_string (n)).id;
        rig.document.setAttribute ("/godot/cue/" + id + "/file", "thunder.wav");
    }

    /*  THE POINTER IS WRITTEN AND NOT TICKED, because the number wanted is
        ticks from the pointer LANDING - and `setStandby` ticks once itself, to
        let the arm settle the way a show does. Counting from after that tick
        would report one fewer than the truth. */
    rig.document.setAttribute (cue::standbyAddressOf (rig.listId), rig.sound);

    auto ticks = 0;
    auto ready = false;

    for (int n = 0; n < 400 && ! ready; ++n)
    {
        ready = rig.prepared (rig.groupId) != nullptr && ! rig.runOf (rig.sound).empty();

        if (! ready)
        {
            rig.tickOnce();
            ++ticks;
        }
    }

    CHECK (ready);

    const auto* prepared = rig.prepared (rig.groupId);
    REQUIRE (prepared != nullptr);

    MESSAGE ("M19  a scene of " << members << " media members: the block was prepared "
             << ticks << " tick(s) after the pointer landed, ending on \""
             << prepared->prepare << "\" (its header is a memo, which nothing can"
                " anticipate); a voice was reserved for "
             << rig.runs.childrenOf (prepared->id).size()
             << " of them - the ones the scene would launch first - and the rest are"
                " armed by the scheduler as the scene runs, which is what their"
                " positions are for");
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


//==============================================================================
TEST_CASE ("parallel lists: GO acts on the focused list, and leaves the other alone")
{
    /*  §3.5 has several lists live at once, each with a standby of its own, so
        that a chain started by timecode or by a trigger can never move the
        operator's position. What decides whose standby GO means is the focus,
        and it is one value rather than a flag per list - so nothing has to be
        un-focused and nothing can end up with two.

        The half worth asserting is the OTHER list: that it sits there with its
        own pointer, untouched, while the focused one is being played. */
    Rig rig;

    const auto second = rig.document.createList ("Background").id;
    const auto foyer = rig.document.createCue (second, 0, "memo", "Foyer loop").id;
    const auto later = rig.document.createCue (second, 1, "memo", "Foyer out").id;

    rig.setStandby (rig.memoId);
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (second), foyer).ok);

    //  Focused on the first list, which is where a show opens.
    CHECK (rig.submitAndTick ("go").applied >= 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.memoId).empty(); }));

    CHECK (rig.runOf (foyer) == "");
    CHECK (rig.document.getAttribute ("/godot/list/" + second + "/standby") == foyer);

    const auto whereTheFirstListIs = rig.standby();

    //  And now the other one.
    CHECK (rig.submitAndTick ("list.focus", { osc::Value::string (second) }).applied >= 1);
    CHECK (rig.submitAndTick ("go").applied >= 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (foyer).empty(); }));

    CHECK (rig.document.getAttribute ("/godot/list/" + second + "/standby") == later);

    /*  And the first list is exactly where its own GO left it. A press meant
        for one list that moved another's pointer would be the failure §3.5 is
        written to prevent, arriving from the inside. */
    CHECK (rig.standby() == whereTheFirstListIs);
}

TEST_CASE ("parallel lists: firing a cue by name moves neither focus nor any standby")
{
    /*  `cue.fire` is what a button on a surface does, and from PR 3.7 it is
        what a trigger does. §3.5 and §3.7 are both explicit: only GO moves the
        standby. A cue fired from outside plays and changes nothing about where
        the operator is - which is the whole reason a background list can be
        driven by something other than a person without the person losing their
        place. */
    Rig rig;

    const auto second = rig.document.createList ("Background").id;
    const auto foyer = rig.document.createCue (second, 0, "memo", "Foyer loop").id;
    rig.document.createCue (second, 1, "memo", "Foyer out");

    rig.setStandby (rig.memoId);
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (second), foyer).ok);

    const auto focusBefore = rig.document.getAttribute ("/godot/list/focus");

    CHECK (rig.submitAndTick ("cue.fire", { osc::Value::string (foyer) }).applied >= 1);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (foyer).empty(); }));

    CHECK (rig.document.getAttribute ("/godot/list/focus") == focusBefore);
    CHECK (rig.document.getAttribute ("/godot/list/" + second + "/standby") == foyer);
    CHECK (rig.standby() == rig.memoId);
}

//==============================================================================
/*  GROUP FADES AS TRIMS — the author's decision 4, 2026-09-06.

    §3.6: a fade aimed at a group is a TRIM over its members, not a write, and
    nested trims compose. Which makes a run's level a sum rather than a value:

        level = ownLevel + every ancestor run's ownLevel

    That is a small change to say and a structural one to have. It is what
    relative fade cues will be built on, and it is the shape Phase 6's DCAs
    need: a fader that trims a group of cues is the same arithmetic reached from
    a different direction. Built here, before either, so that neither has to
    change it.

    THE THING THAT MAKES IT COMPOSE RATHER THAN ACCUMULATE is which number a
    fade takes over from. A fade on a member starts from the member's OWN level
    and not from what it is heard at, so a trim in force while the fade runs is
    not folded into the base and left behind when the group releases it.
*/
namespace
{
    /*  A group of media cues, so that a trim has something with a voice to
        reach. The memo members of GroupRig have no track and no level. */
    struct TrimRig : Rig
    {
        TrimRig()
        {
            groupId = document.createCue (listId, 2, "group", "Scene").id;
            document.setAttribute ("/godot/cue/" + groupId + "/advance", "auto");
            document.setAttribute ("/godot/cue/" + groupId + "/mode", "timeline");

            member = document.createCue (groupId, 0, "media", "Rain").id;
            document.setAttribute ("/godot/cue/" + member + "/file", "thunder.wav");
            document.setAttribute ("/godot/cue/" + member + "/level", "-3");
        }

        /** A fade cue aimed at something, with a duration in seconds. */
        std::string fadeAt (const std::string& target, const char* toDb, const char* seconds)
        {
            const auto id = document.createCue (listId, 3, "fade", "Under").id;

            document.setAttribute ("/godot/cue/" + id + "/target", target);
            document.setAttribute ("/godot/cue/" + id + "/level", toDb);
            document.setAttribute ("/godot/cue/" + id + "/duration", seconds);
            return id;
        }

        /** The run of a cue, or empty. */
        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
        }

        double levelOf (const std::string& cueId) const
        {
            const auto id = runOf (cueId);
            const auto* run = id.empty() ? nullptr : runs.find (id);
            return run != nullptr ? run->level : 0.0;
        }

        double ownLevelOf (const std::string& cueId) const
        {
            const auto id = runOf (cueId);
            const auto* run = id.empty() ? nullptr : runs.find (id);
            return run != nullptr ? run->ownLevel : 0.0;
        }

        /** Ticks until a fade has arrived, or the bound runs out. */
        void settle (int howMany = 60)
        {
            for (int n = 0; n < howMany; ++n)
                tickOnce();
        }

        std::string groupId, member;
    };
}

TEST_CASE ("trim: a fade aimed at a group moves its members and touches none of their levels")
{
    /*  The whole of decision 4 in one case. The member is authored at -3 and is
        heard at -3; the group is trimmed to -6 and the member is heard at -9;
        and the member's OWN level is still -3 throughout, because nothing wrote
        to it. */
    TrimRig rig;
    rig.setStandby (rig.groupId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.settle (10);

    REQUIRE_FALSE (rig.runOf (rig.member).empty());

    CHECK (rig.ownLevelOf (rig.member) == doctest::Approx (-3.0));
    CHECK (rig.levelOf (rig.member) == doctest::Approx (-3.0));

    /*  A GROUP RUN HAS NO VOICE, so its trim starts at nothing. */
    CHECK (rig.ownLevelOf (rig.groupId) == doctest::Approx (0.0));

    const auto trim = rig.fadeAt (rig.groupId, "-6", "0.2");
    rig.submitAndTick ("cue.fire", { osc::Value::string (trim) });
    rig.settle (20);

    /*  THE GROUP CARRIES THE TRIM. */
    CHECK (rig.ownLevelOf (rig.groupId) == doctest::Approx (-6.0));
    CHECK (rig.levelOf (rig.groupId) == doctest::Approx (-6.0));

    /*  THE MEMBER IS HEARD SIX DECIBELS DOWN, and its own level never moved. */
    CHECK (rig.levelOf (rig.member) == doctest::Approx (-9.0));
    CHECK (rig.ownLevelOf (rig.member) == doctest::Approx (-3.0));

    /*  AND THE VOICE WAS TOLD. What reaches the audio side is the effective
        level, which is the number the trim exists to produce. */
    REQUIRE_FALSE (rig.audio.levels.empty());
    CHECK (rig.audio.levels.back().second == doctest::Approx (-9.0));
}

TEST_CASE ("trim: a member's own fade and its group's trim compose in dB, and each returns alone")
{
    /*  §3.6's "nested trims compose", seen from the member's side: two fades
        running over one cue, one aimed at it and one at the group above it, and
        the answer is their sum. Then each is released and the other is still
        exactly where it was - which is the property a trim has and a write does
        not. */
    TrimRig rig;
    rig.setStandby (rig.groupId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.settle (10);

    const auto trim = rig.fadeAt (rig.groupId, "-6", "0.2");
    const auto own = rig.fadeAt (rig.member, "-10", "0.2");

    rig.submitAndTick ("cue.fire", { osc::Value::string (trim) });
    rig.submitAndTick ("cue.fire", { osc::Value::string (own) });
    rig.settle (25);

    CHECK (rig.ownLevelOf (rig.member) == doctest::Approx (-10.0));
    CHECK (rig.levelOf (rig.member) == doctest::Approx (-16.0));

    /*  THE GROUP LETS GO, and the member is at what its own fade left it at -
        not at -16, and not back at -3. */
    const auto release = rig.fadeAt (rig.groupId, "0", "0.2");
    rig.submitAndTick ("cue.fire", { osc::Value::string (release) });
    rig.settle (25);

    CHECK (rig.levelOf (rig.member) == doctest::Approx (-10.0));
    CHECK (rig.ownLevelOf (rig.member) == doctest::Approx (-10.0));
}

TEST_CASE ("trim: a nested group's trim adds to its parent's")
{
    /*  Trims compose downwards through as many levels as the show has, which is
        what makes this the structure a DCA can be built on: a fader trimming a
        group that is itself inside a trimmed group is two numbers added, not a
        special case. */
    TrimRig rig;

    const auto inner = rig.document.createCue (rig.groupId, 1, "group", "Inside").id;
    rig.document.setAttribute ("/godot/cue/" + inner + "/advance", "auto");
    rig.document.setAttribute ("/godot/cue/" + inner + "/mode", "timeline");

    const auto deep = rig.document.createCue (inner, 0, "media", "Deep").id;
    rig.document.setAttribute ("/godot/cue/" + deep + "/file", "thunder.wav");
    rig.document.setAttribute ("/godot/cue/" + deep + "/level", "-2");

    rig.setStandby (rig.groupId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.settle (15);
    rig.audio.completeArms (rig.engine);
    rig.settle (15);

    REQUIRE_FALSE (rig.runOf (deep).empty());
    CHECK (rig.levelOf (deep) == doctest::Approx (-2.0));

    const auto outerTrim = rig.fadeAt (rig.groupId, "-6", "0.2");
    const auto innerTrim = rig.fadeAt (inner, "-4", "0.2");

    rig.submitAndTick ("cue.fire", { osc::Value::string (outerTrim) });
    rig.submitAndTick ("cue.fire", { osc::Value::string (innerTrim) });
    rig.settle (25);

    /*  -2 authored, -4 from the group it is in, -6 from the group that is in. */
    CHECK (rig.levelOf (deep) == doctest::Approx (-12.0));

    /*  AND THE INNER GROUP'S OWN LEVEL IS THE TRIM IN FORCE ON ITS MEMBERS,
        which is its own plus its parent's - one rule for both kinds of run, so
        a client never has to ask which it is holding. */
    CHECK (rig.ownLevelOf (inner) == doctest::Approx (-4.0));
    CHECK (rig.levelOf (inner) == doctest::Approx (-10.0));
}

TEST_CASE ("trim: a fade on a trimmed member starts from where the member is, not from what it is heard at")
{
    /*  THE LINE THAT MAKES TRIMS COMPOSE. A fade takes over from the run's OWN
        level. Taking over from the effective one would fold the trim into the
        base: the member would be heard six decibels lower than it should while
        the fade ran, and would keep the six when the group let go.

        Measured as the first level the fade writes, which is where it started
        from rather than where it is going. */
    TrimRig rig;
    rig.setStandby (rig.groupId);

    rig.submitAndTick ("go");
    rig.audio.completeArms (rig.engine);
    rig.settle (10);

    const auto trim = rig.fadeAt (rig.groupId, "-6", "0.1");
    rig.submitAndTick ("cue.fire", { osc::Value::string (trim) });
    rig.settle (15);

    REQUIRE (rig.levelOf (rig.member) == doctest::Approx (-9.0));

    /*  A long fade, so the first tick of it is far from its destination and the
        starting point is unambiguous. */
    const auto own = rig.fadeAt (rig.member, "-40", "2");
    rig.submitAndTick ("cue.fire", { osc::Value::string (own) });
    rig.tickOnce();

    /*  ONE TICK IN, its own level has moved a fiftieth of the way from -3 to
        -40, which is about -3.7. Had it taken over from -9 it would be near
        -9.6, and would be heard at -15.6 rather than at -12.7. */
    const auto ownLevel = rig.ownLevelOf (rig.member);

    INFO ("one tick in, its own level is " << ownLevel);
    CHECK (ownLevel < -3.0);
    CHECK (ownLevel > -5.0);

    /*  And what it is HEARD at is that plus the trim, still. */
    CHECK (rig.levelOf (rig.member) == doctest::Approx (ownLevel - 6.0));
}

TEST_CASE ("fade: a target this show does not contain is a bad-target, not a no-op")
{
    /*  THE DIFFERENCE THE `refers` COLUMN MADE CHECKABLE. §3.8 makes a fade
        aimed at a cue that has FINISHED a silent no-op - the cue was real and
        it ended, which happens during tech and is not a mistake. A fade aimed
        at an identifier this show does not contain is a different thing: it
        names nothing, and it will name nothing on every GO for the rest of the
        run.

        `wfg validate` has already said so on a laptop with nothing plugged in.
        This is what happens if nobody read it. */
    Rig rig;

    const auto fade = rig.document.createCue (rig.listId, 2, "fade", "Under").id;

    /*  Written past setAttribute, which refuses an identifier that names
        nothing: the point is a show somebody edited by hand, or one whose
        target was deleted after it was written. */
    auto cue = rig.document.findById (fade);
    cue.setProperty (juce::Identifier ("target"), "ZZZZZZZZ", nullptr);

    rig.submitAndTick ("cue.fire", { osc::Value::string (fade) });
    rig.tickOnce();

    const auto runId = rig.runOf (fade);
    REQUIRE_FALSE (runId.empty());

    const auto* run = rig.runs.find (runId);
    REQUIRE (run != nullptr);

    CHECK (run->error == cue::runError::badTarget);
    CHECK (run->state == cue::runState::failed);
}

TEST_CASE ("fade: a target that is real and not running is still a silent no-op")
{
    /*  The other side of the same line, and the reason it is a line: §3.8 says
        so, and an operator whose cue ended early has not made a mistake. */
    Rig rig;

    const auto fade = rig.document.createCue (rig.listId, 2, "fade", "Under").id;
    rig.document.setAttribute ("/godot/cue/" + fade + "/target", rig.mediaId);

    rig.submitAndTick ("cue.fire", { osc::Value::string (fade) });
    rig.tickOnce();

    const auto runId = rig.runOf (fade);
    REQUIRE_FALSE (runId.empty());

    const auto* run = rig.runs.find (runId);
    REQUIRE (run != nullptr);

    CHECK (run->error.empty());
    CHECK (run->state == cue::runState::done);
}

//==============================================================================
TEST_CASE ("kill: a cue armed and never launched gives its voice back")
{
    /*  THE LEAK PR 4.1 CLOSES, and it is the second of two.

        `observeEdges` ends a run on a playing-to-stopped edge, which is the
        right question for every run that played. A run ARMED at standby and
        killed before it ever sounded gives no such edge, ever: it stayed
        `stopping`, and `holdsTrack()` is `track >= 0 && ! isFinished()`, so it
        held its voice for the rest of the session. The sweep in `advanceWaits`
        did not reach it either - that one skips anything holding a track,
        because it was written for fades and groups, which hold none.

        The symptom arrives somewhere else entirely: the next cue the pointer
        reaches fails with `no-track`, pointing at a rig that is fine. */
    Rig rig;
    rig.setStandby (rig.mediaId);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.runs.all().size() == 1u);
    const auto id = rig.runs.all().front().id;

    REQUIRE (rig.runs.find (id)->state == cue::runState::armed);
    REQUIRE (rig.runs.find (id)->track == 0);
    REQUIRE (rig.runs.lowestFreeTrack (4) == 1);     // held, and rightly so

    rig.submitAndTick ("run.kill", { osc::Value::string (id) });

    /*  One tick to tell the audio side and see that nothing is sounding. */
    rig.tickOnce();

    CHECK (rig.runs.find (id)->isFinished());
    CHECK (rig.runs.lowestFreeTrack (4) == 0);
}

TEST_CASE ("kill: every voice an operator armed and abandoned comes back")
{
    /*  The same thing at the scale it is felt at, and the answer changed under
        it in PR 4.11 - for the better, and this case is where that is said.

        A pointer walked down a list of media cues used to arm each one and hold
        every voice it touched until something killed it: eight cues, eight
        voices, on a rig with four. Moving the pointer on now gives the last
        one's voice back, because a cue nobody fired holding a voice for the
        rest of the show is a leak rather than a preparation. So the walk leaves
        exactly ONE armed cue - the one the pointer is on - and the kill this
        case is named for still has to free that one.

        What was really being tested is unchanged: an armed, never-launched run
        can be ended, and its voice comes back. There is simply less of it to
        do, which is the point. */
    Rig rig;

    std::vector<std::string> cues;

    for (int n = 0; n < 4; ++n)
    {
        const auto id = rig.document.createCue (rig.listId, 2 + n, "media",
                                                "Sound " + std::to_string (n)).id;
        rig.document.setAttribute ("/godot/cue/" + id + "/file", "thunder.wav");
        cues.push_back (id);
    }

    for (const auto& cueId : cues)
    {
        rig.setStandby (cueId);
        rig.audio.completeArms (rig.engine);
        rig.tickOnce();
    }

    REQUIRE (rig.runs.all().size() == cues.size());

    /*  THREE OF THE FOUR ARE ALREADY OVER, given back as the pointer left
        them, and each says why. */
    auto abandoned = 0;

    for (const auto& run : rig.runs.all())
        if (run.isFinished() && run.warning == cue::runWarning::revoked)
            ++abandoned;

    CHECK (abandoned == 3);

    //  And one voice is held: the cue the pointer is standing on.
    CHECK (rig.runs.lowestFreeTrack (4) == 1);

    for (const auto& run : rig.runs.all())
        rig.submitAndTick ("run.kill", { osc::Value::string (run.id) });

    rig.tickOnce();

    CHECK (rig.runs.lowestFreeTrack (4) == 0);

    for (const auto& run : rig.runs.all())
        CHECK (run.isFinished());
}

//==============================================================================
TEST_CASE ("group: a run says which part of itself it is in")
{
    /*  `/godot/run/<id>/phase` was drawn in §12.2 and published by nothing, so
        the console has been rendering an empty string in its place since the
        group scheduler landed.

        A READOUT, mirrored from the job that holds it: the scheduler decides
        the phase and the run carries a copy for a client to watch. So it is
        never logged, and a replay - which runs no scheduler - leaves it empty,
        exactly as it leaves `position` and `rangeIteration`. */
    GroupRig rig;
    rig.setStandby (rig.groupId);

    CHECK (rig.submitAndTick ("go").applied == 1);

    REQUIRE (rig.runs.all().size() == 1u);
    const auto groupRun = rig.runs.all().front().id;

    /*  IT STARTS IN `entering`, which is its own pre-wait, and reaches its
        members on the tick after - because the mirror runs at the top of the
        job loop, before the phase it is about to move to has been chosen. A
        readout is a tick behind the decision by construction, which is what a
        readout is. */
    rig.tickOnce();
    CHECK (rig.runs.find (groupRun)->phase == "entering");

    /*  And then its members: this group has no header, and an absent phase is
        skipped rather than entered. */
    rig.tickOnce();
    CHECK (rig.runs.find (groupRun)->phase == "members");

    /*  And a member's own run has none. A phase is a thing a group has. */
    const auto member = rig.runOf (rig.first);
    REQUIRE (! member.empty());
    CHECK (rig.runs.find (member)->phase.empty());

    CHECK (rig.runToCompletion (groupRun) < 400);
}


//==============================================================================
/*  THE STEP HISTORY: the waypoints nobody had to keep.

    PRD §3.13 wants manual waypoints, and the author's own shape for them
    (decision R) is that the operator should not have to make any: every applied
    trigger is already a place the show was, so the engine keeps the last
    sixty-four of them and offers them back when somebody needs to go there.

    A STEP IS A LOAD-TO-TIME TARGET. `<cue> -1` is "standby on it, nothing of it
    done", which is exactly where the show was the instant before that press -
    so going back a step is the jump of PR 4.8 with a target that was written
    down rather than dragged.

    WRITTEN BY THE HANDLER, WHICH IS WHY A REPLAY HAS IT. The alternative was a
    hook noticing runs appear, and it would have been wrong twice over: a replay
    runs no hooks, so a replayed session would have had no history; and a hook
    cannot tell a GO from a trigger, which is the one thing the origin letter is
    for. `persist=none`, so the show on disk never carries it: it is what the
    machine happened to be doing (§4.10), and tomorrow's rehearsal starts blank.
*/
namespace
{
    struct HistoryRig : Rig
    {
        HistoryRig()
        {
            second = document.createCue (listId, 2, "memo", "Two").id;
            third = document.createCue (listId, 3, "memo", "Three").id;
        }

        /** Parks the pointer through the COMMAND, so a replay is given it too. */
        void park (const std::string& cueId)
        {
            REQUIRE (submitAndTick ("standby.set",
                                    { osc::Value::string (cueId) }).applied == 1);
        }

        /*  The steps of a list, oldest first, spelled as the node spells them
            - one string, which is also what a failure prints. */
        std::string spelled (const std::string& list = {}) const
        {
            std::string out;

            for (const auto& step : runner.listState()
                                      .historyOf (list.empty() ? listId : list))
            {
                if (! out.empty())
                    out += ' ';

                out += cue::spellStep (step);
            }

            return out;
        }

        /** Just the cues, oldest first. */
        std::vector<std::string> cuesStepped (const std::string& list = {}) const
        {
            std::vector<std::string> out;

            for (const auto& step : runner.listState()
                                      .historyOf (list.empty() ? listId : list))
                out.push_back (step.cue);

            return out;
        }

        std::string second, third;
    };
}

TEST_CASE ("history: three GOs are three steps, in the order they were pressed")
{
    HistoryRig rig;
    rig.park (rig.memoId);

    /*  Not `applied == 1`: a tick that fires a cue is also the tick a
        previous one's end is written down on, and how many records that is is
        the scheduler's business rather than this case's. */
    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.submitAndTick ("go").applied >= 1);
    CHECK (rig.submitAndTick ("go").applied >= 1);

    /*  THE CUE THAT FIRED, NOT THE CUE THE POINTER LANDED ON. A step is a place
        the show WAS, so it names what the press did; the pointer had already
        moved on by the time anybody could read it. */
    CHECK (rig.cuesStepped()
             == std::vector<std::string> { rig.memoId, rig.second, rig.third });

    const auto& steps = rig.runner.listState().historyOf (rig.listId);
    REQUIRE (steps.size() == 3u);

    /*  Each on its own tick, and each says which. A history without ticks could
        not tell a double press from an act. */
    CHECK (steps[0].tick < steps[1].tick);
    CHECK (steps[1].tick < steps[2].tick);

    for (const auto& step : steps)
        CHECK (step.origin == 'g');

    /*  And the total is across every list, which is what lets a hook notice a
        step without comparing each list against what it had. */
    CHECK (rig.runner.listState().stepsTaken() == 3u);
}

TEST_CASE ("history: a cue fired by name is a step, and says it was not a press")
{
    /*  ALL THREE APPLIED TRIGGERS ARE STEPS, because all three are places the
        show was - and the letter is the difference between them. A history that
        could not say which steps nobody pressed could not explain a scene that
        started on its own. */
    HistoryRig rig;

    CHECK (rig.submitAndTick ("cue.fire",
                              { osc::Value::string (rig.third) }).applied == 1);

    const auto& steps = rig.runner.listState().historyOf (rig.listId);
    REQUIRE (steps.size() == 1u);
    CHECK (steps[0].cue == rig.third);
    CHECK (steps[0].origin == 'f');

    /*  AND THE POINTER DID NOT MOVE, which is the reason `cue.fire` is not
        `go` - so this history has a step whose cue nobody was standing on. */
    CHECK (rig.standby() != rig.third);
}

TEST_CASE ("history: a trigger's step says a trigger took it")
{
    HistoryRig rig;

    const auto trigger = rig.document.createTrigger (rig.third, "osc");
    REQUIRE (trigger.ok);
    REQUIRE (rig.document.setAttribute ("/godot/trigger/" + trigger.id + "/address",
                                        "/desk/go").ok);

    CHECK (rig.submitAndTick ("trigger.fire",
                              { osc::Value::string (trigger.id) }).applied == 1);

    const auto& steps = rig.runner.listState().historyOf (rig.listId);
    REQUIRE (steps.size() == 1u);
    CHECK (steps[0].cue == rig.third);
    CHECK (steps[0].origin == 't');
}

TEST_CASE ("history: a step lands on the list that holds the cue, however deep it is")
{
    /*  `cue.fire` and `trigger.fire` name a CUE, and a history belongs to a
        LIST - so the step climbs, exactly as `fireStandby` climbs to find whose
        pointer to move. A member three groups down is its list's step. */
    HistoryRig rig;

    const auto scene = rig.document.createCue (rig.listId, 4, "group", "Scene").id;
    const auto inner = rig.document.createCue (scene, 0, "group", "Inner").id;
    const auto deep = rig.document.createCue (inner, 0, "memo", "Deep").id;

    /*  A second list, whose history must stay its own. */
    const auto other = rig.document.createList ("Foyer").id;
    const auto elsewhere = rig.document.createCue (other, 0, "memo", "Doors").id;

    CHECK (rig.submitAndTick ("cue.fire", { osc::Value::string (deep) }).applied >= 1);
    CHECK (rig.submitAndTick ("cue.fire",
                              { osc::Value::string (elsewhere) }).applied >= 1);

    CHECK (rig.cuesStepped() == std::vector<std::string> { deep });
    CHECK (rig.cuesStepped (other) == std::vector<std::string> { elsewhere });
}

TEST_CASE ("history: a step is spelled for the node the same way in every locale")
{
    /*  The node carries sixty-four of these in one string, so a step is spelled
        with colons inside and the spaces are left to separate them. Every field
        is an integer or an identifier - there is no number to format - which is
        the reason this one readout needs no locale care while `aim` does. */
    CHECK (cue::spellStep ({ 1234, "MA100000", 'g' }) == "1234:MA100000:g");
    CHECK (cue::spellStep ({ 0, "SA000001", 't' }) == "0:SA000001:t");
}

TEST_CASE ("history: it keeps sixty-four steps and forgets the sixty-fifth")
{
    /*  BOUNDED BECAUSE THE NODE IS ONE STRING, and because the use is going
        back a few steps rather than reading an evening: an operator who wants
        act one again asks the solver for act one, not the history. What has to
        survive the bound is the NEWEST, which is the end a jump comes from. */
    cue::ListState state;

    for (int n = 0; n < 100; ++n)
        state.stepped ("LIST0001", { n, "CUE" + std::to_string (n), 'g' });

    const auto& steps = state.historyOf ("LIST0001");

    CHECK (steps.size() == cue::ListState::kept);
    CHECK (steps.front().cue == "CUE36");           // 100 - 64
    CHECK (steps.back().cue == "CUE99");

    /*  The total counts every step ever taken, not what is kept: a hook watches
        it to notice one, and a counter that stopped at sixty-four would stop
        telling it anything. */
    CHECK (state.stepsTaken() == 100u);

    /*  And a list nobody stepped has no history rather than an empty entry with
        a name, which is what a readout would publish. */
    CHECK (state.historyOf ("LIST0002").empty());
}

TEST_CASE ("history: a replay reproduces it, with no audio and no hooks")
{
    /*  THE WHOLE ARGUMENT FOR WRITING IT IN THE HANDLER, made executable. A
        replay applies the same records to the same show and must arrive at the
        same history - which it can only do if nothing about the history was
        decided by something a replay does not run.

        The show is handed over as its own canonical XML, which is what
        `wfg replay --bundle` does; the pointer was parked through a COMMAND, so
        the log carries it and the replayed session starts where this one did. */
    HistoryRig session;
    session.park (session.memoId);

    session.submitAndTick ("go");
    session.submitAndTick ("cue.fire", { osc::Value::string (session.third) });
    session.submitAndTick ("go");

    const auto show = doc::CanonicalXml::write (session.document);
    const auto original = LogFile::parse (session.engine.log().contents());
    REQUIRE (original.errors.empty());

    HistoryRig fresh;
    fresh.runner.setPlayer (nullptr);               // no audio side at all

    doc::ReadResult read = doc::CanonicalXml::read (show, fresh.document);
    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);

    /*  ASKED BY THE SESSION'S LIST IDENTIFIER, because that is the one the
        replayed show has: the fresh rig's own constructor drew a list before
        the XML was read over it, and that list no longer exists. Which is the
        replay's own shape - a log means nothing except against the show it was
        recorded on. */
    CHECK (fresh.spelled (session.listId) == session.spelled());
    CHECK (fresh.runner.listState().stepsTaken()
             == session.runner.listState().stepsTaken());
}


//==============================================================================
/*  THE PERSISTENT SECTION: checked, not fired (§3.29, decision S).

    A persistent cue is the thing that should be running at all times and is put
    back if it is not - a room tone, a rain bed, a desk value the show depends
    on. It lives in a section of its own rather than in a header, because a
    header fires once and never resets while this re-asserts.

    AFTER AN APPLIED TRIGGER, AND NEVER ON ITS OWN CLOCK, which is the PRD's
    reason rather than an implementation convenience: "a tick-rate check makes a
    stop impossible, a trigger-rate check is human-paced". An operator who kills
    the bed has until their next press to decide something else - and if they do
    not, the kill is remembered anyway.

    THE ASSERTION IS A MODE OF THE SOLVER, not a second mechanism, and the case
    that proves it is the stop cue: a stop before the pointer that names the bed
    suspends it, and nothing in the assertion knows what a stop is. The
    last-writer walk sees it, exactly as it sees one before a jump.
*/
namespace
{
    struct PersistentRig : Rig
    {
        PersistentRig()
        {
            const auto made = document.createPersistent (listId);
            REQUIRE (made.ok);
            section = made.id;

            bed = document.createCue (section, 0, "media", "Rain").id;
            document.setAttribute ("/godot/cue/" + bed + "/file", "rain.wav");
        }

        /*  A step: a GO on the list, which is what makes the section check.

            PARKED FIRST, because a GO with the pointer off the end of the list
            fires nothing and is therefore not a step at all - which is correct,
            and made this rig's steps stop happening after two of them. An
            operator going back to a cue and pressing again is an ordinary
            evening; this is that. */
        void step (const std::string& from = {})
        {
            REQUIRE (submitAndTick ("standby.set",
                                    { osc::Value::string (from.empty() ? memoId : from) })
                       .applied == 1);

            REQUIRE (submitAndTick ("go").applied >= 1);
            tickOnce();               // the hook's own tick, after the handler's
        }

        /*  Ticks until the assertion has had its say. It waits for the
            observation sweep it asked for, and gives up after half a second -
            so a rig with no desk at all still has to let that go by. */
        void settle (int ticks = 30)
        {
            for (int n = 0; n < ticks; ++n)
                tickOnce();
        }

        const cue::Run* liveBed() const { return runs.liveRunOf (bed); }

        std::string section, bed;
    };
}

TEST_CASE ("persistent: the section is published, and its cues say where they sit")
{
    PersistentRig rig;

    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, rig.engine.commands(), mounts, rig.runs };
    parameters.markStale();

    tree::EngineState state;
    const auto snapshot = parameters.publish (0, state);
    REQUIRE (snapshot != nullptr);

    const auto spelled = [&snapshot] (const std::string& address)
    {
        const auto* node = snapshot->find (address);
        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        return node->soleValue()->getString();
    };

    CHECK (spelled ("/godot/list/" + rig.listId + "/persistentOrder") == rig.bed);

    /*  AND NOT AMONG THE MEMBERS. A section in `order` would be a row the
        pointer walks through and GO fires, which is the one thing it is not. */
    const auto members = spelled ("/godot/list/" + rig.listId + "/order");

    INFO ("order: " << members);
    CHECK (members.find (rig.section) == std::string::npos);
    CHECK (members.find (rig.bed) == std::string::npos);

    /*  The identifier of the section itself, so that `cue.create` can name it
        as a parent - which is how anything gets INTO it. */
    CHECK (spelled ("/godot/list/" + rig.listId + "/persistent") == rig.section);

    /*  And the cue's own role, the fourth word §12.5 drew and 4.1 built. */
    CHECK (spelled ("/godot/cue/" + rig.bed + "/role") == "persistent");
}

TEST_CASE ("persistent: the pointer skips the section and cannot be parked in it")
{
    /*  The cursor skips it as it skips a footer, and `standby.set` refuses -
        with `not-a-stop` rather than `not-in-list`, because the cue IS in this
        list and the remedy is somewhere else entirely.

        THE CODE WAS SPELLED `not-manual-path` UNTIL 2026-09-16, when the
        pointer learned to stand inside every group and what was left being
        refused stopped being a question about nesting. The section is one of
        the three things still refused, so this case did not move with the
        rename - which is the point of naming the code here rather than only
        counting the rejection. */
    PersistentRig rig;

    rig.setStandby (rig.mediaId);

    for (int n = 0; n < 6; ++n)
        rig.submitAndTick ("standby.next");

    CHECK (rig.standby() != rig.bed);

    const auto refused = rig.submitAndTick ("standby.set", { osc::Value::string (rig.bed) });
    CHECK (refused.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notAStop) != std::string::npos);
    CHECK (rig.standby() != rig.bed);
}

TEST_CASE ("persistent: a bed that ended on its own is put back at the next trigger")
{
    /*  THE WHOLE POINT, in one case: the file ran out, nobody noticed, and the
        next press has the room back. And not before - the check is human-paced
        by design, so the silence between the end and the press is real. */
    PersistentRig rig;

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    const auto* first = rig.liveBed();
    REQUIRE (first != nullptr);
    CHECK (first->asserted);

    const auto firstRun = first->id;

    /*  It ends the way a file running out ends it. */
    REQUIRE (rig.engine.submit (origin::engine, "run.ended",
                                { osc::Value::string (firstRun) }));
    rig.tickOnce();

    CHECK (rig.runs.find (firstRun)->isFinished());
    CHECK (rig.liveBed() == nullptr);

    /*  Ticks alone do nothing: this is not a tick-rate check. */
    rig.settle (60);
    CHECK (rig.liveBed() == nullptr);

    /*  The next press, and it is back - as a NEW run, marked as the machine's. */
    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    const auto* again = rig.liveBed();
    REQUIRE (again != nullptr);
    CHECK (again->id != firstRun);
    CHECK (again->asserted);

    /*  And the record says who did it and why. */
    const auto log = rig.engine.log().contents();
    CHECK (log.find ("run.assert") != std::string::npos);
}

TEST_CASE ("persistent: the assertion moves neither standby nor focus")
{
    /*  §3.5: only GO moves the pointer. A machine action that moved it would
        take the operator's place away while their back was turned. */
    PersistentRig rig;

    rig.step();

    /*  Where the press left it, before the assertion has had its say. */
    const auto where = rig.standby();

    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    REQUIRE (rig.liveBed() != nullptr);          // it did assert something
    CHECK (rig.standby() == where);
}

TEST_CASE ("persistent: a kill leaves it silent, and a load-to-time brings it back")
{
    /*  DECISION S, both halves. A kill is run-local and for the session: an
        operator who stops the bed has stopped it, and a machine that put it
        back at the next press would be a machine they were fighting. A
        load-to-time is the one thing that lifts it, because it is the same
        question asked again with a new answer. */
    PersistentRig rig;

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    const auto* live = rig.liveBed();
    REQUIRE (live != nullptr);

    REQUIRE (rig.submitAndTick ("run.kill",
                                { osc::Value::string (live->id) }).applied == 1);

    /*  The audio side reports it stopped, as it does for any kill. */
    REQUIRE (rig.engine.submit (origin::engine, "run.ended",
                                { osc::Value::string (rig.runOf (rig.bed)) }));
    rig.tickOnce();

    CHECK (rig.liveBed() == nullptr);

    rig.step();
    rig.settle();

    CHECK (rig.liveBed() == nullptr);
    CHECK (rig.runner.isSuspended (rig.bed));

    /*  And back, because the operator asked the question again. */
    REQUIRE (rig.engine.submit ("cli", "list.aim",
                                { osc::Value::string (rig.listId),
                                  osc::Value::string (rig.memoId),
                                  osc::Value::float64 (-1.0) }));
    rig.tickOnce();
    REQUIRE (rig.submitAndTick ("list.loadToTime",
                                { osc::Value::string (rig.listId) }).applied >= 1);

    CHECK_FALSE (rig.runner.isSuspended (rig.bed));

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    CHECK (rig.liveBed() != nullptr);
}

TEST_CASE ("persistent: a double Esc leaves it silent, suspends nothing, and the next GO brings it back")
{
    /*  PRD §3.29: "a double Esc does not [suspend]: the next GO restoring the
        declared world is the point of declaring it". Until 2026-09-26 it did -
        `run.killAll` marked every root `killed`, which is how the running
        pane's kill suspends, so one double Esc silenced the section for the
        rest of the session (namespace draft §18.8). */
    PersistentRig rig;

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    const auto* live = rig.liveBed();
    REQUIRE (live != nullptr);
    const auto first = live->id;

    REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);

    const auto* killed = rig.runs.find (first);
    REQUIRE (killed != nullptr);
    CHECK (killed->skipFooter);
    CHECK_FALSE (killed->killed);

    REQUIRE (rig.engine.submit (origin::engine, "run.ended", { osc::Value::string (first) }));
    rig.tickOnce();

    CHECK (rig.liveBed() == nullptr);

    /*  Silent until somebody presses: ticks alone put nothing back. */
    rig.settle (60);
    CHECK (rig.liveBed() == nullptr);

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    CHECK_FALSE (rig.runner.isSuspended (rig.bed));

    const auto* again = rig.liveBed();
    REQUIRE (again != nullptr);
    CHECK (again->id != first);
    CHECK (again->asserted);
}

TEST_CASE ("persistent: a double Esc right after a GO takes back the pass that GO opened, and the next GO brings the section back")
{
    /*  PRD §3.29: after a double Esc the NEXT GO restores the declared world.
        A pass the GO before the press had already opened put the bed back right
        after the press instead - opened in the press's own drain and run on the
        next tick, or run in the press's tick and its record drained behind the
        press (the review, 2026-10-02, namespace draft §23.10). */
    PersistentRig rig;
    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (rig.memoId) }).applied == 1);

    auto sameDrain = true;

    SUBCASE ("in the GO's own drain") {}
    SUBCASE ("on the tick after the GO, its pass decided ahead of the press") { sameDrain = false; }

    REQUIRE (rig.engine.submit ("cli", "go", {}));

    if (! sameDrain)
        rig.tickOnce();

    REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
    rig.tickOnce();

    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    CHECK (rig.runOf (rig.bed).empty());
    CHECK (rig.liveBed() == nullptr);

    /*  And the next GO brings it back, as it does after any double Esc. */
    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    REQUIRE (rig.liveBed() != nullptr);
    CHECK (rig.liveBed()->asserted);
}

TEST_CASE ("persistent: a jump leaves the section sounding")
{
    /*  The section is outside the jump: the solver never plans it, so the
        load-to-time sweep - which ends every run of the list the plan does not
        name - ended a sounding bed, and a jump is not a step, so nothing put
        it back until the next GO (2026-09-26, namespace draft §18.8). The same
        run, still sounding, is the whole answer. */
    PersistentRig rig;

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    const auto* live = rig.liveBed();
    REQUIRE (live != nullptr);
    const auto sounding = live->id;

    REQUIRE (rig.engine.submit ("cli", "list.aim",
                                { osc::Value::string (rig.listId),
                                  osc::Value::string (rig.mediaId),
                                  osc::Value::float64 (0.5) }));
    rig.tickOnce();
    REQUIRE (rig.submitAndTick ("list.loadToTime",
                                { osc::Value::string (rig.listId) }).applied >= 1);
    rig.settle();

    /*  The jump happened - the plan built its cue - so the sweep before it ran
        and passed the bed by, rather than never running at all. */
    REQUIRE_FALSE (rig.runOf (rig.mediaId).empty());

    const auto* still = rig.runs.find (sounding);
    REQUIRE (still != nullptr);
    CHECK_FALSE (still->isFinished());
    REQUIRE (rig.liveBed() != nullptr);
    CHECK (rig.liveBed()->id == sounding);
}

TEST_CASE ("persistent: a stop before the pointer suspends it, and the solver is what sees that")
{
    /*  The case that says this is a MODE of the solver rather than a second
        mechanism: nothing in the assertion knows what a stop cue is. The
        last-writer walk reads the rows before the pointer, exactly as it does
        for a jump, and the bed is simply not in the plan. */
    PersistentRig rig;

    const auto halt = rig.document.createCue (rig.listId, 2, "transport", "Kill the rain").id;
    rig.document.setAttribute ("/godot/cue/" + halt + "/target", rig.bed);

    const auto after = rig.document.createCue (rig.listId, 3, "memo", "After").id;

    /*  The pointer BEFORE the stop: the stop has not happened, so the bed is
        asserted. */
    rig.setStandby (rig.memoId);
    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    REQUIRE (rig.liveBed() != nullptr);

    const auto* plan = rig.liveBed();
    const auto runId = plan->id;

    REQUIRE (rig.engine.submit (origin::engine, "run.ended", { osc::Value::string (runId) }));
    rig.tickOnce();

    /*  The pointer AFTER the stop, which is what the document says has
        happened by now. */
    rig.step (after);
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();

    CHECK (rig.liveBed() == nullptr);
}

TEST_CASE ("persistent: a fade in the section warns and is never asserted")
{
    /*  A fade asserts nothing, a stop is what suspends an assertion, and a
        group is a lifetime rather than a state. Each is left where it is,
        ignored, and `wfg validate` is where somebody finds out why. */
    PersistentRig rig;

    const auto fade = rig.document.createCue (rig.section, 1, "fade", "Under").id;
    rig.document.setAttribute ("/godot/cue/" + fade + "/target", rig.mediaId);

    auto said = false;

    for (const auto& problem : rig.document.warnings())
        if (problem.find ("asserts media, mic, osc and midi cues") != std::string::npos)
            said = true;

    CHECK (said);

    rig.setStandby (rig.memoId);
    rig.step();
    rig.settle();

    CHECK (rig.runOf (fade).empty());
}

TEST_CASE ("persistent: the plan is what the section declares, and a disabled cue is not in it")
{
    /*  The solver's own half, asked directly: what SHOULD be true. */
    PersistentRig rig;

    const auto desk = rig.document.createCue (rig.section, 1, "osc", "Desk").id;
    rig.document.setAttribute ("/godot/cue/" + desk + "/address", "/desk/fader");
    rig.document.setAttribute ("/godot/cue/" + desk + "/value", "f:0.75");

    auto plan = cue::solvePersistent (rig.document, nullptr, nullptr, rig.listId, rig.memoId);

    CHECK (plan.ok);
    REQUIRE (plan.runs.size() == 1u);
    CHECK (plan.runs.front().cue == rig.bed);
    REQUIRE (plan.values.size() == 1u);
    CHECK (plan.values.front().address == "/desk/fader");
    CHECK (plan.values.front().writer == desk);

    /*  A cue turned off during tech is still written down and asserts nothing,
        which is the whole difference between disabling one and deleting it. */
    rig.document.setAttribute ("/godot/cue/" + rig.bed + "/enabled", "false");

    plan = cue::solvePersistent (rig.document, nullptr, nullptr, rig.listId, rig.memoId);
    CHECK (plan.runs.empty());
    CHECK (plan.values.size() == 1u);
}

//==============================================================================
TEST_CASE ("fx: the arm carries the cue's inserts against the set, an edit pushes what moved, a withdrawn value goes back to the preset")
{
    /*  Decision AE and §17.4: every voice carries the whole set; a cue says
        which entries it switches in and what values it sets. At the arm the
        request carries one FxSetting per entry in chain order; while the cue
        sounds, an edit to the row pushes only what changed - one value, one
        switch, one value withdrawn as -1 - and a quiet tick pushes nothing. */
    RoutedRig rig;
    rig.setMedia (rig.mediaId, 2);
    rig.aimAt (rig.mediaId, rig.main);

    const auto gain = rig.document.createPlugin ("Test gain", "godot:test-gain", "VST3", "", "");
    REQUIRE (gain.ok);
    const auto verb = rig.document.createPlugin ("Verb", "VST3-0badf00d-verb", "VST3", "", "");
    REQUIRE (verb.ok);

    /*  Only the gain is switched in, with one value; the verb has no Fx at all. */
    const auto fx = rig.document.createFx (rig.mediaId, gain.id, "");
    REQUIRE (fx.ok);
    REQUIRE (rig.document.setAttribute ("/godot/fx/" + fx.id + "/values", "0:0.25").ok);

    const auto run = rig.play();
    REQUIRE (rig.runs.find (run) != nullptr);
    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);

    const auto& armed = rig.audio.lastArmFx;
    REQUIRE (armed.size() == 2u);
    CHECK (armed[0].slot == 0);
    CHECK (armed[0].enabled);
    CHECK (armed[0].fxId == fx.id);
    REQUIRE (armed[0].values.size() == 1u);
    CHECK (armed[0].values[0].first == 0);
    CHECK (armed[0].values[0].second == doctest::Approx (0.25f));
    CHECK (armed[1].slot == 1);
    CHECK_FALSE (armed[1].enabled);
    CHECK (armed[1].values.empty());

    for (int i = 0; i < 3; ++i)
        rig.tickOnce();

    CHECK (rig.audio.fxValues.empty());
    CHECK (rig.audio.fxEnables.empty());

    //  Edited while it sounds: p0 moved, p1 appeared.
    rig.submitAndTick ("node.set", { osc::Value::string ("/godot/fx/" + fx.id + "/values"),
                                     osc::Value::string ("0:0.75 1:1") });
    rig.tickOnce();
    REQUIRE (rig.audio.fxValues.size() == 2u);
    CHECK (rig.audio.fxValues[0].track == track);
    CHECK (rig.audio.fxValues[0].slot == 0);
    CHECK (rig.audio.fxValues[0].parameter == 0);
    CHECK (rig.audio.fxValues[0].value == doctest::Approx (0.75f));
    CHECK (rig.audio.fxValues[1].parameter == 1);
    CHECK (rig.audio.fxValues[1].value == doctest::Approx (1.0f));
    CHECK (rig.audio.fxEnables.empty());

    SUBCASE ("a value withdrawn from the row is pushed as -1, back to the preset")
    {
        rig.submitAndTick ("node.set", { osc::Value::string ("/godot/fx/" + fx.id + "/values"),
                                         osc::Value::string ("1:1") });
        rig.tickOnce();
        REQUIRE (rig.audio.fxValues.size() == 3u);
        CHECK (rig.audio.fxValues[2].parameter == 0);
        CHECK (rig.audio.fxValues[2].value == doctest::Approx (-1.0f));
    }

    SUBCASE ("the switch is pushed once, and only the switch")
    {
        rig.submitAndTick ("node.set", { osc::Value::string ("/godot/fx/" + fx.id + "/enabled"),
                                         osc::Value::string ("false") });
        rig.tickOnce();
        REQUIRE (rig.audio.fxEnables.size() == 1u);
        CHECK (rig.audio.fxEnables[0].track == track);
        CHECK (rig.audio.fxEnables[0].slot == 0);
        CHECK_FALSE (rig.audio.fxEnables[0].enabled);
        CHECK (rig.audio.fxValues.size() == 2u);
    }

    SUBCASE ("and a tick with nothing edited pushes nothing at all")
    {
        const auto pushes = rig.audio.fxValues.size();

        for (int i = 0; i < 10; ++i)
            rig.tickOnce();

        CHECK (rig.audio.fxValues.size() == pushes);
    }
}

TEST_CASE ("fx: an insert switched in before GO asks for the cue's state; one switched in while it sounds does not")
{
    /*  Found 2026-09-28. A cue armed at standby with its insert out asks the
        voice for no state - the arm's rule loads one only for what is switched
        in - so switching it in before GO sent the switch alone, and the cue
        played through whatever the instance last held: the previous cue's
        state on that voice, which the arm's own comment says must never be
        heard under this one. Now the switch asks for the cue's state (the
        preset's own, when it has none), and the launch waits for it. While the
        cue sounds nothing is loaded - the knobs follow live - and the lane's
        reset on the switch clears what it held (ProxyTests). */
    RoutedRig rig;
    rig.setMedia (rig.mediaId, 2);
    rig.aimAt (rig.mediaId, rig.main);

    const auto gain = rig.document.createPlugin ("Test gain", "godot:test-gain", "VST3", "", "");
    REQUIRE (gain.ok);
    const auto fx = rig.document.createFx (rig.mediaId, gain.id, "");
    REQUIRE (fx.ok);
    REQUIRE (rig.document.setAttribute ("/godot/fx/" + fx.id + "/enabled", "false").ok);

    const auto switchTo = [&] (const char* value)
    {
        rig.submitAndTick ("node.set", { osc::Value::string ("/godot/fx/" + fx.id + "/enabled"),
                                         osc::Value::string (value) });
        rig.tickOnce();
    };

    //  Armed at standby with the insert out: the arm asks for nothing of it.
    rig.setStandby (rig.mediaId);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.mediaId).empty(); }));
    const auto run = rig.runOf (rig.mediaId);
    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);
    REQUIRE (rig.audio.lastArmFx.size() == 1u);
    CHECK_FALSE (rig.audio.lastArmFx[0].enabled);
    CHECK (rig.audio.fxStates.empty());

    //  Switched in while it waits: the switch, and the preset's own state.
    switchTo ("true");
    REQUIRE (rig.audio.fxEnables.size() == 1u);
    CHECK (rig.audio.fxEnables[0].enabled);
    REQUIRE (rig.audio.fxStates.size() == 1u);
    CHECK (rig.audio.fxStates[0].track == track);
    CHECK (rig.audio.fxStates[0].slot == 0);
    CHECK (rig.audio.fxStates[0].path.empty());

    //  Out and in again before GO: asked again - the instance may have been
    //  given to nobody else, but the rule costs nothing when the state is held.
    switchTo ("false");
    switchTo ("true");
    CHECK (rig.audio.fxStates.size() == 2u);

    //  GO, and switched out and in while it sounds: the switch, never a state.
    rig.audio.completeArms (rig.engine);
    CHECK (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (run)->launchedAtSample > 0; }));

    switchTo ("false");
    switchTo ("true");
    CHECK (rig.audio.fxEnables.size() == 5u);
    CHECK (rig.audio.fxEnables.back().enabled);
    CHECK (rig.audio.fxStates.size() == 2u);
}

TEST_CASE ("fx: the slots are the graph's - an entry added since has none, one moved ahead keeps its own, one taken out is switched out")
{
    /*  2026-09-26: a set edited after the graph was built used to send a
        cue's settings by the set's order as it stood NOW, so an entry put
        ahead of another sent that other's switch and values to the wrong
        plugin. The graph's own slots come from the plugin table. */
    RoutedRig rig;
    rig.setMedia (rig.mediaId, 2);
    rig.aimAt (rig.mediaId, rig.main);

    const auto gain = rig.document.createPlugin ("Test gain", "godot:test-gain", "VST3", "", "");
    REQUIRE (gain.ok);
    const auto verb = rig.document.createPlugin ("Verb", "VST3-0badf00d-verb", "VST3", "", "");
    REQUIRE (verb.ok);

    //  The graph was built with the two, in that order.
    plugin::PluginTable table;
    table.setBuilt ({ gain.id, verb.id });
    rig.runner.setPlugins (&table);

    const auto verbFx = rig.document.createFx (rig.mediaId, verb.id, "");
    REQUIRE (verbFx.ok);
    REQUIRE (rig.document.setAttribute ("/godot/fx/" + verbFx.id + "/values", "0:0.5").ok);

    SUBCASE ("an entry added since the graph was built is not sent, and the built ones keep their slots")
    {
        const auto late = rig.document.createPlugin ("Late", "VST3-00000000-late", "VST3", "", "");
        REQUIRE (late.ok);

        //  Put at the head of the set, where the document would number it slot 0.
        auto plugins = rig.document.root().getChildWithName ("Audio").getChildWithName ("Plugins");
        const auto lateNode = rig.document.findById (late.id);
        plugins.moveChild (plugins.indexOf (lateNode), 0, nullptr);

        const auto lateFx = rig.document.createFx (rig.mediaId, late.id, "");
        REQUIRE (lateFx.ok);

        rig.play();
        const auto& armed = rig.audio.lastArmFx;
        REQUIRE (armed.size() == 2u);
        CHECK (armed[0].slot == 0);
        CHECK_FALSE (armed[0].enabled);          // the gain: no Fx on the cue
        CHECK (armed[1].slot == 1);
        CHECK (armed[1].enabled);                // the verb, still slot 1
        CHECK (armed[1].fxId == verbFx.id);
    }

    SUBCASE ("an entry taken out since keeps its slot, switched out")
    {
        const auto gainFx = rig.document.createFx (rig.mediaId, gain.id, "");
        REQUIRE (gainFx.ok);

        auto plugins = rig.document.root().getChildWithName ("Audio").getChildWithName ("Plugins");
        plugins.removeChild (rig.document.findById (gain.id), nullptr);

        rig.play();
        const auto& armed = rig.audio.lastArmFx;
        REQUIRE (armed.size() == 2u);
        CHECK (armed[0].slot == 0);
        CHECK_FALSE (armed[0].enabled);          // its Fx is on the cue, its entry is not in the set
        CHECK (armed[1].slot == 1);
        CHECK (armed[1].enabled);
    }
}

TEST_CASE ("width: a cue its inserts made stereo plays its sides apart where there is room, summed where there is not")
{
    /*  The author's decision of 2026-09-26: a mono cue with a stereo insert
        switched in comes out stereo; where the routing has room for two sides
        it plays them apart, where it has room for one they are summed at a
        half each, and a cue is never made narrower than its file. */
    RoutedRig rig;
    rig.setMedia (rig.mediaId, 1);
    std::string problem;

    SUBCASE ("onto a stereo direct out: side to channel")
    {
        rig.aimAt (rig.mediaId, rig.main);
        CHECK (rig.widenedSpreadOf (rig.mediaId, 2, problem) == "0>0@1.000 1>1@1.000");
        CHECK (problem.empty());

        //  And with no insert widening it, the mono cue onto everything, as ever.
        CHECK (rig.spreadOf (rig.mediaId, problem) == "0>0@1.000 0>1@1.000");
    }

    SUBCASE ("onto a mono direct out: the two sides summed at a half")
    {
        const auto mono = rig.addBus ("Centre", 6, 1);
        rig.aimAt (rig.mediaId, mono);
        CHECK (rig.widenedSpreadOf (rig.mediaId, 2, problem) == "0>6@0.500 1>6@0.500");
        CHECK (problem.empty());
    }

    SUBCASE ("through a written one-row route: summed into its row")
    {
        rig.addRoute (rig.mediaId, rig.main, "1 0.5");
        CHECK (rig.widenedSpreadOf (rig.mediaId, 2, problem) == "0>0@0.500 0>1@0.250 1>0@0.500 1>1@0.250");
    }

    SUBCASE ("through a written two-row route: as written")
    {
        rig.addRoute (rig.mediaId, rig.main, "1 0 0 1");
        CHECK (rig.widenedSpreadOf (rig.mediaId, 2, problem) == "0>0@1.000 1>1@1.000");
    }

    SUBCASE ("a stereo file its inserts leave stereo routes exactly as it did")
    {
        rig.setMedia (rig.mediaId, 2);
        rig.aimAt (rig.mediaId, rig.main);
        CHECK (rig.widenedSpreadOf (rig.mediaId, 2, problem) == rig.spreadOf (rig.mediaId, problem));
    }
}

TEST_CASE ("width: the arm sends each insert the cue's width there, and a plugin coming up widens a sounding cue")
{
    RoutedRig rig;
    rig.setMedia (rig.mediaId, 1);
    rig.aimAt (rig.mediaId, rig.main);

    const auto verb = rig.document.createPlugin ("Verb", "VST3-0badf00d-verb", "VST3", "", "");
    REQUIRE (verb.ok);
    REQUIRE (rig.document.createFx (rig.mediaId, verb.id, "").ok);

    plugin::PluginTable table;
    table.setBuilt ({ verb.id });
    rig.runner.setPlugins (&table);

    //  Not up yet: counted as taking the cue at its width, which is mono.
    const auto run = rig.play();
    const auto track = rig.runs.find (run)->track;

    REQUIRE (rig.audio.lastArmFx.size() == 1u);
    CHECK (rig.audio.lastArmFx[0].feed == 1);
    CHECK (rig.audio.lastArmFx[0].back == 1);

    //  The plugin comes up, stereo in and out: the cue is stereo from the next tick.
    plugin::PluginTable::Status loaded;
    loaded.state = "loaded";
    loaded.inputs = 2;
    loaded.outputs = 2;
    table.set (verb.id, loaded);
    rig.tickOnce();

    REQUIRE_FALSE (rig.audio.fxShapes.empty());
    CHECK (rig.audio.fxShapes.back().track == track);
    CHECK (rig.audio.fxShapes.back().feed == 1);
    CHECK (rig.audio.fxShapes.back().back == 2);
    CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000");
}

TEST_CASE ("eq: the arm carries the cue's EQ, an edit reaches the voice once, a quiet tick not at all")
{
    /*  PHASE 9a's parameter path, on the tick thread's side: the twenty-three rows
        are read through the schema at the arm and ride the request; while the
        cue sounds, a write to one of them - a rotary, a panel, the page - is
        pushed to that voice on the next tick and to no other; nothing is
        pushed while nobody edits; and eq.reset is one command that lands as
        one push. The audio side is a fake here; the sound is AudioTests'. */
    RoutedRig rig;

    rig.setMedia (rig.mediaId, 2);
    rig.aimAt (rig.mediaId, rig.main);

    //  Shaped before it plays, so the arm has something to carry.
    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/eqB2Gain", "6");
    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/eqB2Freq", "1000");
    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/eqB3Gain", "-4");
    rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/eqB3On", "false");

    const auto run = rig.play();
    REQUIRE (rig.runs.find (run) != nullptr);

    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);

    const auto armed = rig.audio.lastArmEq;
    CHECK (armed.on);
    CHECK (armed.band[1].gain == doctest::Approx (6.0f));
    CHECK (armed.band[1].freq == doctest::Approx (1000.0f));
    CHECK (armed.band[0].gain == doctest::Approx (0.0f));
    CHECK_FALSE (armed.hpf);

    //  A band switched off rides the arm off, its gain kept (2026-09-25).
    CHECK (armed.band[1].on);
    CHECK_FALSE (armed.band[2].on);
    CHECK (armed.band[2].gain == doctest::Approx (-4.0f));

    /*  THE ARM CARRIED IT, so the first ticks push nothing through the live
        door: what the voice holds and what the cue says are already one. */
    for (int i = 0; i < 3; ++i)
        rig.tickOnce();

    CHECK (rig.audio.eqPushes == 0);

    //  Edited while it sounds: the high-pass switched in.
    rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/" + rig.mediaId + "/eqHpf"),
                                     osc::Value::string ("true") });
    rig.tickOnce();

    CHECK (rig.audio.eqPushes == 1);
    CHECK (rig.audio.eqs[track].hpf);
    CHECK (rig.audio.eqs[track].band[1].gain == doctest::Approx (6.0f));

    SUBCASE ("and a tick with nothing edited pushes nothing at all")
    {
        const auto pushes = rig.audio.eqPushes;

        for (int i = 0; i < 10; ++i)
            rig.tickOnce();

        CHECK (rig.audio.eqPushes == pushes);
    }

    SUBCASE ("and eq.reset is one command, every row back, one push")
    {
        const auto pushes = rig.audio.eqPushes;

        rig.submitAndTick ("eq.reset", { osc::Value::string (rig.mediaId) });
        rig.tickOnce();

        CHECK (rig.audio.eqPushes == pushes + 1);
        CHECK (rig.audio.eqs[track].isIdentity());
        CHECK_FALSE (rig.audio.eqs[track].hpf);
        CHECK (rig.audio.eqs[track].band[1].gain == doctest::Approx (0.0f));

        /*  The document says flat too. The tree keeps a value written at its
            default - sparseness is the canonical WRITER's, which omits it - so
            the question is what the row reads, not whether it is there. */
        CHECK (rig.document.getAttribute ("/godot/cue/" + rig.mediaId + "/eqB2Gain").value_or ("?") == "0");
        CHECK (rig.document.getAttribute ("/godot/cue/" + rig.mediaId + "/eqHpf").value_or ("?") == "false");
    }

    SUBCASE ("and a band's switch is pushed with its gain kept, and eq.reset puts it back on")
    {
        const auto pushes = rig.audio.eqPushes;

        rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/" + rig.mediaId + "/eqB2On"),
                                         osc::Value::string ("false") });
        rig.tickOnce();

        CHECK (rig.audio.eqPushes == pushes + 1);
        CHECK_FALSE (rig.audio.eqs[track].band[1].on);
        CHECK (rig.audio.eqs[track].band[1].gain == doctest::Approx (6.0f));

        rig.submitAndTick ("eq.reset", { osc::Value::string (rig.mediaId) });
        rig.tickOnce();

        CHECK (rig.audio.eqs[track].band[1].on);
        CHECK (rig.audio.eqs[track].band[2].on);
        CHECK (rig.document.getAttribute ("/godot/cue/" + rig.mediaId + "/eqB3On").value_or ("?") == "true");
    }

    SUBCASE ("and eq.reset on a cue that is not media is refused")
    {
        const auto outcome = rig.submitAndTick ("eq.reset", { osc::Value::string (rig.memoId) });
        CHECK (outcome.rejected > 0);
    }
}

//==============================================================================
//  A locked show's EQ and sends, ridden live (author, 2026-09-25).

namespace
{
    /*  A ROUTED RIG WITH THE DOORS SERVE INSTALLS: `node.set` answered by the
        live layer in front of the document, `send.create` too, `eq.reset`
        holding flat live under the lock, Keep and Discard, the transaction
        hook asking `isLiveEdit` - and a tree to read what a client sees. Two
        mix channels: the foldback, which the cue sends to, and a reverb it
        does not. */
    struct LiveRig : RoutedRig
    {
        LiveRig()
        {
            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::eitherOf (cue::liveWriteFor (runs, dcas, document),
                                                          cue::eitherOf (cue::liveEditFor (live, document),
                                                                         cue::fxWriteFor (document, nullptr, &live))),
                                           cue::liveSendFor (live, document));
            cue::registerCueCommands (engine.commands(), document, focus, &live);
            cue::registerLiveCommands (engine.commands(), document, live);

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       if (cue::isLiveWrite (appliedCommand.name, coerced)
                                             || cue::isLiveEdit (appliedCommand.name, coerced, document, live))
                                           return;

                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            runner.setLiveEdits (&live);
            parameters.setLiveEdits (&live);

            reverb = addBus ("Reverb", 6, 2);
            document.findById (foldback).setProperty (juce::Identifier ("kind"), "mix", nullptr);
            document.findById (reverb).setProperty (juce::Identifier ("kind"), "mix", nullptr);

            setMedia (mediaId, 2);
            aimAt (mediaId, main);
        }

        void lock (bool on)
        {
            REQUIRE (document.setAttribute ("/godot/document/locked", on ? "true" : "false").ok);
        }

        Engine::TickResult set (const std::string& address, osc::Value value)
        {
            return submitAndTick ("node.set", { osc::Value::string (address), std::move (value) });
        }

        std::string eq (const std::string& row) const
        {
            return "/godot/cue/" + mediaId + "/" + row;
        }

        std::string saved (const std::string& address) const
        {
            return document.getAttribute (address).value_or ("?");
        }

        /*  What a client reads at an address: the tree published now. */
        std::string published (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);

            const auto* node = snapshot->find (address);

            if (node == nullptr || node->values.size() != 1)
                return "<none>";

            const auto& value = node->values.front();

            if (value.isString())   return value.getString();
            if (value.isBool())     return value.getBool() ? "true" : "false";
            if (value.isNumber())   return osc::formatDouble (value.asDouble());

            return "<?>";
        }

        std::size_t steps() const
        {
            return static_cast<std::size_t> (document.history (doc::UndoDomain::document)
                                                 .getUndoDescriptions().size());
        }

        cue::LiveEdits live;
        cue::DcaTable dcas;
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::string reverb;
    };
}

TEST_CASE ("live: under the lock an EQ turn is heard, written to nothing, and no step of the history")
{
    LiveRig rig;
    const auto run = rig.play();
    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);

    rig.lock (true);
    const auto steps = rig.steps();
    const auto pushes = rig.audio.eqPushes;

    //  A turn on a locked show: applied, and heard on the next tick.
    CHECK (rig.set (rig.eq ("eqB2Gain"), osc::Value::float64 (6.0)).applied == 1);
    rig.tickOnce();

    CHECK (rig.audio.eqPushes == pushes + 1);
    CHECK (rig.audio.eqs[track].band[1].gain == doctest::Approx (6.0f));

    //  WRITTEN TO NOTHING: the show still says nought, and nothing is on its history.
    CHECK (rig.saved (rig.eq ("eqB2Gain")) == "0");
    CHECK (rig.steps() == steps);

    //  WHAT A CLIENT SEES: the value heard, at the saved value's address, and what rides.
    CHECK (rig.published (rig.eq ("eqB2Gain")) == "6");
    CHECK (rig.published (rig.eq ("live")) == "eqB2Gain");
    CHECK (rig.published ("/godot/document/live") == "1");

    //  A band's switch rides the same way.
    CHECK (rig.set (rig.eq ("eqB2On"), osc::Value::boolean (false)).applied == 1);
    rig.tickOnce();
    CHECK_FALSE (rig.audio.eqs[track].band[1].on);
    CHECK (rig.published ("/godot/document/live") == "2");

    SUBCASE ("turned back to what the show says, nothing rides")
    {
        CHECK (rig.set (rig.eq ("eqB2Gain"), osc::Value::float64 (0.0)).applied == 1);
        CHECK (rig.live.rowOf (rig.mediaId, "eqB2Gain") == nullptr);
        CHECK (rig.published ("/godot/document/live") == "1");
    }

    SUBCASE ("a bad value is refused as the show would refuse it")
    {
        CHECK (rig.set (rig.eq ("eqB2Gain"), osc::Value::float64 (99.0)).rejected == 1);
        CHECK (rig.set (rig.eq ("eqB2Freq"), osc::Value::string ("loud")).rejected == 1);
    }

    SUBCASE ("Keep is refused under the lock, and once unlocked is one undo step")
    {
        CHECK (rig.submitAndTick ("live.keep").rejected == 1);

        rig.lock (false);
        CHECK (rig.submitAndTick ("live.keep").applied == 1);

        CHECK (rig.saved (rig.eq ("eqB2Gain")) == "6");
        CHECK (rig.saved (rig.eq ("eqB2On")) == "false");
        CHECK (rig.live.empty());
        CHECK (rig.published ("/godot/document/live") == "0");
        CHECK (rig.steps() == steps + 1);

        //  And Undo takes the whole of it back.
        CHECK (rig.submitAndTick ("undo").applied == 1);
        CHECK (rig.saved (rig.eq ("eqB2Gain")) == "0");
        CHECK (rig.saved (rig.eq ("eqB2On")) == "true");
    }

    SUBCASE ("Discard lets the cue go back to its saved sound")
    {
        rig.lock (false);
        CHECK (rig.submitAndTick ("live.drop").applied == 1);
        rig.tickOnce();

        CHECK (rig.live.empty());
        CHECK (rig.audio.eqs[track].band[1].gain == doctest::Approx (0.0f));
        CHECK (rig.audio.eqs[track].band[1].on);
        CHECK (rig.published (rig.eq ("eqB2Gain")) == "0");
        CHECK (rig.steps() == steps);
    }

    SUBCASE ("unlocked, an edit to a row that rides writes the show and the live value is gone")
    {
        rig.lock (false);
        CHECK (rig.set (rig.eq ("eqB2Gain"), osc::Value::float64 (3.0)).applied == 1);

        CHECK (rig.saved (rig.eq ("eqB2Gain")) == "3");
        CHECK (rig.live.rowOf (rig.mediaId, "eqB2Gain") == nullptr);
        CHECK (rig.live.rowOf (rig.mediaId, "eqB2On") != nullptr);
        CHECK (rig.steps() == steps + 1);
    }
}

TEST_CASE ("live: under the lock a send rides live, a new one is made live, and Keep makes it real")
{
    LiveRig rig;
    const auto foldback = rig.addSend (rig.mediaId, rig.foldback, -6.0);
    const auto run = rig.play();
    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);

    rig.lock (true);
    const auto steps = rig.steps();

    //  The foldback send up to -3: heard, and not written.
    CHECK (rig.set ("/godot/send/" + foldback + "/level", osc::Value::float64 (-3.0)).applied == 1);
    rig.tickOnce();
    CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>4@0.708 1>5@0.708");
    CHECK (rig.saved ("/godot/send/" + foldback + "/level") == "-6");
    CHECK (rig.published ("/godot/send/" + foldback + "/level") == "-3");
    CHECK (rig.published ("/godot/send/" + foldback + "/live") == "true");

    /*  A SEND TO A MIX CHANNEL THE CUE DID NOT SEND TO (author: "new sends
        ride live too"): made live, its identifier on the applied record. */
    const auto made = rig.submitAndTick ("send.create", { osc::Value::string (rig.mediaId),
                                                          osc::Value::string (rig.reverb),
                                                          osc::Value::string (""),
                                                          osc::Value::string ("-10") });
    REQUIRE (made.applied == 1);
    rig.tickOnce();

    const auto created = rig.live.createdSendsOf (rig.mediaId);
    REQUIRE (created.size() == 1u);
    const auto reverb = created.front();

    CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>4@0.708 1>5@0.708 0>6@0.316 1>7@0.316");
    CHECK (rig.published ("/godot/send/" + reverb + "/bus") == rig.reverb);
    CHECK (rig.published ("/godot/send/" + reverb + "/live") == "true");
    CHECK (rig.published ("/godot/cue/" + rig.mediaId + "/sends") == foldback + " " + reverb);

    //  The show's mix channels in output order - a Send page's rotaries.
    CHECK (rig.published ("/godot/audio/mixes") == rig.foldback + " " + rig.reverb);
    CHECK_FALSE (rig.document.findById (reverb).isValid());

    //  Its level and its switch ride through the same door as any send's.
    CHECK (rig.set ("/godot/send/" + reverb + "/level", osc::Value::float64 (-20.0)).applied == 1);
    CHECK (rig.published ("/godot/send/" + reverb + "/level") == "-20");

    //  One send per bus per cue, across the show and the layer - and only into a mix channel.
    CHECK (rig.submitAndTick ("send.create", { osc::Value::string (rig.mediaId),
                                               osc::Value::string (rig.reverb) }).rejected == 1);
    CHECK (rig.submitAndTick ("send.create", { osc::Value::string (rig.mediaId),
                                               osc::Value::string (rig.foldback) }).rejected == 1);
    CHECK (rig.submitAndTick ("send.create", { osc::Value::string (rig.mediaId),
                                               osc::Value::string (rig.main) }).rejected == 1);

    //  A send switched off under the lock leaves the mix.
    CHECK (rig.set ("/godot/send/" + foldback + "/on", osc::Value::boolean (false)).applied == 1);
    rig.tickOnce();
    CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>6@0.100 1>7@0.100");
    CHECK (rig.steps() == steps);

    SUBCASE ("Keep writes the levels, the switch and the new send, with the identifier it rode under")
    {
        rig.lock (false);
        CHECK (rig.submitAndTick ("live.keep").applied == 1);

        CHECK (rig.saved ("/godot/send/" + foldback + "/level") == "-3");
        CHECK (rig.saved ("/godot/send/" + foldback + "/on") == "false");
        REQUIRE (rig.document.findById (reverb).isValid());
        CHECK (rig.saved ("/godot/send/" + reverb + "/bus") == rig.reverb);
        CHECK (rig.saved ("/godot/send/" + reverb + "/level") == "-20");
        CHECK (rig.live.empty());
        CHECK (rig.steps() == steps + 1);

        //  And it sounds as it did.
        rig.tickOnce();
        CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>6@0.100 1>7@0.100");
    }

    SUBCASE ("Discard: the saved sends again, and the new one gone")
    {
        rig.lock (false);
        CHECK (rig.submitAndTick ("live.drop").applied == 1);
        rig.tickOnce();

        CHECK (rig.pushedOn (track) == "0>0@1.000 1>1@1.000 0>4@0.501 1>5@0.501");
        CHECK (rig.published ("/godot/send/" + reverb + "/bus") == "<none>");
        CHECK_FALSE (rig.document.ids().isTaken (reverb));
    }

    SUBCASE ("unlocked, a send into a bus a live one already goes to is refused")
    {
        rig.lock (false);
        CHECK (rig.submitAndTick ("send.create", { osc::Value::string (rig.mediaId),
                                                   osc::Value::string (rig.reverb) }).rejected == 1);
    }
}

TEST_CASE ("live: under the lock a plugin's parameter rides live, heard on the next tick, kept as one step or let go")
{
    /*  The FX page's third decision (author, 2026-09-26): a turn on an
        insert's parameter while the show is locked rides as an EQ turn does -
        heard, written to nothing, no step of the history, kept or dropped
        once unlocked. */
    LiveRig rig;
    const auto verb = rig.document.createPlugin ("Verb", "VST3-0badf00d-verb", "VST3", "", "");
    REQUIRE (verb.ok);
    const auto fx = rig.document.createFx (rig.mediaId, verb.id, "");
    REQUIRE (fx.ok);
    REQUIRE (rig.document.setAttribute ("/godot/fx/" + fx.id + "/values", "0:0.25").ok);

    const auto row = "/godot/fx/" + fx.id + "/values";
    const auto p0 = "/godot/fx/" + fx.id + "/p0";
    const auto p1 = "/godot/fx/" + fx.id + "/p1";

    const auto run = rig.play();
    const auto track = rig.runs.find (run)->track;
    REQUIRE (track >= 0);
    rig.tickOnce();

    /*  What the voice was last told a parameter is. */
    const auto heard = [&rig, track] (int parameter) -> float
    {
        for (auto push = rig.audio.fxValues.rbegin(); push != rig.audio.fxValues.rend(); ++push)
            if (push->track == track && push->slot == 0 && push->parameter == parameter)
                return push->value;

        return -2.0f;
    };

    rig.lock (true);
    const auto steps = rig.steps();

    CHECK (rig.set (p0, osc::Value::float64 (0.75)).applied == 1);
    CHECK (rig.set (p1, osc::Value::float64 (1.0)).applied == 1);
    rig.tickOnce();

    //  HEARD.
    CHECK (heard (0) == doctest::Approx (0.75f));
    CHECK (heard (1) == doctest::Approx (1.0f));

    //  WRITTEN TO NOTHING, and counted with what else rides.
    CHECK (rig.saved (row) == "0:0.25");
    CHECK (rig.steps() == steps);
    CHECK (rig.live.size() == 2u);
    CHECK (rig.published ("/godot/document/live") == "2");

    //  A bad value is refused as the show would refuse it.
    CHECK (rig.set (p0, osc::Value::float64 (1.5)).rejected == 1);
    CHECK (rig.set ("/godot/fx/FX0NOPE0/p0", osc::Value::float64 (0.5)).rejected == 1);

    SUBCASE ("turned back to what the show says, nothing rides there")
    {
        CHECK (rig.set (p0, osc::Value::float64 (0.25)).applied == 1);
        rig.tickOnce();

        CHECK (heard (0) == doctest::Approx (0.25f));
        CHECK (rig.live.size() == 1u);
    }

    SUBCASE ("Keep is refused under the lock; unlocked it is one undo step, and nothing moves in the sound")
    {
        CHECK (rig.submitAndTick ("live.keep").rejected == 1);

        rig.lock (false);
        const auto pushes = rig.audio.fxValues.size();
        CHECK (rig.submitAndTick ("live.keep").applied == 1);
        rig.tickOnce();

        CHECK (rig.saved (row) == "0:0.75 1:1");
        CHECK (rig.live.empty());
        CHECK (rig.steps() == steps + 1);
        CHECK (rig.audio.fxValues.size() == pushes);

        CHECK (rig.submitAndTick ("undo").applied == 1);
        CHECK (rig.saved (row) == "0:0.25");
    }

    SUBCASE ("Discard puts the voice back to the show's value, and one the show never set back to the preset")
    {
        rig.lock (false);
        CHECK (rig.submitAndTick ("live.drop").applied == 1);
        rig.tickOnce();

        CHECK (rig.live.empty());
        CHECK (heard (0) == doctest::Approx (0.25f));
        CHECK (heard (1) == doctest::Approx (-1.0f));
        CHECK (rig.saved (row) == "0:0.25");
        CHECK (rig.steps() == steps);
    }

    SUBCASE ("a write once unlocked is the show's, and lets go of what rode at that parameter only")
    {
        rig.lock (false);
        CHECK (rig.set (p0, osc::Value::float64 (0.5)).applied == 1);

        CHECK (rig.saved (row) == "0:0.5");
        CHECK (rig.live.size() == 1u);
        CHECK (rig.steps() == steps + 1);
    }
}

TEST_CASE ("live: eq.reset under the lock holds flat live; unlocked it lets go of what rode first")
{
    LiveRig rig;
    REQUIRE (rig.document.setAttribute (rig.eq ("eqB2Gain"), "6").ok);
    REQUIRE (rig.document.setAttribute (rig.eq ("eqHpf"), "true").ok);

    rig.lock (true);
    CHECK (rig.submitAndTick ("eq.reset", { osc::Value::string (rig.mediaId) }).applied == 1);

    //  Only what differs from flat rides: two rows, the show untouched.
    CHECK (rig.live.rowsOf (rig.mediaId) == "eqB2Gain eqHpf");
    CHECK (rig.saved (rig.eq ("eqB2Gain")) == "6");
    CHECK (rig.published (rig.eq ("eqB2Gain")) == "0");
    CHECK (rig.published (rig.eq ("eqHpf")) == "false");

    //  Unlocked, the reset lets go of what rode and writes the show.
    CHECK (rig.set (rig.eq ("eqB3Gain"), osc::Value::float64 (-4.0)).applied == 1);
    rig.lock (false);
    CHECK (rig.submitAndTick ("eq.reset", { osc::Value::string (rig.mediaId) }).applied == 1);

    CHECK (rig.live.empty());
    CHECK (rig.saved (rig.eq ("eqB2Gain")) == "0");
    CHECK (rig.saved (rig.eq ("eqB3Gain")) == "0");
}

//==============================================================================
namespace
{
    /*  A media cue with a level lane drawn on it (namespace draft §20): the
        fade rig, whose media cue is the one the lane is drawn over, with the
        sample clock put where a check wants the file to be. */
    struct LaneRig : FadeRig
    {
        void drawLane (const std::string& text)
        {
            REQUIRE (document.setAttribute ("/godot/cue/" + mediaId + "/levelLane", text).ok);
        }

        /*  Fires the media cue, lets the disk answer, and waits for the launch
            to be placed, the voice sounding - `startMedia`'s steps, with the
            arm's request kept for a check on what the voice was snapped to. */
        std::string launch()
        {
            fire (mediaId);
            REQUIRE_FALSE (audio.arms.empty());
            armed = audio.arms.back();

            audio.completeArms (engine);
            tickOnce();
            tickOnce();

            const auto id = runs.all().front().id;
            REQUIRE (runs.find (id)->launchedAtSample > 0);

            audio.playing.insert (runs.find (id)->track);
            tickOnce();

            return id;
        }

        /*  ONE TICK ON, WITH THE SOUND `seconds` PAST ITS LAUNCH ONE TICK FROM
            NOW - which is where the lane is read (decision DC), so a check at
            a second of the file is a check of that second's level. */
        void hearAt (const std::string& id, double seconds)
        {
            const auto launched = runs.find (id)->launchedAtSample;

            audio.samples = launched + static_cast<std::int64_t> (std::llround (seconds * 48000.0)) - 960;
            tickOnce();
        }

        cue::ArmRequest armed;
    };
}

TEST_CASE ("level lane: the voice follows the lane over the file, read a tick ahead, and starts at its first word")
{
    LaneRig rig;
    rig.drawLane ("0 -40 2 0");             // up from -40 over the first two seconds

    const auto id = rig.launch();

    /*  THE ARM IS SNAPPED WITH THE LANE IN IT (DC): a lane drawn up from
        silence starts its voice there, not at the cue's level sliding down. */
    CHECK (rig.armed.levelDb == doctest::Approx (-40.0));

    /*  And the lane is its own term: the run's own level is still the cue's,
        which is what a fade aimed at it would take over from. */
    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (0.0));
    CHECK (rig.runs.find (id)->level == doctest::Approx (-40.0));

    /*  A second into the file, halfway up - and the voice was told so. */
    rig.hearAt (id, 1.0);
    CHECK (rig.runs.find (id)->laneDb == doctest::Approx (-20.0));
    CHECK (rig.runs.find (id)->level == doctest::Approx (-20.0));
    REQUIRE_FALSE (rig.audio.levels.empty());
    CHECK (rig.audio.levels.back().second == doctest::Approx (-20.0));

    /*  Past the last point the lane holds what the last point says. */
    rig.hearAt (id, 3.5);
    CHECK (rig.runs.find (id)->level == doctest::Approx (0.0));
}

TEST_CASE ("level lane: it is read on the file's clock, from the start offset")
{
    /*  SECONDS OF THE FILE, not of the cue (§20.2): a cue that starts two
        seconds into its file starts two seconds into its lane. */
    LaneRig rig;
    rig.drawLane ("0 0 4 -40");
    rig.setCue (rig.mediaId, "startOffset", "2");

    const auto id = rig.launch();

    CHECK (rig.armed.levelDb == doctest::Approx (-20.0));

    rig.hearAt (id, 1.0);                   // the file's third second
    CHECK (rig.runs.find (id)->level == doctest::Approx (-30.0));
}

TEST_CASE ("level lane: an offset on the cue's level, which a fade and a hand move beside it")
{
    /*  DECISION CZ. The lane is one more term of the sum: a cue written at -6
        under a lane at -10 plays at -16, a fade aimed at it moves the cue's
        own level and not the lane's, and a hand on a strip adds to both. */
    LaneRig rig;
    rig.setCue (rig.mediaId, "level", "-6");
    rig.drawLane ("0 -10");

    const auto id = rig.launch();

    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (-6.0));
    CHECK (rig.runs.find (id)->level == doctest::Approx (-16.0));

    rig.fire (rig.fadeId);                  // to -20 over a second

    for (int i = 0; i < 60; ++i)
        rig.tickOnce();

    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (-20.0));
    CHECK (rig.runs.find (id)->level == doctest::Approx (-30.0));

    rig.runs.find (id)->trim = -3.0;
    rig.tickOnce();

    CHECK (rig.runs.find (id)->level == doctest::Approx (-33.0));
}

TEST_CASE ("level lane: an edit reaches a sounding cue on the next tick, and clearing it takes it away")
{
    /*  DECISION DB: drawing while a loop plays is how a lane gets shaped. */
    LaneRig rig;

    const auto id = rig.launch();
    CHECK (rig.runs.find (id)->level == doctest::Approx (0.0));

    rig.drawLane ("0 -12");
    rig.tickOnce();

    CHECK (rig.runs.find (id)->level == doctest::Approx (-12.0));
    REQUIRE_FALSE (rig.audio.levels.empty());
    CHECK (rig.audio.levels.back().second == doctest::Approx (-12.0));

    rig.drawLane ("");
    rig.tickOnce();

    CHECK (rig.runs.find (id)->level == doctest::Approx (0.0));
}

TEST_CASE ("level lane: a jump takes the lane to the second jumped to")
{
    /*  §3.10's "scrubs when the clip scrubs" (DA): `run.seek` re-arms the
        voice at a second of the file, and the lane is read from there. */
    LaneRig rig;
    rig.drawLane ("0 0 10 -20");

    const auto id = rig.launch();
    rig.audio.arms.clear();

    CHECK (rig.submitAndTick ("run.seek", { osc::Value::string (id),
                                            osc::Value::float64 (5.0) }).applied == 1);

    /*  Snapped at the lane's word for the fifth second. */
    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms.front().levelDb == doctest::Approx (-10.0));

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.runs.find (id)->launchedAtSample > 0);

    rig.hearAt (id, 1.0);                   // the file's sixth second
    CHECK (rig.runs.find (id)->level == doctest::Approx (-12.0));
}

TEST_CASE ("level lane: a looping slice hears the same stretch of the lane on every pass")
{
    /*  DA, and the reason it is the file's clock: a slice that loops plays
        the same seconds of the file again, and so the same stretch of the
        lane. Read before the launch at the slice's in-point, not at the
        file's start, which the lane says something different about. */
    LaneRig rig;

    const auto range = rig.document.createRange (rig.mediaId, 2.0, 4.0);
    REQUIRE (range.ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + range.id + "/loops", "0").ok);

    rig.drawLane ("0 0 2 -20 4 0");

    const auto id = rig.launch();

    CHECK (rig.armed.levelDb == doctest::Approx (-20.0));

    rig.hearAt (id, 1.0);                   // the first pass, the file's third second
    CHECK (rig.runs.find (id)->level == doctest::Approx (-10.0));

    rig.hearAt (id, 2.5);                   // the second pass, half a second in
    CHECK (rig.runs.find (id)->level == doctest::Approx (-15.0));

    rig.hearAt (id, 5.0);                   // the third pass, a second in
    CHECK (rig.runs.find (id)->level == doctest::Approx (-10.0));
}

TEST_CASE ("level lane: across a slice boundary it keeps the outgoing slice until the sound crosses")
{
    /*  §20.4's second difference from the playhead. The boundary is placed
        a little ahead and moves the slice's clock when it is placed; the
        level stays the outgoing slice's until the crossing itself. */
    LaneRig rig;
    rig.audio.slots = 2;

    REQUIRE (rig.document.createRange (rig.mediaId, 0.0, 1.0).ok);
    REQUIRE (rig.document.createRange (rig.mediaId, 5.0, 6.0).ok);

    rig.drawLane ("0 0 1 -20 5 -40 6 -40");

    const auto id = rig.launch();
    const auto endsAt = rig.runs.find (id)->launchedAtSample + 48000;

    /*  Two ticks before the end: inside the placement horizon, so the
        boundary is placed on this tick. */
    rig.audio.samples = endsAt - 1920;
    rig.tickOnce();

    REQUIRE_FALSE (rig.audio.stopsAt.empty());
    REQUIRE (rig.audio.stopsAt.back().second == endsAt);
    REQUIRE (rig.runs.find (id)->rangeStartedAtSample == endsAt);

    /*  Read a tick ahead, still 540 samples short of the crossing: the
        outgoing slice's last hundredth of a second, not the incoming one. */
    rig.audio.samples = endsAt - 1500;
    rig.tickOnce();

    CHECK (rig.runs.find (id)->laneDb == doctest::Approx (-20.0 * (48000.0 - 540.0) / 48000.0));

    /*  And past it, the incoming slice's own second. */
    rig.audio.samples = endsAt - 480;
    rig.tickOnce();

    CHECK (rig.runs.find (id)->laneDb == doctest::Approx (-40.0));
}

//==============================================================================
/*  A CUE'S SPEED, AS THE RUNNER KEEPS IT (namespace draft §22.4): read at the
    arm, placed on the voice at the launch and a horizon ahead of every change,
    and read back off the same breakpoints for the playhead, the lane and a
    slice's boundary. The fake player keeps every breakpoint it is given. At
    48 kHz a tick is 960 samples, and the fake's 128-sample blocks make the
    horizon two ticks: 1920. */
TEST_CASE ("speed: a cue at one places its launch and nothing more, and its playhead is the count it always was")
{
    LaneRig rig;
    const auto id = rig.launch();
    const auto launched = rig.runs.find (id)->launchedAtSample;

    REQUIRE (rig.audio.ratePoints.size() == 1u);
    CHECK (rig.audio.ratePoints[0].sample == launched);
    CHECK (rig.audio.ratePoints[0].speed == doctest::Approx (1.0));
    CHECK (rig.runs.find (id)->rateClock.isIdentityFrom (static_cast<double> (launched)));

    rig.audio.samples = launched + 72000;
    rig.tickOnce();

    CHECK (rig.runs.find (id)->position == doctest::Approx (1.5));
    CHECK (rig.runs.find (id)->rateNow == doctest::Approx (1.0));
    CHECK (rig.audio.ratePoints.size() == 1u);
}

TEST_CASE ("speed: a cue at a half plays its file at half from its launch")
{
    LaneRig rig;
    rig.setCue (rig.mediaId, "rate", "0.5");

    const auto id = rig.launch();
    const auto launched = rig.runs.find (id)->launchedAtSample;

    REQUIRE_FALSE (rig.audio.ratePoints.empty());
    CHECK (rig.audio.ratePoints.front().sample == launched);
    CHECK (rig.audio.ratePoints.front().speed == doctest::Approx (0.5));

    rig.audio.samples = launched + 96000;   // two seconds on
    rig.tickOnce();

    CHECK (rig.runs.find (id)->position == doctest::Approx (1.0));
    CHECK (rig.runs.find (id)->rateNow == doctest::Approx (0.5));
}

TEST_CASE ("speed: an edit reaches a sounding cue a horizon later over a tick, and an edit of anything else places nothing")
{
    /*  DECISION DW. The change is held at the speed the voice had until a
        horizon from now - so it lands after anything already placed - then a
        straight line over a tick. */
    LaneRig rig;
    const auto id = rig.launch();
    const auto launched = rig.runs.find (id)->launchedAtSample;

    rig.audio.samples = launched + 48000;
    rig.tickOnce();
    const auto placed = rig.audio.ratePoints.size();

    rig.setCue (rig.mediaId, "level", "-3");
    rig.tickOnce();
    CHECK (rig.audio.ratePoints.size() == placed);

    rig.setCue (rig.mediaId, "rate", "2");
    const auto now = rig.audio.samples;
    rig.tickOnce();

    REQUIRE (rig.audio.ratePoints.size() == placed + 2);
    const auto hold = rig.audio.ratePoints[placed];
    const auto target = rig.audio.ratePoints[placed + 1];

    CHECK (hold.sample == now + 1920);
    CHECK (hold.speed == doctest::Approx (1.0));
    CHECK (target.sample == now + 1920 + 960);
    CHECK (target.speed == doctest::Approx (2.0));
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (2.0));

    /*  The file: at one until the hold, a tick ramping from one to two, then
        a second at two. */
    rig.audio.samples = target.sample + 48000;
    rig.tickOnce();

    const auto expected = static_cast<double> (hold.sample - launched) / 48000.0
                            + 0.5 * (1.0 + 2.0) * 960.0 / 48000.0 + 2.0;

    CHECK (rig.runs.find (id)->position == doctest::Approx (expected));
    CHECK (rig.runs.find (id)->rateNow == doctest::Approx (2.0));

    /*  Nothing placed again while nothing changes. */
    rig.tickOnce();
    CHECK (rig.audio.ratePoints.size() == placed + 2);
}

TEST_CASE ("speed: the mode is the arm's, and a sounding cue keeps the one it was armed with")
{
    /*  DECISION DV: a mode rebuilds Tracktion's graph, so it changes at the
        next arm and never under a sounding cue. */
    LaneRig rig;
    rig.setCue (rig.mediaId, "rateMode", "timestretch");

    const auto id = rig.launch();

    CHECK (rig.armed.stretch);
    CHECK (rig.runs.find (id)->stretch);

    const auto placed = rig.audio.ratePoints.size();
    rig.audio.arms.clear();

    rig.setCue (rig.mediaId, "rateMode", "varispeed");
    rig.tickOnce();

    CHECK (rig.audio.arms.empty());
    CHECK (rig.runs.find (id)->stretch);
    CHECK (rig.audio.ratePoints.size() == placed);
}

TEST_CASE ("speed: a stretched cue is held to the stretcher's limit, a resampled one is not")
{
    for (const auto stretch : { true, false })
    {
        INFO ("mode " << std::string (stretch ? "timestretch" : "varispeed"));

        LaneRig rig;
        rig.audio.speedLimit = 18.75;
        rig.setCue (rig.mediaId, "rateMode", stretch ? "timestretch" : "varispeed");
        rig.setCue (rig.mediaId, "rate", "20");

        const auto id = rig.launch();

        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (stretch ? 18.75 : 20.0));
        REQUIRE_FALSE (rig.audio.ratePoints.empty());
        CHECK (rig.audio.ratePoints.front().speed == doctest::Approx (stretch ? 18.75 : 20.0));
    }
}

TEST_CASE ("speed: a jump keeps the run's speed and mode")
{
    LaneRig rig;
    rig.setCue (rig.mediaId, "rateMode", "timestretch");
    rig.setCue (rig.mediaId, "rate", "0.5");

    const auto id = rig.launch();
    rig.audio.arms.clear();

    CHECK (rig.submitAndTick ("run.seek", { osc::Value::string (id),
                                            osc::Value::float64 (2.0) }).applied == 1);

    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms.front().stretch);

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.runs.find (id)->launchedAtSample > 0);
    REQUIRE_FALSE (rig.audio.ratePoints.empty());
    CHECK (rig.audio.ratePoints.back().sample == rig.runs.find (id)->launchedAtSample);
    CHECK (rig.audio.ratePoints.back().speed == doctest::Approx (0.5));
}

TEST_CASE ("speed: a level lane follows the file at the speed it plays")
{
    /*  The lane is the recording's (§20.2, DA): at twice the speed, a second
        after the launch is the file's second second. */
    LaneRig rig;
    rig.setCue (rig.mediaId, "rate", "2");
    rig.drawLane ("0 0 4 -40");

    const auto id = rig.launch();

    rig.hearAt (id, 1.0);
    CHECK (rig.runs.find (id)->laneDb == doctest::Approx (-20.0));
}

TEST_CASE ("speed: a slice's boundary is where the file reaches its end, at the speed")
{
    /*  One second of file at one and a half is two thirds of a second: 32 000
        samples, placed where the lane's case places it at one. */
    LaneRig rig;
    rig.audio.slots = 2;

    REQUIRE (rig.document.createRange (rig.mediaId, 0.0, 1.0).ok);
    REQUIRE (rig.document.createRange (rig.mediaId, 5.0, 6.0).ok);
    rig.setCue (rig.mediaId, "rate", "1.5");

    const auto id = rig.launch();
    const auto endsAt = rig.runs.find (id)->launchedAtSample + 32000;

    rig.audio.samples = endsAt - 1920;
    rig.tickOnce();

    REQUIRE_FALSE (rig.audio.stopsAt.empty());
    CHECK (rig.audio.stopsAt.back().second == endsAt);
    CHECK (rig.runs.find (id)->rangeStartedAtSample == endsAt);
}

TEST_CASE ("speed: held at nought, a slice's pass never ends and an advance waits for the file to move")
{
    LaneRig rig;
    rig.audio.slots = 2;

    const auto range = rig.document.createRange (rig.mediaId, 0.0, 1.0);
    REQUIRE (range.ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + range.id + "/loops", "0").ok);
    REQUIRE (rig.document.createRange (rig.mediaId, 5.0, 6.0).ok);
    rig.setCue (rig.mediaId, "rate", "0");

    const auto id = rig.launch();
    const auto launched = rig.runs.find (id)->launchedAtSample;

    CHECK (rig.submitAndTick ("run.advance", { osc::Value::string (id) }).applied == 1);

    rig.audio.samples = launched + 480000;  // ten seconds on
    rig.tickOnce();

    CHECK (rig.audio.stopsAt.empty());
    CHECK (rig.runs.find (id)->position == doctest::Approx (0.0));
}

TEST_CASE ("speed: slowed and brought back to one, the playhead keeps the file's count")
{
    /*  The Runner's side of the fault found in S.3: once the slow stretch has
        been let go of, the clock still knows the run was not at one from its
        launch on. */
    LaneRig rig;
    rig.setCue (rig.mediaId, "rate", "0.5");

    const auto id = rig.launch();
    const auto launched = rig.runs.find (id)->launchedAtSample;

    rig.audio.samples = launched + 96000;   // a second of file in two
    rig.tickOnce();

    rig.setCue (rig.mediaId, "rate", "1");
    const auto now = rig.audio.samples;
    rig.tickOnce();

    //  Well past the ramp, and past whatever the clock let go of.
    rig.audio.samples = now + 480000;
    rig.tickOnce();
    rig.tickOnce();

    const auto hold = now + 1920;
    const auto expected = static_cast<double> (hold - launched) * 0.5 / 48000.0
                            + 0.5 * (0.5 + 1.0) * 960.0 / 48000.0
                            + static_cast<double> (now + 480000 - (hold + 960)) / 48000.0;

    CHECK (rig.runs.find (id)->position == doctest::Approx (expected));
}

//==============================================================================
/*  FADES ON THE SPEED (namespace draft §22.6): a fade cue's `rateOn` moves its
    target's speed to its `rate` over its duration, beside the level or
    instead of it. A speed job's key is `rate:` and the run, so it takes over a
    speed fade and leaves a level fade alone; a fade that moves both is two
    jobs under one run. Played with the clock moving a tick of samples a tick,
    as a sound card's does, so the run's clock lets go of its past. */
namespace
{
    /*  The rig's own fade - or another - turned into one that moves the
        speed: to `rate` over `seconds`, the level left alone unless asked. */
    void aimAtTheSpeed (LaneRig& rig, const std::string& fadeId, const char* rate,
                        const char* seconds, bool level = false)
    {
        rig.setCue (fadeId, "levelOn", level ? "true" : "false");
        rig.setCue (fadeId, "rateOn", "true");
        rig.setCue (fadeId, "rate", rate);
        rig.setCue (fadeId, "duration", seconds);
    }

    std::string newFade (LaneRig& rig, int at)
    {
        const auto id = rig.document.createCue (rig.listId, at, "fade", "Again").id;
        rig.setCue (id, "target", rig.mediaId);
        return id;
    }

    std::size_t speedJobs (const LaneRig& rig)
    {
        const auto& jobs = rig.runner.fades();
        return static_cast<std::size_t> (std::count_if (jobs.begin(), jobs.end(),
                                                        [] (const cue::FadeJob& job) { return job.movesRate; }));
    }

    void play (LaneRig& rig, int ticks)
    {
        for (int n = 0; n < ticks; ++n)
        {
            rig.audio.samples += 960;
            rig.tickOnce();
        }
    }

    /*  Plays until `ready` holds, a tick at a time, and answers whether it
        ever did. */
    template <typename Predicate>
    bool playUntil (LaneRig& rig, Predicate ready, int bound)
    {
        for (int n = 0; n < bound && ! ready(); ++n)
            play (rig, 1);

        return ready();
    }

    int reportsOf (LaneRig& rig, const char* command, const std::string& runId)
    {
        auto count = 0;

        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.command == command && ! record.args.empty()
                  && record.args.front().getString() == runId)
                ++count;

        return count;
    }
}

TEST_CASE ("speed fade: a fade that moves only the speed takes it along its curve and leaves the level where it is")
{
    LaneRig rig;
    const auto id = rig.launch();

    //  The rig's fade says -20 dB, and with its level switch off that is nobody's business.
    aimAtTheSpeed (rig, rig.fadeId, "0.5", "1");
    rig.fire (rig.fadeId);

    const auto fade = rig.runs.all().back().id;
    REQUIRE (rig.runs.find (fade)->cue == rig.fadeId);
    CHECK (rig.runs.find (fade)->state == cue::runState::playing);

    REQUIRE (rig.runner.fades().size() == 1u);
    const auto job = rig.runner.fades().front();
    CHECK (job.movesRate);
    CHECK (job.target == "rate:" + id);
    CHECK (job.heldRun() == id);
    CHECK (job.fromRate == doctest::Approx (1.0));
    CHECK (job.toRate == doctest::Approx (0.5));

    play (rig, 25);

    const auto halfway = rig.runs.find (id)->ownRate;
    INFO ("halfway through, at " << halfway);
    CHECK (halfway < 0.95);
    CHECK (halfway > 0.55);
    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (0.0));

    //  On the voice as it moves: the last breakpoint placed is where the fade has got to.
    REQUIRE_FALSE (rig.audio.ratePoints.empty());
    CHECK (rig.audio.ratePoints.back().speed == doctest::Approx (halfway));

    CHECK (playUntil (rig, [&] { return rig.runs.find (fade)->isFinished(); }, 40));
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (0.5));
    CHECK (rig.audio.ratePoints.back().speed == doctest::Approx (0.5));
    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (0.0));

    //  Arrived, it stops nothing it was not asked to, and reports once.
    CHECK (rig.runs.find (id)->state == cue::runState::playing);
    CHECK (rig.runner.fades().empty());
    CHECK (reportsOf (rig, "run.ended", fade) == 1);

    //  And the readout says what the voice plays at, once the horizon has passed.
    play (rig, 4);
    CHECK (rig.runs.find (id)->rateNow == doctest::Approx (0.5));
}

TEST_CASE ("speed fade: a speed fade takes over a speed fade from where it has got to, and leaves a level fade moving")
{
    /*  DECISION EA: takeover is per run AND per thing moved. */
    LaneRig rig;
    const auto id = rig.launch();

    //  The rig's own fade moves the level: to -20 over two seconds.
    rig.setCue (rig.fadeId, "duration", "2");
    rig.fire (rig.fadeId);
    const auto levelFade = rig.runs.all().back().id;

    const auto up = newFade (rig, 4);
    aimAtTheSpeed (rig, up, "2", "1");
    rig.fire (up);
    const auto upRun = rig.runs.all().back().id;

    CHECK (rig.runner.fades().size() == 2u);
    CHECK (speedJobs (rig) == 1u);

    play (rig, 20);
    const auto reached = rig.runs.find (id)->ownRate;
    REQUIRE (reached > 1.1);

    const auto down = newFade (rig, 5);
    aimAtTheSpeed (rig, down, "0.5", "1");
    rig.fire (down);

    //  One speed job, the new one, starting where the speed stood as it was fired.
    REQUIRE (rig.runner.fades().size() == 2u);
    REQUIRE (speedJobs (rig) == 1u);

    for (const auto& job : rig.runner.fades())
    {
        if (! job.movesRate)
            continue;

        CHECK (job.fromRate >= reached);
        CHECK (job.fromRate == doctest::Approx (rig.runs.find (id)->ownRate));
        CHECK (job.toRate == doctest::Approx (0.5));
    }

    //  The speed fade taken over is over; the level fade is not.
    play (rig, 1);
    CHECK (rig.runs.find (upRun)->isFinished());
    CHECK_FALSE (rig.runs.find (levelFade)->isFinished());

    play (rig, 55);
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (0.5));

    CHECK (playUntil (rig, [&] { return rig.runs.find (levelFade)->isFinished(); }, 60));
    CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (-20.0));
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (0.5));
}

TEST_CASE ("speed fade: one fade moving both is two jobs under one run, which ends once, when the second is done")
{
    LaneRig rig;
    const auto id = rig.launch();

    aimAtTheSpeed (rig, rig.fadeId, "2", "1", true);     // and the level, to -20
    rig.fire (rig.fadeId);
    const auto both = rig.runs.all().back().id;

    REQUIRE (rig.runner.fades().size() == 2u);
    CHECK (speedJobs (rig) == 1u);

    for (const auto& job : rig.runner.fades())
        CHECK (job.self == both);

    play (rig, 10);

    //  A level fade takes the level, and only the level: the first run goes on while its speed moves.
    const auto level = newFade (rig, 4);
    rig.setCue (level, "level", "0");
    rig.setCue (level, "duration", "3");
    rig.fire (level);
    const auto levelRun = rig.runs.all().back().id;

    play (rig, 2);
    CHECK_FALSE (rig.runs.find (both)->isFinished());
    CHECK (speedJobs (rig) == 1u);
    CHECK (reportsOf (rig, "run.ended", both) == 0);

    //  And it ends when the speed arrives, once, with the level fade still going.
    CHECK (playUntil (rig, [&] { return rig.runs.find (both)->isFinished(); }, 60));
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (2.0));
    CHECK_FALSE (rig.runs.find (levelRun)->isFinished());

    play (rig, 5);
    CHECK (reportsOf (rig, "run.ended", both) == 1);

    SUBCASE ("and left alone, the two arrive together and the run reports once")
    {
        LaneRig again;
        const auto sounding = again.launch();

        aimAtTheSpeed (again, again.fadeId, "0.5", "1", true);
        again.fire (again.fadeId);
        const auto run = again.runs.all().back().id;

        CHECK (playUntil (again, [&] { return again.runs.find (run)->isFinished(); }, 60));
        CHECK (again.runs.find (sounding)->ownRate == doctest::Approx (0.5));
        CHECK (again.runs.find (sounding)->ownLevel == doctest::Approx (-20.0));

        play (again, 5);
        CHECK (reportsOf (again, "run.ended", run) == 1);
    }
}

TEST_CASE ("speed fade: a stop at the end waits a horizon and a tick, until the speed has been heard")
{
    /*  DECISION ED. The fake's horizon is two ticks; the last ramp takes one
        more. A stop cut in with the fade's last tick would never let the
        voice play the end of its ramp. */
    SUBCASE ("a fade that moves only the speed carries the stop")
    {
        LaneRig rig;
        const auto id = rig.launch();
        const auto track = rig.runs.find (id)->track;

        aimAtTheSpeed (rig, rig.fadeId, "0", "1");
        rig.setCue (rig.fadeId, "stopWhenDone", "true");

        const auto firedAt = rig.tick;
        rig.fire (rig.fadeId);

        REQUIRE (rig.runner.fades().size() == 1u);
        CHECK (rig.runner.fades().front().stopWhenDone);
        CHECK (rig.runner.fades().front().stopsAtTick == firedAt + 50 + 2 + 1);
        CHECK (rig.runs.find (id)->state == cue::runState::stopping);

        play (rig, 51);
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (0.0));
        CHECK (rig.audio.playing.count (track) == 1u);

        play (rig, 3);
        CHECK (rig.audio.playing.count (track) == 0u);
    }

    SUBCASE ("a fade that moves both: the level's job carries it, as late")
    {
        LaneRig rig;
        rig.launch();

        aimAtTheSpeed (rig, rig.fadeId, "0", "1", true);
        rig.setCue (rig.fadeId, "stopWhenDone", "true");

        const auto firedAt = rig.tick;
        rig.fire (rig.fadeId);

        REQUIRE (rig.runner.fades().size() == 2u);

        for (const auto& job : rig.runner.fades())
        {
            INFO (std::string (job.movesRate ? "the speed's job" : "the level's job"));
            CHECK (job.stopWhenDone == ! job.movesRate);

            if (! job.movesRate)
                CHECK (job.stopsAtTick == firedAt + 50 + 2 + 1);
        }
    }

    SUBCASE ("a level fade alone stops when its level arrives, as it always did")
    {
        LaneRig rig;
        rig.launch();
        rig.setCue (rig.fadeId, "stopWhenDone", "true");

        const auto firedAt = rig.tick;
        rig.fire (rig.fadeId);

        REQUIRE (rig.runner.fades().size() == 1u);
        CHECK (rig.runner.fades().front().stopsAtTick == firedAt + 50);
    }
}

TEST_CASE ("speed fade: Esc fades the level and leaves the speed where it has got to")
{
    /*  DECISION EB. */
    SUBCASE ("a speed fade under way")
    {
        LaneRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

        const auto id = rig.launch();
        const auto track = rig.runs.find (id)->track;

        aimAtTheSpeed (rig, rig.fadeId, "2", "2");
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        play (rig, 25);

        rig.audio.samples += 960;
        REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);

        const auto held = rig.runs.find (id)->ownRate;
        INFO ("the speed at Esc: " << held);
        REQUIRE (held > 1.1);
        REQUIRE (held < 1.9);

        //  The panic fade's level job, and no speed job left.
        CHECK (speedJobs (rig) == 0u);
        CHECK (rig.runner.fades().size() == 1u);

        play (rig, 10);
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (held));
        CHECK (rig.runs.find (id)->state == cue::runState::stopping);
        CHECK (rig.runs.find (fade)->isFinished());

        CHECK (playUntil (rig, [&] { return rig.audio.playing.count (track) == 0u; }, 50));
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (held));
    }

    SUBCASE ("a speed fade's own stop, due later, goes with it and never hands the cue back")
    {
        LaneRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

        const auto id = rig.launch();
        const auto track = rig.runs.find (id)->track;

        aimAtTheSpeed (rig, rig.fadeId, "0.5", "10");
        rig.setCue (rig.fadeId, "stopWhenDone", "true");
        rig.fire (rig.fadeId);

        play (rig, 10);
        REQUIRE (rig.runs.find (id)->state == cue::runState::stopping);

        rig.audio.samples += 960;
        REQUIRE (rig.submitAndTick ("run.stopAll").applied == 1);
        CHECK (speedJobs (rig) == 0u);

        //  Stopping all the way down - never handed back to `playing` by the fade Esc ended.
        auto handedBack = false;

        for (int n = 0; n < 45; ++n)
        {
            play (rig, 1);
            handedBack = handedBack || rig.runs.find (id)->state == cue::runState::playing;
        }

        CHECK_FALSE (handedBack);
        CHECK (playUntil (rig, [&] { return rig.audio.playing.count (track) == 0u; }, 20));
    }
}

TEST_CASE ("speed fade: a double Esc drops its stop with every other action, and cuts at once")
{
    LaneRig rig;
    const auto id = rig.launch();
    const auto track = rig.runs.find (id)->track;

    aimAtTheSpeed (rig, rig.fadeId, "0.5", "10");
    rig.setCue (rig.fadeId, "stopWhenDone", "true");
    rig.fire (rig.fadeId);

    play (rig, 10);
    REQUIRE (rig.audio.playing.count (track) == 1u);

    rig.audio.samples += 960;
    REQUIRE (rig.submitAndTick ("run.killAll").applied == 1);
    play (rig, 1);

    CHECK (speedJobs (rig) == 0u);
    CHECK (rig.audio.playing.count (track) == 0u);
    CHECK (playUntil (rig, [&] { return rig.runs.find (id)->isFinished(); }, 5));
}

TEST_CASE ("speed fade: aimed at nothing with a speed it moves nothing, and a fade with neither switch ends at once")
{
    /*  DECISION EC, and the fade that moves nothing at all. */
    SUBCASE ("a group")
    {
        TrimRig rig;
        rig.setStandby (rig.groupId);

        rig.submitAndTick ("go");
        rig.audio.completeArms (rig.engine);
        rig.settle (10);

        REQUIRE_FALSE (rig.runOf (rig.member).empty());

        const auto fade = rig.fadeAt (rig.groupId, "-6", "0.2");
        rig.document.setAttribute ("/godot/cue/" + fade + "/levelOn", "false");
        rig.document.setAttribute ("/godot/cue/" + fade + "/rateOn", "true");
        rig.document.setAttribute ("/godot/cue/" + fade + "/rate", "2");

        rig.submitAndTick ("cue.fire", { osc::Value::string (fade) });

        CHECK (std::none_of (rig.runner.fades().begin(), rig.runner.fades().end(),
                             [] (const cue::FadeJob& job) { return job.movesRate; }));

        rig.settle (3);
        CHECK (rig.runs.find (rig.runOf (fade))->isFinished());
        CHECK (rig.runs.find (rig.runOf (rig.member))->ownRate == doctest::Approx (1.0));
        CHECK (rig.ownLevelOf (rig.groupId) == doctest::Approx (0.0));
    }

    SUBCASE ("a DCA, whose fade reads no target")
    {
        LaneRig rig;
        const auto id = rig.launch();

        const auto dca = rig.document.createDca ("Music");
        REQUIRE (dca.ok);

        //  The rig's fade still names the cue: a fade that names a DCA does not read it.
        aimAtTheSpeed (rig, rig.fadeId, "2", "1");
        rig.setCue (rig.fadeId, "dca", dca.id);
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        CHECK (speedJobs (rig) == 0u);

        play (rig, 2);
        CHECK (rig.runs.find (fade)->isFinished());
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (1.0));
    }

    SUBCASE ("neither switch")
    {
        LaneRig rig;
        const auto id = rig.launch();

        rig.setCue (rig.fadeId, "levelOn", "false");
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        play (rig, 2);
        CHECK (rig.runs.find (fade)->isFinished());
        CHECK (rig.runs.find (fade)->state != cue::runState::failed);
        CHECK (rig.runs.find (id)->ownLevel == doctest::Approx (0.0));
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (1.0));
    }

    SUBCASE ("a cue that is not running")
    {
        LaneRig rig;

        aimAtTheSpeed (rig, rig.fadeId, "2", "1");
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        rig.tickOnce();
        rig.tickOnce();
        CHECK (rig.runs.find (fade)->isFinished());
        CHECK (rig.runs.find (fade)->state != cue::runState::failed);
        CHECK (rig.runner.fades().empty());
    }
}

TEST_CASE ("speed fade: an edit of the cue's speed under the fade waits, and lands as the fade lets go")
{
    /*  DECISION DW's exception: a fade holds the run, and the document waits
        - then is read again the tick the fade lets go, not at whatever edit
        comes next. */
    SUBCASE ("edited under it")
    {
        LaneRig rig;
        const auto id = rig.launch();

        aimAtTheSpeed (rig, rig.fadeId, "2", "1");
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        play (rig, 10);
        rig.setCue (rig.mediaId, "rate", "0.5");
        play (rig, 2);

        const auto moving = rig.runs.find (id)->ownRate;
        INFO ("under the fade, at " << moving);
        CHECK (moving > 1.1);

        CHECK (playUntil (rig, [&] { return rig.runs.find (fade)->isFinished(); }, 60));
        play (rig, 1);
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (0.5));
    }

    SUBCASE ("not edited: the speed stays where the fade took it, whatever else is edited")
    {
        LaneRig rig;
        const auto id = rig.launch();

        aimAtTheSpeed (rig, rig.fadeId, "2", "1");
        rig.fire (rig.fadeId);
        const auto fade = rig.runs.all().back().id;

        CHECK (playUntil (rig, [&] { return rig.runs.find (fade)->isFinished(); }, 60));
        play (rig, 1);
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (2.0));

        rig.setCue (rig.mediaId, "level", "-3");
        play (rig, 2);
        CHECK (rig.runs.find (id)->ownRate == doctest::Approx (2.0));
    }
}

TEST_CASE ("speed fade: a stretched cue's speed fade is held to the stretcher's limit")
{
    LaneRig rig;
    rig.audio.speedLimit = 18.75;
    rig.setCue (rig.mediaId, "rateMode", "timestretch");

    const auto id = rig.launch();

    aimAtTheSpeed (rig, rig.fadeId, "20", "0.2");
    rig.fire (rig.fadeId);

    REQUIRE (speedJobs (rig) == 1u);
    CHECK (rig.runner.fades().front().toRate == doctest::Approx (18.75));

    play (rig, 15);
    CHECK (rig.runs.find (id)->ownRate == doctest::Approx (18.75));
}

//==============================================================================
/*  DOH! - TAKING BACK THE LAST GO (PRD §3.32, namespace draft §24; D1,
    2026-10-01).

    A GO pressed before its moment, taken back: the pointer, the list's
    `finished` flag, the GO debounce and the history go back to where that GO
    found them; what it started comes down - over the panic fade when it was
    heard, at once when it was not - with no footer and nothing killed; what
    the horizon made after it is given back rather than taken down; and an act
    the GO ended is brought back to life, its footer taken down with the GO. The
    next GO starts the cue over (a resume is D2's). Every case here failed on
    the code before this stage, where `go.doh` was an unknown command. */
namespace
{
    /*  Doh!, pressed as a client presses it: one named command. */
    Engine::TickResult doh (Rig& rig)
    {
        return rig.submitAndTick ("go.doh");
    }

    bool refusedFor (const Rig& rig, const char* reason)
    {
        return rig.engine.lastError().find (reason) != std::string::npos;
    }

    std::string finishedOf (const Rig& rig)
    {
        return rig.document.getAttribute (cue::finishedAddressOf (rig.listId)).value_or ("");
    }

    std::size_t runsFor (const Rig& rig, const std::string& cueId)
    {
        return static_cast<std::size_t> (std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                                         [&cueId] (const cue::Run& run) { return run.cue == cueId; }));
    }

    /*  The newest run of a cue, whatever its state, or empty. */
    std::string newestRunOf (const Rig& rig, const std::string& cueId)
    {
        std::string out;

        for (const auto& run : rig.runs.all())
            if (run.cue == cueId)
                out = run.id;

        return out;
    }

    /*  A media cue made to sound the way the audio side would: the disk
        answers, the launch is placed - `run.started` - and the track plays. */
    std::string hear (Rig& rig, const std::string& cueId)
    {
        rig.audio.completeArms (rig.engine);

        REQUIRE (rig.tickUntil ([&]
        {
            const auto* live = rig.runs.liveRunOf (cueId);
            return live != nullptr && live->launchedAtSample > 0;
        }, 20));

        const auto id = rig.runs.liveRunOf (cueId)->id;
        rig.audio.playing.insert (rig.runs.find (id)->track);
        rig.tickOnce();
        return id;
    }

    bool endsWith (const std::string& text, const std::string& tail)
    {
        return text.size() >= tail.size() && text.compare (text.size() - tail.size(), tail.size(), tail) == 0;
    }

    const cue::GroupJob* liveJobOf (const Rig& rig, const std::string& runId)
    {
        for (const auto& job : rig.runner.groups())
            if (job.run == runId && ! job.retired)
                return &job;

        return nullptr;
    }

    /*  THE SESSION AGAIN, FROM ITS LOG ALONE (namespace draft §24.1): a fresh
        engine with no audio side reads the show and the records and must reach
        the same runs - the same state, the same GO, the same cause, taken back
        or not. A handler that decided from something a replay lacks shows
        here, and nowhere else. */
    void replaysTheSame (Rig& session)
    {
        const auto show = doc::CanonicalXml::write (session.document);
        const auto original = LogFile::parse (session.engine.log().contents());
        REQUIRE (original.errors.empty());

        Rig fresh;
        fresh.runner.setPlayer (nullptr);
        REQUIRE (doc::CanonicalXml::read (show, fresh.document).ok);

        const auto result = replay (fresh.engine, original);

        for (const auto& mismatch : result.mismatches)
            MESSAGE (mismatch);

        CHECK (result.ok);

        for (const auto& run : session.runs.all())
        {
            INFO ("run " << run.id << " of " << run.cue);
            const auto* again = fresh.runs.find (run.id);
            REQUIRE (again != nullptr);
            CHECK (again->state == run.state);
            CHECK (again->takenBack == run.takenBack);
            CHECK (again->goSerial == run.goSerial);
            CHECK (again->causedBy == run.causedBy);
        }
    }
}

TEST_CASE ("go.doh: takes back the last GO - the pointer, finished, the debounce, and a step that says Doh!")
{
    HistoryRig rig;
    rig.park (rig.memoId);

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.standby() == rig.second);

    const auto taken = rig.runner.listState().stepsTaken();

    /*  Half a second between GOs from here on: the GO taken back must not make
        the corrected one a bounce. */
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0.5").ok);

    CHECK (doh (rig).rejected == 0);
    CHECK (rig.standby() == rig.memoId);
    CHECK (finishedOf (rig) == "false");

    //  The GO's step is gone, and a step says the Doh! took its place.
    CHECK (rig.spelled().find (":" + rig.memoId + ":g") == std::string::npos);
    CHECK (endsWith (rig.spelled(), ":" + rig.memoId + ":d"));
    CHECK (rig.runner.listState().stepsTaken() == taken + 1);

    //  The corrected GO, well inside half a second of the one taken back.
    CHECK (rig.submitAndTick ("go").rejected == 0);
    CHECK_FALSE (refusedFor (rig, "too-soon"));
    CHECK (rig.standby() == rig.second);
    CHECK (runsFor (rig, rig.memoId) == 2u);
}

TEST_CASE ("go.doh: the end of a list is taken back, finished and all")
{
    Rig rig;
    rig.setStandby (rig.memoId);                         // the last cue there is

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.standby().empty());
    REQUIRE (finishedOf (rig) == "true");

    CHECK (doh (rig).rejected == 0);
    CHECK (rig.standby() == rig.memoId);
    CHECK (finishedOf (rig) == "false");

    CHECK (rig.submitAndTick ("go").rejected == 0);
    CHECK (runsFor (rig, rig.memoId) == 2u);
}

TEST_CASE ("go.doh: past the window it is too-late and nothing moves; nought is off; ten by default")
{
    {
        const doc::ShowDocument fresh;
        CHECK (fresh.getAttribute ("/godot/list/dohWindow") == std::optional<std::string> ("10"));
    }

    Rig rig;

    SUBCASE ("one second: forty-nine ticks after the GO is inside it, fifty is not")
    {
        REQUIRE (rig.document.setAttribute ("/godot/list/dohWindow", "1").ok);
        rig.setStandby (rig.mediaId);

        const auto goTick = rig.tick;
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        SUBCASE ("forty-nine")
        {
            while (rig.tick < goTick + 49)
                rig.tickOnce();

            CHECK (doh (rig).rejected == 0);
            CHECK (rig.standby() == rig.mediaId);
        }

        SUBCASE ("fifty")
        {
            while (rig.tick < goTick + 50)
                rig.tickOnce();

            CHECK (doh (rig).rejected == 1);
            CHECK (refusedFor (rig, "too-late"));
            CHECK (rig.standby() == rig.memoId);
        }
    }

    SUBCASE ("nought is off: refused at once")
    {
        REQUIRE (rig.document.setAttribute ("/godot/list/dohWindow", "0").ok);
        rig.setStandby (rig.mediaId);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (doh (rig).rejected == 1);
        CHECK (refusedFor (rig, "too-late"));
        CHECK (rig.standby() == rig.memoId);
    }
}

TEST_CASE ("go.doh: nothing to take back is refused, and so is a second Doh")
{
    Rig rig;

    CHECK (doh (rig).rejected == 1);
    CHECK (refusedFor (rig, "nothing-to-take-back"));

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.standby() == rig.mediaId);

    /*  ONE PRESS, ONE GO: there is no GO before it to reach for, and in this
        stage no resume for a second press to forget. */
    CHECK (doh (rig).rejected == 1);
    CHECK (refusedFor (rig, "nothing-to-take-back"));
    CHECK (rig.standby() == rig.mediaId);
}

TEST_CASE ("go.doh: a refused GO and an empty GO are not the last GO")
{
    SUBCASE ("a bounced GO")
    {
        Rig rig;
        REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0.5").ok);
        rig.setStandby (rig.mediaId);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.submitAndTick ("go").rejected == 1);       // too soon

        CHECK (doh (rig).rejected == 0);
        CHECK (rig.standby() == rig.mediaId);
    }

    SUBCASE ("a GO on an empty standby, at the end of a list")
    {
        Rig rig;
        rig.setStandby (rig.memoId);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby().empty());
        REQUIRE (rig.submitAndTick ("go").rejected == 0);       // applied, and fires nothing

        CHECK (doh (rig).rejected == 0);
        CHECK (rig.standby() == rig.memoId);
    }
}

TEST_CASE ("go.doh: the lock does not stop it; it does stop the window being edited")
{
    Rig rig;
    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (rig.document.isLocked());

    CHECK (doh (rig).rejected == 0);
    CHECK (rig.standby() == rig.mediaId);

    const auto edit = rig.submitAndTick ("node.set", { osc::Value::string ("/godot/list/dohWindow"),
                                                       osc::Value::float64 (5.0) });
    CHECK (edit.rejected == 1);
    CHECK (refusedFor (rig, "locked"));
}

TEST_CASE ("go.doh: after a fire by name or a trigger on the GO's list, Doh! is refused with its own sentence")
{
    SUBCASE ("by name, and by a trigger, on the list; by name on another")
    {
        HistoryRig rig;
        rig.park (rig.mediaId);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        const auto fired = rig.runOf (rig.mediaId);
        REQUIRE (! fired.empty());

        auto refused = true;

        SUBCASE ("a cue of the list fired by name")
        {
            REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (rig.third) }).rejected == 0);
        }

        SUBCASE ("a trigger on a cue of the list")
        {
            const auto trigger = rig.document.createTrigger (rig.third, "osc");
            REQUIRE (trigger.ok);
            REQUIRE (rig.document.setAttribute ("/godot/trigger/" + trigger.id + "/address", "/desk/go").ok);
            REQUIRE (rig.submitAndTick ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 0);
        }

        SUBCASE ("a cue of another list fired by name")
        {
            const auto other = rig.document.createList ("Foyer").id;
            const auto doors = rig.document.createCue (other, 0, "memo", "Doors").id;
            REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (doors) }).rejected == 0);
            refused = false;
        }

        if (refused)
        {
            CHECK (doh (rig).rejected == 1);
            CHECK (refusedFor (rig, "trigger-after-go"));
            CHECK (rig.standby() == rig.memoId);                    // still past the GO
            CHECK_FALSE (rig.runs.find (fired)->takenBack);
        }
        else
        {
            CHECK (doh (rig).rejected == 0);
            CHECK (rig.standby() == rig.mediaId);
        }
    }

    SUBCASE ("the GO's own start cue firing its target is the GO's, not a trigger")
    {
        Rig rig;
        const auto starter = rig.document.createCue (rig.listId, 2, "start", "Start the memo").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.memoId).ok);

        rig.setStandby (starter);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.memoId).empty(); }, 10));

        CHECK (doh (rig).rejected == 0);
        CHECK (rig.standby() == starter);
    }

    SUBCASE ("a start cue an EARLIER GO set going fires on the list after this GO: refused")
    {
        Rig rig;
        const auto scene = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
        const auto starter = rig.document.createCue (scene, 0, "start", "Start the memo").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.memoId).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/preWait", "0.4").ok);
        const auto later = rig.document.createCue (rig.listId, 3, "memo", "Later").id;

        rig.setStandby (scene);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the scene, an earlier GO
        REQUIRE (rig.standby() == later);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the GO a Doh! would take back

        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.memoId).empty(); }, 60));

        CHECK (doh (rig).rejected == 1);
        CHECK (refusedFor (rig, "trigger-after-go"));
    }
}

TEST_CASE ("go.doh: a sound it started comes down over the panic fade, no footer, no kill")
{
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto id = hear (rig, rig.mediaId);
    const auto track = rig.runs.find (id)->track;

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (id)->startedAtTick >= 0);

    REQUIRE (doh (rig).rejected == 0);

    const auto* run = rig.runs.find (id);
    CHECK (run->state == cue::runState::stopping);
    CHECK (run->takenBack);
    CHECK_FALSE (run->skipFooter);
    CHECK_FALSE (run->killed);

    std::vector<cue::FadeJob> on;

    for (const auto& job : rig.runner.fades())
        if (job.target == id)
            on.push_back (job);

    REQUIRE (on.size() == 1u);
    CHECK_FALSE (on.front().reportsSelf);
    CHECK (on.front().ticksTotal == 50);
    CHECK (on.front().stopWhenDone);

    for (int n = 0; n < 55; ++n)
        rig.tickOnce();

    CHECK (std::find (rig.audio.stopped.begin(), rig.audio.stopped.end(), track) != rig.audio.stopped.end());
    CHECK (std::find (rig.audio.kills.begin(), rig.audio.kills.end(), track) == rig.audio.kills.end());
    CHECK (rig.runs.find (id)->isFinished());
}

TEST_CASE ("go.doh: a cue nobody heard yet is taken back whole, and the next GO waits its full pre-wait")
{
    Rig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.mediaId + "/preWait", "2").ok);

    rig.setStandby (rig.mediaId);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.runs.liveRunOf (rig.mediaId) != nullptr);
    REQUIRE (rig.runs.liveRunOf (rig.mediaId)->state == cue::runState::waiting);

    for (int n = 0; n < 50; ++n)
        rig.tickOnce();

    REQUIRE (doh (rig).rejected == 0);

    /*  The pointer is back on the cue, and the standby has it armed again,
        made ready and nobody's GO. */
    REQUIRE (rig.tickUntil ([&]
    {
        const auto* live = rig.runs.liveRunOf (rig.mediaId);
        return live != nullptr && live->state == cue::runState::armed
                 && ! live->prepare.empty() && ! live->takenBack;
    }, 10));

    const auto goTick = rig.tick;
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto* live = rig.runs.liveRunOf (rig.mediaId);
    REQUIRE (live != nullptr);
    CHECK (live->state == cue::runState::waiting);
    CHECK (live->dueTick == goTick + 100);
}

TEST_CASE ("go.doh: a member taken back from a running manual group leaves the group running and no footer")
{
    ManualRig rig;

    /*  A third member that sounds: the scene's own memos would have ended. */
    const auto bell = rig.document.createCue (rig.groupId, 3, "media", "Bell").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bell + "/file", "bell.wav").ok);
    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0.4").ok);

    rig.setStandby (rig.first);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.standby() == bell);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the bell: the GO taken back

    const auto act = rig.runOf (rig.groupId);
    const auto old = hear (rig, bell);
    REQUIRE (rig.runs.find (old)->parent == act);

    REQUIRE (doh (rig).rejected == 0);

    CHECK_FALSE (rig.runs.find (act)->isFinished());
    CHECK (rig.runs.find (old)->takenBack);
    CHECK (rig.standby() == bell);

    const auto* job = liveJobOf (rig, act);
    REQUIRE (job != nullptr);
    CHECK (std::find (job->phaseRuns.begin(), job->phaseRuns.end(), old) == job->phaseRuns.end());
    CHECK (job->hasTaken (old));

    //  Its voice fades out and goes; only then is the bell armed again, under the act.
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (old)->isFinished(); }, 60));
    CHECK (rig.runOf (closing).empty());

    REQUIRE (rig.tickUntil ([&]
    {
        const auto* live = rig.runs.liveRunOf (bell);
        return live != nullptr && live->id != old && live->state == cue::runState::armed;
    }, 20));

    const auto again = rig.runs.liveRunOf (bell)->id;
    CHECK (rig.runs.find (again)->parent == act);

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (again)->launchedAtSample > 0; }, 20));
    CHECK (rig.audio.arms.empty());                          // no new arm: the one made ahead launched

    //  And the footer runs once, after it.
    rig.audio.playing.insert (rig.runs.find (again)->track);
    rig.tickOnce();
    rig.audio.playing.erase (rig.runs.find (again)->track);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (act)->isFinished(); }, 60));
    CHECK (runsFor (rig, closing) == 1u);
}

TEST_CASE ("go.doh: only the last GO's runs - even two GOs in one tick")
{
    Rig rig;
    const auto rain = rig.document.createCue (rig.listId, 1, "media", "Rain").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);

    rig.setStandby (rig.mediaId);

    SUBCASE ("on two ticks")
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (doh (rig).rejected == 0);
    }

    SUBCASE ("in one tick")
    {
        REQUIRE (rig.engine.submit ("cli", "go", {}));
        REQUIRE (rig.engine.submit ("cli", "go", {}));
        REQUIRE (rig.engine.submit ("cli", "go.doh", {}));
        REQUIRE (rig.tickOnce().rejected == 0);
    }

    CHECK (rig.standby() == rain);

    const auto thunder = newestRunOf (rig, rig.mediaId);
    const auto rainRun = newestRunOf (rig, rain);
    REQUIRE (! thunder.empty());
    REQUIRE (! rainRun.empty());

    CHECK_FALSE (rig.runs.find (thunder)->takenBack);
    CHECK_FALSE (rig.runs.find (thunder)->isFinished());
    CHECK (rig.runs.find (thunder)->state != cue::runState::stopping);
    CHECK (rig.runs.find (rainRun)->takenBack);
}

TEST_CASE ("go.doh: a start cue's target goes with its GO, and a fire queued behind the Doh is dropped")
{
    Rig rig;
    const auto starter = rig.document.createCue (rig.listId, 2, "start", "Start the thunder").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.mediaId).ok);

    //  Parked through the command, so the replay below is given the pointer too.
    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (starter) }).rejected == 0);
    rig.tickOnce();                                         // and the horizon sees it, as a park does

    SUBCASE ("the GO and the Doh in one tick: the target's fire next tick makes nothing")
    {
        REQUIRE (rig.engine.submit ("cli", "go", {}));
        REQUIRE (rig.engine.submit ("cli", "go.doh", {}));
        REQUIRE (rig.tickOnce().rejected == 0);
        CHECK (rig.standby() == starter);

        rig.tickOnce();
        rig.tickOnce();
        CHECK (rig.runOf (rig.mediaId).empty());

        /*  THE FIRE SAYS WHICH GO CAUSED IT, so a replay - which runs no hook -
            reads the cause from the record and drops it the same way. */
        const auto parsed = LogFile::parse (rig.engine.log().contents());
        const auto fire = std::find_if (parsed.records.begin(), parsed.records.end(),
                                        [] (const auto& record) { return record.command == "cue.fire"; });
        REQUIRE (fire != parsed.records.end());
        REQUIRE (fire->args.size() == 3u);
        CHECK (fire->args[0].getString() == rig.mediaId);
    }

    SUBCASE ("a Doh three ticks later takes the target's run back with its GO")
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        rig.tickOnce();
        rig.tickOnce();
        rig.tickOnce();

        const auto target = rig.runOf (rig.mediaId);
        REQUIRE (! target.empty());
        CHECK (rig.runs.find (target)->goSerial == 1u);

        REQUIRE (doh (rig).rejected == 0);
        CHECK (rig.runs.find (target)->takenBack);
    }

    /*  AND A REPLAY READS THE CAUSE OFF THE RECORD: the target's runs carry the
        same GO, and a fire whose GO was taken back makes nothing there too. */
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    replaysTheSame (rig);
}

TEST_CASE ("go.doh: run.started stamps when it started; a run taken back stays stopping")
{
    Engine engine;
    cue::RunTable runs;
    cue::registerRunCommands (engine.commands(), runs);

    runs.create ("RN000001", "CQ000001", "media");
    REQUIRE (engine.submit ("engine", "run.started", { osc::Value::string ("RN000001") }));
    engine.processTick (7);

    CHECK (runs.find ("RN000001")->startedAtTick == 7);
    CHECK (runs.find ("RN000001")->state == cue::runState::playing);

    runs.create ("RN000002", "CQ000002", "media");
    runs.find ("RN000002")->takenBack = true;
    runs.find ("RN000002")->state = cue::runState::stopping;

    REQUIRE (engine.submit ("engine", "run.started", { osc::Value::string ("RN000002") }));
    engine.processTick (9);

    CHECK (runs.find ("RN000002")->state == cue::runState::stopping);
    CHECK (runs.find ("RN000002")->startedAtTick == 9);
}

TEST_CASE ("go.doh: decision N passes over a taken-back run - a GO inside the Doh fade starts the cue again")
{
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto old = hear (rig, rig.mediaId);

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (old)->takenBack);

    rig.tickOnce();
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    CHECK (runsFor (rig, rig.mediaId) == 2u);

    const auto fresh = newestRunOf (rig, rig.mediaId);
    REQUIRE (fresh != old);
    CHECK_FALSE (rig.runs.find (fresh)->takenBack);
    CHECK (rig.runs.find (fresh)->launchRequested);
    CHECK (rig.runs.find (old)->state == cue::runState::stopping);
}

TEST_CASE ("go.doh: a child spawned into a taken-back group is done")
{
    GroupRig rig;
    rig.setCue (rig.first, "preWait", "10");                 // hold the group in its members

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto groupRun = rig.runOf (rig.groupId);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    /*  A spawn its job decided before the Doh, drained after it. */
    REQUIRE (rig.engine.submit ("cli", "go.doh", {}));
    REQUIRE (rig.engine.submit ("engine", "run.spawn", { osc::Value::string (groupRun),
                                                         osc::Value::string (rig.second) }));
    const auto spawnTick = rig.tick;
    rig.tickOnce();

    const auto spawned = rig.runOf (rig.second);
    REQUIRE (! spawned.empty());
    CHECK (rig.runs.find (spawned)->state == cue::runState::done);
    CHECK (rig.runs.find (spawned)->endedAtTick == spawnTick);
    CHECK (rig.runs.find (spawned)->track < 0);
    CHECK (rig.runs.find (groupRun)->takenBack);
}

TEST_CASE ("go.doh: an Esc after the GO - the Doh moves the pointer back and leaves the runs to Esc")
{
    GroupRig rig;
    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
    rig.setCue (rig.first, "preWait", "10");

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto groupRun = rig.runOf (rig.groupId);

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    REQUIRE (doh (rig).rejected == 0);

    CHECK_FALSE (rig.runs.find (groupRun)->takenBack);
    CHECK (rig.standby() == rig.groupId);

    REQUIRE (rig.runToCompletion (groupRun) < 400);
    CHECK_FALSE (rig.runOf (closing).empty());               // Esc's footer ran
}

TEST_CASE ("go.doh: every run the GO creates or adopts carries its serial, and a later GO's do not")
{
    SUBCASE ("a prepared block, the member armed under it, and what its job spawns")
    {
        PrepareRig rig;
        rig.setStandby (rig.sound);
        REQUIRE (rig.tickUntil ([&] { return rig.prepared (rig.groupId) != nullptr
                                              && ! rig.runOf (rig.sound).empty(); }));

        const auto block = rig.prepared (rig.groupId)->id;
        const auto armed = rig.runOf (rig.sound);

        //  The horizon's work is nobody's GO, and says when it was made.
        CHECK (rig.runs.find (block)->goSerial == 0u);
        CHECK (rig.runs.find (block)->preparedAfterGo == 0);
        CHECK (rig.runs.find (armed)->goSerial == 0u);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.runs.find (block)->goSerial == 1u);
        CHECK (rig.runs.find (armed)->goSerial == 1u);

        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.opening).empty(); }));
        CHECK (rig.runs.find (rig.runOf (rig.opening))->goSerial == 1u);
    }

    SUBCASE ("the standby's arm, launched; and the next GO's run")
    {
        Rig rig;
        rig.setStandby (rig.mediaId);

        const auto armed = rig.runOf (rig.mediaId);
        REQUIRE (! armed.empty());
        CHECK (rig.runs.find (armed)->goSerial == 0u);
        CHECK (rig.runs.find (armed)->preparedAfterGo == -1);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.runs.find (armed)->goSerial == 1u);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.runs.find (rig.runOf (rig.memoId))->goSerial == 2u);
    }

    SUBCASE ("a member the horizon armed under a running act, asked for by the next GO")
    {
        ManualRig rig;
        const auto one = rig.document.createCue (rig.groupId, 0, "media", "One").id;
        const auto two = rig.document.createCue (rig.groupId, 1, "media", "Two").id;

        for (const auto& id : { one, two })
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/file", "thunder.wav").ok);

        rig.setStandby (one);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == two);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (two).empty(); }));

        const auto armedTwo = rig.runOf (two);
        CHECK (rig.runs.find (armedTwo)->goSerial == 0u);
        CHECK (rig.runs.find (armedTwo)->preparedAfterGo == 1);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.runs.find (armedTwo)->goSerial == 2u);
    }

    /*  THE DESIGN'S J2 SUBCASE, written with J2 (2026-10-01, namespace draft
        §23.9): until then that GO entered a fresh run beside the block. */
    SUBCASE ("a scene the horizon made ready under a running act, adopted whole by the GO on its row")
    {
        ManualRig rig;
        const auto scene = rig.document.createCue (rig.groupId, 1, "group", "Storm").id;
        rig.setCue (scene, "mode", "timeline");
        const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
        rig.setCue (rain, "file", "thunder.wav");

        rig.setStandby (rig.first);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == scene);
        REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr
                                              && ! rig.runOf (rain).empty(); }));

        const auto block = rig.runs.preparedRunOf (scene)->id;
        const auto armed = rig.runOf (rain);
        CHECK (rig.runs.find (block)->goSerial == 0u);
        CHECK (rig.runs.find (block)->preparedAfterGo == 1);
        CHECK (rig.runs.find (armed)->goSerial == 0u);

        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        CHECK (rig.runs.find (block)->goSerial == 2u);
        CHECK (rig.runs.find (armed)->goSerial == 2u);
        CHECK (runsFor (rig, scene) == 1u);                        // the same run, none beside it
    }
}

TEST_CASE ("go.doh: history - a Doh! step is not a firing, to the solver or to the live recorder")
{
    HistoryRig rig;
    rig.park (rig.memoId);

    REQUIRE (rig.submitAndTick ("record.start").rejected == 0);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (doh (rig).rejected == 0);

    /*  Without the solver's guard the `d` step reads as the cue's firing and
        the answer is "history", read against the Doh's tick. */
    const auto plan = cue::solveHistory (rig.document, nullptr, nullptr,
                                         { rig.listId, rig.memoId, 0.0 },
                                         rig.runner.listState().historyOf (rig.listId));
    CHECK (plan.how == "order");

    REQUIRE (rig.submitAndTick ("record.stop").rejected == 0);

    auto targets = 0;

    for (const auto& list : rig.document.root().getChildWithName (juce::Identifier ("Lists")))
        for (const auto& take : list)
            for (const auto& start : take)
                if (start.getType().toString() == "Start"
                     && rig.document.getAttribute ("/godot/cue/" + start[juce::Identifier ("id")].toString().toStdString()
                                                   + "/target").value_or ("") == rig.memoId)
                    ++targets;

    CHECK (targets == 0);
}

TEST_CASE ("go.doh: a session with a Doh replays record for record, with no audio")
{
    HistoryRig session;
    REQUIRE (session.document.setAttribute ("/godot/audio/panicFade", "1").ok);
    session.park (session.mediaId);

    REQUIRE (session.submitAndTick ("go").rejected == 0);
    hear (session, session.mediaId);

    for (int n = 0; n < 50; ++n)
        session.tickOnce();

    REQUIRE (doh (session).rejected == 0);

    //  Fired by name inside the Doh fade: a fresh run beside the one fading out.
    REQUIRE (session.submitAndTick ("cue.fire", { osc::Value::string (session.mediaId) }).rejected == 0);
    session.audio.completeArms (session.engine);

    for (int n = 0; n < 60; ++n)
        session.tickOnce();

    REQUIRE (session.submitAndTick ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        session.tickOnce();

    const auto show = doc::CanonicalXml::write (session.document);
    const auto original = LogFile::parse (session.engine.log().contents());
    REQUIRE (original.errors.empty());

    HistoryRig fresh;
    fresh.runner.setPlayer (nullptr);

    doc::ReadResult read = doc::CanonicalXml::read (show, fresh.document);
    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);
    CHECK (fresh.spelled (session.listId) == session.spelled());

    for (const auto& run : session.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = fresh.runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->state == run.state);
        CHECK (again->takenBack == run.takenBack);
    }
}

TEST_CASE ("go.doh: a stop cue fired by name on another list leaves the GO's sound to that stop, and the session replays")
{
    HistoryRig session;
    const auto other = session.document.createList ("Effects").id;
    const auto stopper = session.document.createCue (other, 0, "transport", "Out").id;
    REQUIRE (session.document.setAttribute ("/godot/cue/" + stopper + "/target", session.mediaId).ok);

    session.park (session.mediaId);
    REQUIRE (session.submitAndTick ("go").rejected == 0);
    const auto id = hear (session, session.mediaId);

    REQUIRE (session.submitAndTick ("cue.fire", { osc::Value::string (stopper) }).rejected == 0);
    CHECK (session.runs.find (id)->stopAsked);

    REQUIRE (doh (session).rejected == 0);
    CHECK_FALSE (session.runs.find (id)->takenBack);         // that stop's, footer and all
    CHECK (session.standby() == session.mediaId);

    REQUIRE (session.submitAndTick ("cue.fire", { osc::Value::string (session.mediaId) }).rejected == 0);

    for (int n = 0; n < 10; ++n)
        session.tickOnce();

    const auto show = doc::CanonicalXml::write (session.document);
    const auto original = LogFile::parse (session.engine.log().contents());
    REQUIRE (original.errors.empty());

    HistoryRig fresh;
    fresh.runner.setPlayer (nullptr);

    doc::ReadResult read = doc::CanonicalXml::read (show, fresh.document);
    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);
    REQUIRE (fresh.runs.find (id) != nullptr);
    CHECK_FALSE (fresh.runs.find (id)->takenBack);
}

TEST_CASE ("go.doh: a second press inside the GO debounce is refused too-soon and changes nothing")
{
    Rig rig;
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0.5").ok);

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto old = hear (rig, rig.mediaId);

    REQUIRE (doh (rig).rejected == 0);
    const auto dohTick = rig.tick - 1;

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    SUBCASE ("pressed again: a bounce, a held key, a second hand")
    {
        CHECK (doh (rig).rejected == 1);
        CHECK (refusedFor (rig, "too-soon"));
        CHECK (rig.standby() == rig.mediaId);
        CHECK (rig.runs.find (old)->takenBack);

        while (rig.tick < dohTick + 30)
            rig.tickOnce();

        CHECK (doh (rig).rejected == 1);
        CHECK (refusedFor (rig, "nothing-to-take-back"));
    }

    SUBCASE ("a GO in between is not a bounce: the next Doh takes that GO back")
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.standby() == rig.memoId);

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (doh (rig).rejected == 0);
        CHECK (rig.standby() == rig.mediaId);
    }
}

TEST_CASE ("go.doh: a root another hand is already stopping is left to that stop, footer and all")
{
    GroupRig rig;
    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;
    rig.setCue (rig.first, "preWait", "10");

    rig.setStandby (rig.groupId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (rig.first).empty(); }));

    const auto groupRun = rig.runOf (rig.groupId);

    SUBCASE ("the running pane's stop")
    {
        REQUIRE (rig.submitAndTick ("run.stop", { osc::Value::string (groupRun) }).rejected == 0);
    }

    SUBCASE ("a stop cue on another list, fired by name")
    {
        const auto other = rig.document.createList ("Effects").id;
        const auto stopper = rig.document.createCue (other, 0, "transport", "Out").id;
        rig.setCue (stopper, "target", rig.groupId);
        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (stopper) }).rejected == 0);
    }

    REQUIRE (doh (rig).rejected == 0);
    CHECK_FALSE (rig.runs.find (groupRun)->takenBack);
    CHECK (rig.standby() == rig.groupId);

    REQUIRE (rig.runToCompletion (groupRun) < 400);
    CHECK_FALSE (rig.runOf (closing).empty());
}

namespace
{
    /*  A timeline scene of two memos on the history rig's list, at nought and
        three seconds - something a seek can re-seat. */
    std::string timelineScene (HistoryRig& rig, int index)
    {
        const auto scene = rig.document.createCue (rig.listId, index, "group", "Scene").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);

        rig.document.createCue (scene, 0, "memo", "One");
        const auto later = rig.document.createCue (scene, 1, "memo", "Two").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + later + "/preWait", "3").ok);
        return scene;
    }

    bool stepped (const HistoryRig& rig, const std::string& cueId, char origin)
    {
        for (const auto& step : rig.runner.listState().historyOf (rig.listId))
            if (step.cue == cueId && step.origin == origin)
                return true;

        return false;
    }
}

TEST_CASE ("go.doh: after a seek on the GO's scene, the GO's step is still the one erased")
{
    HistoryRig rig;
    const auto scene = timelineScene (rig, 4);

    rig.park (scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto sceneRun = rig.runOf (scene);
    REQUIRE (! sceneRun.empty());

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (sceneRun),
                                              osc::Value::float64 (2.0) }).rejected == 0);
    REQUIRE (doh (rig).rejected == 0);

    CHECK_FALSE (stepped (rig, scene, 'g'));
    CHECK (stepped (rig, scene, 'd'));

    const auto plan = cue::solveHistory (rig.document, nullptr, nullptr, { rig.listId, scene, 0.0 },
                                         rig.runner.listState().historyOf (rig.listId));
    CHECK (plan.how == "order");
}

TEST_CASE ("go.doh: the step a start cue's target wrote under the GO goes with it, even after a seek")
{
    SUBCASE ("a media target")
    {
        HistoryRig rig;
        const auto starter = rig.document.createCue (rig.listId, 4, "start", "Start the thunder").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.mediaId).ok);

        rig.park (starter);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return stepped (rig, rig.mediaId, 'f'); }, 10));

        REQUIRE (doh (rig).rejected == 0);
        CHECK_FALSE (stepped (rig, starter, 'g'));
        CHECK_FALSE (stepped (rig, rig.mediaId, 'f'));

        const auto plan = cue::solveHistory (rig.document, nullptr, nullptr, { rig.listId, rig.mediaId, 0.0 },
                                             rig.runner.listState().historyOf (rig.listId));
        CHECK (plan.how == "order");
    }

    SUBCASE ("a timeline scene target, sought before the Doh")
    {
        HistoryRig rig;
        const auto scene = timelineScene (rig, 4);
        const auto starter = rig.document.createCue (rig.listId, 5, "start", "Start the scene").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", scene).ok);

        rig.park (starter);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (scene).empty(); }, 10));

        REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (rig.runOf (scene)),
                                                  osc::Value::float64 (2.0) }).rejected == 0);
        REQUIRE (doh (rig).rejected == 0);

        CHECK_FALSE (stepped (rig, starter, 'g'));
        CHECK_FALSE (stepped (rig, scene, 'f'));
    }

    SUBCASE ("the same cue fired by another list's start cue before the GO keeps its step")
    {
        HistoryRig rig;
        const auto starter = rig.document.createCue (rig.listId, 4, "start", "Start the thunder").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", rig.mediaId).ok);

        const auto other = rig.document.createList ("Effects").id;
        const auto elsewhere = rig.document.createCue (other, 0, "start", "Start it from here").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + elsewhere + "/target", rig.mediaId).ok);

        REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (elsewhere) }).rejected == 0);
        REQUIRE (rig.tickUntil ([&] { return stepped (rig, rig.mediaId, 'f'); }, 10));

        rig.park (starter);
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        for (int n = 0; n < 3; ++n)
            rig.tickOnce();

        REQUIRE (doh (rig).rejected == 0);
        CHECK (stepped (rig, rig.mediaId, 'f'));
    }
}

TEST_CASE ("go.doh: the last cue of an act taken back - the act lives on, its header is not run again, its footer runs once")
{
    ManualRig rig;
    const auto opening = rig.document.createCue (rig.roleOf (rig.groupId, "header"), 0, "memo", "Pre-arm").id;
    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

    //  Parked through the command, so the replay at the end is given the pointer too.
    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (rig.first) }).rejected == 0);
    rig.tickOnce();                                         // and the horizon sees it, as a park does
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (rig.first);
                                  return ! id.empty() && rig.runs.find (id)->isFinished(); }));
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (rig.second);
                                  return ! id.empty() && rig.runs.find (id)->isFinished(); }));

    const auto act = rig.runOf (rig.groupId);
    const auto firstRun = rig.runOf (rig.first);
    const auto secondRun = rig.runOf (rig.second);

    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the act's last member
    REQUIRE (rig.standby() == rig.after);

    SUBCASE ("pressed once the act has ended, footer and all")
    {
        REQUIRE (rig.tickUntil ([&] { return rig.runs.find (act)->isFinished(); }));
        REQUIRE (runsFor (rig, closing) == 1u);

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        REQUIRE (doh (rig).rejected == 0);
    }

    SUBCASE ("pressed in the very tick whose hook decided the act's end")
    {
        REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (closing);
                                      return ! id.empty() && rig.runs.find (id)->isFinished(); }));
        REQUIRE_FALSE (rig.runs.find (act)->isFinished());

        const auto dohTick = rig.tick;
        REQUIRE (doh (rig).rejected == 0);                   // the act's `run.ended` drains after it

        /*  THE CASE'S OWN PREMISE, read off the log rather than assumed: the
            act's end, decided by its job in this very tick, was applied after
            the Doh - and ignored, the act being alive below. */
        const auto records = LogFile::parse (rig.engine.log().contents()).records;
        const auto named = [&act] (const LogRecord& record) { return ! record.args.empty()
                                                                       && record.args[0].isString()
                                                                       && record.args[0].getString() == act; };
        const auto dohAt = std::find_if (records.begin(), records.end(), [dohTick] (const LogRecord& record)
        {
            return record.kind == LogRecord::Kind::applied && record.command == "go.doh" && record.tick == dohTick;
        });

        REQUIRE (dohAt != records.end());
        CHECK (std::any_of (dohAt, records.end(), [&named, dohTick] (const LogRecord& record)
        {
            return record.kind == LogRecord::Kind::applied && record.command == "run.ended"
                     && record.tick == dohTick && named (record);
        }));
    }

    CHECK (rig.runs.find (act)->state == cue::runState::playing);
    CHECK (rig.standby() == rig.third);

    const auto* job = liveJobOf (rig, act);
    REQUIRE (job != nullptr);
    CHECK (job->phase == std::string (cue::groupPhase::members));
    CHECK (job->phaseRuns == std::vector<std::string> { firstRun, secondRun });

    const auto footerRun = rig.runOf (closing);
    REQUIRE (! footerRun.empty());
    CHECK (rig.runs.find (footerRun)->takenBack);
    CHECK (rig.runs.find (footerRun)->causedBy == 3u);

    //  The corrected GO joins the act: no header again, the footer once more.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (act)->isFinished(); }));

    CHECK (runsFor (rig, rig.groupId) == 1u);
    CHECK (runsFor (rig, opening) == 1u);
    CHECK (runsFor (rig, rig.third) == 2u);
    CHECK (rig.runs.find (newestRunOf (rig, rig.third))->parent == act);
    CHECK (runsFor (rig, closing) == 2u);
    CHECK_FALSE (rig.runs.find (newestRunOf (rig, closing))->takenBack);

    /*  AND A REPLAY BRINGS THE ACT BACK THE SAME WAY: the stale end of the
        Doh's own tick ignored by the tick the log keeps, not by a hook. */
    replaysTheSame (rig);
}

TEST_CASE ("go.doh: an act whose end ended the act above it brings both back, innermost first")
{
    Rig rig;
    const auto outer = rig.document.createCue (rig.listId, 2, "group", "Act").id;
    const auto inner = rig.document.createCue (outer, 0, "group", "Scene").id;
    const auto one = rig.document.createCue (inner, 0, "memo", "One").id;
    const auto two = rig.document.createCue (inner, 1, "memo", "Two").id;

    const auto innerRelease = rig.document.createCue (rig.document.createRole (inner, "footer").id, 0,
                                                      "memo", "Scene release").id;
    const auto outerRelease = rig.document.createCue (rig.document.createRole (outer, "footer").id, 0,
                                                      "memo", "Act release").id;

    rig.setStandby (one);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (one);
                                  return ! id.empty() && rig.runs.find (id)->isFinished(); }));
    REQUIRE (rig.standby() == two);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    const auto outerRun = rig.runOf (outer);
    const auto innerRun = rig.runOf (inner);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (outerRun)->isFinished(); }));

    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.runs.find (innerRun)->state == cue::runState::playing);
    CHECK (rig.runs.find (outerRun)->state == cue::runState::playing);
    CHECK (rig.standby() == two);

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (outerRun)->isFinished(); }));

    CHECK (runsFor (rig, outer) == 1u);
    CHECK (runsFor (rig, inner) == 1u);
    CHECK (runsFor (rig, innerRelease) == 2u);
    CHECK (runsFor (rig, outerRelease) == 2u);
    CHECK_FALSE (rig.runs.find (newestRunOf (rig, innerRelease))->takenBack);
    CHECK_FALSE (rig.runs.find (newestRunOf (rig, outerRelease))->takenBack);
}

TEST_CASE ("go.doh: an act that loops is not brought back, and its next round is left alone")
{
    ManualRig rig;
    rig.setCue (rig.groupId, "loops", "2");

    rig.setStandby (rig.first);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();
    }

    const auto act = rig.runOf (rig.groupId);
    const auto lastOfRoundOne = newestRunOf (rig, rig.third);
    REQUIRE_FALSE (rig.runs.find (act)->isFinished());

    const auto before = rig.runs.all().size();

    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.runs.find (lastOfRoundOne)->takenBack);

    for (std::size_t n = 0; n < before; ++n)
    {
        const auto& run = rig.runs.all()[n];

        if (run.id != lastOfRoundOne && run.goSerial != 3u)
            CHECK_FALSE (run.takenBack);
    }

    CHECK_FALSE (rig.runs.find (act)->takenBack);
    CHECK (rig.runs.find (act)->unadoptedAt < 0);
}

TEST_CASE ("go.doh: a seek withdraws a stop whatever the hook did, and the session replays")
{
    HistoryRig session;
    const auto other = session.document.createList ("Effects").id;
    const auto fadeOut = session.document.createCue (other, 0, "fade", "Fade it out").id;

    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>> {
             { "target", session.mediaId }, { "level", "-40" }, { "duration", "5" }, { "stopWhenDone", "true" } })
        REQUIRE (session.document.setAttribute ("/godot/cue/" + fadeOut + "/" + name, value).ok);

    session.park (session.mediaId);
    REQUIRE (session.submitAndTick ("go").rejected == 0);
    const auto id = hear (session, session.mediaId);

    REQUIRE (session.submitAndTick ("cue.fire", { osc::Value::string (fadeOut) }).rejected == 0);
    const auto fadeRun = newestRunOf (session, fadeOut);
    REQUIRE (session.runs.find (id)->stopAsked);

    //  The fade's own run killed: its hook hands the sound back to playing, with no record.
    REQUIRE (session.submitAndTick ("run.kill", { osc::Value::string (fadeRun) }).rejected == 0);
    session.tickOnce();
    session.tickOnce();

    REQUIRE (session.submitAndTick ("run.seek", { osc::Value::string (id),
                                                  osc::Value::float64 (1.0) }).rejected == 0);
    CHECK_FALSE (session.runs.find (id)->stopAsked);

    REQUIRE (doh (session).rejected == 0);
    CHECK (session.runs.find (id)->takenBack);

    REQUIRE (session.submitAndTick ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        session.tickOnce();

    const auto show = doc::CanonicalXml::write (session.document);
    const auto original = LogFile::parse (session.engine.log().contents());
    REQUIRE (original.errors.empty());

    HistoryRig fresh;
    fresh.runner.setPlayer (nullptr);

    doc::ReadResult read = doc::CanonicalXml::read (show, fresh.document);
    REQUIRE (read.ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);
    REQUIRE (fresh.runs.find (id) != nullptr);
    CHECK (fresh.runs.find (id)->takenBack);
}

TEST_CASE ("go.doh: a scene taken back is prepared again whole once its member's voice is free")
{
    PrepareRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0.4").ok);

    rig.setStandby (rig.sound);
    REQUIRE (rig.tickUntil ([&] { return rig.prepared (rig.groupId) != nullptr
                                          && ! rig.runOf (rig.sound).empty(); }));
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    const auto block = rig.prepared (rig.groupId)->id;

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto old = hear (rig, rig.sound);

    const auto prepares = [&rig]
    {
        const auto parsed = LogFile::parse (rig.engine.log().contents());
        return std::count_if (parsed.records.begin(), parsed.records.end(),
                              [] (const auto& record) { return record.command == "run.prepare"; });
    };

    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.runs.find (block)->takenBack);
    CHECK (rig.runs.find (old)->takenBack);

    //  A fresh block at D+1; the member waits for the old voice before it is armed.
    REQUIRE (rig.tickUntil ([&] { return rig.prepared (rig.groupId) != nullptr
                                          && rig.prepared (rig.groupId)->id != block; }, 10));
    const auto fresh = rig.prepared (rig.groupId)->id;
    const auto before = prepares();

    rig.tickOnce();
    CHECK_FALSE (rig.runs.hasChildFor (fresh, rig.sound));

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (old)->isFinished(); }, 60));
    REQUIRE (rig.tickUntil ([&] { return rig.runs.hasChildFor (fresh, rig.sound); }, 10));
    CHECK (prepares() > before);

    const auto again = newestRunOf (rig, rig.sound);
    CHECK (rig.runs.find (again)->parent == fresh);

    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (again)->launchedAtSample > 0; }, 30));
    CHECK (rig.audio.arms.empty());
}

TEST_CASE ("go.doh: what the horizon made after the GO on another list is not given back")
{
    Rig rig;
    const auto foyer = rig.document.createList ("Foyer").id;
    const auto scene = rig.document.createCue (foyer, 0, "group", "Doors").id;
    const auto rain = rig.document.createCue (scene, 0, "media", "Rain").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);

    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    REQUIRE (rig.document.setAttribute ("/godot/list/focus", foyer).ok);
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf (foyer), rain).ok);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.preparedRunOf (scene) != nullptr; }, 10));

    const auto block = rig.runs.preparedRunOf (scene)->id;
    CHECK (rig.runs.find (block)->preparedAfterGo == 1);

    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)) == std::optional<std::string> (rig.mediaId));

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK_FALSE (rig.runs.find (block)->isFinished());
    CHECK (rig.runs.find (block)->state == cue::runState::preparing);
}

//==============================================================================
/*  DOH!, AFTER ITS REVIEW (2026-10-01, D1). Each case pins a road the first
    build of D1 left open and failed on that build - or, where it says it is a
    net, pins a road that was already right and that no case reached, so that it
    stays right. */
TEST_CASE ("go.doh: an unheard scene whose next member was spawned and not yet launched is given back, not held for ever")
{
    /*  A SEQUENCE SPAWNS ITS NEXT MEMBER A TICK BEFORE IT LAUNCHES IT, and a
        Doh drained in between finds that member armed: here a fade, which the
        Doh leaves alone and whose launch it then refuses. Nothing would ever
        end it, and the scene waited on it before giving itself back - for
        ever, its pre-sends never put back, an act around it never ending. */
    Rig rig;
    const auto scene = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);

    rig.document.createCue (scene, 0, "memo", "Line");
    const auto dip = rig.document.createCue (scene, 1, "fade", "Dip").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/target", rig.mediaId).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/level", "-20").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/duration", "1").ok);
    rig.document.createCue (scene, 2, "memo", "Last");

    rig.setStandby (scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto sceneRun = rig.runOf (scene);

    //  The tick whose drain spawned the fade; its launch is the next tick's hook's.
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (dip).empty(); }, 20));
    const auto dipRun = rig.runOf (dip);
    REQUIRE (rig.runs.find (dipRun)->state == cue::runState::armed);

    //  The Doh drains before that launch.
    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.runs.find (dipRun)->takenBack);

    CHECK (rig.tickUntil ([&] { return rig.runs.find (sceneRun)->isFinished(); }, 60));
    CHECK (rig.runs.find (dipRun)->isFinished());
    CHECK (rig.runs.find (sceneRun)->warning == cue::runWarning::revoked);
}

TEST_CASE ("go.doh: an unheard scene whose fade-and-stop on a cue outside it then loses its job to a double Esc still goes")
{
    /*  THE DOH LEAVES A FADE THE GO FIRED TO RUN - its target is not the GO's -
        and the scene it sits in waited for it before giving itself back. A
        double Esc then dropped the fade's job, and its run sat `playing` with
        nothing to end it: the scene waited for ever, through the press that
        promises everything stops. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    rig.startMedia();                                         // the bed, fired by name, sounding

    const auto scene = rig.document.createCue (rig.listId, 4, "group", "Lights out").id;
    rig.setCue (scene, "mode", "timeline");
    const auto out = rig.document.createCue (scene, 0, "fade", "Bed out").id;
    rig.setCue (out, "target", rig.mediaId);
    rig.setCue (out, "level", "-40");
    rig.setCue (out, "duration", "3");
    rig.setCue (out, "stopWhenDone", "true");
    const auto hold = rig.document.createCue (scene, 1, "memo", "Hold").id;
    rig.setCue (hold, "preWait", "5");

    rig.setStandby (scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto sceneRun = rig.runOf (scene);

    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (out);
                                  return ! id.empty() && rig.runs.find (id)->launchRequestedAtTick > 0; }, 20));
    const auto outRun = rig.runOf (out);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (sceneRun)->takenBack);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.killAll").rejected == 0);

    CHECK (rig.tickUntil ([&] { return rig.runs.find (sceneRun)->isFinished(); }, 60));
    CHECK (rig.runs.find (outRun)->isFinished());
}

TEST_CASE ("go.doh: an unheard scene inside an older act, its fade-and-stop let go by Esc - the scene goes, and Esc runs the act's footer")
{
    /*  ESC LETS GO OF A FADE-AND-STOP WHOSE STOP LANDS FIRST: the job lands it
        held by nobody's run, and the run is ended with the rest - by its scene.
        A scene the Doh had taken back waited for that run instead, so it never
        went, and the act around it, which Esc was bringing down gracefully,
        waited on the scene: its footer never ran (§4.4). */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    rig.startMedia();

    const auto act = rig.document.createCue (rig.listId, 4, "group", "Act").id;
    const auto opening = rig.document.createCue (act, 0, "memo", "Opening").id;
    const auto scene = rig.document.createCue (act, 1, "group", "Lights out").id;
    rig.setCue (scene, "mode", "timeline");
    const auto out = rig.document.createCue (scene, 0, "fade", "Bed out").id;
    rig.setCue (out, "target", rig.mediaId);
    rig.setCue (out, "level", "-40");
    rig.setCue (out, "duration", "3");
    rig.setCue (out, "stopWhenDone", "true");
    const auto hold = rig.document.createCue (scene, 1, "memo", "Hold").id;
    rig.setCue (hold, "preWait", "10");
    const auto closing = rig.document.createCue (rig.document.createRole (act, "footer").id, 0, "memo", "Release").id;

    rig.setStandby (opening);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // into the act: an earlier GO
    const auto actRun = rig.runOf (act);
    REQUIRE (! actRun.empty());

    rig.setStandby (scene);
    const auto* ready = rig.runs.preparedRunOf (scene);    // made ready under the act
    REQUIRE (ready != nullptr);
    const auto block = ready->id;

    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the GO a Doh! takes back

    /*  THE GO'S OWN RUN OF THE SCENE IS THE BLOCK THE HORIZON MADE UNDER THE ACT
        (J2, namespace draft §23.9): adopted, stamped with the GO - until J2 the
        GO started the scene cold, beside that block. */
    const auto sceneRun = newestRunOf (rig, scene);
    REQUIRE (sceneRun == block);
    REQUIRE (runsFor (rig, scene) == 1u);
    REQUIRE (rig.runs.find (sceneRun)->goSerial == 2u);
    REQUIRE (rig.runs.find (sceneRun)->parent == actRun);

    REQUIRE (rig.tickUntil ([&] { const auto id = newestRunOf (rig, out);
                                  return ! id.empty() && rig.runs.find (id)->launchRequestedAtTick > 0; }, 20));
    const auto fadeFrom = rig.runs.find (newestRunOf (rig, out))->launchRequestedAtTick;

    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (sceneRun)->takenBack);
    REQUIRE_FALSE (rig.runs.find (actRun)->isFinished());

    //  Esc with the fade's stop due before the panic fade's: Esc lets go of the fade's job.
    while (rig.tick < fadeFrom + 120)
        rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);

    CHECK (rig.tickUntil ([&] { return rig.runs.find (actRun)->isFinished(); }, 150));
    CHECK (rig.runs.find (sceneRun)->isFinished());
    CHECK_FALSE (rig.runOf (closing).empty());
}

TEST_CASE ("go.doh: an unheard scene taken back in its footer, a scene in that footer with it - neither waits on the other")
{
    /*  A FOOTER IS LEFT TO FINISH by a graceful stop, and a scene inside one
        the Doh was giving back waited for the outer one to give it back whole:
        an unheard scene caught in its footer, the footer holding a timeline,
        waited on that timeline while the timeline waited on it - for ever,
        through Esc and a double Esc. A Doh runs no footer, so a scene nobody
        heard is given back from its footer as from anywhere else. */
    Rig rig;
    const auto scene = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    rig.document.createCue (scene, 0, "memo", "Line");

    const auto release = rig.document.createCue (rig.document.createRole (scene, "footer").id, 0,
                                                 "group", "Release").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/mode", "timeline").ok);
    rig.document.createCue (release, 0, "memo", "Lights");
    const auto later = rig.document.createCue (release, 1, "memo", "Later").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + later + "/preWait", "5").ok);

    rig.setStandby (scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto sceneRun = rig.runOf (scene);

    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (release);
                                  return ! id.empty() && rig.runs.find (id)->state == cue::runState::playing; }, 40));
    const auto releaseRun = rig.runOf (release);

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (sceneRun)->takenBack);
    REQUIRE (rig.runs.find (releaseRun)->takenBack);

    CHECK (rig.tickUntil ([&] { return rig.runs.find (sceneRun)->isFinished(); }, 60));
    CHECK (rig.runs.find (releaseRun)->isFinished());
}

TEST_CASE ("go.doh: a sound whose own scene's fade-and-stop lands first still comes down when the scene ends that fade's run")
{
    /*  THE STOP DUE SOONER WINS: a heard sound its scene's own fade-and-stop is
        already taking out is given no Doh fade. But the scene then stops its
        members - that fade's run among them - and a fade whose own run is
        stopped hands its target back to `playing`. The scene's job, finding a
        member playing again, stopped it at once: the sound was cut where the
        fade had got to, short of the stop it was fading to. A sound with no such
        job above it played on. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    const auto scene = rig.document.createCue (rig.listId, 4, "group", "Storm").id;
    rig.setCue (scene, "mode", "timeline");
    const auto bell = rig.document.createCue (scene, 0, "media", "Bell").id;
    rig.setCue (bell, "file", "bell.wav");
    const auto close = rig.document.createCue (scene, 1, "fade", "Bell out").id;
    rig.setCue (close, "target", bell);
    rig.setCue (close, "level", "-40");
    rig.setCue (close, "duration", "2");
    rig.setCue (close, "stopWhenDone", "true");
    rig.setCue (close, "preWait", "1");

    rig.setStandby (scene);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto sceneRun = rig.runOf (scene);
    const auto bellRun = hear (rig, bell);

    REQUIRE (rig.tickUntil ([&] { const auto id = rig.runOf (close);
                                  return ! id.empty() && rig.runs.find (id)->launchRequestedAtTick > 0; }, 80));
    const auto fadeFrom = rig.runs.find (rig.runOf (close))->launchRequestedAtTick;

    //  A quarter of the fade left: its stop lands before a Doh fade would.
    while (rig.tick < fadeFrom + 75)
        rig.tickOnce();

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (bellRun)->takenBack);

    for (int n = 0; n < 60; ++n)
    {
        rig.tickOnce();
        CHECK (rig.runs.find (bellRun)->state != cue::runState::playing);
    }

    //  Down where its own fade-and-stop puts it, at that fade's stop - not cut short of it.
    CHECK (rig.runs.find (bellRun)->isFinished());
    CHECK (rig.runs.find (bellRun)->endedAtTick >= fadeFrom + 100);
    CHECK (rig.runs.find (sceneRun)->isFinished());
}

TEST_CASE ("esc: a cue sought during its fade-and-stop still comes down")
{
    /*  ESC'S OWN FADE TAKES OVER WHATEVER A SOUNDING VOICE CARRIES (§23): a
        seek puts a fading cue back to `playing` with its fade-and-stop still
        holding it. Doh!'s first build of the per-voice fade Esc shares let
        that sooner stop stand, still held by the stop cue's own run - which Esc
        then stops, handing the voice back to `playing`: the cue played on after
        Esc. Esc's fade is Esc's again. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);
    rig.setCue (rig.fadeId, "duration", "2");
    rig.setCue (rig.fadeId, "stopWhenDone", "true");

    const auto id = rig.startMedia();
    REQUIRE (rig.fire (rig.fadeId).rejected == 0);
    const auto fadeFrom = rig.runs.find (newestRunOf (rig, rig.fadeId))->launchRequestedAtTick;

    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (id), osc::Value::float64 (1.0) }).rejected == 0);
    REQUIRE (rig.runs.find (id)->state == cue::runState::playing);

    //  The fade's stop is due before Esc's would be.
    while (rig.tick < fadeFrom + 60)
        rig.tickOnce();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);

    for (int n = 0; n < 80; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (id)->isFinished());
}

TEST_CASE ("go.doh: an act whose last member stops the act is brought back with no stop on its account - the corrected GO's stop is its own")
{
    /*  THE REVIVE GIVES THE ACT BACK ITS STOP ACCOUNT, as every handler that
        gives a run back does (§24, HB). It did not: the act kept the early GO's
        ask, so the corrected GO's own stop was only "asked again", the footer it
        set off was nobody's - not the corrected GO's to take back, not held
        back for a device left to its operator - and a Doh of the corrected GO
        found the act "stopped by another hand" and left it ended. */
    Rig rig;
    const auto act = rig.document.createCue (rig.listId, 2, "group", "Act").id;
    const auto one = rig.document.createCue (act, 0, "memo", "One").id;
    rig.document.createCue (act, 1, "memo", "Two");
    const auto out = rig.document.createCue (act, 2, "transport", "End the act").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + out + "/target", act).ok);
    const auto closing = rig.document.createCue (rig.document.createRole (act, "footer").id, 0, "memo", "Release").id;
    rig.document.createCue (rig.listId, 3, "memo", "After");

    rig.setStandby (one);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();
    }

    const auto actRun = rig.runOf (act);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (actRun)->isFinished(); }, 40));
    REQUIRE (runsFor (rig, closing) == 1u);

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.runs.find (actRun)->state == cue::runState::playing);
    CHECK_FALSE (rig.runs.find (actRun)->stopAsked);
    CHECK (rig.standby() == out);

    //  The corrected GO: its own stop on the act, and the footer it sets off.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (actRun)->isFinished(); }, 40));
    REQUIRE (runsFor (rig, closing) == 2u);
    CHECK (rig.runs.find (newestRunOf (rig, closing))->causedBy == 4u);

    //  So a Doh of the corrected GO brings the act back again.
    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.runs.find (actRun)->state == cue::runState::playing);
}

TEST_CASE ("go.doh: after an Esc the Doh's own step puts no persistent bed back")
{
    /*  A DOH NEVER UNDOES AN ESC (PRD §3.32): after one, only the pointer and
        the values on devices that take back go back. The Doh's `d` step opened
        the persistent section's pass all the same, and a bed Esc had brought
        down came back at the press, before any GO. */
    PersistentRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

    rig.step();
    rig.settle();
    rig.audio.completeArms (rig.engine);
    rig.settle();
    REQUIRE (rig.liveBed() != nullptr);

    rig.step (rig.mediaId);                                 // the GO a Doh! takes back
    rig.settle();

    REQUIRE (rig.submitAndTick ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.liveBed() == nullptr; }, 60));

    const auto asserts = [&rig]
    {
        const auto parsed = LogFile::parse (rig.engine.log().contents());
        return std::count_if (parsed.records.begin(), parsed.records.end(),
                              [] (const LogRecord& record) { return record.command == "run.assert"; });
    };

    const auto before = asserts();

    REQUIRE (doh (rig).rejected == 0);
    CHECK (rig.standby() == rig.mediaId);
    rig.settle();

    CHECK (asserts() == before);
    CHECK (rig.liveBed() == nullptr);

    //  The next GO is a step like any other: the section asserts the bed again.
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    rig.settle();
    CHECK (asserts() > before);
}

TEST_CASE ("go.doh: a corrected GO in the Doh's own drain plays the act's member it fires")
{
    /*  BORN DONE ONLY FROM A RECORD OF THE DOH'S OWN TICK (§24, GZ): a spawn the
        act's job decided before the Doh, on the state the Doh undid. The first
        build applied the rule to every spawn under an act brought back in that
        tick - so a GO drained with the Doh, from a script or a second hand, had
        its member born done: the act took its last member for played and ran
        its footer, and the corrected GO played nothing. */
    ManualRig rig;
    const auto closing = rig.document.createCue (rig.roleOf (rig.groupId, "footer"), 0, "memo", "Release").id;

    rig.setStandby (rig.first);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.submitAndTick ("go").rejected == 0);

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();
    }

    const auto act = rig.runOf (rig.groupId);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (act)->isFinished(); }, 40));
    const auto taken = newestRunOf (rig, rig.third);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    //  The Doh and the corrected GO, in one drain.
    REQUIRE (rig.engine.submit ("cli", "go.doh", {}));
    REQUIRE (rig.engine.submit ("cli", "go", {}));
    REQUIRE (rig.tickOnce().rejected == 0);

    const auto again = newestRunOf (rig, rig.third);
    REQUIRE (again != taken);
    CHECK (rig.runs.find (again)->parent == act);

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (act)->isFinished(); }, 40));
    CHECK (rig.runs.find (again)->launchRequestedAtTick > 0);
    CHECK (runsFor (rig, closing) == 2u);
}

TEST_CASE ("go.doh: a heard cue is armed again once its old voice has gone, and the corrected GO launches that arm")
{
    /*  THE TOP-LEVEL ROAD OF THE STANDBY'S WAIT FOR THE OLD VOICE (§24.2): no
        second voice while the old one fades, an arm the moment it has gone, and
        the corrected GO an ordinary armed launch - not a cold one, which the
        disk would make late. A net: the road was right, and no case reached it. */
    FadeRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "1").ok);

    rig.setStandby (rig.mediaId);
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    const auto old = hear (rig, rig.mediaId);

    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.standby() == rig.mediaId);

    const auto othersLive = [&rig, &old]
    {
        return std::count_if (rig.runs.all().begin(), rig.runs.all().end(), [&rig, &old] (const cue::Run& one)
        {
            return one.cue == rig.mediaId && one.id != old && ! one.isFinished();
        });
    };

    for (int n = 0; n < 10; ++n)
    {
        rig.tickOnce();
        CHECK (othersLive() == 0);
    }

    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (old)->isFinished(); }, 60));
    REQUIRE (rig.tickUntil ([&]
    {
        const auto* live = rig.runs.liveRunOf (rig.mediaId);
        return live != nullptr && live->id != old && live->state == cue::runState::armed
                 && ! live->prepare.empty();
    }, 20));

    const auto again = rig.runs.liveRunOf (rig.mediaId)->id;
    rig.audio.completeArms (rig.engine);
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&] { return rig.runs.find (again)->launchedAtSample > 0; }, 20));
    CHECK (rig.audio.arms.empty());
}

TEST_CASE ("go.doh: the horizon's arm of the act's next member, made after the GO, is revoked by the Doh itself")
{
    /*  PASS H'S OTHER HALF (§24.2, GY): an arm the horizon made after the GO is
        not the GO's, and nothing of it was heard or sent - it is revoked on the
        spot, its voice let go, never taken back. A net: the road was right, and
        no case reached it. */
    ManualRig rig;
    const auto bell = rig.document.createCue (rig.groupId, 1, "media", "Bell").id;
    rig.setCue (bell, "file", "bell.wav");

    rig.setStandby (rig.first);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // into the act, which is this GO's
    REQUIRE (rig.standby() == bell);

    const auto act = rig.runOf (rig.groupId);
    REQUIRE (rig.tickUntil ([&] { return ! rig.runOf (bell).empty(); }, 20));
    const auto arm = rig.runOf (bell);

    CHECK (rig.runs.find (arm)->parent == act);
    CHECK (rig.runs.find (arm)->goSerial == 0u);
    CHECK (rig.runs.find (arm)->preparedAfterGo == 1);
    CHECK_FALSE (rig.runs.find (arm)->prepare.empty());

    REQUIRE (doh (rig).rejected == 0);

    const auto* revoked = rig.runs.find (arm);
    CHECK (revoked->state == cue::runState::done);
    CHECK (revoked->warning == cue::runWarning::revoked);
    CHECK_FALSE (revoked->takenBack);
    CHECK (rig.runs.find (act)->takenBack);
}

TEST_CASE ("go.doh: a jump read from the history never plans the cue a Doh took back")
{
    /*  THE SOLVER'S PAST, NOT ONLY ITS CLOCK (§24.4): a `d` step names the cue
        whose GO was taken back, and read as a firing it would plan that cue as
        sounding from the moment of the Doh. The cases above pin the aimed
        cue's own clock; here the aim is a later cue, and the past is walked. A
        net: the guard was right, and no case reached it. */
    HistoryRig rig;
    const auto boom = rig.document.createCue (rig.listId, 3, "media", "Boom").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + boom + "/file", "boom.wav").ok);

    const auto pointAt = [&rig] (const std::string& cueId)
    {
        REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (cueId) }).rejected == 0);
    };

    pointAt (rig.second);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // Two
    REQUIRE (rig.standby() == boom);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // Boom, too early
    REQUIRE (doh (rig).rejected == 0);
    REQUIRE (rig.standby() == boom);

    pointAt (rig.third);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // Three, past it

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto plan = cue::solveHistory (rig.document, nullptr, nullptr, { rig.listId, rig.third, 1.0 },
                                         rig.runner.listState().historyOf (rig.listId));
    CHECK (plan.how == "history");
    CHECK (std::none_of (plan.runs.begin(), plan.runs.end(),
                         [&boom] (const cue::PlannedRun& planned) { return planned.cue == boom; }));
}

TEST_CASE ("go.doh: a seek on a scene after a Doh moves the scene's firing, never the Doh's step")
{
    /*  A SEEK RE-DATES THE SCENE'S NEWEST FIRING (§24.4), and a `d` step fired
        nothing: a scene fired by name, then GO'd too early and taken back, has
        its `d` newest - a seek of the run fired by name must move its `f`, or a
        later jump places the scene at the Doh. Written for the guard, which no
        case reached, it failed first on something older than D1: `seekGroup`
        re-dated the step through a run pointer its own seat had moved, so no
        step moved at all. */
    HistoryRig rig;
    const auto scene = timelineScene (rig, 4);

    REQUIRE (rig.submitAndTick ("cue.fire", { osc::Value::string (scene) }).rejected == 0);
    const auto byName = rig.runOf (scene);
    REQUIRE (! byName.empty());

    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string (scene) }).rejected == 0);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);       // the scene again, beside the first
    REQUIRE (doh (rig).rejected == 0);

    const auto tickOf = [&rig, &scene] (char origin)
    {
        std::int64_t at = -1;

        for (const auto& step : rig.runner.listState().historyOf (rig.listId))
            if (step.cue == scene && step.origin == origin)
                at = step.tick;

        return at;
    };

    const auto dohStep = tickOf ('d');
    REQUIRE (dohStep >= 0);
    REQUIRE (tickOf ('f') >= 0);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto seekTick = rig.tick;
    REQUIRE (rig.submitAndTick ("run.seek", { osc::Value::string (byName), osc::Value::float64 (2.0) }).rejected == 0);

    CHECK (tickOf ('f') == seekTick - 100);
    CHECK (tickOf ('d') == dohStep);
}

TEST_CASE ("go.doh: a jump forgets the last GO - a Doh after it has nothing to take back")
{
    /*  A JUMP REWRITES THE HISTORY THE GO LIVED IN (§24.4): the record goes with
        it, and a Doh pressed after the jump moves nothing. A net for a road no
        case reached. */
    Rig rig;
    rig.setStandby (rig.mediaId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);

    REQUIRE (rig.engine.submit ("cli", "list.aim", { osc::Value::string (rig.listId),
                                                     osc::Value::string (rig.memoId),
                                                     osc::Value::float64 (0.0) }));
    rig.tickOnce();
    REQUIRE (rig.submitAndTick ("list.loadToTime", { osc::Value::string (rig.listId) }).rejected == 0);

    const auto whereTheJumpLeftIt = rig.standby();

    CHECK (doh (rig).rejected == 1);
    CHECK (refusedFor (rig, "nothing-to-take-back"));
    CHECK (rig.standby() == whereTheJumpLeftIt);
}

TEST_CASE ("go.doh: the GO's cue deleted before the Doh - refused at the pointer's door, and nothing moves")
{
    /*  THE POINTER'S OWN DOOR IS THE LAST REFUSAL (§24.4): a cue gone is no place
        to put the pointer back, and the Doh refuses before anything - history,
        flag, runs - has moved. A net for a road no case reached. */
    HistoryRig rig;
    rig.park (rig.memoId);
    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    const auto memoRun = rig.runOf (rig.memoId);
    const auto history = rig.spelled();
    const auto pointer = rig.standby();
    const auto finished = finishedOf (rig);

    REQUIRE (rig.document.remove (rig.memoId).ok);

    CHECK (doh (rig).rejected == 1);
    CHECK (rig.standby() == pointer);
    CHECK (finishedOf (rig) == finished);
    CHECK (rig.spelled() == history);
    CHECK_FALSE (rig.runs.find (memoRun)->takenBack);
}

//==============================================================================
/*  AFTER A DOUBLE ESC, NOTHING GO.DOT STARTED KEEPS WRITING (2026-10-02, H5,
    namespace draft §23.11).

    WHAT `park` PROMISES. Every row's panic value is `park` - the parameter
    rests where it is (PRD §4.6) - and a resting place only means something if
    nothing moves the parameter after the press. §4.4's double Esc "drops all
    actions": so after it, nothing Go.dot started may write again.

    WHAT IS LIVE, AND WHAT IS ONLY WATCHED. The rig runs these writers at once,
    and the case requires each to have moved just before the press: the voice's
    level (a level lane and a level fade), its speed (a speed fade, DS), a DCA's
    trim (a DCA fade), a desk's values and a value its rate cap holds back (a
    looping stream of network cues), and launches and arms (a short media cue
    in that stream, launched afresh every round). After the press it watches
    every one of them, and the whole published tree, for fifty ticks. The
    routing, the EQ, the inserts, a second effects sweep, a stop placed ahead,
    and a stop or a kill after the settle are WATCHED, NOT EXERCISED: nothing in
    the rig writes them, so their checks guard against a writer appearing, and
    prove nothing about one being stopped. MIDI is not here at all: H4's
    `MidiTests` cases are what cover a double Esc on it (§23.10).

    THE ONE MOVE IT ACCEPTS is not here but in `LaneRecordTests`: a fader taken
    for lane recording stays taken and shows the lane's start two ticks after
    the press (decisions DN and DG, kept by JE). It is a surface's readout, not
    an action on the show; the case there pins it for the author to rule on. */
namespace
{
    /*  A desk with a writable float fader, a writable integer scene and a
        read-only meter, as `NetworkCueTests` mounts one. The stream writes the
        fader. */
    constexpr const char* stopRigDesk = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "fader": { "FULL_PATH": "/fader", "TYPE": "f", "ACCESS": 3 },
        "scene": { "FULL_PATH": "/scene", "TYPE": "i", "ACCESS": 3 },
        "meter": { "FULL_PATH": "/meter", "TYPE": "f", "ACCESS": 1 }
      }
    })JSON";

    /*  EVERY WRITER THE RIG CAN RUN CHEAPLY, AT ONCE: the lane rig's media cue,
        sounding on a DCA with a level lane drawn over it; a four-second fade on
        its level and another on its speed; a four-second fade on the DCA's
        trim; and a stream - a group looping for ever, a network cue writing a
        desk fader, a short media cue, and the fader again - into a device
        capped at half a hertz, so after its first send every later value is
        held back for the length of the case.

        ITS OWN TICK, because the base rig's neither moves the sample clock,
        flushes a sender nor answers the disk. Here the disk answers every arm
        at the next tick, and the stream's media cue sounds for three ticks from
        its launch and then stops, as a file that short would; a run already
        being stopped is never sounded. The sender has no socket, which is a
        complete configuration (MountSender.h): `pending()` is what it would
        still send. */
    struct StopRig : LaneRig
    {
        StopRig()
        {
            runner.setDcas (&dcas);
            runner.setMounts (&mounts, &sender);
            parameters.setDcas (&dcas);

            tree::MountDeclaration desk;
            desk.id = "K3PV7WRB";
            desk.prefix = "/desk";
            desk.namespaceFile = "namespaces/desk.json";
            desk.port = 9000;
            desk.rateCap = 0.5;                 // one send of an address every hundred flushes
            REQUIRE (mounts.load (desk, stopRigDesk).ok);

            band = document.createDca ("Band").id;
            put ("/godot/cue/" + mediaId + "/dca", band);
            put ("/godot/cue/" + fadeId + "/duration", "4");

            dcaFadeId = document.createCue (listId, 4, "fade", "Band down").id;
            put ("/godot/cue/" + dcaFadeId + "/dca", band);
            put ("/godot/cue/" + dcaFadeId + "/level", "-20");
            put ("/godot/cue/" + dcaFadeId + "/duration", "4");

            streamId = document.createCue (listId, 5, "group", "Stream").id;
            put ("/godot/cue/" + streamId + "/advance", "auto");
            put ("/godot/cue/" + streamId + "/loops", "0");
            member (0, "f:0.25");

            blipId = document.createCue (streamId, 1, "media", "Blip").id;
            REQUIRE_FALSE (blipId.empty());
            put ("/godot/cue/" + blipId + "/file", "blip.wav");

            member (2, "f:0.75");

            //  The speed fade (DS): the level left alone, the speed to a half.
            speedFadeId = document.createCue (listId, 6, "fade", "Slow down").id;
            put ("/godot/cue/" + speedFadeId + "/target", mediaId);
            put ("/godot/cue/" + speedFadeId + "/levelOn", "false");
            put ("/godot/cue/" + speedFadeId + "/rateOn", "true");
            put ("/godot/cue/" + speedFadeId + "/rate", "0.5");
            put ("/godot/cue/" + speedFadeId + "/duration", "4");

            drawLane ("0 0 10 -40");
        }

        void put (const std::string& address, const std::string& text)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, text).ok, address << " = " << text);
        }

        void member (int index, const std::string& atom)
        {
            const auto id = document.createCue (streamId, index, "osc", "Fader").id;
            REQUIRE_FALSE (id.empty());

            put ("/godot/cue/" + id + "/address", "/desk/fader");
            put ("/godot/cue/" + id + "/value", atom);
            put ("/godot/cue/" + id + "/wait", "none");
        }

        /*  THE DISK AND THE SHORT FILE, done by hand ahead of each tick. Every
            arm asked for is answered - and counted first, because answering
            empties the fake's list - and the stream's media cue sounds from the
            tick it is seen playing, for three ticks. */
        void answerTheAudioSide()
        {
            armsAsked += audio.arms.size();
            audio.completeArms (engine);

            for (const auto& run : runs.all())
            {
                if (run.cue != blipId || run.isFinished() || run.track < 0 || run.launchedAtSample <= 0)
                    continue;

                const auto since = blipSince.find (run.id);

                if (since == blipSince.end())
                {
                    if (run.state == cue::runState::playing)
                    {
                        blipSince.emplace (run.id, tick);
                        audio.playing.insert (run.track);
                    }
                }
                else if (tick - since->second >= 3)
                {
                    audio.playing.erase (run.track);
                }
            }
        }

        /*  One tick of the real loop, the sound a tick further on: the audio
            side answers, the Runner observes, the engine applies, and what the
            tick wrote leaves. */
        Engine::TickResult step()
        {
            answerTheAudioSide();
            audio.samples += 960;
            runner.beforeTick (engine, tick);
            auto result = engine.processTick (tick++);
            sender.flush();
            return result;
        }

        Engine::TickResult send (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return step();
        }

        /*  Steps until `ready` holds and answers how many steps that took, or
            -1 when it never did within `bound`. */
        template <typename Predicate>
        int stepsUntil (Predicate ready, int bound)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return n;

                step();
            }

            return ready() ? bound : -1;
        }

        bool allFinished() const
        {
            return std::all_of (runs.all().begin(), runs.all().end(),
                                [] (const cue::Run& run) { return run.isFinished(); });
        }

        std::shared_ptr<const tree::TreeSnapshot> publish()
        {
            parameters.markStale();
            state.tick = tick;
            return parameters.publish (tick, state);
        }

        cue::DcaTable dcas;
        tree::MountTable mounts;
        tree::MountSender sender;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::string band, dcaFadeId, speedFadeId, streamId, blipId;
        std::size_t armsAsked = 0;
        std::map<std::string, std::int64_t> blipSince;
    };

    /*  WHAT A PUBLISHED TREE MAY CHANGE WITH NOTHING WRITING: the clock, and
        nothing else (JF). Kept to what the case below needed, each entry with
        its reason, because every entry is a place the invariant stops looking. */
    bool isClockReadout (const std::string& address)
    {
        //  The engine's tick: it is the clock, and it moves by being one.
        return address == "/godot/engine/tick";
    }
}

TEST_CASE ("double Esc: nothing Go.dot started keeps writing, for fifty ticks after")
{
    StopRig rig;

    //  EVERY WRITER, running.
    const auto media = rig.launch();

    CHECK (rig.send ("cue.fire", { osc::Value::string (rig.fadeId) }).rejected == 0);
    CHECK (rig.send ("cue.fire", { osc::Value::string (rig.speedFadeId) }).rejected == 0);
    CHECK (rig.send ("cue.fire", { osc::Value::string (rig.dcaFadeId) }).rejected == 0);
    CHECK (rig.send ("cue.fire", { osc::Value::string (rig.streamId) }).rejected == 0);

    for (int n = 0; n < 20; ++n)
        rig.step();

    /*  EACH LIVE WRITER SEEN MOVING in the twenty ticks before the press - long
        enough for the stream to go round with its media cue in it. Without
        this the invariant below passes by watching writers that had already
        stopped, which is how a guard like this fails without anybody
        noticing. */
    {
        const auto levels = rig.audio.levels.size();
        const auto ratePoints = rig.audio.ratePoints.size();
        const auto launches = rig.audio.launches.size();
        const auto arms = rig.armsAsked;
        const auto revision = rig.mounts.revision();
        const auto trim = rig.dcas.trimOf (rig.band);

        for (int n = 0; n < 20; ++n)
            rig.step();

        REQUIRE (rig.audio.levels.size() > levels);         // the lane and the level fade, on the voice
        REQUIRE (rig.audio.ratePoints.size() > ratePoints); // the speed fade, on the voice
        REQUIRE (rig.audio.launches.size() > launches);     // the stream's media cue, launched again
        REQUIRE (rig.armsAsked > arms);                     // ... and armed again before it
        REQUIRE (rig.mounts.revision() > revision);         // the stream, on the desk's tree
        REQUIRE (rig.dcas.trimOf (rig.band) < trim);        // the DCA fade, going down
        REQUIRE (rig.runner.fades().size() == 3u);          // level, speed and DCA, all still running
        REQUIRE (rig.sender.pending() == 1u);               // a value the cap is holding back
        REQUIRE_FALSE (rig.runs.find (media)->isFinished());
    }

    SUBCASE ("on its own") {}

    SUBCASE ("in the middle of Esc's panic fade")
    {
        /*  Every real double Esc is two presses (§23.2, ER): the second lands
            on Esc's teardown, inside its fade. */
        rig.put ("/godot/audio/panicFade", "2");
        CHECK (rig.send ("run.stopAll").rejected == 0);

        for (int n = 0; n < 10; ++n)
            rig.step();

        REQUIRE_FALSE (rig.runs.find (media)->isFinished());
    }

    //  THE PRESS.
    CHECK (rig.send ("run.killAll").rejected == 0);

    /*  FROM THE PRESS ON: what no tick after it may add to. */
    const auto& audio = rig.audio;
    const auto launches = audio.launches.size();
    const auto arms = rig.armsAsked;
    const auto ratePoints = audio.ratePoints.size();
    const auto stopsAt = audio.stopsAt.size();
    const auto routingPushes = audio.routingPushes;
    const auto eqPushes = audio.eqPushes;
    const auto fxEnables = audio.fxEnables.size();
    const auto fxValues = audio.fxValues.size();
    const auto fxShapes = audio.fxShapes.size();
    const auto fxStates = audio.fxStates.size();
    const auto sweeps = audio.sweeps.size();
    const auto levels = audio.levels.size();
    const auto trim = rig.dcas.trimOf (rig.band);
    const auto revision = rig.mounts.revision();

    //  The value the cap was holding went with the press (H4).
    CHECK (rig.sender.pending() == 0u);

    /*  EVERYTHING ENDS, AND PROMPTLY. Measured on this rig (2026-10-02): every
        run is over three ticks after the press on its own, and one inside
        Esc's fade, with no level, speed, launch or arm reaching the audio side
        in between. The bound is a tick above the slower, so it fails when the
        teardown takes a tick more than it does today - which is the point: a
        double Esc is immediate. The ticks are the engine's own, counted by
        this rig and owned by it; nothing here waits on a clock it does not
        drive. */
    const auto settledIn = rig.stepsUntil ([&rig] { return rig.allFinished(); }, 50);
    INFO ("every run over " << settledIn << " ticks after the press");
    REQUIRE (settledIn >= 0);
    CHECK (settledIn <= 4);

    const auto stopped = audio.stopped.size();
    const auto kills = audio.kills.size();
    const auto before = rig.publish();

    //  FIFTY TICKS, the sample clock still running, as a sound card's would.
    for (int n = 0; n < 50; ++n)
        rig.step();

    CHECK (audio.launches.size() == launches);
    CHECK (rig.armsAsked == arms);
    CHECK (audio.ratePoints.size() == ratePoints);
    CHECK (audio.stopsAt.size() == stopsAt);
    CHECK (audio.routingPushes == routingPushes);
    CHECK (audio.eqPushes == eqPushes);
    CHECK (audio.fxEnables.size() == fxEnables);
    CHECK (audio.fxValues.size() == fxValues);
    CHECK (audio.fxShapes.size() == fxShapes);
    CHECK (audio.fxStates.size() == fxStates);
    CHECK (audio.sweeps.size() == sweeps);
    CHECK (audio.stopped.size() == stopped);
    CHECK (audio.kills.size() == kills);

    /*  AT MOST ONE LEVEL A VOICE AFTER THE PRESS: the tick after it applies
        levels before it cuts the voice (`applyLevels` runs before
        `enforceStops`), so one push may still reach a voice being cut - and
        none after that. */
    std::map<int, int> pushesAfterThePress;

    for (auto at = levels; at < audio.levels.size(); ++at)
        ++pushesAfterThePress[audio.levels[at].first];

    for (const auto& [track, pushes] : pushesAfterThePress)
    {
        INFO ("level pushes to track " << track << " after the press: " << pushes);
        CHECK (pushes <= 1);
    }

    //  The DCA rests where its fade had got to (plan decision 8: park on release).
    CHECK (same (rig.dcas.trimOf (rig.band), trim));

    //  Nothing more reached the desk's tree, and nothing waits to reach the desk.
    CHECK (rig.mounts.revision() == revision);
    CHECK (rig.sender.pending() == 0u);

    CHECK (rig.runner.fades().empty());
    CHECK (rig.runner.sends().empty());

    /*  AND THE WHOLE PUBLISHED TREE, which is what every client and every
        surface reads: nothing appears, and nothing but the clock changes. A
        node may leave - a finished run leaves the tree once its retention is
        up - and leaving is not writing. */
    const auto moved = tree::diff (*before, *rig.publish());

    CHECK (moved.added.empty());

    std::string unexplained;

    for (const auto& address : moved.valueChanged)
        if (! isClockReadout (address))
            unexplained += address + "\n";

    INFO ("changed after the press, with nothing meant to write:\n" << unexplained);
    CHECK (unexplained.empty());
}
