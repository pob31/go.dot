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
    A SAMPLING CHANNEL'S TAKE, AS COMMANDS (Phase 9c, stage 9c.3, namespace
    draft §19.3-§19.6): the account the take verbs move, and the presses the
    Runner's hook places. Against a player of the test's own that answers for
    the fixture's two rack channels - Looper, which samples, and Plain, which
    does not - and records every press, `through` and report.

    What is pinned: the rows as published; Rec and Rec, the take closed at the
    length the audio thread reports; the refusals, each from the account, the
    document or the run table; Undo and Clear with no cue; a transport cue's
    presses, and nothing against a cue not sounding; GO's `onGo` - now, or
    when the channel frees for a cue that waited; the cue letting go holding
    the take; the loop points' door, clamped and on no history; and `through`
    following the cue's row.

    The fixture (tests/fixtures/bundles/take): list TK000001 with mic cue
    TK000002 "Loop voice" (onGo wait) and TK000008 "Scene 5 loop" (onGo loop,
    through on) on channel TK000011 "Looper" (a ten-second take, two layers,
    its test gain before the recorder); transport cues TK000006 (record) and
    TK000007 (loop) aimed at TK000002; channel TK000013 "Plain", no recorder.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/cue/TakeCommands.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace wfg;
using cue::TakeVerb;

namespace
{
    juce::File takeBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/take" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    osc::Value text (const std::string& value) { return osc::Value::string (value); }

    /*  The audio side: two voices and the two rack channels, Looper as track 2
        and Plain as track 3. Arms complete when the test says so; a stop ends
        the sound at once; and the take's doors are recorded, with the reports
        the test hands it given back at the next ask. */
    struct TakePlayer final : cue::Player
    {
        struct Posted
        {
            std::string channel;
            TakeVerb verb;
            std::int64_t sample;
            double in, out;
        };

        int trackCount() const override              { return 2; }
        int slotCount() const override               { return 1; }
        int channelsPerTrack() const override        { return 2; }
        int blockSize() const override               { return 128; }
        int sampleRate() const override              { return 48000; }
        std::int64_t samplesElapsed() const override { return samples; }

        void requestArm (const cue::ArmRequest& request) override  { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t) override
        {
            playing.insert (track);
            return true;
        }

        bool stop (int track) override
        {
            stops.push_back (track);
            playing.erase (track);
            return true;
        }

        bool stopAtSample (int track, int, std::int64_t) override  { return stop (track); }
        void setLevelDb (int, double) override {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override                  { return playing.count (track) > 0; }
        bool isArmReady (int track) const override                 { return ready.count (track) > 0; }

        int rackTrackOf (const std::string& channelId) const override
        {
            const auto found = rack.find (channelId);
            return found != rack.end() ? found->second : -1;
        }

        bool openLive (int track, std::int64_t, double) override
        {
            playing.insert (track);
            return true;
        }

        bool kill (int track) override
        {
            kills.push_back (track);
            playing.erase (track);
            return true;
        }

        bool postTake (const std::string& channel, TakeVerb verb, std::int64_t sample,
                       double in, double out) override
        {
            posts.push_back ({ channel, verb, sample, in, out });
            return true;
        }

        void setTakeThrough (const std::string& channel, bool through) override  { throughs[channel] = through; }

        std::vector<TakeReport> takeReports (const std::vector<std::string>&) override
        {
            auto out = reports;
            reports.clear();
            return out;
        }

        double takePlayhead (const std::string&) const override  { return 1.25; }

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

        std::map<std::string, int> rack { { "TK000011", 2 }, { "TK000013", 3 } };
        std::int64_t samples = 0;

        std::vector<cue::ArmRequest> arms;
        std::vector<int> stops, kills;
        std::set<int> playing, ready;
        std::vector<Posted> posts;
        std::map<std::string, bool> throughs;
        std::vector<TakeReport> reports;
    };

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (takeBundle(), document).ok);

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document, &takes));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            cue::registerTakeCommands (engine.commands(), takes, runs, document);

            /*  SERVE'S TRANSACTION HOOK, so a ride on a live row is seen to open
                none: every other command opens one. */
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       if (! cue::isLiveWrite (appliedCommand.name, coerced))
                                           document.beginTransaction (appliedCommand.name, tickIndex,
                                                                      submitted.origin, coerced);
                                   });

            runner.setDcas (&dcas);
            runner.setTakes (&takes);
            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            parameters.setTakes (&takes);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            audio.samples += 960;
            return result;
        }

        /*  WHETHER THE COMMAND WENT THROUGH, judged by nothing refused rather
            than by one applied: the Runner's own records - the standby arm the
            fixture's standby asks for - are applied in the same ticks. */
        bool applied (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            const auto result = tickOnce();
            return result.applied >= 1 && result.rejected == 0;
        }

        /** The reason a command was refused, or nothing when it was applied. */
        std::string refusal (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));

            if (tickOnce().rejected == 0)
                return {};

            return engine.lastError();
        }

        template <typename Predicate>
        bool tickUntil (Predicate ready, int bound = 200)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return true;

                tickOnce();
            }

            return ready();
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            const cue::Run* found = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    found = runs.find (run.id);

            return found;
        }

        /*  Fires a cue, lets its arm answer, and ticks until its launch is
            placed - or the bound says it never was. */
        const cue::Run* fireAndLaunch (const std::string& cueId)
        {
            REQUIRE (engine.submit ("cli", "cue.fire", { text (cueId) }));
            tickOnce();
            audio.completeArms (engine);
            tickUntil ([this, &cueId] { const auto* run = runOf (cueId); return run != nullptr && run->launchedAtSample > 0; });
            return runOf (cueId);
        }

        std::string at (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        const cue::Take& take() const { return takes.of ("TK000011"); }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        cue::TakeTable takes;
        cue::Focus focus;
        tree::MountTable mounts;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (23) };
        cue::Runner runner { document, runs, runIds, focus };
        TakePlayer audio;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 0;
    };

    const std::string looper = "TK000011";
}

//==============================================================================
TEST_CASE ("take: a sampling channel publishes its take, and a mic cue its onGo and through")
{
    Rig rig;

    CHECK (rig.at ("/godot/slot/TK000011/take") == "empty");
    CHECK (rig.at ("/godot/slot/TK000011/takeLength") == "0");
    CHECK (rig.at ("/godot/slot/TK000011/takeLayers") == "0");
    CHECK (rig.at ("/godot/slot/TK000011/loopIn") == "0");
    CHECK (rig.at ("/godot/slot/TK000011/loopOut") == "0");
    CHECK (rig.at ("/godot/slot/TK000011/takeSeconds") == "10");
    CHECK (rig.at ("/godot/slot/TK000011/layers") == "2");
    CHECK (rig.at ("/godot/plugin/TK000012/side") == "before");

    CHECK (rig.at ("/godot/cue/TK000002/onGo") == "wait");
    CHECK (rig.at ("/godot/cue/TK000002/through") == "false");
    CHECK (rig.at ("/godot/cue/TK000008/onGo") == "loop");
    CHECK (rig.at ("/godot/cue/TK000008/through") == "true");
    CHECK (rig.at ("/godot/cue/TK000006/verb") == "record");
}

TEST_CASE ("take: Rec, Rec - the take records, closes and loops, at the length the audio thread reports")
{
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    //  Rec: the account at once, the press at the next tick, where a launch would be placed.
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    CHECK (rig.take().state == "recording");

    const auto posted = rig.audio.posts.size();
    const auto expected = rig.audio.samples + 2 * 960;     // the launch latency: two ticks at 128 frames
    rig.tickOnce();
    REQUIRE (rig.audio.posts.size() == posted + 1);
    CHECK (rig.audio.posts.back().channel == looper);
    CHECK (rig.audio.posts.back().verb == TakeVerb::record);
    CHECK (rig.audio.posts.back().sample == expected);

    //  Rec again: closed, looping - its length not known until the audio thread says.
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    CHECK (rig.take().state == "looping");
    CHECK (rig.take().length == doctest::Approx (0.0));

    //  The audio thread closed it at two and a half seconds: logged, and the account knows it.
    rig.audio.reports.push_back ({ looper, "pressed", 2.5 });
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.take().state == "looping");
    CHECK (rig.take().length == doctest::Approx (2.5));
    CHECK (rig.take().loopIn == doctest::Approx (0.0));
    CHECK (rig.take().loopOut == doctest::Approx (2.5));
    CHECK (rig.at ("/godot/slot/TK000011/take") == "looping");
    CHECK (rig.at ("/godot/slot/TK000011/takeLength") == "2.5");
    CHECK (rig.at ("/godot/slot/TK000011/playhead") == "1.25");

    //  Rec while it loops: a layer; Rec: closed, one layer on the take.
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    CHECK (rig.take().state == "overdubbing");
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    CHECK (rig.take().state == "looping");
    CHECK (rig.take().layers == 1);
    CHECK (rig.at ("/godot/slot/TK000011/takeLayers") == "1");

    //  A take filling its memory by itself is said on the channel.
    rig.audio.reports.push_back ({ looper, "full", 10.0 });
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.at ("/godot/slot/TK000011/takeProblem") == "the take reached its 10 seconds and was closed");
}

TEST_CASE ("take: refused from the account, the document and the run table - never from the audio side")
{
    Rig rig;

    CHECK (rig.refusal ("take.record", { text ("NQNQNQNQ") }).find (reason::unknownId) != std::string::npos);
    CHECK (rig.refusal ("take.record", { text ("TK000013") }).find (reason::badValue) != std::string::npos);

    //  NO CUE SOUNDING: nothing to record - and Undo and Clear need none.
    CHECK (rig.refusal ("take.record", { text (looper) }).find (reason::notRunning) != std::string::npos);
    CHECK (rig.refusal ("take.loop", { text (looper) }).find (reason::notRunning) != std::string::npos);
    CHECK (rig.refusal ("take.overdub", { text (looper) }).find (reason::notRunning) != std::string::npos);
    CHECK (rig.refusal ("take.undo", { text (looper) }).empty());
    CHECK (rig.refusal ("take.clear", { text (looper) }).empty());

    //  EVERY LAYER IN USE: the channel keeps two.
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    for (int press = 0; press < 6; ++press)
        REQUIRE (rig.applied ("take.record", { text (looper) }));

    CHECK (rig.take().state == "looping");
    CHECK (rig.take().layers == 2);
    CHECK (rig.refusal ("take.record", { text (looper) }).find (reason::layersFull) != std::string::npos);
    CHECK (rig.refusal ("take.overdub", { text (looper) }).find (reason::layersFull) != std::string::npos);

    //  Undo one, and there is room again.
    REQUIRE (rig.applied ("take.undo", { text (looper) }));
    CHECK (rig.take().layers == 1);
    CHECK (rig.refusal ("take.record", { text (looper) }).empty());
    CHECK (rig.take().state == "overdubbing");

    //  Undo while a layer is laid abandons it; Undo on the take alone takes nothing.
    REQUIRE (rig.applied ("take.undo", { text (looper) }));
    CHECK (rig.take().state == "looping");
    CHECK (rig.take().layers == 1);
    REQUIRE (rig.applied ("take.undo", { text (looper) }));
    REQUIRE (rig.applied ("take.undo", { text (looper) }));
    CHECK (rig.take().layers == 0);
    CHECK (rig.take().state == "looping");

    //  Clear empties it.
    REQUIRE (rig.applied ("take.clear", { text (looper) }));
    CHECK (rig.take().state == "empty");
}

TEST_CASE ("take: a transport cue presses the take its target sounds through, and does nothing against a cue not sounding")
{
    Rig rig;

    //  Not sounding yet: applied, the transport cue's own run over, the take untouched.
    REQUIRE (rig.applied ("cue.fire", { text ("TK000006") }));
    CHECK (rig.take().state == "empty");

    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    REQUIRE (rig.applied ("cue.fire", { text ("TK000006") }));     // record
    CHECK (rig.take().state == "recording");
    REQUIRE (rig.applied ("cue.fire", { text ("TK000007") }));     // loop
    CHECK (rig.take().state == "looping");

    //  AND IT STOPS NOTHING: the mic cue sounds on.
    const auto* run = rig.runOf ("TK000002");
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::playing);
    CHECK (rig.audio.stops.empty());
}

TEST_CASE ("take: GO does what the cue's onGo says - now, or when the channel frees for a cue that waited")
{
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    //  A take, recorded and closed.
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    rig.audio.reports.push_back ({ looper, "pressed", 3.0 });
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.take().state == "looping");

    /*  THE SECOND CUE, onGo loop, fired while the first holds the channel: it
        waits, and its GO waits with it. */
    REQUIRE (rig.applied ("cue.fire", { text ("TK000008") }));
    const auto* second = rig.runOf ("TK000008");
    REQUIRE (second != nullptr);
    CHECK_FALSE (second->pending.empty());
    CHECK (second->takeOnGoPending);

    /*  THE FIRST LETS GO - stopped by itself, since Esc would stop the cue
        waiting as well: the take held as it does - never lost - and the second,
        taking the channel, loops it. */
    REQUIRE (rig.applied ("run.stop", { text (rig.runOf ("TK000002")->id) }));
    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("TK000002")->isFinished(); }));
    CHECK (rig.take().state == "looping");
    CHECK (rig.take().length == doctest::Approx (3.0));

    const auto* taking = rig.runOf ("TK000008");
    REQUIRE (taking != nullptr);
    CHECK_FALSE (taking->takeOnGoPending);

    //  The presses went in that order, placed at the next tick: held, then looped.
    rig.tickOnce();
    std::vector<TakeVerb> verbs;

    for (const auto& posted : rig.audio.posts)
        verbs.push_back (posted.verb);

    REQUIRE (verbs.size() >= 2u);
    CHECK (verbs[verbs.size() - 2] == TakeVerb::hold);
    CHECK (verbs.back() == TakeVerb::loop);
}

TEST_CASE ("take: the cue letting go holds the take, a first pass closed as it stood - Esc and a double Esc alike")
{
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    //  Esc while it records: held, the length to come from the audio thread.
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    REQUIRE (rig.applied ("run.stopAll"));
    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("TK000002")->isFinished(); }));
    CHECK (rig.take().state == "held");

    rig.audio.reports.push_back ({ looper, "held", 1.5 });
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.take().state == "held");
    CHECK (rig.take().length == doctest::Approx (1.5));

    //  GO again with onGo wait: silent still. Loop plays it.
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);
    CHECK (rig.take().state == "held");
    REQUIRE (rig.applied ("take.loop", { text (looper) }));
    CHECK (rig.take().state == "looping");

    //  A double Esc stops everything that sounds and unmakes nothing that was recorded.
    REQUIRE (rig.applied ("run.killAll"));
    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("TK000002")->isFinished(); }));
    CHECK (rig.take().state == "held");
    CHECK (rig.take().length == doctest::Approx (1.5));
}

TEST_CASE ("take: the loop points' door keeps them in the take and two crossfades apart, and opens no history")
{
    Rig rig;
    const auto point = [&rig] (const char* row, const char* value)
    {
        return rig.refusal ("node.set", { text (std::string ("/godot/slot/TK000011/") + row), text (value) });
    };

    //  No take yet: applied and ignored.
    CHECK (point ("loopIn", "1.0").empty());
    CHECK (rig.take().loopIn == doctest::Approx (0.0));

    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    rig.audio.reports.push_back ({ looper, "pressed", 4.0 });
    rig.tickOnce();
    rig.tickOnce();

    const auto& history = rig.document.history (doc::UndoDomain::document);
    const auto undoable = history.getNumActionsInCurrentTransaction();
    const auto canUndoBefore = history.canUndo();

    CHECK (point ("loopIn", "1.0").empty());
    CHECK (point ("loopOut", "3.0").empty());
    CHECK (rig.take().loopIn == doctest::Approx (1.0));
    CHECK (rig.take().loopOut == doctest::Approx (3.0));
    CHECK (rig.at ("/godot/slot/TK000011/loopIn") == "1");
    CHECK (rig.at ("/godot/slot/TK000011/loopOut") == "3");

    //  Pushed past each other, each stops two crossfades short.
    CHECK (point ("loopIn", "3.5").empty());
    CHECK (rig.take().loopIn == doctest::Approx (3.0 - cue::TakeTable::shortestLoopSeconds));
    CHECK (point ("loopOut", "0.5").empty());
    CHECK (rig.take().loopOut == doctest::Approx (rig.take().loopIn + cue::TakeTable::shortestLoopSeconds));
    CHECK (point ("loopOut", "99").empty());
    CHECK (rig.take().loopOut == doctest::Approx (4.0));

    //  The recorder was handed both points each time.
    REQUIRE_FALSE (rig.audio.posts.empty());
    rig.tickOnce();
    CHECK (rig.audio.posts.back().verb == TakeVerb::points);
    CHECK (rig.audio.posts.back().out == doctest::Approx (4.0));

    //  NOT A STEP OF THE HISTORY (decision CQ).
    CHECK (history.getNumActionsInCurrentTransaction() == undoable);
    CHECK (history.canUndo() == canUndoBefore);

    //  A channel the show does not declare, and a value that is no number.
    CHECK (rig.refusal ("node.set", { text ("/godot/slot/NQNQNQNQ/loopIn"), text ("1") }).find (reason::unknownId)
             != std::string::npos);
    CHECK (point ("loopIn", "soon").find (reason::typeMismatch) != std::string::npos);
}

TEST_CASE ("take: through follows the row of the cue holding the channel")
{
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);
    rig.tickOnce();

    REQUIRE (rig.audio.throughs.count (looper) == 1u);
    CHECK_FALSE (rig.audio.throughs[looper]);

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/TK000002/through"), text ("true") }));
    rig.tickOnce();
    CHECK (rig.audio.throughs[looper]);
}

TEST_CASE ("take: a load to time passes a transport cue's press by - it stops nothing")
{
    /*  §19.6: record, loop, overdub and clear are not stops, so a walk that
        counts what the transport cues before a point have stopped must not
        count them (Solver, SlotAnalysis). */
    Rig rig;
    const cue::Reader read;

    const auto recordCue = rig.document.findById ("TK000006");
    REQUIRE (recordCue.isValid());
    CHECK (cue::isTakePress (read, recordCue));
    CHECK (cue::isTakePress (read, rig.document.findById ("TK000007")));
    CHECK_FALSE (cue::isTakePress (read, rig.document.findById ("TK000002")));
}
