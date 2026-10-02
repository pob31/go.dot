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

/*  PHASE 9b'S MIC CUE IN THE DOCUMENT (namespace draft 18.2, decisions BW, BX
    and CE): a live input played as a cue. What the show says about one before
    it makes a sound - that it is a cue first, a sound second and a live input
    third, published with a media cue's sound rows and its own three and
    nothing about a file; that its inserts are its channel's plugins and never
    the set's, and a media cue's never a channel's; that it takes the children
    a sound is made of and refuses the ones that name a file; and that `wfg
    validate` says, before the show, each reason one would fail when fired.

    Driven through the engine, because what a replay reproduces is the applied
    record.

    AND HOW ONE RUNS (stage 9b.5, namespace draft 18.5), against a player of
    the test's own that answers for the fixture's one rack channel: GO claims
    the channel and arms on its track with no file; the launch is the gate,
    over the cue's fade-in; a second cue on a held channel waits with no track
    and takes it when the first ends; what would fail one fails it in words;
    Esc lets the tail ring and a kill does not; and a fade that stops it
    closes its input and leaves its level where it was.

    AND WHERE IT STANDS IN THE SHOW (stage 9b.6): armed ahead at standby, as a
    media cue is; kept sounding through a jump rather than relaunched, having
    no offset to be put at; asserted in the persistent section, silenced by a
    double Esc and back at the next GO; and trimmed by the DCA it is marked
    with.
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
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace wfg;

namespace
{
    juce::File micBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/mic" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    /*  The fixture: list MC000001 holding mic cue MC000002 ("Voix solo", on
        input MC000021, through channel MC000011 "Vox 1", mono to stereo, with
        its test-gain plugin MC000012 switched in as Fx MC000005) and memo
        MC000006; a stereo input MC000022 "Keys"; bus MC000004. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (micBundle(), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick++);
        }

        bool applied (const std::string& command, std::vector<osc::Value> args = {})
        {
            return apply (command, std::move (args)).applied == 1;
        }

        std::string at (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        bool exists (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return snapshot->find (address) != nullptr;
        }

        /*  What `wfg validate` says that mentions this text: the errors and
            the warnings both, as the verb prints them. */
        std::vector<std::string> problemsAbout (const std::string& text) const
        {
            std::vector<std::string> out;

            for (const auto& said : { document.validate(), document.warnings() })
                for (const auto& problem : said)
                    if (problem.find (text) != std::string::npos)
                        out.push_back (problem);

            return out;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::Focus focus;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
    };

    osc::Value text (const char* value) { return osc::Value::string (value); }
    osc::Value text (const std::string& value) { return osc::Value::string (value); }
}

//==============================================================================
TEST_CASE ("mic: a mic cue is a cue, then a sound, then a live input - and nothing about a file")
{
    Rig rig;

    /*  THE FIXTURE AS PUBLISHED: its kind, its own three rows, and a media
        cue's sound rows at their defaults. */
    const std::string cue = "/godot/cue/MC000002/";
    CHECK (rig.at (cue + "kind") == "mic");
    CHECK (rig.at (cue + "input") == "MC000021");
    CHECK (rig.at (cue + "channel") == "MC000011");
    CHECK (rig.at (cue + "fadeIn") == "0.5");
    CHECK (rig.at (cue + "level") == "0");
    CHECK (rig.at (cue + "eqB1Freq") == "100");
    CHECK (rig.exists (cue + "directOut"));
    CHECK (rig.exists (cue + "sends"));

    for (const auto* fileRow : { "file", "startOffset", "channels", "duration", "hash", "initialLevel", "strip" })
    {
        INFO (fileRow);
        CHECK_FALSE (rig.exists (cue + fileRow));
    }

    /*  MADE AS ANY CUE IS, from the new-cue bar's command: empty input and
        channel, which is what a new mic cue starts as, and a fade-in of nought. */
    REQUIRE (rig.applied ("cue.create", { text ("MC000001"), osc::Value::int32 (2), text ("mic"),
                                          text ("Keys"), text ("MC000030") }));

    const std::string made = "/godot/cue/MC000030/";
    CHECK (rig.at (made + "kind") == "mic");
    CHECK (rig.at (made + "input").empty());
    CHECK (rig.at (made + "channel").empty());
    CHECK (rig.at (made + "fadeIn") == "0");

    REQUIRE (rig.applied ("node.set", { text (made + "input"), text ("MC000022") }));
    REQUIRE (rig.applied ("node.set", { text (made + "channel"), text ("MC000011") }));
    REQUIRE (rig.applied ("node.set", { text (made + "fadeIn"), text ("2") }));
    CHECK (rig.at (made + "input") == "MC000022");
    CHECK (rig.at (made + "fadeIn") == "2");

    /*  THE CHILDREN A SOUND IS MADE OF, and none of those that name a file. */
    CHECK (rig.applied ("route.create", { text ("MC000030"), text ("MC000004") }));
    CHECK (rig.applied ("send.create", { text ("MC000030"), text ("MC000004") }));
    CHECK_FALSE (rig.applied ("range.create", { text ("MC000030"), osc::Value::float64 (0.0),
                                                osc::Value::float64 (1.0) }));

    /*  Its EQ is a sound row like the rest, and eq.reset takes it back to flat. */
    REQUIRE (rig.applied ("node.set", { text (made + "eqB2Gain"), text ("6") }));
    CHECK (rig.at (made + "eqB2Gain") == "6");
    CHECK (rig.applied ("eq.reset", { text ("MC000030") }));
    CHECK (rig.at (made + "eqB2Gain") == "0");
}

TEST_CASE ("mic: its inserts are its channel's plugins, never the set's - and a media cue's never a channel's")
{
    Rig rig;

    /*  The fixture's insert names the channel's plugin, and is published with
        that plugin's name and its place in the channel's chain. */
    CHECK (rig.at ("/godot/cue/MC000002/fx") == "MC000005");
    CHECK (rig.at ("/godot/fx/MC000005/name") == "Test gain");
    CHECK (rig.at ("/godot/fx/MC000005/index") == "0");

    /*  A second plugin on Vox 1, one in the show's set, and a media cue. */
    REQUIRE (rig.applied ("channel.plugin", { text ("MC000011"), text ("Verb"), text ("godot:test-gain"),
                                              text ("VST3"), text (""), text ("MC000013") }));
    REQUIRE (rig.applied ("plugin.create", { text ("Set gain"), text ("godot:test-gain"), text ("VST3"),
                                             text (""), text ("MC000040") }));
    REQUIRE (rig.applied ("cue.create", { text ("MC000001"), osc::Value::int32 (2), text ("media"),
                                          text ("A file"), text ("MC000041") }));

    /*  THE CHANNEL'S SECOND PLUGIN: taken, at its place in the channel's chain. */
    REQUIRE (rig.applied ("fx.create", { text ("MC000002"), text ("MC000013"), text ("MC000050") }));
    CHECK (rig.at ("/godot/fx/MC000050/index") == "1");
    CHECK (rig.at ("/godot/cue/MC000002/fx") == "MC000005 MC000050");

    /*  THE SET'S PLUGIN ON A MIC CUE, and the channel's on a media cue: each is
        the wrong kind of plugin for that cue, bad-value, and nothing is made. */
    const auto setOnMic = rig.apply ("fx.create", { text ("MC000002"), text ("MC000040") });
    CHECK (setOnMic.applied == 0);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);

    const auto channelOnMedia = rig.apply ("fx.create", { text ("MC000041"), text ("MC000013") });
    CHECK (channelOnMedia.applied == 0);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);

    /*  And the set's plugin on the media cue, as ever. */
    CHECK (rig.applied ("fx.create", { text ("MC000041"), text ("MC000040") }));

    /*  A MIC CUE WHOSE CHANNEL CHANGES: its insert still names the plugin it
        named, which is no longer on its channel, and so names nothing - its
        name and index are empty rather than another channel's. */
    REQUIRE (rig.applied ("channel.create", { text ("stereo"), text ("MC000060") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/channel"), text ("MC000060") }));
    CHECK (rig.at ("/godot/cue/MC000002/fx").empty());
    CHECK (rig.at ("/godot/fx/MC000005/name").empty());
}

TEST_CASE ("mic: wfg validate says, before the show, each reason a mic cue would fail when fired")
{
    Rig rig;

    /*  The fixture is sound. */
    CHECK (rig.problemsAbout ("Mic[").empty());

    const auto set = [&rig] (const std::string& row, const std::string& value)
    {
        REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/" + row), text (value) }));
    };

    SUBCASE ("no input, no channel")
    {
        set ("input", "");
        set ("channel", "");

        const auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 2u);
        CHECK (said[0].find ("takes no input") != std::string::npos);
        CHECK (said[1].find ("plays through no rack channel") != std::string::npos);
    }

    SUBCASE ("a shared channel")
    {
        REQUIRE (rig.applied ("node.set", { text ("/godot/slot/MC000011/access"), text ("shared") }));

        const auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("Vox 1, a shared channel") != std::string::npos);
        CHECK (said[0].find ("bad-channel") != std::string::npos);
    }

    SUBCASE ("an input its channel cannot take")
    {
        //  Keys is stereo; Vox 1 is mono to stereo and takes one channel.
        set ("input", "MC000022");

        auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("takes Keys, stereo, through Vox 1, which takes mono") != std::string::npos);
        CHECK (said[0].find ("bad-width") != std::string::npos);

        //  A stereo channel takes it; the mono voice then does not fit.
        REQUIRE (rig.applied ("node.set", { text ("/godot/slot/MC000011/class"), text ("stereo") }));
        CHECK (rig.problemsAbout ("Mic[MC000002]").empty());

        set ("input", "MC000021");
        said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("takes Voix solo, mono, through Vox 1, which takes stereo") != std::string::npos);
    }

    SUBCASE ("an input or a channel that is the wrong kind of thing")
    {
        set ("input", "MC000004");
        set ("channel", "MC000021");

        const auto said = rig.problemsAbout ("[MC000002]");
        CHECK (std::any_of (said.begin(), said.end(), [] (const std::string& problem)
                            { return problem.find ("@input: names \"MC000004\", which is a bus and not a input")
                                       != std::string::npos; }));
        CHECK (std::any_of (said.begin(), said.end(), [] (const std::string& problem)
                            { return problem.find ("@channel: names \"MC000021\", which is a input and not a"
                                                   " rackChannel") != std::string::npos; }));
    }
}

TEST_CASE ("mic: a mic cue's direct out is let go with the bus it named, as a media cue's is")
{
    Rig rig;

    REQUIRE (rig.applied ("bus.create", { text ("direct"), osc::Value::int32 (1), osc::Value::int32 (-1),
                                          text ("MC000070") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/directOut"), text ("MC000070") }));
    CHECK (rig.at ("/godot/cue/MC000002/directOut") == "MC000070");

    REQUIRE (rig.applied ("bus.delete", { text ("MC000070") }));
    CHECK (rig.at ("/godot/cue/MC000002/directOut").empty());
}

//==============================================================================
namespace
{
    /*  The audio side for a show with two voices and the fixture's rack
        channel, Vox 1, built as track 2. Arms complete when the test says
        so, as the disk answers in GoTests; a stop ends the sound at once - a
        tail that rings out in no time - and every live door is recorded. */
    struct MicPlayer final : cue::Player
    {
        int trackCount() const override              { return 2; }
        int slotCount() const override               { return 1; }
        int channelsPerTrack() const override        { return 2; }
        int blockSize() const override               { return 128; }
        int sampleRate() const override              { return 48000; }
        std::int64_t samplesElapsed() const override { return samples; }

        void requestArm (const cue::ArmRequest& request) override  { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t sample) override
        {
            launches.push_back ({ track, sample });
            playing.insert (track);
            return true;
        }

        bool stop (int track) override
        {
            stops.push_back (track);

            /*  A real rack channel goes on sounding after a stop while its
                plugins ring out (decision CG); `ringsOnStop` lets a test keep
                it sounding, as HostPlayer's isPlaying does. */
            if (! (ringsOnStop && isRack (track)))
                playing.erase (track);

            return true;
        }

        bool isRack (int track) const
        {
            return std::any_of (rack.begin(), rack.end(),
                                [track] (const auto& entry) { return entry.second == track; });
        }

        bool stopAtSample (int track, int, std::int64_t) override  { return stop (track); }
        void setLevelDb (int track, double levelDb) override       { levels[track] = levelDb; }
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override                  { return playing.count (track) > 0; }
        bool isArmReady (int track) const override                 { return ready.count (track) > 0; }

        int rackTrackOf (const std::string& channelId) const override
        {
            const auto found = rack.find (channelId);
            return found != rack.end() ? found->second : -1;
        }

        bool openLive (int track, std::int64_t sample, double fadeIn) override
        {
            opens.push_back ({ track, sample, fadeIn });
            playing.insert (track);
            return true;
        }

        void shutLive (int track, double seconds) override  { shuts.push_back ({ track, seconds }); }

        bool kill (int track) override
        {
            kills.push_back (track);
            playing.erase (track);
            return true;
        }

        /*  A double Esc's sweep of Go.dot's own effects (namespace draft
            §23.6), counted. */
        void resetEffects (const std::vector<int>&) override
        {
            ++effectResets;
        }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            armed.insert (armed.end(), arms.begin(), arms.end());
            arms.clear();
        }

        std::map<std::string, int> rack { { "MC000011", 2 } };
        std::int64_t samples = 0;
        bool ringsOnStop = false;

        std::vector<cue::ArmRequest> arms, armed;
        std::vector<std::pair<int, std::int64_t>> launches;
        std::vector<std::tuple<int, std::int64_t, double>> opens;
        std::vector<std::pair<int, double>> shuts;
        std::vector<int> stops, kills;
        std::map<int, double> levels;
        std::set<int> playing, ready;

        int effectResets = 0;
    };

    struct RunRig
    {
        RunRig()
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (micBundle(), document).ok);

            /*  A script presses GO here, as fast as the ticks come; a show that
                says nothing refuses a GO inside half a second (2026-09-28). */
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setDcas (&dcas);
            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            audio.samples += 960;
            return result;
        }

        Engine::TickResult submitAndTick (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return tickOnce();
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
            submitAndTick ("cue.fire", { osc::Value::string (cueId) });
            audio.completeArms (engine);
            tickUntil ([this, &cueId] { const auto* run = runOf (cueId); return run != nullptr && run->launchedAtSample > 0; });
            return runOf (cueId);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (19) };
        cue::Runner runner { document, runs, runIds, focus };
        MicPlayer audio;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("mic: GO claims the channel, arms on its track with no file, and the launch opens the gate over the fade-in")
{
    RunRig rig;

    const auto* run = rig.fireAndLaunch ("MC000002");
    REQUIRE (run != nullptr);

    CHECK (run->track == 2);
    CHECK (std::find (run->claims.begin(), run->claims.end(), "MC000011") != run->claims.end());
    CHECK (run->pending.empty());
    CHECK (run->error.empty());

    REQUIRE (rig.audio.armed.size() == 1u);
    const auto& arm = rig.audio.armed.front();
    CHECK (arm.live);
    CHECK (arm.track == 2);
    CHECK (arm.mediaFile.empty());
    CHECK (arm.firstInput == 0);
    CHECK (arm.inputWidth == 1);
    CHECK (arm.ranges.empty());

    /*  ITS INSERTS ARE ITS CHANNEL'S: one slot, the test gain, switched in. */
    REQUIRE (arm.fx.size() == 1u);
    CHECK (arm.fx[0].fxId == "MC000005");
    CHECK (arm.fx[0].enabled);

    /*  THE LAUNCH IS THE GATE, at the launch's own sample, over half a second. */
    REQUIRE (rig.audio.opens.size() == 1u);
    CHECK (std::get<0> (rig.audio.opens.front()) == 2);
    CHECK (std::get<1> (rig.audio.opens.front()) == run->launchedAtSample);
    CHECK (std::get<2> (rig.audio.opens.front()) == doctest::Approx (0.5));
    CHECK (rig.audio.launches.empty());
    CHECK (run->state == cue::runState::playing);
}

TEST_CASE ("mic: a cue on a held channel waits with no track, says so, and takes the channel when it frees")
{
    RunRig rig;

    //  A second mic cue on Vox 1, taking the same voice.
    REQUIRE (rig.document.createCue ("MC000001", 1, "mic", "Voix deux", "MC000031").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000031/input", "MC000021").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000031/channel", "MC000011").ok);

    /*  IDS AND NOT POINTERS across a GO: a new run can move the table's
        storage, and a pointer held over it points at nothing. */
    const auto* first = rig.fireAndLaunch ("MC000002");
    REQUIRE (first != nullptr);
    REQUIRE (first->track == 2);
    const auto firstId = first->id;

    rig.submitAndTick ("cue.fire", { osc::Value::string ("MC000031") });
    rig.tickOnce();

    const auto* second = rig.runOf ("MC000031");
    REQUIRE (second != nullptr);

    /*  WAITING: armed, no track, the channel in its queue - and nothing asked
        of the audio side, which would have moved the sounding cue's voice. */
    CHECK (second->track < 0);
    CHECK (second->state == cue::runState::armed);
    CHECK (second->pending == std::vector<std::string> { "MC000011" });
    CHECK (rig.audio.arms.empty());
    CHECK (rig.audio.opens.size() == 1u);

    /*  THE FIRST ENDS - a stop, and a tail that rings out at once - and the
        second takes the channel through `run.arm`, arms on the track and
        launches. */
    rig.submitAndTick ("run.stop", { osc::Value::string (firstId) });

    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.arms.empty(); }));
    rig.audio.completeArms (rig.engine);

    REQUIRE (rig.tickUntil ([&rig] { return rig.audio.opens.size() == 2u; }));

    second = rig.runOf ("MC000031");
    REQUIRE (second != nullptr);
    CHECK (second->track == 2);
    CHECK (second->pending.empty());
    CHECK (std::find (second->claims.begin(), second->claims.end(), "MC000011") != second->claims.end());
    CHECK (rig.runOf ("MC000002")->isFinished());
}

TEST_CASE ("mic: what would stop a mic cue fails its run in words")
{
    RunRig rig;

    const auto errorOf = [&rig] (const std::string& cueId)
    {
        rig.submitAndTick ("cue.fire", { osc::Value::string (cueId) });
        rig.tickUntil ([&rig, &cueId] { const auto* run = rig.runOf (cueId); return run != nullptr && run->isFinished(); }, 20);
        const auto* run = rig.runOf (cueId);
        return run != nullptr ? run->error : std::string ("(no run)");
    };

    const auto make = [&rig] (const char* id, const char* input, const char* channel)
    {
        REQUIRE (rig.document.createCue ("MC000001", 2, "mic", id, id).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + std::string (id) + "/input", input).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + std::string (id) + "/channel", channel).ok);
    };

    make ("MC000040", "", "MC000011");
    CHECK (errorOf ("MC000040") == "no-input");

    //  Keys is stereo, and Vox 1 takes one channel.
    make ("MC000041", "MC000022", "MC000011");
    CHECK (errorOf ("MC000041") == "bad-width");

    make ("MC000042", "MC000021", "");
    CHECK (errorOf ("MC000042") == "bad-channel");

    //  A channel declared since the graph was built, until Load now.
    REQUIRE (rig.document.createRackChannel ("mono", "MC000050").ok);
    make ("MC000043", "MC000021", "MC000050");
    CHECK (errorOf ("MC000043") == "not-built");

    //  A shared channel is never a mic cue's.
    REQUIRE (rig.document.setAttribute ("/godot/slot/MC000050/access", "shared").ok);
    make ("MC000044", "MC000021", "MC000050");
    CHECK (errorOf ("MC000044") == "bad-channel");
}

TEST_CASE ("mic: Esc lets the tail ring out, and a kill does not")
{
    /*  AND ONLY THE DOUBLE ESC SWEEPS (namespace draft §23.6): Go.dot's own
        effects, every track's, emptied once a press - asked in the press's own
        tick, the kill coming a tick later - and never by Esc or by the pane's
        kill of one cue, which reach that cue alone. */
    SUBCASE ("Esc: a stop, the channel's plugins left to ring")
    {
        RunRig rig;
        const auto* run = rig.fireAndLaunch ("MC000002");
        REQUIRE (run != nullptr);

        rig.submitAndTick ("run.stop", { osc::Value::string (run->id) });
        rig.tickOnce();

        CHECK (rig.audio.stops == std::vector<int> { 2 });
        CHECK (rig.audio.kills.empty());
        CHECK (rig.tickUntil ([&rig] { return rig.runOf ("MC000002")->isFinished(); }, 20));
        CHECK (rig.audio.effectResets == 0);
    }

    SUBCASE ("Esc on every root: no sweep")
    {
        RunRig rig;
        REQUIRE (rig.fireAndLaunch ("MC000002") != nullptr);

        rig.submitAndTick ("run.stopAll");

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (rig.audio.effectResets == 0);
    }

    SUBCASE ("a kill from the running pane: silence, nothing left ringing")
    {
        RunRig rig;
        const auto* run = rig.fireAndLaunch ("MC000002");
        REQUIRE (run != nullptr);

        rig.submitAndTick ("run.kill", { osc::Value::string (run->id) });
        rig.tickOnce();

        CHECK (rig.audio.kills == std::vector<int> { 2 });
        CHECK (rig.audio.stops.empty());
        CHECK (rig.audio.effectResets == 0);
    }

    SUBCASE ("double Esc: the same kill, and Go.dot's own effects swept once")
    {
        RunRig rig;
        REQUIRE (rig.fireAndLaunch ("MC000002") != nullptr);

        rig.submitAndTick ("run.killAll");
        CHECK (rig.audio.effectResets == 1);             // the handler's, in the press's tick
        CHECK (rig.audio.kills.empty());

        rig.tickOnce();
        CHECK (rig.audio.kills == std::vector<int> { 2 });

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (rig.audio.effectResets == 1);
    }
}

namespace
{
    /*  A scene of one mic cue on Vox 1, fired by name and opened: the group
        MC000090 (automatic) holding MC000091, and a stop cue MC000092 aimed at
        the group. The standby is moved off the fixture's own mic cue first, so
        that no arm of its holds the channel. */
    void openScene (RunRig& rig)
    {
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000006").ok);

        REQUIRE (rig.document.createCue ("MC000001", 2, "group", "Scene", "MC000090").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/MC000090/advance", "auto").ok);
        REQUIRE (rig.document.createCue ("MC000090", 0, "mic", "Voix groupe", "MC000091").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/MC000091/input", "MC000021").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/MC000091/channel", "MC000011").ok);

        REQUIRE (rig.document.createCue ("MC000001", 3, "transport", "Scene out", "MC000092").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/MC000092/target", "MC000090").ok);

        rig.tickOnce();
        rig.tickOnce();

        rig.submitAndTick ("cue.fire", { osc::Value::string ("MC000090") });
        REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.arms.empty(); }));
        rig.audio.completeArms (rig.engine);
        REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.opens.empty(); }));
    }
}

TEST_CASE ("mic: Esc on a group lets its mic member's tail ring out, and a double Esc cuts it")
{
    /*  THE SAME PAIR ONE LEVEL IN (namespace draft §23, 2026-09-30). A stopping
        group ended its members with `run.kill`, so a mic cue inside a scene was
        cut dead by Esc - its reverb reset - where Esc on the same cue alone lets
        the tail ring (CG). Now the group stops its members the way it was
        stopped itself: Esc rings out, at the cue's own level, and only a double
        Esc cuts. */
    for (const auto* how : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (how));
        RunRig rig;

        /*  A CUT, so what reaches the voice is the group's stop itself: with a
            panic fade the fade's own end would stop the member either way. */
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);

        openScene (rig);

        const auto cut = std::string (how) == "run.killAll";

        rig.submitAndTick (how);
        REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.stops.empty() || ! rig.audio.kills.empty(); }, 20));

        if (cut)
        {
            CHECK (rig.audio.kills == std::vector<int> { 2 });
            CHECK (rig.audio.stops.empty());
        }
        else
        {
            CHECK (rig.audio.stops == std::vector<int> { 2 });
            CHECK (rig.audio.kills.empty());
            CHECK_FALSE (rig.runOf ("MC000091")->killed);

            /*  AND THE TAIL IS HEARD: the output stays at the cue's level while
                the channel rings, the input shut behind it. */
            CHECK (rig.audio.levels[2] > -60.0);
        }

        CHECK (rig.tickUntil ([&rig] { return rig.runOf ("MC000090")->isFinished(); }));
    }
}

TEST_CASE ("mic: a real double Esc - Esc, then Esc again inside the panic fade - kills a group's mic member")
{
    /*  EVERY DOUBLE ESC IS TWO PRESSES. The client and the D700 send
        `run.stopAll` on the first and `run.killAll` on the second, within 750 ms,
        so the kill lands inside the panic fade Esc began - with the member
        already on its way out, stopped by its group and held by the fade. The
        kill reaches it through its group's job a tick later, and the stop that
        the fade's letting go makes due was issued in between, on the member's
        own mark: a stop, not a kill, and the reverb rang through the press that
        promises everything is cut (CN). Read from above now: a run under a
        killed group is cut however it was asked to stop (namespace draft
        §23.2). */
    RunRig rig;                                          // the show's own panic fade: a second
    openScene (rig);

    rig.submitAndTick ("run.stopAll");

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.audio.stops.empty());                   // still fading
    REQUIRE (rig.audio.kills.empty());

    rig.submitAndTick ("run.killAll");
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.stops.empty() || ! rig.audio.kills.empty(); }, 20));

    CHECK (rig.audio.kills == std::vector<int> { 2 });
    CHECK (rig.audio.stops.empty());
    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("MC000090")->isFinished(); }));
}

TEST_CASE ("mic: a double Esc after Esc's stop has landed still cuts the tail it left ringing - alone or in a group")
{
    /*  THE SECOND PRESS AFTER THE FIRST HAS DONE ITS WORK (namespace draft
        §23.2). With the panic fade at nought - or any fade shorter than the gap
        between the two presses - Esc's stop reaches the mic cue before the
        double Esc does: its input shut, its reverb ringing out, as Esc
        promises (CG). The stop had been issued, and a run whose stop was
        issued was passed over for good, so the kill that followed never
        reached the audio side and the tail rang through the press that
        promises everything is cut (CN). A kill after a stop now goes through,
        once. */
    for (const bool inAGroup : { false, true })
    {
        INFO ((inAGroup ? "a group's member" : "a cue on its own"));
        RunRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);
        rig.audio.ringsOnStop = true;

        const std::string cue = inAGroup ? "MC000091" : "MC000002";

        if (inAGroup)
            openScene (rig);
        else
            REQUIRE (rig.fireAndLaunch (cue) != nullptr);

        rig.submitAndTick ("run.stopAll");
        REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.stops.empty(); }, 20));
        CHECK (rig.audio.kills.empty());

        for (int n = 0; n < 5; ++n)                      // the tail rings on
            rig.tickOnce();

        REQUIRE_FALSE (rig.runOf (cue)->isFinished());

        rig.submitAndTick ("run.killAll");
        CHECK (rig.tickUntil ([&rig] { return ! rig.audio.kills.empty(); }, 20));
        CHECK (rig.audio.kills == std::vector<int> { 2 });
        CHECK (rig.tickUntil ([&rig, &cue] { return rig.runOf (cue)->isFinished(); }, 20));

        for (int n = 0; n < 5; ++n)                      // and only once
            rig.tickOnce();

        CHECK (rig.audio.kills.size() == 1u);
    }
}

TEST_CASE ("mic: a stop cue aimed at a group stops its mic member rather than killing it - behind the group's silenced level")
{
    /*  A STOP, NOT A KILL, which is what the group's graceful stop now sends
        (namespace draft §23.2): the input shut and the channel left to ring.

        WHAT IS HEARD OF IT IS NOTHING, AND THAT IS A KNOWN GAP (§23.2, gap a),
        not this rule: a stop cue aimed at a group moves the GROUP's own level -
        to silence at once for the `hard` verb - and a group's level trims every
        run under it, so the member's output is already at silence when its
        input is shut. Pinned as it is, so that the day the gap is ruled on this
        is the line that says so. */
    RunRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);
    openScene (rig);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("MC000092") });
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.stops.empty() || ! rig.audio.kills.empty(); }, 20));

    CHECK (rig.audio.stops == std::vector<int> { 2 });
    CHECK (rig.audio.kills.empty());
    CHECK_FALSE (rig.runOf ("MC000091")->killed);

    CHECK (rig.audio.levels[2] <= -60.0);               // gap (a): the tail rings behind silence

    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("MC000090")->isFinished(); }));
}

TEST_CASE ("mic: a mic cue in the header of a scene the horizon prepares is armed with its gate shut, and opens at GO")
{
    /*  A PREPARATION IS ANTICIPATION, WHICH NOBODY HEARS (§13.1, §18.5): a mic
        cue made ready is its channel claimed and its plugins set, the gate
        shut. The horizon launched every header cue but a media one, so a mic
        cue in a scene's header opened its gate while the pointer was only
        resting on the scene - an open microphone before any GO, and one no Esc
        was stopping, since what was only made ready is left alone (namespace
        draft §23.3). It is armed now, as a media cue is, and the GO that
        enters the scene opens it. */
    RunRig rig;

    REQUIRE (rig.document.createCue ("MC000001", 2, "group", "Scene", "MC000090").ok);   // manual
    REQUIRE (rig.document.createCue ("MC000090", 0, "memo", "Line", "MC000094").ok);

    const auto header = rig.document.createRole ("MC000090", "header");
    REQUIRE (header.ok);
    REQUIRE (rig.document.createCue (header.id, 0, "mic", "Voix entree", "MC000093").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000093/input", "MC000021").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000093/channel", "MC000011").ok);

    //  The standby on the scene: the horizon prepares it, and the fixture's own mic lets go of Vox 1.
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000090").ok);
    REQUIRE (rig.tickUntil ([&rig] { return rig.runOf ("MC000093") != nullptr; }));
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.arms.empty(); }));
    rig.audio.completeArms (rig.engine);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    //  MADE READY: the channel its own, on its track, and the gate shut.
    const auto* mic = rig.runOf ("MC000093");
    REQUIRE (mic != nullptr);

    const auto micId = mic->id;
    CHECK (mic->state == cue::runState::armed);
    CHECK (mic->track == 2);
    CHECK_FALSE (mic->prepare.empty());
    CHECK (rig.audio.opens.empty());

    //  GO enters the scene, and its header opens that very run.
    rig.submitAndTick ("go");
    CHECK (rig.tickUntil ([&rig] { return ! rig.audio.opens.empty(); }, 20));
    CHECK (rig.runOf ("MC000093")->id == micId);
}

TEST_CASE ("mic: a fade that stops a mic cue closes its input over the fade and leaves its level")
{
    RunRig rig;
    const auto* run = rig.fireAndLaunch ("MC000002");
    REQUIRE (run != nullptr);
    const auto level = run->level;

    //  A stop cue, fading over two seconds, aimed at the mic cue.
    REQUIRE (rig.document.createCue ("MC000001", 2, "transport", "Out", "MC000060").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000060/target", "MC000002").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000060/verb", "fade").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000060/duration", "2").ok);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("MC000060") });

    /*  THE INPUT, over the fade: the level goes into the chain, so the tail
        rings on at the cue's level. */
    REQUIRE (rig.audio.shuts.size() == 1u);
    CHECK (rig.audio.shuts.front().first == 2);
    CHECK (rig.audio.shuts.front().second == doctest::Approx (2.0));

    //  Half way through, the run's level has not moved.
    for (int n = 0; n < 50; ++n)
        rig.tickOnce();

    CHECK (rig.runOf ("MC000002")->level == doctest::Approx (level));
    CHECK (rig.runOf ("MC000002")->state == cue::runState::stopping);

    //  And at its end, the stop that frees the channel once the tail is quiet.
    CHECK (rig.tickUntil ([&rig] { return rig.runOf ("MC000002")->isFinished(); }, 120));
    CHECK (rig.audio.stops == std::vector<int> { 2 });
}

TEST_CASE ("mic: at standby a mic cue is armed ahead - its channel claimed, the gate shut - and GO opens it")
{
    RunRig rig;

    //  The fixture parks the standby on the mic cue; a tick lets the pointer's arm happen.
    rig.tickOnce();
    rig.tickOnce();

    const auto* armed = rig.runOf ("MC000002");
    REQUIRE (armed != nullptr);
    CHECK (armed->state == cue::runState::armed);
    CHECK (armed->track == 2);
    CHECK_FALSE (armed->prepare.empty());
    CHECK (std::find (armed->claims.begin(), armed->claims.end(), "MC000011") != armed->claims.end());
    REQUIRE (rig.audio.arms.size() == 1u);
    CHECK (rig.audio.arms.front().live);
    CHECK (rig.audio.opens.empty());

    const auto armedId = armed->id;
    rig.audio.completeArms (rig.engine);
    rig.submitAndTick ("go");
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.opens.empty(); }));

    //  The run the pointer armed is the one that opens: no second arm.
    CHECK (rig.runOf ("MC000002")->id == armedId);
    CHECK (rig.audio.armed.size() == 1u);
}

TEST_CASE ("mic: a jump past a sounding mic cue keeps it sounding rather than relaunching it")
{
    RunRig rig;

    const auto* run = rig.fireAndLaunch ("MC000002");
    REQUIRE (run != nullptr);
    const auto micRun = run->id;

    //  Aimed at the memo after it: the mic was fired before and never stopped.
    REQUIRE (rig.engine.submit ("cli", "list.aim", { osc::Value::string ("MC000001"), osc::Value::string ("MC000006"),
                                                     osc::Value::float64 (0.0) }));
    rig.tickOnce();
    REQUIRE (rig.submitAndTick ("list.loadToTime", { osc::Value::string ("MC000001") }).applied == 1);
    rig.tickOnce();

    /*  THE SAME RUN, STILL SOUNDING: not ended, not stopped on the audio side,
        not armed a second time, the channel still its. */
    const auto* kept = rig.runs.find (micRun);
    REQUIRE (kept != nullptr);
    CHECK_FALSE (kept->isFinished());
    CHECK (kept->state == cue::runState::playing);
    CHECK (rig.audio.stops.empty());
    CHECK (rig.audio.kills.empty());
    CHECK (rig.audio.armed.size() == 1u);
    CHECK (rig.runs.holderOf ("MC000011") == kept);
}

TEST_CASE ("mic: in the persistent section a mic cue is asserted, silenced by a double Esc, and back at the next GO")
{
    RunRig rig;

    const auto section = rig.document.createPersistent ("MC000001", "MC000070");
    REQUIRE (section.ok);
    REQUIRE (rig.document.createCue ("MC000070", 0, "mic", "Ambient mic", "MC000071").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000071/input", "MC000021").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000071/channel", "MC000011").ok);

    /*  A step: a GO on the memo, which is what makes the section check. */
    const auto step = [&rig]
    {
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000006").ok);
        rig.tickOnce();
        rig.submitAndTick ("go");

        for (int n = 0; n < 5; ++n)
        {
            rig.tickOnce();
            rig.audio.completeArms (rig.engine);
        }

        rig.tickUntil ([&rig] { const auto* run = rig.runs.liveRunOf ("MC000071");
                                return run != nullptr && run->state == cue::runState::playing; }, 50);
    };

    step();

    const auto* bed = rig.runs.liveRunOf ("MC000071");
    REQUIRE (bed != nullptr);
    CHECK (bed->asserted);
    CHECK (bed->state == cue::runState::playing);
    const auto first = bed->id;

    /*  A DOUBLE ESC SILENCES IT - a kill, nothing left ringing - and does not
        suspend it. */
    rig.submitAndTick ("run.killAll");
    rig.tickUntil ([&rig, &first] { return rig.runs.find (first)->isFinished(); }, 20);
    CHECK (rig.runs.find (first)->isFinished());
    CHECK (rig.audio.kills == std::vector<int> { 2 });

    step();

    const auto* again = rig.runs.liveRunOf ("MC000071");
    REQUIRE (again != nullptr);
    CHECK (again->id != first);
    CHECK (again->asserted);
    CHECK (again->state == cue::runState::playing);
}

TEST_CASE ("mic: in the persistent section Esc brings a mic cue down, and the next GO relaunches it, even inside the fade")
{
    /*  K8 (2026-10-02): Esc on a persistent MEDIA cue is a pause; a mic cue has
        no second to remember, so its resume is a relaunch, as before. A GO
        inside Esc's fade found the old run still on its way out and asserted
        nothing until the step after; the pass now owes the cue and relaunches
        it once the fade has ended. */
    RunRig rig;

    const auto section = rig.document.createPersistent ("MC000001", "MC000070");
    REQUIRE (section.ok);
    REQUIRE (rig.document.createCue ("MC000070", 0, "mic", "Ambient mic", "MC000071").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000071/input", "MC000021").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000071/channel", "MC000011").ok);

    const auto go = [&rig]
    {
        REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000006").ok);
        rig.tickOnce();
        rig.submitAndTick ("go");
    };

    const auto sounding = [&rig]
    {
        for (int n = 0; n < 5; ++n)
        {
            rig.tickOnce();
            rig.audio.completeArms (rig.engine);
        }

        rig.tickUntil ([&rig] { const auto* run = rig.runs.liveRunOf ("MC000071");
                                return run != nullptr && run->state == cue::runState::playing; }, 80);
    };

    go();
    sounding();

    const auto* bed = rig.runs.liveRunOf ("MC000071");
    REQUIRE (bed != nullptr);
    REQUIRE (bed->state == cue::runState::playing);
    const auto first = bed->id;

    rig.submitAndTick ("run.stopAll");

    auto insideFade = false;

    SUBCASE ("the next GO once the fade has ended")
    {
        rig.tickUntil ([&rig, &first] { return rig.runs.find (first)->isFinished(); }, 80);
        REQUIRE (rig.runs.find (first)->isFinished());
    }

    SUBCASE ("the next GO inside the fade") { insideFade = true; }

    go();

    if (insideFade)
    {
        REQUIRE_FALSE (rig.runs.find (first)->isFinished());
        rig.tickUntil ([&rig, &first] { return rig.runs.find (first)->isFinished(); }, 80);
    }

    sounding();

    const auto* again = rig.runs.liveRunOf ("MC000071");
    REQUIRE (again != nullptr);
    CHECK (again->id != first);
    CHECK (again->asserted);
    CHECK (again->state == cue::runState::playing);
    CHECK (rig.audio.kills.empty());
}

TEST_CASE ("mic: the DCA a mic cue is marked with trims it")
{
    RunRig rig;

    REQUIRE (rig.document.createDca ("Voices", "MC000080").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000002/dca", "MC000080").ok);

    const auto* run = rig.fireAndLaunch ("MC000002");
    REQUIRE (run != nullptr);
    const auto id = run->id;
    const auto before = run->level;

    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/dca/MC000080/trim"),
                                              osc::Value::float64 (-6.0) }).applied == 1);

    for (int n = 0; n < 3; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (id)->level == doctest::Approx (before - 6.0));
}

TEST_CASE ("go.doh: a scene taken back runs no footer, and its mic member's tail rings")
{
    /*  PRD §3.32 (D1, 2026-10-01): Doh! brings what the GO started down the
        way Esc would - a mic's input shut, its reverb left to ring - and runs no
        footer, because a Doh is a pause and not an end. Never a kill: a naive
        Doh that marked the scene `skipFooter` would have cut the tail dead.
        Failed before D1: `go.doh` was an unknown command. */
    RunRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/audio/panicFade", "0").ok);
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000006").ok);

    REQUIRE (rig.document.createCue ("MC000001", 2, "group", "Scene", "MC000090").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000090/advance", "auto").ok);
    REQUIRE (rig.document.createCue ("MC000090", 0, "mic", "Voix groupe", "MC000091").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000091/input", "MC000021").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/MC000091/channel", "MC000011").ok);

    const auto footer = rig.document.createRole ("MC000090", "footer");
    REQUIRE (footer.ok);
    REQUIRE (rig.document.createCue (footer.id, 0, "memo", "Release", "MC000095").ok);

    rig.tickOnce();
    REQUIRE (rig.document.setAttribute (cue::standbyAddressOf ("MC000001"), "MC000090").ok);
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE (rig.submitAndTick ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.arms.empty(); }));
    rig.audio.completeArms (rig.engine);
    REQUIRE (rig.tickUntil ([&rig] { return ! rig.audio.opens.empty(); }));
    rig.tickOnce();

    /*  The GO's own runs, named: once the pointer is back on the scene the
        horizon makes it ready again, and the newest run of either cue is that
        preparation's. */
    const auto scene = rig.runOf ("MC000090")->id;
    const auto voice = rig.runOf ("MC000091")->id;

    REQUIRE (rig.submitAndTick ("go.doh").rejected == 0);
    REQUIRE (rig.tickUntil ([&rig, &scene] { return rig.runs.find (scene)->isFinished(); }, 60));

    CHECK (rig.runOf ("MC000095") == nullptr);                 // no footer
    CHECK (rig.audio.stops == std::vector<int> { 2 });         // its input shut, the tail left to ring
    CHECK (rig.audio.kills.empty());
    CHECK (rig.runs.find (voice)->takenBack);
    CHECK_FALSE (rig.runs.find (voice)->killed);
}
