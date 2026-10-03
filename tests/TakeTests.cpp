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
#include <wfg/engine/log/EventLog.h>
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

        /*  KEEP'S TWO DOORS (§19.8): what was asked to be written, and the
            reports the test hands back at the next ask. */
        struct Kept
        {
            std::string channel, stem, folder;
        };

        bool keepTake (const std::string& channel, const std::string& stem, const std::string& folder) override
        {
            keeps.push_back ({ channel, stem, folder });
            return true;
        }

        std::vector<KeptReport> keptTakes() override
        {
            auto out = kept;
            kept.clear();
            return out;
        }

        std::vector<Kept> keeps;
        std::vector<KeptReport> kept;

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
            runner.setMediaFolder ("MEDIA");
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

        /** The arguments the last `command` record of the log was applied with. */
        std::vector<std::string> appliedWith (const std::string& command)
        {
            std::vector<std::string> out;

            for (const auto& record : LogFile::parse (engine.log().contents()).records)
            {
                if (record.kind != LogRecord::Kind::applied || record.command != command)
                    continue;

                out.clear();

                for (const auto& arg : record.args)
                    out.push_back (arg.isString() ? arg.getString() : std::string ("?"));
            }

            return out;
        }

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

TEST_CASE ("take: GO on a mic cue the standby armed ahead does what its onGo says, once")
{
    /*  THE ORDINARY GO, and the one that did nothing to the take (namespace
        draft §23, 2026-09-30). A mic cue at standby is armed ahead - its channel
        claimed, the gate shut - and GO on it launches that run rather than
        firing the cue afresh. That road never reached the take: `onGo` was acted
        on only where a cue is fired cold, so Scene 5's GO found the take held
        and left it silent.

        WITH A PRE-WAIT the GO arrives at the cue when the wait is over, through
        the road a cue fired cold takes - so the take waits with its cue, and
        the loop is asked for there and not a second time. */
    for (const auto* wait : { "0", "0.2" })
    {
        INFO ("preWait " << wait);
        Rig rig;
        REQUIRE (rig.document.setAttribute ("/godot/cue/TK000008/preWait", wait).ok);
        REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

        //  A take, recorded and closed, then held when its cue lets go of the channel.
        REQUIRE (rig.applied ("take.record", { text (looper) }));
        REQUIRE (rig.applied ("take.record", { text (looper) }));
        rig.audio.reports.push_back ({ looper, "pressed", 3.0 });
        rig.tickOnce();
        rig.tickOnce();
        REQUIRE (rig.take().state == "looping");

        REQUIRE (rig.applied ("run.stop", { text (rig.runOf ("TK000002")->id) }));
        REQUIRE (rig.tickUntil ([&rig] { return rig.runOf ("TK000002")->isFinished(); }));
        REQUIRE (rig.take().state == "held");

        //  The standby on Scene 5 loop, whose onGo is loop: armed ahead, the channel its own.
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("TK000001"), "TK000008").ok);
        REQUIRE (rig.tickUntil ([&rig]
        {
            const auto* run = rig.runOf ("TK000008");
            return run != nullptr && run->track >= 0;
        }));

        const auto armedId = rig.runOf ("TK000008")->id;
        CHECK_FALSE (rig.runOf ("TK000008")->prepare.empty());
        rig.audio.completeArms (rig.engine);
        rig.tickOnce();

        const auto loops = [&rig]
        {
            return std::count_if (rig.audio.posts.begin(), rig.audio.posts.end(),
                                  [] (const TakePlayer::Posted& posted)
                                  {
                                      return posted.channel == looper && posted.verb == TakeVerb::loop;
                                  });
        };

        const auto loopsBefore = loops();

        //  GO launches that very run, and loops the take it found - after the wait, when there is one.
        REQUIRE (rig.applied ("go"));
        CHECK (rig.runOf ("TK000008")->id == armedId);

        if (std::string (wait) == "0")
        {
            CHECK (rig.runOf ("TK000008")->launchRequested);
        }
        else
        {
            CHECK (rig.runOf ("TK000008")->state == cue::runState::waiting);
            CHECK (rig.take().state == "held");
            REQUIRE (rig.tickUntil ([&rig] { return rig.take().state == "looping"; }, 30));
        }

        CHECK (rig.take().state == "looping");

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (loops() == loopsBefore + 1);
    }
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

//==============================================================================
TEST_CASE ("take: Keep writes the closed take through the audio side, and Undo and Clear wait until it says it is done")
{
    /*  Phase 9c, stage 9c.6 (namespace draft 19.8): take.keep is refused for
        a take not closed, asked of the audio side at the next tick with the
        channel's name and the show's media, and holds Undo, Clear and another
        Keep off `busy` until take.kept says what the writer did. */
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    CHECK (rig.refusal ("take.keep", { text (looper) }).find ("not-closed") != std::string::npos);
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    CHECK (rig.refusal ("take.keep", { text (looper) }).find ("not-closed") != std::string::npos);

    REQUIRE (rig.applied ("take.record", { text (looper) }));
    rig.audio.reports.push_back ({ looper, "pressed", 2.5 });
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.applied ("take.overdub", { text (looper) }));
    REQUIRE (rig.applied ("take.loop", { text (looper) }));
    REQUIRE (rig.take().layers == 1);

    //  KEEP: the account busy at once, the writer asked at the next tick.
    REQUIRE (rig.applied ("take.keep", { text (looper) }));
    CHECK (rig.take().keeping);
    CHECK (rig.at ("/godot/slot/TK000011/keeping") == "true");
    rig.tickOnce();
    REQUIRE (rig.audio.keeps.size() == 1u);
    CHECK (rig.audio.keeps[0].channel == looper);
    CHECK (rig.audio.keeps[0].stem == "Looper");
    CHECK (rig.audio.keeps[0].folder == "MEDIA");

    //  WHILE IT WRITES: nothing empties what it reads, and it is not written twice.
    CHECK (rig.refusal ("take.undo", { text (looper) }).find ("busy") != std::string::npos);
    CHECK (rig.refusal ("take.clear", { text (looper) }).find ("busy") != std::string::npos);
    CHECK (rig.refusal ("take.keep", { text (looper) }).find ("busy") != std::string::npos);

    //  WRITTEN: logged with the name the writer found, and Undo is Undo again.
    rig.audio.kept.push_back ({ looper, "takes/Looper take 1.wav", "" });
    rig.tickOnce();
    rig.tickOnce();
    CHECK_FALSE (rig.take().keeping);
    CHECK (rig.take().kept == "takes/Looper take 1.wav");
    CHECK (rig.at ("/godot/slot/TK000011/kept") == "takes/Looper take 1.wav");
    CHECK (rig.appliedWith ("take.kept") == std::vector<std::string> { looper, "takes/Looper take 1.wav", "" });
    REQUIRE (rig.applied ("take.undo", { text (looper) }));
    CHECK (rig.take().layers == 0);

    //  A WRITER THAT COULD NOT says why, on the channel, and lets go.
    REQUIRE (rig.applied ("take.keep", { text (looper) }));
    rig.audio.kept.push_back ({ looper, "", "the disk would not take the whole take" });
    rig.tickOnce();
    rig.tickOnce();
    CHECK_FALSE (rig.take().keeping);
    CHECK (rig.take().problem == "the take could not be kept: the disk would not take the whole take");

    CHECK (rig.refusal ("take.keep", { text ("TK000013") }).find ("bad-value") != std::string::npos);
    CHECK (rig.refusal ("take.keep", { text ("NQNQNQNQ") }).find ("unknown-id") != std::string::npos);
}

TEST_CASE ("take: Keep as cue makes a media cue after the one sounding there, looping the take between its points, in one step")
{
    /*  Phase 9c, stage 9c.6 (namespace draft 19.8): the cue follows the mic
        cue holding the channel, plays the file the writer named, and loops
        between the points the take had when Keep was pressed - its
        identifiers in take.kept's record, so a replay makes the same ones. A
        change of the show: refused under the lock. */
    Rig rig;
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    REQUIRE (rig.applied ("take.record", { text (looper) }));
    rig.audio.reports.push_back ({ looper, "pressed", 2.5 });
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.applied ("node.set", { text ("/godot/slot/TK000011/loopIn"), osc::Value::float64 (0.5) }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/slot/TK000011/loopOut"), osc::Value::float64 (2.0) }));

    //  UNDER THE LOCK: the file alone, never the cue.
    REQUIRE (rig.applied ("node.set", { text ("/godot/document/locked"), osc::Value::boolean (true) }));
    CHECK (rig.refusal ("take.keep", { text (looper), osc::Value::boolean (true) }).find ("locked") != std::string::npos);
    REQUIRE (rig.applied ("node.set", { text ("/godot/document/locked"), osc::Value::boolean (false) }));

    REQUIRE (rig.applied ("take.keep", { text (looper), osc::Value::boolean (true) }));

    //  A point ridden while the file is written does not move the cue's range.
    REQUIRE (rig.applied ("node.set", { text ("/godot/slot/TK000011/loopIn"), osc::Value::float64 (1.0) }));

    rig.tickOnce();
    rig.audio.kept.push_back ({ looper, "takes/Looper take 1.wav", "" });
    rig.tickOnce();
    rig.tickOnce();

    const auto applied = rig.appliedWith ("take.kept");
    REQUIRE (applied.size() == 6u);
    const auto& made = applied[3];
    const auto& range = applied[4];

    //  AFTER LOOP VOICE, in its list.
    const auto order = client::model::words (rig.at ("/godot/list/TK000001/order"));
    const auto mic = std::find (order.begin(), order.end(), "TK000002");
    REQUIRE (mic != order.end());
    REQUIRE (mic + 1 != order.end());
    CHECK (*(mic + 1) == made);

    CHECK (rig.at ("/godot/cue/" + made + "/kind") == "media");
    CHECK (rig.at ("/godot/cue/" + made + "/name") == "Looper take 1");
    CHECK (rig.at ("/godot/cue/" + made + "/file") == "takes/Looper take 1.wav");
    CHECK (rig.at ("/godot/range/" + range + "/cue") == made);
    CHECK (rig.at ("/godot/range/" + range + "/in") == "0.5");
    CHECK (rig.at ("/godot/range/" + range + "/out") == "2");
    CHECK (rig.at ("/godot/range/" + range + "/loops") == "0");

    //  ONE STEP: Undo takes the cue away whole, and leaves the file kept.
    REQUIRE (rig.applied ("undo"));
    CHECK (rig.at ("/godot/cue/" + made + "/kind").empty());
    CHECK (rig.take().kept == "takes/Looper take 1.wav");
}

//==============================================================================
TEST_CASE ("go.doh: a take press the GO made is put back by its exact inverse, or said")
{
    /*  Doh! D3 (2026-10-03, PRD §3.32, namespace draft §24.13; the design's test
        18, GU): a press a transport cue of the GO's made on a take is undone by
        its inverse while the take is still where the press left it - Undo for
        a start of recording or of a layer, Undo then Hold for a layer begun on
        a held take, Hold for a held take looped; a first pass closed, and a
        clear, cannot be undone, and are said (L17). Run on the engine sources
        of 5fd0e76 with this case, it failed: every press stood. */
    Rig rig;
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);
    REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);

    const auto goOn = [&rig] (const std::string& cueId)
    {
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("TK000001"), cueId).ok);
        rig.tickOnce();
        REQUIRE (rig.applied ("go"));
    };

    const auto looping = [&rig]
    {
        REQUIRE (rig.applied ("take.record", { text (looper) }));
        REQUIRE (rig.applied ("take.record", { text (looper) }));
        rig.audio.reports.push_back ({ looper, "pressed", 3.0 });
        rig.tickOnce();
        rig.tickOnce();
        REQUIRE (rig.take().state == "looping");
    };

    /*  HELD: the cue letting go holds the take (§19.6); the cue fired again
        to sound through it, its own `onGo` waiting. */
    const auto held = [&rig, &looping]
    {
        looping();
        REQUIRE (rig.applied ("run.stop", { text (rig.runOf ("TK000002")->id) }));
        REQUIRE (rig.tickUntil ([&rig] { return rig.runOf ("TK000002")->isFinished(); }));
        REQUIRE (rig.take().state == "held");
        REQUIRE (rig.fireAndLaunch ("TK000002") != nullptr);
        REQUIRE (rig.take().state == "held");
    };

    std::string after, back;
    auto said = false;

    SUBCASE ("empty to recording: undone, the take empty again")
    {
        goOn ("TK000006");
        after = "recording";
        back = "empty";
    }

    SUBCASE ("looping to overdubbing: undone, looping again with no layer added")
    {
        looping();
        goOn ("TK000006");
        after = "overdubbing";
        back = "looping";
    }

    SUBCASE ("held to overdubbing: undone, then held again")
    {
        held();
        goOn ("TK000006");
        after = "overdubbing";
        back = "held";
    }

    SUBCASE ("held to looping: held again")
    {
        held();
        goOn ("TK000007");
        after = "looping";
        back = "held";
    }

    SUBCASE ("recording to looping, a first pass closed: cannot be undone, and said")
    {
        REQUIRE (rig.applied ("take.record", { text (looper) }));
        goOn ("TK000007");
        after = "looping";
        back = "looping";
        said = true;
    }

    SUBCASE ("a clear: cannot be undone, and said")
    {
        REQUIRE (rig.document.createCue ("TK000001", 9, "transport", "Clear it", "TK000099").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/TK000099/target", "TK000002").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/TK000099/verb", "clear").ok);
        looping();
        goOn ("TK000099");
        after = "empty";
        back = "empty";
        said = true;
    }

    REQUIRE (rig.take().state == after);
    const auto layers = rig.take().layers;

    REQUIRE (rig.applied ("go.doh"));
    rig.tickOnce();

    CHECK (rig.take().state == back);
    CHECK (rig.take().layers == layers);

    const auto& report = rig.runner.listState().dohReport().text;
    INFO (report);
    CHECK ((report.find ("cannot be undone") != std::string::npos) == said);
}
