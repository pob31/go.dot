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
    What a publish costs the tick (namespace draft §31.5, M62), and that what it
    publishes is still the table's and the file's.

    THE TICK PUBLISHES EVERY TWENTY MILLISECONDS, and a publish that takes
    longer than a tick is a clock that never catches up: Windows CI went red
    three runs in a row on `blackbox.first-sound` for exactly that, in the
    Debug build it runs, until the sort was mended (2026-10-11, ae496b3). So
    the cost is measured here on the show that driver plays - the phase2
    bundle, wired as `wfg tree` wires it, with the installed presets and five
    runs going - and on M18's five hundred cues, idle and after an edit.

    REPORTED AND NEVER ASSERTED, for the reason M18 gives: a wall clock on a
    shared runner is a flaky test that teaches people to re-run the suite.
    What the split between a node and its shape guarantees is checked by the
    second case, which asks the published nodes what they say and compares it
    with the rows and the file they came from.

    A serialisation surface, so both cases run under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/audio/EditRenderer.h>
#include <wfg/engine/audio/MediaAnalyser.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/CurveCommands.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneCommands.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/TakeCommands.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/SectionCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/process/PatchEditor.h>
#include <wfg/engine/surface/SurfaceCommands.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/tree/AuthoringCommands.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/PresetCommands.h>
#include <wfg/engine/tree/PresetTable.h>
#include <wfg/engine/tree/RawSender.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/video/Conversion.h>
#include <wfg/engine/video/FfmpegInstall.h>
#include <wfg/engine/video/VideoCommands.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    juce::File bundleNamed (const char* name)
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/" + name };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    /*  M18'S SHOW (AnalysisTests.cpp): five hundred media cues over twenty
        slots of one device, built straight into the document as M18 builds
        it, since five hundred trips through the door measure the door. */
    void buildFiveHundredCues (doc::ShowDocument& document)
    {
        //  Seeded, so the identifiers - and the order they sort the tree in - are the same every run.
        document.ids() = doc::IdRegistry::withSeed (18);

        const auto listId = document.createList ("Main").id;
        const auto mount = document.createMount ("/wfs", "namespaces/wfs.json");
        REQUIRE (mount.ok);
        document.findById (mount.id).setProperty (juce::Identifier ("port"), 9000, nullptr);

        auto audio = document.root().getChildWithName ("Audio");
        REQUIRE (audio.isValid());

        juce::ValueTree bus { "Bus" };
        bus.setProperty (juce::Identifier ("id"), "BS000001", nullptr);
        bus.setProperty (juce::Identifier ("name"), "WFS send", nullptr);
        bus.setProperty (juce::Identifier ("width"), 12, nullptr);
        audio.addChild (bus, -1, nullptr);

        std::vector<std::string> slots;

        for (int n = 0; n < 20; ++n)
        {
            const auto slot = document.createSlot (mount.id, "/wfs/input/" + std::to_string (10 + n));
            REQUIRE (slot.ok);

            auto node = document.findById (slot.id);
            node.setProperty (juce::Identifier ("bus"), "BS000001", nullptr);
            node.setProperty (juce::Identifier ("width"), 1, nullptr);
            node.setProperty (juce::Identifier ("firstChannel"), n, nullptr);
            slots.push_back (slot.id);
        }

        auto list = document.findById (listId);
        REQUIRE (list.isValid());

        for (int n = 0; n < 500; ++n)
        {
            juce::ValueTree media { "Media" };
            media.setProperty (juce::Identifier ("id"), juce::String ("MA" + std::to_string (100000 + n)), nullptr);
            media.setProperty (juce::Identifier ("name"), juce::String ("Cue ") + juce::String (n), nullptr);
            media.setProperty (juce::Identifier ("file"), "tone.wav", nullptr);

            juce::ValueTree feed { "Feed" };
            feed.setProperty (juce::Identifier ("id"), juce::String ("FA" + std::to_string (100000 + n)), nullptr);
            feed.setProperty (juce::Identifier ("slot"), juce::String (slots[static_cast<std::size_t> (n % 20)]), nullptr);
            feed.setProperty (juce::Identifier ("gains"), "1", nullptr);
            media.addChild (feed, -1, nullptr);

            list.addChild (media, -1, nullptr);
        }
    }

    /*  THE TREE AS `wfg tree` WIRES IT - every command family registered, the
        bundle's devices loaded - with what serve adds that a dump has not:
        the installed presets (twenty-four of them, three hundred rows), the
        list state, the media records and five runs going. */
    struct ServedTree
    {
        explicit ServedTree (const juce::File& bundle)
            : folder (bundle)
        {
            if (folder.isDirectory())
                REQUIRE (doc::Bundle::open (folder, document).ok);
            else
                buildFiveHundredCues (document);

            takes.setQueueing (false);
            runner.setDcas (&dcas);
            runner.setTakes (&takes);
            runner.setLanes (&lanes);
            runner.setCurves (&curveTable);

            auto& registry = engine.commands();
            doc::registerDocumentCommands (registry, document);
            cue::registerCueCommands (registry, document, focus);
            surface::registerSurfaceCommands (registry, document, surfaceTable);
            cue::registerLiveCommands (registry, document, liveEdits);
            cue::registerRunCommands (registry, runs, [this] { audio::stopOutputTest (audioState); });
            cue::registerTakeCommands (registry, takes, runs, document, &runner.runNames());
            cue::registerGoCommands (registry, engine, runner, document, focus, runIds);
            cue::registerLaneCommands (registry, engine, runner, document, lanes);
            cue::registerCurveCommands (registry, engine, runner, document, curveTable);
            video::registerVideoCommands (registry, nullptr);
            video::registerConversionCommands (registry, document, nullptr, &runner.runNames());
            audio::registerAnalyserCommands (registry, nullptr);
            doc::registerSectionCommands (registry, document);
            audio::registerEditRenderCommands (registry, nullptr, document);
            video::ffmpeg::registerInstallCommands (registry, nullptr);
            process::editor::registerCommands (registry, document, nullptr, nullptr);

            {
                plugin::PluginCommandHooks hooks;
                hooks.knows = plugin::pluginKnownBy (document);
                hooks.locked = plugin::showLockedBy (document);
                plugin::registerPluginCommands (registry, pluginTable, std::move (hooks));
            }

            tree::registerTreeCommands (registry, touches);
            tree::registerAuthoringCommands (registry, document, answers);
            tree::registerMountCommands (registry, document, mounts, folder);
            tree::registerDescriptionCommands (registry, document, mounts, folder, answers);
            tree::registerPresetCommands (registry, document, mounts, folder, &presets);
            audio::registerAudioCommands (registry, audioState);
            audio::registerAudioSettingsCommands (engine, document, runner, runs, audioState);
            pair::registerPairCommands (registry, pairing);

            if (folder.isDirectory())
                for (const auto& problem : tree::loadAllMountsFromBundle (document, mounts, folder))
                    FAIL ("a device of the fixture did not load: " << problem);

            presets.scanQuickly (std::string (WFG_REPO_ROOT) + "/presets/devices");

            mediaInfo = std::make_unique<audio::MediaInfo> (
                document, folder.isDirectory() ? folder.getChildFile ("media").getFullPathName().toStdString()
                                               : std::string());

            parameters.setMediaInfo (mediaInfo.get());
            parameters.setListState (&runner.listState());
            parameters.setPresets (&presets);
            parameters.setDcas (&dcas);
            parameters.setTakes (&takes);
            parameters.setLanes (&lanes);
            parameters.setCurves (&curveTable);
            parameters.setLiveEdits (&liveEdits);
            parameters.setPlugins (&pluginTable);
            parameters.setSurfaces (&surfaceTable);
            parameters.setListenStatus ([] (const std::string&) { return std::string ("off"); });

            state.version = "test";
            state.documentPath = folder.getFullPathName().toStdString();
            state.documentName = folder.isDirectory() ? folder.getFileName().toStdString() : std::string ("M18");
            state.clock = "device";
            state.sampleRate = 48000;
            state.blockSize = 256;
            state.samplesPerTick = 960;
        }

        /*  FIVE RUNS GOING, on the first cues of the show's first list:
            forty-odd rows each, every tick, which is the runtime half's
            biggest part on a show that is playing. */
        void startRuns()
        {
            std::vector<std::string> cues;

            const auto lists = document.root().getChildWithName ("Lists");

            for (const auto list : lists)
                for (const auto child : list)
                    if (child.hasProperty (juce::Identifier ("id")) && cues.size() < 5)
                        cues.push_back (child[juce::Identifier ("id")].toString().toStdString());

            REQUIRE (! cues.empty());

            for (std::size_t n = 0; n < 5; ++n)
            {
                const auto& cueId = cues[n % cues.size()];
                const auto runId = "RN" + std::to_string (100000 + n);

                runs.create (runId, cueId, "media");

                auto* run = runs.find (runId);
                REQUIRE (run != nullptr);
                run->state = cue::runState::playing;
                run->track = static_cast<int> (n);
                run->media = "tone.wav";
                run->position = 1.25 + static_cast<double> (n);
                run->level = -6.0;
            }
        }

        std::shared_ptr<const tree::TreeSnapshot> publish (std::int64_t tick)
        {
            state.tick = tick;
            state.documentRevision = document.showRevision();
            return parameters.publish (tick, state);
        }

        juce::File folder;
        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        tree::RawSender answers;
        tree::PresetTable presets;
        cue::RunTable runs;
        cue::DcaTable dcas;
        cue::TakeTable takes;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (62);
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        cue::LaneTable lanes;
        cue::CurveTable curveTable;
        audio::AudioState audioState;
        surface::SurfaceTable surfaceTable;
        cue::LiveEdits liveEdits;
        plugin::PluginTable pluginTable;
        pair::Pairing pairing;
        std::unique_ptr<audio::MediaInfo> mediaInfo;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
    };

    struct Timings
    {
        double median = 0.0, least = 0.0, most = 0.0;
    };

    Timings summarise (std::vector<double> samples)
    {
        std::sort (samples.begin(), samples.end());
        return { samples[samples.size() / 2], samples.front(), samples.back() };
    }

    double millisecondsOf (std::chrono::steady_clock::duration duration)
    {
        return std::chrono::duration<double, std::milli> (duration).count();
    }

    /*  `count` publishes, each timed alone; `edited` marks the show half stale
        before each, as an applied command does on the tick thread. */
    Timings timePublishes (ServedTree& served, std::int64_t& tick, int count, bool edited)
    {
        std::vector<double> samples;

        for (int n = 0; n < count; ++n)
        {
            if (edited)
                served.parameters.markStale();

            const auto before = std::chrono::steady_clock::now();
            const auto snapshot = served.publish (tick++);
            samples.push_back (millisecondsOf (std::chrono::steady_clock::now() - before));

            REQUIRE (snapshot != nullptr);
        }

        return summarise (std::move (samples));
    }

    const char* buildName()
    {
       #if defined (NDEBUG)
        return "Release";
       #else
        return "Debug";
       #endif
    }
}

//==============================================================================
TEST_CASE ("M62: what a publish costs the tick, idle and after an edit, reported and not asserted")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto measure = [] (const char* name, const juce::File& bundle)
    {
        ServedTree served { bundle };
        served.startRuns();

        std::int64_t tick = 1;

        const auto firstStart = std::chrono::steady_clock::now();
        const auto first = served.publish (tick++);
        const auto firstCost = millisecondsOf (std::chrono::steady_clock::now() - firstStart);

        //  Warm: the allocator's free lists and every lazily made table.
        timePublishes (served, tick, 10, false);

        const auto idle = timePublishes (served, tick, 100, false);
        const auto edited = timePublishes (served, tick, 20, true);
        const auto nodes = served.publish (tick++)->size();

        MESSAGE ("M62  " << std::string (name) << ", " << std::string (buildName()) << ", " << nodes
                 << " nodes, 5 runs: first publish " << firstCost << " ms; idle publish median " << idle.median
                 << " ms (" << idle.least << " to " << idle.most << "); after markStale median "
                 << edited.median << " ms (" << edited.least << " to " << edited.most << ")");

        CHECK (first->size() > 0);
    };

    measure ("the first-sound fixture (phase2)", bundleNamed ("phase2"));
    measure ("M18's 500 cues", juce::File());
}

//==============================================================================
/*  WHAT A NODE SAYS ABOUT ITSELF IS ITS ROW'S, AND A MOUNTED NODE'S IS ITS
    FILE'S: the description, the unit, the range, an enumeration, the rate
    cap, whether it may be anticipated and what it rests at, read off the
    published node and compared with the table row it was made from - on the
    show half, the runtime half and a run - and with the device's file for a
    node of somebody else's namespace. */
TEST_CASE ("tree: a published node says what its row says, and a mounted node what its file says")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    ServedTree served { bundleNamed ("phase2") };
    served.startRuns();

    const auto snapshot = served.publish (7);

    const auto rowOf = [] (std::string_view owner, std::string_view name) -> const doc::AttributeRow&
    {
        for (const auto* row : doc::Schema::rowsForOwner (owner))
            if (row->name == name)
                return *row;

        FAIL ("no row " << std::string (owner) << "/" << std::string (name));
        throw std::logic_error ("unreachable");
    };

    const auto checkRow = [&snapshot] (const std::string& address, const doc::AttributeRow& row)
    {
        INFO ("at " << address);

        const auto* node = snapshot->find (address);
        REQUIRE (node != nullptr);

        CHECK (node->description() == std::string (row.description));
        CHECK (node->unit() == std::string (row.unit));
        CHECK (node->hasMinimum() == row.hasMin);
        CHECK (node->hasMaximum() == row.hasMax);

        if (row.hasMin)
            CHECK (node->minimum() == doctest::Approx (row.minimum));

        if (row.hasMax)
            CHECK (node->maximum() == doctest::Approx (row.maximum));

        REQUIRE (node->enumValues().size() == row.numEnumValues);

        for (std::size_t i = 0; i < row.numEnumValues; ++i)
            CHECK (node->enumValues()[i] == std::string (row.enumValues[i]));

        CHECK (node->rateCap() == doctest::Approx (row.rateCap));
        CHECK (node->anticipatable() == row.anticipatable);
        CHECK (node->panic() == std::string (row.panic));
        CHECK (node->panicValues().empty());
    };

    //  The show half: a cue's level (a range, a unit) and an OSC cue's wait (an enumeration).
    checkRow ("/godot/cue/P9XKC2WR/level", rowOf ("sound", "level"));
    checkRow ("/godot/cue/M5TQ7XVA/wait", rowOf ("osc", "wait"));

    //  The runtime half: the tick (a unit), the pairing's role (an enumeration).
    checkRow ("/godot/engine/tick", rowOf ("engine", "tick"));
    checkRow ("/godot/pair/role", rowOf ("pair", "role"));

    //  A run's rows, made every tick: its state (an enumeration), its position (a range, a unit).
    checkRow ("/godot/run/RN100000/state", rowOf ("run", "state"));
    checkRow ("/godot/run/RN100004/position", rowOf ("run", "position"));

    //  Two nodes from one row share what the row says, and keep their own values.
    const auto* first = snapshot->find ("/godot/run/RN100000/position");
    const auto* second = snapshot->find ("/godot/run/RN100001/position");
    REQUIRE (first != nullptr);
    REQUIRE (second != nullptr);
    CHECK (first->description() == second->description());
    CHECK (first->values != second->values);

    //  ONE shape, made once for the row and shared - this tick, and the next.
    CHECK (first->shape != nullptr);
    CHECK (first->shape == second->shape);

    const auto later = served.publish (8);
    const auto* again = later->find ("/godot/run/RN100000/position");
    REQUIRE (again != nullptr);
    CHECK (again->shape == first->shape);

    //  A device's node, as its file describes it (namespaces/desk.json).
    const auto* fader = snapshot->find ("/desk/fader");
    REQUIRE (fader != nullptr);
    CHECK (fader->description() == "Grand master, 0 to 1.");
    CHECK (fader->typeTags == "f");
    CHECK (fader->access == tree::Access::readWrite);
    CHECK (fader->hasMinimum());
    CHECK (fader->minimum() == doctest::Approx (0.0));
    CHECK (fader->hasMaximum());
    CHECK (fader->maximum() == doctest::Approx (1.0));
    CHECK (fader->enumValues().empty());

    const auto* scene = snapshot->find ("/desk/scene");
    REQUIRE (scene != nullptr);
    CHECK (scene->description() == "The scene to recall.");
    CHECK (scene->maximum() == doctest::Approx (999.0));

    //  And one of somebody else's nodes that declares a GODOT key (minimal's console).
    ServedTree minimal { bundleNamed ("minimal") };
    const auto other = minimal.publish (1);
    std::size_t declared = 0;

    for (const auto* node : other->all())
        if (node->panic() == "snap")
        {
            ++declared;
            CHECK (node->rateCap() == doctest::Approx (25.0));
            CHECK (node->anticipatable());
        }

    CHECK (declared > 0);
}

//==============================================================================
namespace
{
    /*  A SOUND CARD THAT PLAYS EACH VOICE A WHILE, for M68: an arm is answered
        on the next tick, as a disk answers, and a voice sounds a third of a
        second from its launch and stops on its own, so the Runner sees its
        edge and ends the run as it ends one at a file's end. */
    struct SessionPlayer final : cue::Player
    {
        int trackCount() const override                  { return 16; }
        int slotCount() const override                   { return 1; }
        std::int64_t samplesElapsed() const override     { return samples; }
        int blockSize() const override                   { return 128; }
        int sampleRate() const override                  { return 48000; }
        int channelsPerTrack() const override            { return 2; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t sample) override
        {
            endsAt[track] = sample + voiceSamples;
            return true;
        }

        bool stop (int track) override
        {
            endsAt.erase (track);
            return true;
        }

        bool stopAtSample (int track, int, std::int64_t sample) override
        {
            if (const auto found = endsAt.find (track); found != endsAt.end())
                found->second = std::min (found->second, sample);

            return true;
        }

        void setLevelDb (int, double) override {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}

        bool isPlaying (int track) const override
        {
            const auto found = endsAt.find (track);
            return found != endsAt.end() && samples < found->second;
        }

        bool isArmReady (int track) const override { return ready.count (track) > 0; }

        void answerArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        std::int64_t samples = 0;
        std::int64_t voiceSamples = 16000;
        std::vector<cue::ArmRequest> arms;
        std::map<int, std::int64_t> endsAt;
        std::set<int> ready;
    };

    /*  AN EVENING ON THE FIRST-SOUND SHOW, as the tick thread serves it: the
        Runner's hook, the engine's tick and the publish, in that order, on a
        show that goes on firing. Every eight ticks the next of three GOs - the
        fixture's sound, its fade, and a scene added here (an automatic group of
        a memo, an OSC cue and a fade) - each made ready two ticks before it is
        fired, as a pointer reaches a cue while the operator reads the next
        line. Six runs every 24 ticks, most of them ended within the next 24. */
    struct Evening
    {
        explicit Evening (ServedTree& servedTree)
            : served (servedTree)
        {
            served.engine.log().openInMemory ({});
            served.runner.setPlayer (&player);
            served.runner.setSamplesPerTick (960);

            listId = "7K2QM9X4";
            const auto scene = served.document.createCue (listId, 5, "group", "Scene");
            REQUIRE (scene.ok);
            served.document.setAttribute ("/godot/cue/" + scene.id + "/advance", "auto");

            REQUIRE (served.document.createCue (scene.id, 0, "memo", "Note").ok);

            const auto osc = served.document.createCue (scene.id, 1, "osc", "Desk");
            REQUIRE (osc.ok);
            served.document.setAttribute ("/godot/cue/" + osc.id + "/address", "/desk/fader");
            served.document.setAttribute ("/godot/cue/" + osc.id + "/value", "f:0.5");

            const auto fade = served.document.createCue (scene.id, 2, "fade", "Down");
            REQUIRE (fade.ok);
            served.document.setAttribute ("/godot/cue/" + fade.id + "/target", "B3N8R5TW");
            served.document.setAttribute ("/godot/cue/" + fade.id + "/duration", "0.1");
            served.document.setAttribute ("/godot/cue/" + fade.id + "/level", "-10");

            cycle = { "B3N8R5TW", "E4GP6QSC", scene.id };
        }

        struct Cost
        {
            double before = 0.0, process = 0.0, publish = 0.0;
        };

        Cost step()
        {
            const auto phase = tick % 8;

            if (phase == 0)
                served.document.setAttribute (cue::standbyAddressOf (listId), cycle[next++ % cycle.size()]);
            else if (phase == 2)
                served.engine.submit ("cli", "go", {});

            player.samples = tick * 960;
            player.answerArms (served.engine);

            Cost cost;
            const auto start = std::chrono::steady_clock::now();
            served.runner.beforeTick (served.engine, tick);
            const auto hooked = std::chrono::steady_clock::now();
            served.engine.processTick (tick);
            const auto processed = std::chrono::steady_clock::now();
            const auto snapshot = served.publish (tick);
            const auto published = std::chrono::steady_clock::now();

            REQUIRE (snapshot != nullptr);

            cost.before = millisecondsOf (hooked - start);
            cost.process = millisecondsOf (processed - hooked);
            cost.publish = millisecondsOf (published - processed);
            ++tick;
            return cost;
        }

        /*  THE FINISHED RUNS THE EVENING HAS MADE, copied under new names until
            the table holds `target` runs, as an evening of GOs leaves them:
            each family - a group's run and its members' - keeps its parent and
            children among its own copies, and every copy ended long ago. What
            the tick pays for a run that ended an hour ago is what it pays for
            one of these. */
        void growTo (std::size_t target)
        {
            std::vector<cue::Run> seeds;

            for (const auto& run : served.runs.all())
                if (run.isFinished() && run.id.rfind ("ZZ", 0) != 0)
                    seeds.push_back (run);

            REQUIRE (! seeds.empty());

            while (served.runs.all().size() < target)
            {
                std::map<std::string, std::string> renamed;

                for (const auto& seed : seeds)
                    renamed[seed.id] = "ZZ" + doc::Id::encode (copies++).substr (2);

                for (const auto& seed : seeds)
                {
                    if (served.runs.all().size() >= target)
                        break;

                    auto copy = seed;
                    copy.id = renamed[seed.id];

                    const auto parent = renamed.find (seed.parent);
                    copy.parent = parent != renamed.end() ? parent->second : std::string();

                    copy.children.clear();

                    for (const auto& child : seed.children)
                        if (const auto found = renamed.find (child); found != renamed.end())
                            copy.children.push_back (found->second);

                    copy.endedAtTick = 1;

                    served.runs.create (copy.id, copy.cue, copy.kind);

                    auto* made = served.runs.find (copy.id);
                    REQUIRE (made != nullptr);
                    *made = std::move (copy);
                }
            }
        }

        ServedTree& served;
        SessionPlayer player;
        std::string listId;
        std::vector<std::string> cycle;
        std::size_t next = 0;
        std::int64_t tick = 1;
        std::uint64_t copies = 0;
    };

    double percentileOf (std::vector<double> samples, double fraction)
    {
        std::sort (samples.begin(), samples.end());
        const auto at = static_cast<std::size_t> (fraction * static_cast<double> (samples.size() - 1));
        return samples[at];
    }
}

//==============================================================================
/*  WHAT THE RUNS A SESSION HAS MADE COST THE TICK (namespace draft §31.5, M68).

    M64 (§60.6) ran a time group firing a member every 3 s, and every 1.32 s,
    for many minutes in the Debug build: no fire was late for the first eight
    passes, then late launches grew until nearly every one was, the tick up to
    133 ms behind its audio (400 to 520 ms at 1.32 s). The run table keeps every
    run of the session, so whatever the tick pays for each one grows with the
    evening, whatever the show.

    An evening on the first-sound show (above), and between its stretches its
    finished runs copied until the table holds 100, 1000, 2500 and 5000; each
    stretch times 96 ticks of it going on - the Runner's hook, the engine's tick
    and the publish, each alone. REPORTED AND NEVER ASSERTED, for M18's reason;
    what the mend guarantees is asserted in RunTests, by what the tick visits
    rather than by a clock. */
TEST_CASE ("M68: what the runs a session has made cost the tick, reported and not asserted")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    ServedTree served { bundleNamed ("phase2") };
    Evening evening { served };

    //  The evening's first ten seconds: the runs the copies are made from.
    for (int n = 0; n < 500; ++n)
        evening.step();

    MESSAGE ("M68  " << std::string (buildName()) << ": " << served.runs.all().size()
             << " runs made by the evening's first 500 ticks");

    for (const std::size_t target : { std::size_t { 0 }, std::size_t { 100 }, std::size_t { 1000 },
                                      std::size_t { 2500 }, std::size_t { 5000 } })
    {
        if (target > 0)
            evening.growTo (target);

        for (int n = 0; n < 24; ++n)
            evening.step();

        const auto held = served.runs.all().size();
        std::vector<double> before, process, publish, whole;

        for (int n = 0; n < 96; ++n)
        {
            const auto cost = evening.step();
            before.push_back (cost.before);
            process.push_back (cost.process);
            publish.push_back (cost.publish);
            whole.push_back (cost.before + cost.process + cost.publish);
        }

        MESSAGE ("M68  " << std::string (buildName()) << ", " << held << " runs in the table, "
                 << served.runs.current().size() << " the tick reads: a tick median "
                 << percentileOf (whole, 0.5) << " ms, 90th percentile " << percentileOf (whole, 0.9)
                 << " ms, worst " << percentileOf (whole, 1.0) << " ms; the hook " << percentileOf (before, 0.5)
                 << " ms, the engine's tick " << percentileOf (process, 0.5) << " ms, the publish "
                 << percentileOf (publish, 0.5) << " ms (medians)");
    }

    CHECK (served.runs.all().size() >= 5000u);
}
