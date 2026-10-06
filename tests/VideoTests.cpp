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

/*  PHASE 8a'S VIDEO CUE, CANVASES AND OUTPUTS IN THE DOCUMENT (namespace draft
    35): what the show says about a picture before anything draws it. That a
    video cue is a cue first and a picture second - its source, canvas, layer,
    opacity, colour and fade-in, and nothing of a sound; that a canvas and an
    output are made by their own commands, in containers that appear at a fixed
    place when first wanted, so a show with no picture gains no line; that an
    output names a canvas and nothing else; and that `wfg validate` says, before
    the show, that a video cue on no canvas shows nothing.

    Driven through the engine, because what a replay reproduces is the applied
    record.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/video/VideoSink.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    juce::File videoBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    juce::File noPictureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/mic" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    /*  The fixture: list VD000001 holding video cue VD000002 ("Blue wash", a
        fill of #2040A0 over a second, on canvas VD000011 "Stage") and memo
        VD000003; output VD000021 "Face" showing that canvas. */
    struct Rig
    {
        explicit Rig (const juce::File& bundle = videoBundle())
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (bundle, document).ok);
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

        std::vector<std::string> problemsAbout (const std::string& text) const
        {
            std::vector<std::string> out;

            for (const auto& said : { document.validate(), document.warnings() })
                for (const auto& problem : said)
                    if (problem.find (text) != std::string::npos)
                        out.push_back (problem);

            return out;
        }

        std::vector<std::string> showChildren() const
        {
            std::vector<std::string> out;

            for (const auto& child : document.root())
                out.push_back (child.getType().toString().toStdString());

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
TEST_CASE ("video: a video cue is a cue, then a picture - and nothing of a sound")
{
    Rig rig;

    const std::string cue = "/godot/cue/VD000002/";
    CHECK (rig.at (cue + "kind") == "video");
    CHECK (rig.at (cue + "name") == "Blue wash");
    CHECK (rig.at (cue + "source") == "fill");
    CHECK (rig.at (cue + "canvas") == "VD000011");
    CHECK (rig.at (cue + "layer") == "0");
    CHECK (rig.at (cue + "opacity") == "100");
    CHECK (rig.at (cue + "paint") == "#2040A0");
    CHECK (rig.at (cue + "fadeIn") == "1");

    for (const auto* soundRow : { "level", "directOut", "sends", "eqB1Freq", "input", "strip" })
    {
        INFO (soundRow);
        CHECK_FALSE (rig.exists (cue + soundRow));
    }

    /*  MADE AS ANY CUE IS, born with its source and its canvas - what a line
        of the new-cue list sends - and a fill of black at full opacity on
        layer nought until somebody says otherwise. */
    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (2), text ("video"),
                                          text ("Black plate"), text ("VD000030"),
                                          text ("source"), text ("fill"), text ("canvas"), text ("VD000011") }));

    const std::string made = "/godot/cue/VD000030/";
    CHECK (rig.at (made + "kind") == "video");
    CHECK (rig.at (made + "canvas") == "VD000011");
    CHECK (rig.at (made + "paint") == "#000000");
    CHECK (rig.at (made + "opacity") == "100");
    CHECK (rig.at (made + "fadeIn") == "0");

    REQUIRE (rig.applied ("node.set", { text (made + "layer"), osc::Value::int32 (3) }));
    REQUIRE (rig.applied ("node.set", { text (made + "opacity"), osc::Value::float64 (50.0) }));
    REQUIRE (rig.applied ("node.set", { text (made + "paint"), text ("#FFFFFF") }));
    CHECK (rig.at (made + "layer") == "3");
    CHECK (rig.at (made + "opacity") == "50");
    CHECK (rig.at (made + "paint") == "#FFFFFF");

    /*  NOT A SOUND: no destination, no send, no range. A trigger, as every
        cue has. */
    CHECK_FALSE (rig.applied ("route.create", { text ("VD000030"), text ("VD000004") }));
    CHECK_FALSE (rig.applied ("send.create", { text ("VD000030"), text ("VD000004") }));
    CHECK_FALSE (rig.applied ("range.create", { text ("VD000030"), osc::Value::float64 (0.0),
                                                osc::Value::float64 (1.0) }));
}

TEST_CASE ("video: the canvas and the output of the fixture, as the show says them")
{
    Rig rig;

    CHECK (rig.at ("/godot/canvas/order") == "VD000011");
    CHECK (rig.at ("/godot/canvas/VD000011/name") == "Stage");
    CHECK (rig.at ("/godot/canvas/VD000011/width") == "1920");
    CHECK (rig.at ("/godot/canvas/VD000011/height") == "1080");

    CHECK (rig.at ("/godot/videoOutput/order") == "VD000021");
    CHECK (rig.at ("/godot/videoOutput/VD000021/name") == "Face");
    CHECK (rig.at ("/godot/videoOutput/VD000021/canvas") == "VD000011");
    CHECK (rig.at ("/godot/videoOutput/VD000021/enabled") == "true");
    CHECK (rig.at ("/godot/videoOutput/VD000021/display").empty());

    /*  A canvas's size is written like any row, and stays within its range. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/canvas/VD000011/width"), osc::Value::int32 (3840) }));
    CHECK (rig.at ("/godot/canvas/VD000011/width") == "3840");
    CHECK_FALSE (rig.applied ("node.set", { text ("/godot/canvas/VD000011/width"), osc::Value::int32 (4) }));

    /*  AN OUTPUT'S DISPLAY is the show's, by name, and its identifier beside
        it is state - the MIDI port's twin rows. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/videoOutput/VD000021/display"), text ("EPSON PJ") }));
    CHECK (rig.at ("/godot/videoOutput/VD000021/display") == "EPSON PJ");
}

TEST_CASE ("video: a canvas and an output are made by their commands, at a fixed place, and a show with no picture gains no line")
{
    Rig rig { noPictureBundle() };

    const auto before = rig.showChildren();
    CHECK (std::find (before.begin(), before.end(), "Canvases") == before.end());
    CHECK (std::find (before.begin(), before.end(), "VideoOutputs") == before.end());
    CHECK_FALSE (rig.exists ("/godot/canvas/order"));

    /*  THE OUTPUT FIRST, on no canvas - and its container still lands where
        the canvases' would be followed by it, after the DCAs. */
    REQUIRE (rig.applied ("videoOutput.create", { text ("Wings monitor"), text (""), text ("MC000040") }));
    REQUIRE (rig.applied ("canvas.create", { text ("Cyclo"), text ("MC000041") }));

    const auto after = rig.showChildren();
    REQUIRE (after.size() == before.size() + 2);
    CHECK (after[after.size() - 3] == "Dcas");
    CHECK (after[after.size() - 2] == "Canvases");
    CHECK (after[after.size() - 1] == "VideoOutputs");

    CHECK (rig.at ("/godot/canvas/MC000041/name") == "Cyclo");
    CHECK (rig.at ("/godot/canvas/MC000041/width") == "1920");
    CHECK (rig.at ("/godot/videoOutput/MC000040/name") == "Wings monitor");
    CHECK (rig.at ("/godot/videoOutput/MC000040/canvas").empty());

    /*  AN OUTPUT MADE ON A CANVAS names it; one made on anything else - a cue,
        an identifier nobody has - is refused at the door. */
    REQUIRE (rig.applied ("videoOutput.create", { text ("Face"), text ("MC000041"), text ("MC000042") }));
    CHECK (rig.at ("/godot/videoOutput/MC000042/canvas") == "MC000041");
    CHECK (rig.at ("/godot/videoOutput/order") == "MC000040 MC000042");
    CHECK_FALSE (rig.applied ("videoOutput.create", { text ("Wrong"), text ("MC000002"), text ("MC000043") }));
    CHECK_FALSE (rig.applied ("videoOutput.create", { text ("Wrong"), text ("ZZZZZZZZ"), text ("MC000044") }));

    /*  AND UNDO TAKES THE OBJECT BACK, one step each. */
    REQUIRE (rig.applied ("undo"));
    CHECK_FALSE (rig.exists ("/godot/videoOutput/MC000042/name"));
    CHECK (rig.exists ("/godot/videoOutput/MC000040/name"));
}

TEST_CASE ("video: wfg validate says a video cue on no canvas shows nothing, and a canvas row naming a cue")
{
    Rig rig;

    CHECK (rig.problemsAbout ("Video[VD000002]").empty());

    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (2), text ("video"),
                                          text ("Nowhere"), text ("VD000031") }));

    const auto nowhere = rig.problemsAbout ("Video[VD000031]");
    REQUIRE (nowhere.size() == 1);
    CHECK (nowhere.front().find ("no canvas") != std::string::npos);

    /*  A CANVAS ROW NAMING A CUE is the refers check's: the table says the row
        points at a canvas, and a memo is not one. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000031/canvas"), text ("VD000003") }));
    CHECK_FALSE (rig.problemsAbout ("VD000003").empty());
}

TEST_CASE ("video: a fixture with a picture reads and writes back to the same bytes")
{
    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    juce::MemoryBlock block;
    REQUIRE (videoBundle().getChildFile ("show.xml").loadFileAsData (block));

    const auto written = doc::CanonicalXml::write (document);
    const std::string onDisk (static_cast<const char*> (block.getData()), block.getSize());

    CHECK (written == onDisk);
}

//==============================================================================
/*  AND HOW ONE RUNS (Phase 8a, namespace draft 35.4 and 35.5), against a
    picture side of the test's own that writes down what it is told: GO brings
    the layer up a horizon ahead, over the cue's fade-in, and it holds; Esc
    takes it down to black over the panic fade and the run ends when it gets
    there; a double Esc clears every canvas at once; a stop takes the layer
    off now. With no player, so the points are on the tick's own clock - the
    configuration `wfg serve` without a sound card is in.
*/
namespace
{
    struct FakeSink final : video::Sink
    {
        void show (const video::LayerSpec& spec) override  { shown.push_back (spec); }

        /*  THE OPACITY'S POINTS by layer, and every other value's by layer and
            property: what a fade of the geometry is checked against. */
        void move (const std::string& id, video::Property property, const video::Point& point) override
        {
            if (property == video::Property::opacity)
                points[id].push_back (point);
            else
                geometry[id][property].push_back (point);
        }

        void prepare (const std::vector<std::string>& paths) override  { prepared = paths; }

        void remove (const std::string& id, std::int64_t sample) override
        {
            removed.push_back ({ id, sample });
        }

        void clear() override  { ++clears; }

        std::vector<video::LayerSpec> shown;
        std::map<std::string, std::vector<video::Point>> points;
        std::map<std::string, std::map<video::Property, std::vector<video::Point>>> geometry;
        std::vector<std::string> prepared;
        std::vector<std::pair<std::string, std::int64_t>> removed;
        int clears = 0;
    };

    struct VideoRig
    {
        VideoRig()
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (videoBundle(), document).ok);
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setVideo (&sink);
            runner.setSamplesPerTick (960);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult submitAndTick (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return tickOnce();
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            const cue::Run* found = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    found = runs.find (run.id);

            return found;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (23) };
        cue::Runner runner { document, runs, runIds, focus };
        FakeSink sink;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("video: the opacity between the points - straight, held after the last, nothing before the first")
{
    const std::vector<video::Point> rise { { 1000, 0.0 }, { 2000, 1.0 } };

    CHECK (video::opacityAt (rise, 0) == doctest::Approx (0.0));
    CHECK (video::opacityAt (rise, 1000) == doctest::Approx (0.0));
    CHECK (video::opacityAt (rise, 1500) == doctest::Approx (0.5));
    CHECK (video::opacityAt (rise, 2000) == doctest::Approx (1.0));
    CHECK (video::opacityAt (rise, 9000) == doctest::Approx (1.0));

    /*  TWO AT ONE SAMPLE ARE A STEP, the later winning - a layer put up at
        once, and one taken down where a fade-out begins. */
    const std::vector<video::Point> step { { 100, 0.25 }, { 100, 0.75 }, { 300, 0.0 } };

    CHECK (video::opacityAt (step, 99) == doctest::Approx (0.0));
    CHECK (video::opacityAt (step, 100) == doctest::Approx (0.75));
    CHECK (video::opacityAt (step, 200) == doctest::Approx (0.375));
    CHECK (video::opacityAt (step, 400) == doctest::Approx (0.0));

    /*  A point with no clock is now. */
    const std::vector<video::Point> now { { -1, 0.6 } };
    CHECK (video::opacityAt (now, 0) == doctest::Approx (0.6));
    CHECK (video::lastSampleOf (rise) == 2000);

    CHECK (video::paintFromText ("#2040A0", 7) == 0x2040A0u);
    CHECK (video::paintFromText ("#ffffff", 7) == 0xFFFFFFu);
    CHECK (video::paintFromText ("2040A0", 6) == 0u);
    CHECK (video::paintFromText ("#20G0A0", 7) == 0u);
}

TEST_CASE ("video: GO brings the layer up a horizon ahead over its fade-in, and it holds")
{
    VideoRig rig;

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (3);

    REQUIRE (rig.sink.shown.size() == 1);
    const auto& spec = rig.sink.shown.front();
    const auto* run = rig.runOf ("VD000002");
    REQUIRE (run != nullptr);

    CHECK (spec.id == run->id);
    CHECK (spec.canvas == "VD000011");
    CHECK (spec.layer == 0);
    CHECK (spec.source == "fill");
    CHECK (spec.paint == 0x2040A0u);

    /*  FROM NOTHING TO FULL OVER THE CUE'S SECOND, on Go.dot's samples: the
        fade-in is 48 000 of them at 960 a tick, and it starts two ticks ahead
        of the tick that placed it - the horizon with no player. */
    const auto& points = rig.sink.points[run->id];
    REQUIRE (points.size() == 2);
    CHECK (points[0].value == doctest::Approx (0.0));
    CHECK (points[1].value == doctest::Approx (1.0));
    CHECK (points[1].sample - points[0].sample == 48000);
    CHECK (points[0].sample % 960 == 0);

    /*  STARTED, and still up a long while after: a still holds until
        something stops it (VJ). */
    CHECK (run->state == cue::runState::playing);
    rig.ticks (200);
    CHECK_FALSE (rig.runOf ("VD000002")->isFinished());
    CHECK (rig.sink.removed.empty());
}

TEST_CASE ("video: Esc takes the picture down to black over the panic fade, and the run ends when it is black")
{
    VideoRig rig;

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (100);

    const auto runId = rig.runOf ("VD000002")->id;
    REQUIRE (rig.sink.points[runId].size() == 2);

    rig.submitAndTick ("run.stopAll");
    rig.ticks (1);

    /*  FROM WHERE IT IS - full, a second and more after its fade-in began -
        to nought over the show's panic fade, a second, and gone on the frame
        it gets there. */
    const auto& points = rig.sink.points[runId];
    REQUIRE (points.size() == 4);
    CHECK (points[2].value == doctest::Approx (1.0));
    CHECK (points[3].value == doctest::Approx (0.0));
    CHECK (points[3].sample - points[2].sample == 48000);

    REQUIRE (rig.sink.removed.size() == 1);
    CHECK (rig.sink.removed.front().first == runId);
    CHECK (rig.sink.removed.front().second == points[3].sample);

    /*  NOT ENDED ON THE PRESS: the run is stopping until the picture is
        black, then ended - its footers would run then, as a sound's do. */
    CHECK_FALSE (rig.runs.find (runId)->isFinished());
    rig.ticks (60);
    CHECK (rig.runs.find (runId)->isFinished());
    CHECK (rig.sink.clears == 0);
}

TEST_CASE ("video: a double Esc clears every canvas at once, and a stop takes the layer off now")
{
    {
        VideoRig rig;

        rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
        rig.ticks (10);

        const auto runId = rig.runOf ("VD000002")->id;

        rig.submitAndTick ("run.killAll");
        rig.ticks (2);

        CHECK (rig.sink.clears == 1);
        CHECK (rig.runs.find (runId)->isFinished());

        /*  Cleared, and not taken off a second time when the run ends. */
        CHECK (rig.sink.removed.empty());
    }

    {
        VideoRig rig;

        rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
        rig.ticks (10);

        const auto runId = rig.runOf ("VD000002")->id;

        rig.submitAndTick ("run.stop", { osc::Value::string (runId) });
        rig.ticks (2);

        CHECK (rig.runs.find (runId)->isFinished());
        REQUIRE (rig.sink.removed.size() == 1);
        CHECK (rig.sink.removed.front().first == runId);
        CHECK (rig.sink.removed.front().second == -1);
        CHECK (rig.sink.clears == 0);
    }
}

TEST_CASE ("video: with no picture side a video cue still runs, holds and stops - a replay's configuration")
{
    VideoRig rig;
    rig.runner.setVideo (nullptr);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (5);

    const auto* run = rig.runOf ("VD000002");
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::playing);

    rig.submitAndTick ("run.stopAll");
    rig.ticks (60);
    CHECK (rig.runOf ("VD000002")->isFinished());
}

TEST_CASE ("video: Doh! takes a picture seen down as Esc does, and the corrected GO puts it up again")
{
    VideoRig rig;

    REQUIRE (rig.submitAndTick ("list.focus", { osc::Value::string ("VD000001") }).applied >= 1);
    rig.submitAndTick ("go");
    rig.ticks (10);

    const auto* first = rig.runOf ("VD000002");
    REQUIRE (first != nullptr);
    const auto firstId = first->id;
    REQUIRE (rig.sink.shown.size() == 1);
    REQUIRE (first->startedAtTick >= 0);

    /*  SEEN COUNTS AS HEARD (VK): brought down to black over the panic fade,
        and gone when it gets there - not left up, not cut. */
    rig.submitAndTick ("go.doh");
    rig.ticks (1);

    const auto& points = rig.sink.points[firstId];
    REQUIRE (points.size() == 4);
    CHECK (points[3].value == doctest::Approx (0.0));
    REQUIRE (rig.sink.removed.size() == 1);
    CHECK (rig.sink.removed.front().first == firstId);

    rig.ticks (60);
    CHECK (rig.runs.find (firstId)->isFinished());

    /*  THE CORRECTED GO puts the same cue up again, as a new layer. */
    rig.submitAndTick ("go");
    rig.ticks (5);

    CHECK (rig.sink.shown.size() == 2);
    const auto* again = rig.runOf ("VD000002");
    REQUIRE (again != nullptr);
    CHECK (again->id != firstId);
    CHECK_FALSE (again->isFinished());
}

TEST_CASE ("video: a picture cue at standby is named to the renderer to read, and GO shows it with its geometry")
{
    VideoRig rig;

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Portrait"),
                                                osc::Value::string ("VD000040"),
                                                osc::Value::string ("source"), osc::Value::string ("picture"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("portrait.png"),
                                                osc::Value::string ("fit"), osc::Value::string ("fill"),
                                                osc::Value::string ("scale"), osc::Value::string ("80"),
                                                osc::Value::string ("rotation"), osc::Value::string ("15"),
                                                osc::Value::string ("flipH"), osc::Value::string ("true"),
                                                osc::Value::string ("opacity"), osc::Value::string ("50") }).applied >= 1);

    /*  AT STANDBY, READ AHEAD (VX): its whole path handed to the picture side
        before anybody presses GO. */
    REQUIRE (rig.submitAndTick ("standby.set", { osc::Value::string ("VD000040") }).applied >= 1);
    rig.ticks (2);

    REQUIRE (rig.sink.prepared.size() == 1);
    CHECK (juce::String (rig.sink.prepared.front()).endsWith ("portrait.png"));

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000040") });
    rig.ticks (3);

    REQUIRE (rig.sink.shown.size() == 1);
    const auto& spec = rig.sink.shown.front();
    CHECK (spec.source == "picture");
    CHECK (juce::String (spec.file).endsWith ("portrait.png"));
    CHECK (spec.fit == "fill");
    CHECK (spec.scale == doctest::Approx (80.0));
    CHECK (spec.rotation == doctest::Approx (15.0));
    CHECK (spec.flipH);
    CHECK_FALSE (spec.flipV);

    /*  OPACITY IN % (VR): fifty of them is half. */
    const auto& points = rig.sink.points[spec.id];
    REQUIRE_FALSE (points.empty());
    CHECK (points.back().value == doctest::Approx (0.5));
}

TEST_CASE ("video: a fade cue moves a picture's opacity, scale, offset and turn, and stops it when told")
{
    VideoRig rig;

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (2),
                                                osc::Value::string ("fade"), osc::Value::string ("Drift"),
                                                osc::Value::string ("VD000050"),
                                                osc::Value::string ("target"), osc::Value::string ("VD000002"),
                                                osc::Value::string ("duration"), osc::Value::string ("1"),
                                                osc::Value::string ("video"),
                                                osc::Value::string ("offsetX:10 opacity:50 rotation:90 scale:150") }).applied >= 1);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (80);

    const auto picture = rig.runOf ("VD000002")->id;

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000050") });
    rig.ticks (70);

    /*  EACH VALUE FROM WHERE IT WAS TO WHERE THE FADE SAYS, a point a tick:
        the scale from the cue's 100 % to 150, the opacity from full to half,
        and the fade's own run ended when it arrived. */
    const auto& scales = rig.sink.geometry[picture][video::Property::scale];
    REQUIRE (scales.size() > 40);
    CHECK (scales.front().value == doctest::Approx (100.0).epsilon (0.03));
    CHECK (scales.back().value == doctest::Approx (150.0));
    CHECK (rig.sink.geometry[picture][video::Property::offsetX].back().value == doctest::Approx (10.0));
    CHECK (rig.sink.geometry[picture][video::Property::rotation].back().value == doctest::Approx (90.0));
    CHECK (rig.sink.points[picture].back().value == doctest::Approx (0.5));

    for (std::size_t n = 1; n < scales.size(); ++n)
        CHECK (scales[n].sample >= scales[n - 1].sample);

    CHECK (rig.runOf ("VD000050")->isFinished());
    CHECK_FALSE (rig.runs.find (picture)->isFinished());
    CHECK (rig.sink.removed.empty());

    /*  AND A FADE THAT STOPS WHEN DONE takes the picture away where it
        arrives, and its run ends. */
    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000050/stopWhenDone"),
                                              osc::Value::string ("true") }).applied >= 1);
    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000050/video"),
                                              osc::Value::string ("opacity:0") }).applied >= 1);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000050") });
    rig.ticks (70);

    REQUIRE (rig.sink.removed.size() == 1);
    CHECK (rig.sink.removed.front().first == picture);
    CHECK (rig.runs.find (picture)->isFinished());
}

TEST_CASE ("video: a movie's playhead moves at its speed, wraps for its loops, and its run ends after the last")
{
    VideoRig rig;

    /*  A MOVIE OF TWO SECONDS, as the show knows its length, played twice at
        double speed from half a second in. */
    const std::map<std::string, double> lengths { { "clip.mov", 2.0 } };
    rig.runner.setMediaDurations (&lengths);

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Clip"),
                                                osc::Value::string ("VD000060"),
                                                osc::Value::string ("source"), osc::Value::string ("movie"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("clip.mov"),
                                                osc::Value::string ("rate"), osc::Value::string ("2"),
                                                osc::Value::string ("loops"), osc::Value::string ("2"),
                                                osc::Value::string ("startOffset"), osc::Value::string ("0.5") }).applied >= 1);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000060") });
    rig.ticks (3);

    const auto* run = rig.runOf ("VD000060");
    REQUIRE (run != nullptr);
    const auto id = run->id;

    REQUIRE (rig.sink.shown.size() == 1);
    CHECK (rig.sink.shown.front().source == "movie");

    const auto& times = rig.sink.geometry[id][video::Property::time];
    REQUIRE_FALSE (times.empty());
    CHECK (times.front().value == doctest::Approx (0.5));

    /*  AT TWICE ITS SPEED, a second of the show is two of the file: half a
        second in, the first pass ends after three-quarters of a second, wraps
        - a step back to the top at one sample - and the second ends after one
        more second. */
    rig.ticks (20);
    CHECK_FALSE (rig.runs.find (id)->isFinished());
    CHECK (times.back().value > 0.5);

    rig.ticks (40);

    bool wrapped = false;

    for (std::size_t n = 1; n < times.size(); ++n)
    {
        CHECK (times[n].sample >= times[n - 1].sample);

        if (times[n].sample == times[n - 1].sample && times[n - 1].value == doctest::Approx (2.0)
              && times[n].value == doctest::Approx (0.0))
            wrapped = true;
    }

    CHECK (wrapped);

    rig.ticks (60);
    CHECK (rig.runs.find (id)->isFinished());
    CHECK (times.back().value == doctest::Approx (2.0));
    REQUIRE (rig.sink.removed.size() == 1);
    CHECK (rig.sink.removed.front().first == id);

    /*  ITS LENGTH WAS 0.75 + 1 SECONDS OF THE SHOW: ended near 1.75 s after it
        came up, not before and not long after. */
    const auto playedFor = static_cast<double> (rig.sink.removed.front().second - times.front().sample) / 48000.0;
    CHECK (playedFor == doctest::Approx (1.75).epsilon (0.03));
}
