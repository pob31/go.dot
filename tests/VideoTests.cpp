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

#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Video.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Conversion.h>
#include <wfg/engine/video/DcaOpacity.h>
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/PipedChild.h>
#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/video/VideoSink.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
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

    /*  NOT A SOUND: no destination, no send. A trigger, as every cue has;
        and a Range, as a movie plays one (namespace draft 37.5, WL). */
    CHECK_FALSE (rig.applied ("route.create", { text ("VD000030"), text ("VD000004") }));
    CHECK_FALSE (rig.applied ("send.create", { text ("VD000030"), text ("VD000004") }));
    CHECK (rig.applied ("range.create", { text ("VD000030"), osc::Value::float64 (0.0),
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

        void canvasLevels (const std::vector<std::pair<std::string, double>>& levels) override
        {
            canvasLevelsSent.push_back (levels);
        }

        std::vector<std::vector<std::pair<std::string, double>>> canvasLevelsSent;
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

TEST_CASE ("video: a DCA's trim is how solid a picture is, along the fader's travel and never past its own")
{
    /*  THE AUTHOR'S ENDS (namespace draft 37.5, WE): nothing at the bottom,
        all of it at nought dB, and no more above; between, the show's fader
        travel over where nought dB sits on it - 0.85 of the way up. */
    CHECK (video::opacityForTrim (0.0) == doctest::Approx (1.0));
    CHECK (video::opacityForTrim (6.0) == doctest::Approx (1.0));
    CHECK (video::opacityForTrim (12.0) == doctest::Approx (1.0));
    CHECK (video::opacityForTrim (-6.0) == doctest::Approx (0.785 / 0.85));
    CHECK (video::opacityForTrim (-20.0) == doctest::Approx ((0.2 + 0.65 * 40.0 / 60.0) / 0.85));
    CHECK (video::opacityForTrim (-60.0) == doctest::Approx (0.2 / 0.85));
    CHECK (video::opacityForTrim (-120.0) == doctest::Approx (0.0));
    CHECK (video::opacityForTrim (-std::numeric_limits<double>::infinity()) == doctest::Approx (0.0));

    /*  ON THE PICTURE SIDE, a factor of the opacity's points: all of it
        before the DCA says anything, and what it says after. */
    video::region::LayerReading layer;
    layer.rings[static_cast<int> (video::Property::opacity)].count = 1;
    layer.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, 0.8 };
    CHECK (video::opacityOf (layer, 100) == doctest::Approx (0.8));

    layer.rings[static_cast<int> (video::Property::dca)].count = 1;
    layer.rings[static_cast<int> (video::Property::dca)].points[0] = { 50, 0.5 };
    CHECK (video::opacityOf (layer, 40) == doctest::Approx (0.8));
    CHECK (video::opacityOf (layer, 100) == doctest::Approx (0.4));
}

TEST_CASE ("video: a DCA marked on a video cue, or on its group, is followed while the picture is up")
{
    cue::DcaTable trims;
    VideoRig rig;
    rig.runner.setDcas (&trims);

    const auto pictures = rig.document.createDca ("Pictures").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000002/dca", pictures).ok);
    trims.set (pictures, -20.0);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (3);

    const auto* run = rig.runOf ("VD000002");
    REQUIRE (run != nullptr);

    /*  FROM THE SAMPLE IT COMES UP ON, the opacity's own first point. */
    const auto& dca = rig.sink.geometry[run->id][video::Property::dca];
    REQUIRE (dca.size() == 1);
    CHECK (dca[0].value == doctest::Approx (video::opacityForTrim (-20.0)));
    CHECK (dca[0].sample == rig.sink.points[run->id].front().sample);

    /*  RIDDEN UP PAST NOUGHT: where it stood held to a tick before, then all
        of it - and no more - a horizon ahead. */
    trims.set (pictures, 6.0);
    rig.ticks (1);

    REQUIRE (dca.size() == 3);
    CHECK (dca[1].value == doctest::Approx (video::opacityForTrim (-20.0)));
    CHECK (dca[2].value == doctest::Approx (1.0));
    CHECK (dca[2].sample - dca[1].sample == 960);

    /*  AT REST, nothing more is placed. */
    trims.set (pictures, 3.0);
    rig.ticks (5);
    CHECK (dca.size() == 3);

    /*  A GROUP'S MARK reaches the picture under it, its trim summed with the
        cue's own in dB as a sound's is. */
    const auto scene = rig.document.createCue ("VD000001", 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
    const auto wash = rig.document.createCue (scene, 0, "video", "Wash").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + wash + "/canvas", "VD000011").ok);

    const auto everything = rig.document.createDca ("Everything").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/dca", everything).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + wash + "/dca", pictures).ok);
    trims.set (pictures, -10.0);
    trims.set (everything, -10.0);

    rig.submitAndTick ("cue.fire", { osc::Value::string (scene) });
    rig.ticks (4);

    const auto* washRun = rig.runOf (wash);
    REQUIRE (washRun != nullptr);

    const auto& washDca = rig.sink.geometry[washRun->id][video::Property::dca];
    REQUIRE_FALSE (washDca.empty());
    CHECK (washDca.back().value == doctest::Approx (video::opacityForTrim (-20.0)));
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

TEST_CASE ("video: a movie with no Range plays once from its start offset at its speed, and its run ends there")
{
    VideoRig rig;

    /*  A MOVIE OF TWO SECONDS, as the show knows its length, played at double
        speed from half a second in (namespace draft 37.5, WL: no Range, the
        file once). */
    const std::map<std::string, double> lengths { { "clip.mov", 2.0 } };
    rig.runner.setMediaDurations (&lengths);

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Clip"),
                                                osc::Value::string ("VD000060"),
                                                osc::Value::string ("source"), osc::Value::string ("movie"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("clip.mov"),
                                                osc::Value::string ("rate"), osc::Value::string ("2"),
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

    rig.ticks (20);
    CHECK_FALSE (rig.runs.find (id)->isFinished());
    CHECK (times.back().value > 0.5);

    rig.ticks (40);
    CHECK (rig.runs.find (id)->isFinished());
    CHECK (times.back().value == doctest::Approx (2.0));
    REQUIRE (rig.sink.removed.size() == 1);
    CHECK (rig.sink.removed.front().first == id);

    for (std::size_t n = 1; n < times.size(); ++n)
        CHECK (times[n].value >= times[n - 1].value);

    /*  1.5 SECONDS OF THE FILE AT DOUBLE SPEED: ended near 0.75 s after it
        came up, not before and not long after. */
    const auto playedFor = static_cast<double> (rig.sink.removed.front().second - times.front().sample) / 48000.0;
    CHECK (playedFor == doctest::Approx (0.75).epsilon (0.05));
}

TEST_CASE ("video: a movie's Ranges play as a sound's - each for its passes, then the next, and the run ends after the last")
{
    VideoRig rig;

    const std::map<std::string, double> lengths { { "clip.mov", 2.0 } };
    rig.runner.setMediaDurations (&lengths);

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Clip"),
                                                osc::Value::string ("VD000060"),
                                                osc::Value::string ("source"), osc::Value::string ("movie"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("clip.mov"),
                                                osc::Value::string ("rate"), osc::Value::string ("2") }).applied >= 1);

    /*  TWO RANGES: 0.2 to 1.2 twice, then 1.5 to 1.9 once - a playlist over
        the file, as a media cue's (WL). */
    const auto first = rig.document.createRange ("VD000060", 0.2, 1.2);
    REQUIRE (first.ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + first.id + "/loops", "2").ok);
    REQUIRE (rig.document.createRange ("VD000060", 1.5, 1.9).ok);

    /*  A START OFFSET BESIDE THEM is refused when the show is checked, as a
        sound's is. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000060/startOffset", "0.3").ok);
    CHECK_FALSE (rig.document.validate().empty());
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000060/startOffset", "0").ok);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000060") });
    rig.ticks (3);

    const auto* run = rig.runOf ("VD000060");
    REQUIRE (run != nullptr);
    const auto id = run->id;
    const auto& times = rig.sink.geometry[id][video::Property::time];
    REQUIRE_FALSE (times.empty());
    CHECK (times.front().value == doctest::Approx (0.2));

    rig.ticks (100);
    CHECK (rig.runs.find (id)->isFinished());
    CHECK (times.back().value == doctest::Approx (1.9));

    /*  THE STEPS, each two points on one sample: back to the first range's in
        point once, then on to the second's. */
    std::vector<std::pair<double, double>> steps;

    for (std::size_t n = 1; n < times.size(); ++n)
    {
        CHECK (times[n].sample >= times[n - 1].sample);

        if (times[n].sample == times[n - 1].sample && times[n].value != doctest::Approx (times[n - 1].value))
            steps.push_back ({ times[n - 1].value, times[n].value });
    }

    REQUIRE (steps.size() == 2);
    CHECK (steps[0].first == doctest::Approx (1.2));
    CHECK (steps[0].second == doctest::Approx (0.2));
    CHECK (steps[1].first == doctest::Approx (1.2));
    CHECK (steps[1].second == doctest::Approx (1.5));

    /*  TWO SECONDS OF THE FILE PLAYED (1 + 1 + 0.4) AT DOUBLE SPEED: 1.2 s. */
    REQUIRE (rig.sink.removed.size() == 1);
    const auto playedFor = static_cast<double> (rig.sink.removed.front().second - times.front().sample) / 48000.0;
    CHECK (playedFor == doctest::Approx (1.2).epsilon (0.05));
}

TEST_CASE ("video: a cue's grade reaches the picture side, its curves baked")
{
    VideoRig rig;

    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000002/saturation"),
                                              osc::Value::float64 (50.0) }).applied >= 1);
    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000002/hue"),
                                              osc::Value::float64 (30.0) }).applied >= 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000002/curveRed", "0 0 1 0.5").ok);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (3);

    REQUIRE (rig.sink.shown.size() == 1);
    const auto& grade = rig.sink.shown.front().grade;
    CHECK (grade.saturation == doctest::Approx (50.0));
    CHECK (grade.hue == doctest::Approx (30.0));
    CHECK (grade.contrast == doctest::Approx (100.0));
    CHECK (grade.gamma == doctest::Approx (1.0));
    CHECK (grade.hasCurves);
    CHECK (grade.tables[0][255] == 128);
    CHECK (grade.tables[1][255] == 255);
}

TEST_CASE ("video: a mask cue's outline, feather and inversion reach the picture side")
{
    VideoRig rig;

    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000002/source"),
                                              osc::Value::string ("mask") }).applied >= 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000002/shape", "0.1 0.2 0.9 0.2 0.5 0.8").ok);
    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000002/feather"),
                                              osc::Value::float64 (12.0) }).applied >= 1);
    REQUIRE (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/cue/VD000002/invert"),
                                              osc::Value::string ("true") }).applied >= 1);

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000002") });
    rig.ticks (3);

    REQUIRE (rig.sink.shown.size() == 1);
    const auto& spec = rig.sink.shown.front();
    CHECK (spec.source == "mask");
    REQUIRE (spec.shape.count == 3);
    CHECK (spec.shape.x[1] == doctest::Approx (0.9f));
    CHECK (spec.shape.y[2] == doctest::Approx (0.8f));
    CHECK (spec.shape.feather == doctest::Approx (12.0f));
    CHECK (spec.shape.invert);
}

//==============================================================================
/*  A MOVIE CONVERTED TO HAP (namespace draft 37.5 WF-WJ, 37.6 F.3). */
TEST_CASE ("video: the part of a movie the cues use, and the edit a finished conversion makes")
{
    Rig rig;
    video::registerConversionCommands (rig.engine.commands(), rig.document, nullptr);

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/file"), text ("show/clip.mp4") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("40") }));

    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("video"),
                                          text ("Later"), text ("VD000032") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000032/source"), text ("movie") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000032/file"), text ("show/clip.mp4") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000032/startOffset"), text ("25.5") }));

    /*  FROM THE EARLIEST START, TEN SECONDS EARLIER (WI). */
    CHECK (video::usedSpanOf (rig.document, "show/clip.mp4").start == doctest::Approx (15.5));
    CHECK (video::usedSpanOf (rig.document, "show/clip.mp4").end < 0.0);
    CHECK (video::usedSpanOf (rig.document, "elsewhere.mp4").start == doctest::Approx (0.0));

    /*  WITH RANGES (WL), from the earliest in point to the furthest out
        point, ten seconds either side - and to the file's end while any cue
        naming it has none. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("0") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000032/startOffset"), text ("0") }));
    REQUIRE (rig.document.createRange ("VD000002", 30.0, 45.0, "VD000071").ok);
    REQUIRE (rig.document.createRange ("VD000002", 70.0, 80.0, "VD000072").ok);
    CHECK (video::usedSpanOf (rig.document, "show/clip.mp4").end < 0.0);

    REQUIRE (rig.document.createRange ("VD000032", 22.0, 50.0, "VD000073").ok);
    CHECK (video::usedSpanOf (rig.document, "show/clip.mp4").start == doctest::Approx (12.0));
    CHECK (video::usedSpanOf (rig.document, "show/clip.mp4").end == doctest::Approx (90.0));

    /*  A FINISHED CONVERSION MOVES THEM BACK BY THE CUT, in and out. */
    REQUIRE (rig.applied ("media.converted", { text ("show/clip.mp4"), text ("show/clip (Hap).mov"),
                                               osc::Value::float64 (12.0), text ("") }));
    CHECK (rig.at ("/godot/range/VD000071/in") == "18");
    CHECK (rig.at ("/godot/range/VD000071/out") == "33");
    CHECK (rig.at ("/godot/range/VD000073/in") == "10");
    CHECK (rig.at ("/godot/range/VD000073/out") == "38");
    REQUIRE (rig.applied ("undo"));
    CHECK (rig.at ("/godot/range/VD000073/in") == "22");

    for (const auto* range : { "VD000071", "VD000072", "VD000073" })
        REQUIRE (rig.document.remove (range).ok);

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("40") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000032/startOffset"), text ("25.5") }));

    /*  NOTHING CONVERTS HERE: asked, it is taken and does nothing. A scope
        or a format it does not know is refused. */
    CHECK (rig.applied ("media.convert", { text ("show/clip.mp4"), text ("used"), text ("hap"), osc::Value::boolean (true) }));
    CHECK_FALSE (rig.applied ("media.convert", { text ("show/clip.mp4"), text ("some"), text ("hap") }));
    CHECK_FALSE (rig.applied ("media.convert", { text ("show/clip.mp4"), text ("whole"), text ("h264") }));

    /*  DONE: every cue naming the movie names the HAP file, its start moved
        back by what was cut - one edit, which one undo takes back (WH). */
    REQUIRE (rig.applied ("media.converted", { text ("show/clip.mp4"), text ("show/clip (Hap).mov"),
                                               osc::Value::float64 (15.5), text ("") }));

    CHECK (rig.at ("/godot/cue/VD000002/file") == "show/clip (Hap).mov");
    CHECK (rig.at ("/godot/cue/VD000002/startOffset") == "24.5");
    CHECK (rig.at ("/godot/cue/VD000032/file") == "show/clip (Hap).mov");
    CHECK (rig.at ("/godot/cue/VD000032/startOffset") == "10");

    REQUIRE (rig.applied ("undo"));
    CHECK (rig.at ("/godot/cue/VD000002/file") == "show/clip.mp4");
    CHECK (rig.at ("/godot/cue/VD000002/startOffset") == "40");
    CHECK (rig.at ("/godot/cue/VD000032/file") == "show/clip.mp4");
    CHECK (rig.at ("/godot/cue/VD000032/startOffset") == "25.5");

    CHECK_FALSE (rig.applied ("media.converted", { text ("show/clip.mp4"), text (""), osc::Value::float64 (0.0) }));
    CHECK_FALSE (rig.applied ("media.converted", { text ("show/clip.mp4"), text ("x.mov"), osc::Value::float64 (-1.0) }));
}

/*  AND FOR REAL, where FFmpeg is found: a movie with sound made by FFmpeg,
    converted from a second in with its sound - a HAP movie Go.dot reads,
    as many frames as the span holds, and a WAV as long - then the same
    through the converter's thread, and once cancelled. */
TEST_CASE ("video: a movie converted to HAP by FFmpeg and Go.dot, with its sound")
{
    const auto tools = video::ffmpeg::find();

    if (! tools.found())
        return;

    const auto folder = juce::File::createTempFile ("convert");
    REQUIRE (folder.createDirectory());
    const auto source = folder.getChildFile ("source.mp4");

    video::PipedChild maker;
    REQUIRE (maker.start ({ tools.ffmpeg, "-nostdin", "-v", "error", "-y",
                            "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=3",
                            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
                            "-c:v", "mpeg4", "-q:v", "3", "-c:a", "aac", "-shortest",
                            source.getFullPathName().toStdString() }));
    maker.readAll();
    REQUIRE (maker.wait (60000) == 0);

    video::ConversionRequest request;
    request.id = "one";
    request.sourceName = "source.mp4";
    request.source = source.getFullPathName().toStdString();
    request.targetName = "source (Hap).mov";
    request.target = folder.getChildFile ("source (Hap).mov").getFullPathName().toStdString();
    request.start = 1.0;
    request.soundName = "source (sound).wav";
    request.sound = folder.getChildFile ("source (sound).wav").getFullPathName().toStdString();

    std::string why;
    auto last = 0.0;
    REQUIRE_MESSAGE (video::convertMovie (request, why, [&last] (double done) { last = done; }), why);
    CHECK (last == doctest::Approx (1.0));
    CHECK_FALSE (folder.getChildFile ("source (Hap).mov.part").exists());

    video::movie::MovieFile movie;
    REQUIRE (movie.open (request.target, why));
    CHECK (movie.info().codec == "Hap1");
    CHECK (movie.info().width == 320);
    CHECK (movie.info().height == 180);
    CHECK (std::abs (static_cast<int> (movie.info().frames.size()) - 50) <= 1);
    CHECK (movie.info().frameRate() == doctest::Approx (25.0));

    std::vector<std::uint8_t> bytes, blocks;
    REQUIRE (movie.readFrame (10, bytes));
    auto format = video::hap::Texture::none;
    CHECK (video::hap::unpack (bytes.data(), bytes.size(), format, blocks));

    const auto sound = video::ffmpeg::probe (tools, request.sound);
    CHECK_FALSE (sound.ok);     // no picture in it: a sound alone
    //  Two seconds - from one to three - of one channel of 24-bit at 48 kHz, and a header.
    const auto soundBytes = juce::File (request.sound).getSize();
    CHECK (soundBytes > 48000 * 3 * 19 / 10);
    CHECK (soundBytes < 48000 * 3 * 21 / 10 + 1024);

    /*  THROUGH THE CONVERTER'S THREAD: told when it is done, its status kept. */
    std::mutex held;
    std::condition_variable told;
    std::vector<video::ConversionStatus> finished;

    {
        video::Converter converter ([&] (const video::ConversionRequest&, const video::ConversionStatus& status)
                                    {
                                        const std::lock_guard<std::mutex> guard (held);
                                        finished.push_back (status);
                                        told.notify_all();
                                    });

        CHECK (converter.ffmpegPath() == tools.ffmpeg);

        auto again = request;
        again.id = "two";
        again.start = 0.0;
        again.quality = true;
        again.soundName.clear();
        again.sound.clear();
        again.target = folder.getChildFile ("source (Hap Q).mov").getFullPathName().toStdString();
        converter.enqueue (again);

        std::unique_lock<std::mutex> guard (held);
        REQUIRE (told.wait_for (guard, std::chrono::seconds (120), [&finished] { return ! finished.empty(); }));
        CHECK (finished.front().state == "done");

        const auto statuses = converter.statuses();
        REQUIRE (statuses.size() == 1);
        CHECK (statuses.front().progress == doctest::Approx (1.0));
    }

    video::movie::MovieFile quality;
    REQUIRE (quality.open (folder.getChildFile ("source (Hap Q).mov").getFullPathName().toStdString(), why));
    CHECK (quality.info().codec == "HapY");
    CHECK (std::abs (static_cast<int> (quality.info().frames.size()) - 75) <= 1);

    /*  THE SOUND ALONE, of a movie left as it is: a WAV of the whole file, its
        channels told. */
    video::ConversionRequest soundOnly;
    soundOnly.id = "three";
    soundOnly.sourceName = "source.mp4";
    soundOnly.source = request.source;
    soundOnly.soundName = "alone.wav";
    soundOnly.sound = folder.getChildFile ("alone.wav").getFullPathName().toStdString();
    REQUIRE_MESSAGE (video::convertMovie (soundOnly, why), why);
    CHECK (soundOnly.soundChannels == 1);
    CHECK (juce::File (soundOnly.sound).getSize() > 48000 * 3 * 29 / 10);
    CHECK_FALSE (folder.getChildFile ("alone.wav.part").exists());

    /*  A SOURCE THAT IS NOT A MOVIE fails, said, and leaves nothing behind. */
    auto broken = request;
    broken.source = folder.getChildFile ("source (sound).wav").getFullPathName().toStdString();
    broken.target = folder.getChildFile ("broken.mov").getFullPathName().toStdString();
    broken.sound.clear();
    CHECK_FALSE (video::convertMovie (broken, why));
    CHECK_FALSE (why.empty());
    CHECK_FALSE (folder.getChildFile ("broken.mov").exists());
    CHECK_FALSE (folder.getChildFile ("broken.mov.part").exists());

    folder.deleteRecursively();
}

//==============================================================================
/*  A SOUND LOCKED TO ITS MOVIE (namespace draft 37.5, WJ and WL): the movie
    leads - its start offset, speed and Ranges are copied onto the sound in
    the same edit, refused on the sound, undone together; detached, the sound
    is its own again; the movie deleted, the sound stays. */
TEST_CASE ("video: a sound locked to its movie takes its start, speed and Ranges, in the same edit")
{
    Rig rig;

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/file"), text ("clip.mov") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("2.5") }));

    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("media"),
                                          text ("Clip sound"), text ("VD000040") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/file"), text ("clip (sound).wav") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/rate"), text ("0.5") }));

    /*  LOCKED: the movie's start offset and speed at once. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));
    CHECK (rig.at ("/godot/cue/VD000040/startOffset") == "2.5");
    CHECK (rig.at ("/godot/cue/VD000040/rate") == "1");

    /*  THE MOVIE MOVED, the sound with it; the sound's own refused. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/rate"), text ("1.25") }));
    CHECK (rig.at ("/godot/cue/VD000040/rate") == "1.25");

    const auto refused = rig.apply ("node.set", { text ("/godot/cue/VD000040/startOffset"), text ("9") });
    CHECK (refused.applied == 0);
    CHECK (rig.at ("/godot/cue/VD000040/startOffset") == "2.5");

    /*  ITS LEVEL AND EVERYTHING ELSE ARE ITS OWN. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/level"), text ("-6") }));
    CHECK (rig.at ("/godot/cue/VD000040/level") == "-6");

    /*  RANGES: made, cut, moved and removed on the movie, and the sound's
        follow - under identifiers of their own. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("0") }));
    REQUIRE (rig.applied ("range.create", { text ("VD000002"), osc::Value::float64 (1.0), osc::Value::float64 (5.0),
                                            text ("VD000081") }));
    REQUIRE (rig.applied ("range.create", { text ("VD000002"), osc::Value::float64 (8.0), osc::Value::float64 (9.0),
                                            text ("VD000082") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/range/VD000082/loops"), text ("3") }));

    const auto soundRanges = [&rig]
    {
        std::vector<std::string> out;

        for (const auto& child : rig.document.findById ("VD000040"))
            if (child.hasType ("Range"))
                out.push_back (child["id"].toString().toStdString());

        return out;
    };

    auto ranges = soundRanges();
    REQUIRE (ranges.size() == 2);
    CHECK (ranges[0] != "VD000081");
    CHECK (rig.at ("/godot/range/" + ranges[0] + "/in") == "1");
    CHECK (rig.at ("/godot/range/" + ranges[0] + "/out") == "5");
    CHECK (rig.at ("/godot/range/" + ranges[1] + "/in") == "8");
    CHECK (rig.at ("/godot/range/" + ranges[1] + "/loops") == "3");

    REQUIRE (rig.applied ("range.split", { text ("VD000002"), osc::Value::float64 (3.0), text ("VD000083") }));
    ranges = soundRanges();
    REQUIRE (ranges.size() == 3);
    CHECK (rig.at ("/godot/range/" + ranges[0] + "/out") == "3");
    CHECK (rig.at ("/godot/range/" + ranges[1] + "/in") == "3");
    CHECK (rig.at ("/godot/range/" + ranges[1] + "/out") == "5");

    /*  A RANGE OF THE SOUND is the movie's to change. */
    CHECK (rig.apply ("node.set", { text ("/godot/range/" + ranges[0] + "/in"), text ("0.5") }).applied == 0);
    CHECK (rig.apply ("object.delete", { text (ranges[0]) }).applied == 0);
    CHECK_FALSE (rig.applied ("range.create", { text ("VD000040"), osc::Value::float64 (10.0), osc::Value::float64 (11.0) }));

    REQUIRE (rig.applied ("object.delete", { text ("VD000081") }));
    ranges = soundRanges();
    REQUIRE (ranges.size() == 2);
    CHECK (rig.at ("/godot/range/" + ranges[0] + "/in") == "3");

    /*  ONE UNDO TAKES BACK BOTH. */
    REQUIRE (rig.applied ("undo"));
    CHECK (soundRanges().size() == 3);
    CHECK (rig.at ("/godot/range/" + soundRanges()[0] + "/in") == "1");

    /*  DETACHED, the sound is its own: its rows take an edit, and the movie's
        no longer reach it. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/startOffset"), text ("0.75") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/rate"), text ("2") }));
    CHECK (rig.at ("/godot/cue/VD000040/rate") == "1.25");

    /*  LOCKED AGAIN, then the movie deleted: the sound stays, as a sound of
        its own, and its rows take edits again. */
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));
    CHECK (rig.at ("/godot/cue/VD000040/rate") == "2");
    REQUIRE (rig.applied ("object.delete", { text ("VD000002") }));
    CHECK (rig.exists ("/godot/cue/VD000040/name"));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/rate"), text ("1") }));
}

TEST_CASE ("video: a locked sound's copies of the movie's Ranges take the same identifiers on every machine")
{
    /*  TWO DOCUMENTS, the same edits: the sound's Ranges come out under the
        same identifiers, which is what lets a replay of the log reach them. */
    std::vector<std::string> made[2];

    for (auto& out : made)
    {
        Rig rig;
        REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
        REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("media"),
                                              text ("Sound"), text ("VD000040") }));
        REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));
        REQUIRE (rig.applied ("range.create", { text ("VD000002"), osc::Value::float64 (1.0), osc::Value::float64 (2.0),
                                                text ("VD000081") }));
        REQUIRE (rig.applied ("range.create", { text ("VD000002"), osc::Value::float64 (3.0), osc::Value::float64 (4.0),
                                                text ("VD000082") }));

        for (const auto& child : rig.document.findById ("VD000040"))
            if (child.hasType ("Range"))
                out.push_back (child["id"].toString().toStdString());
    }

    REQUIRE (made[0].size() == 2);
    CHECK (made[0] == made[1]);
}

/*  AND AS IT RUNS: the movie's GO fires its sound with it, as its child, in
    the same tick; a stop on the movie stops the sound; the standby walks past
    the sound, and a group does not fire it on its own (37.5, WJ). */
TEST_CASE ("video: a movie fires its locked sound with it, and stops it with it")
{
    VideoRig rig;

    const std::map<std::string, double> lengths { { "clip.mov", 20.0 }, { "clip (sound).wav", 20.0 } };
    rig.runner.setMediaDurations (&lengths);

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Clip"),
                                                osc::Value::string ("VD000060"),
                                                osc::Value::string ("source"), osc::Value::string ("movie"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("clip.mov") }).applied >= 1);
    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (1),
                                                osc::Value::string ("media"), osc::Value::string ("Clip sound"),
                                                osc::Value::string ("VD000061"),
                                                osc::Value::string ("file"), osc::Value::string ("clip (sound).wav") }).applied >= 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000061/lockedTo", "VD000060").ok);

    /*  THE STANDBY WALKS PAST IT: from the movie, the next stop is the cue
        after the sound. */
    const auto list = rig.document.findById ("VD000001");
    CHECK_FALSE (cue::mayStandOn (list, "VD000061"));
    CHECK (cue::mayStandOn (list, "VD000060"));

    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000060") });
    rig.ticks (2);

    const auto* movie = rig.runOf ("VD000060");
    const auto* sound = rig.runOf ("VD000061");
    REQUIRE (movie != nullptr);
    REQUIRE (sound != nullptr);
    CHECK (sound->parent == movie->id);
    CHECK (rig.runner.goOfRun (sound->id) == rig.runner.goOfRun (movie->id));

    const auto movieId = movie->id;
    const auto soundId = sound->id;
    CHECK_FALSE (rig.runs.find (soundId)->isFinished());

    /*  STOPPED WITH IT. */
    rig.submitAndTick ("run.stop", { osc::Value::string (movieId) });
    rig.ticks (4);
    CHECK (rig.runs.find (movieId)->isFinished());
    CHECK (rig.runs.find (soundId)->isFinished());

    /*  FIRED BY NAME, the sound plays alone, as any cue may be fired. */
    rig.submitAndTick ("cue.fire", { osc::Value::string ("VD000061") });
    rig.ticks (1);
    const auto* alone = rig.runOf ("VD000061");
    REQUIRE (alone != nullptr);
    CHECK (alone->id != soundId);
    CHECK (alone->parent.empty());
}

/*  ARMED WITH ITS MOVIE: the standby on the movie makes its sound ready, as
    it would a sound of its own, and the GO takes that run - the one the arm
    made - as the movie's child, so it launches where the picture comes up. */
namespace
{
    struct ArmingPlayer final : cue::Player
    {
        int trackCount() const override                                  { return 8; }
        void requestArm (const cue::ArmRequest&) override                {}
        int slotCount() const override                                   { return 1; }
        bool launchAtSample (int, int, std::int64_t) override            { return true; }
        bool stop (int) override                                         { return true; }
        bool stopAtSample (int, int, std::int64_t) override              { return true; }
        void setLevelDb (int, double) override                           {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                              { return false; }
        bool isArmReady (int) const override                             { return false; }
        std::int64_t samplesElapsed() const override                     { return 0; }
        int blockSize() const override                                   { return 128; }
        int sampleRate() const override                                  { return 48000; }
        int channelsPerTrack() const override                            { return 2; }
    };
}

TEST_CASE ("video: a movie on standby arms its locked sound, and its GO takes that run")
{
    ArmingPlayer audio;
    VideoRig rig;
    rig.runner.setPlayer (&audio);

    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (0),
                                                osc::Value::string ("video"), osc::Value::string ("Clip"),
                                                osc::Value::string ("VD000060"),
                                                osc::Value::string ("source"), osc::Value::string ("movie"),
                                                osc::Value::string ("canvas"), osc::Value::string ("VD000011"),
                                                osc::Value::string ("file"), osc::Value::string ("clip.mov") }).applied >= 1);
    REQUIRE (rig.submitAndTick ("cue.create", { osc::Value::string ("VD000001"), osc::Value::int32 (1),
                                                osc::Value::string ("media"), osc::Value::string ("Clip sound"),
                                                osc::Value::string ("VD000061"),
                                                osc::Value::string ("file"), osc::Value::string ("clip (sound).wav") }).applied >= 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/VD000061/lockedTo", "VD000060").ok);

    rig.submitAndTick ("list.focus", { osc::Value::string ("VD000001") });
    rig.submitAndTick ("standby.set", { osc::Value::string ("VD000060") });
    rig.ticks (3);

    const auto* armed = rig.runOf ("VD000061");
    REQUIRE (armed != nullptr);
    CHECK (armed->parent.empty());
    CHECK_FALSE (armed->isFinished());
    const auto armedId = armed->id;

    /*  LEFT IN PLACE while the pointer stays on the movie. */
    rig.ticks (3);
    CHECK_FALSE (rig.runs.find (armedId)->isFinished());

    rig.submitAndTick ("go");
    rig.ticks (1);

    const auto* movie = rig.runOf ("VD000060");
    REQUIRE (movie != nullptr);
    const auto* sound = rig.runs.find (armedId);
    REQUIRE (sound != nullptr);
    CHECK (sound->parent == movie->id);
    CHECK (rig.runOf ("VD000061")->id == armedId);
}

/*  A FINISHED CONVERSION WITH ITS SOUND (namespace draft 37.5, WJ): a media
    cue after each movie on the sound's file, locked to it, routed - one edit
    with the movie's, its identifiers on the record; and done again, the same
    cue pointed at the new file rather than a second one made. */
TEST_CASE ("video: a conversion with its sound makes the movie's sound a locked cue after it")
{
    Rig rig;
    video::registerConversionCommands (rig.engine.commands(), rig.document, nullptr);

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/file"), text ("clip.mp4") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/startOffset"), text ("12") }));

    const auto outcome = rig.apply ("media.converted", { text ("clip.mp4"), text ("clip (Hap).mov"),
                                                         osc::Value::float64 (2.0), text ("clip (sound).wav"),
                                                         osc::Value::int32 (2) });
    REQUIRE (outcome.applied == 1);

    const auto members = [&rig]
    {
        std::vector<std::string> out;

        for (const auto& child : rig.document.findById ("VD000001"))
            out.push_back (child["id"].toString().toStdString());

        return out;
    };

    auto list = members();
    REQUIRE (list.size() == 3);
    CHECK (list[0] == "VD000002");
    const auto sound = list[1];
    CHECK (list[2] == "VD000003");

    CHECK (rig.at ("/godot/cue/" + sound + "/kind") == "media");
    CHECK (rig.at ("/godot/cue/" + sound + "/file") == "clip (sound).wav");
    CHECK (rig.at ("/godot/cue/" + sound + "/lockedTo") == "VD000002");
    CHECK (rig.at ("/godot/cue/" + sound + "/name") == "Blue wash (sound)");
    CHECK (rig.at ("/godot/cue/" + sound + "/startOffset") == "10");
    CHECK (rig.at ("/godot/cue/VD000002/file") == "clip (Hap).mov");

    bool routed = false;

    for (const auto& child : rig.document.findById (sound))
        routed = routed || child.hasType ("Route");

    CHECK (routed);

    /*  THE RECORD CARRIES WHAT IT MADE, which a replay re-supplies. */
    const auto logged = LogFile::parse (rig.engine.log().contents());
    REQUIRE_FALSE (logged.records.empty());
    const auto& last = logged.records.back();
    REQUIRE (last.args.size() == 6);
    CHECK (last.args[5].getString() == sound);

    /*  ONE UNDO: the movie as it was, and no sound cue. */
    REQUIRE (rig.applied ("undo"));
    CHECK (members().size() == 2);
    CHECK (rig.at ("/godot/cue/VD000002/file") == "clip.mp4");

    /*  THE SOUND ALONE, of a movie that stays as it is, made under the
        identifier a replay supplies. */
    REQUIRE (rig.applied ("media.converted", { text ("clip.mp4"), text (""), osc::Value::float64 (0.0),
                                               text ("clip (sound).wav"), osc::Value::int32 (1), text ("VD000099") }));
    CHECK (rig.at ("/godot/cue/VD000002/file") == "clip.mp4");
    CHECK (rig.at ("/godot/cue/VD000099/lockedTo") == "VD000002");
    CHECK (rig.at ("/godot/cue/VD000099/startOffset") == "12");

    /*  AGAIN: the sound it has is pointed at the new file. */
    REQUIRE (rig.applied ("media.converted", { text ("clip.mp4"), text (""), osc::Value::float64 (0.0),
                                               text ("clip (sound) 2.wav"), osc::Value::int32 (1) }));
    CHECK (members().size() == 3);
    CHECK (rig.at ("/godot/cue/VD000099/file") == "clip (sound) 2.wav");

    /*  ASKED FOR THE SOUND ALONE, nothing is converted - and with no sound
        asked, "none" means nothing and is refused. */
    CHECK (rig.applied ("media.convert", { text ("clip.mp4"), text ("whole"), text ("none"), osc::Value::boolean (true) }));
    CHECK_FALSE (rig.applied ("media.convert", { text ("clip.mp4"), text ("whole"), text ("none"), osc::Value::boolean (false) }));
}

/*  THE WINDOW'S SIDE (namespace draft 37.6, F.4): the conversions as the
    readout says them and the sentence each change is worth; a sound's lock as
    a menu of the show's movies, and the rows the movie leads drawn, not typed. */
TEST_CASE ("video: the window reads the conversions, and a sound's lock as a menu of movies")
{
    CHECK (client::model::isMovieFile ("clip.MP4"));
    CHECK (client::model::isMovieFile ("show/clip.mov"));
    CHECK_FALSE (client::model::isMovieFile ("clip.wav"));

    client::model::ConversionRow running { "clip.mp4", "converting", 42, {} };
    CHECK (client::model::conversionNews (nullptr, running) == "Converting clip.mp4 to HAP: 42 %.");

    auto later = running;
    later.percent = 47;
    CHECK (client::model::conversionNews (&running, later).empty());
    later.percent = 51;
    CHECK_FALSE (client::model::conversionNews (&running, later).empty());

    client::model::FfmpegInstallRow idle, fetching { "downloading", 34, {}, "github.com" };
    CHECK (client::model::installNews (idle, fetching) == "Downloading FFmpeg from github.com: 34 %.");
    CHECK (client::model::installNews (fetching, fetching).empty());
    CHECK (fetching.running());
    client::model::FfmpegInstallRow ready { "done", 100, {}, "github.com" };
    CHECK (client::model::installNews (fetching, ready) == "FFmpeg is ready: movies can be converted to HAP and previewed.");

    client::model::ConversionRow failed { "clip.mp4", "failed", 0, "Invalid data found" };
    CHECK (client::model::conversionNews (&running, failed) == "clip.mp4 could not be converted to HAP: Invalid data found.");
    CHECK (client::model::conversionNews (&failed, failed).empty());

    Rig rig;

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("media"),
                                          text ("Clip sound"), text ("VD000040") }));

    const auto lockOf = [&rig]
    {
        rig.parameters.markStale();
        rig.snapshot = rig.parameters.publish (rig.tick, rig.state);
        const auto panel = client::model::inspect (*rig.snapshot, "VD000040");
        std::vector<client::model::Field> fields;

        for (const auto& block : panel.blocks)
            for (const auto& field : block.fields)
                fields.push_back (field);

        return fields;
    };

    const auto find = [] (const std::vector<client::model::Field>& fields, const std::string& name)
    {
        for (const auto& field : fields)
            if (field.name == name)
                return field;

        return client::model::Field {};
    };

    auto fields = lockOf();
    auto lock = find (fields, "lockedTo");
    CHECK (lock.label == "locked to movie");
    CHECK (lock.control == client::model::Control::movieRef);
    REQUIRE (lock.choices.size() == 2);
    CHECK (lock.choices[0].first.empty());
    CHECK (lock.choices[1].first == "VD000002");
    CHECK (find (fields, "startOffset").writable);

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));
    fields = lockOf();
    CHECK_FALSE (find (fields, "startOffset").writable);
    CHECK_FALSE (find (fields, "rate").writable);
    CHECK (find (fields, "level").writable);
}

/*  THE DUAL CUE (namespace draft 37.5, WM): the cue list draws a movie and the
    sound locked to it as one cue of two lines - the sound's row marked as the
    movie's second line, the movie's as having one, and the sound no place to
    park. Moved away from its movie, it is a locked sound on a line of its own. */
TEST_CASE ("video: the cue list draws a movie and its locked sound as one cue of two lines")
{
    Rig rig;

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("media"),
                                          text ("Clip sound"), text ("VD000040") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));

    rig.parameters.markStale();
    rig.snapshot = rig.parameters.publish (rig.tick, rig.state);

    client::model::ShowModel show;
    show.refresh (*rig.snapshot, "VD000001");

    const auto cueRows = [&show]
    {
        std::vector<client::model::Row> out;

        for (const auto& row : show.rows())
            if (row.rowKind == client::model::RowKind::cue)
                out.push_back (row);

        return out;
    };

    const auto rows = cueRows();
    REQUIRE (rows.size() == 3);
    CHECK (rows[0].soundBelow);
    CHECK (rows[1].soundOfAbove);
    CHECK (rows[1].followsMovie);
    CHECK_FALSE (rows[1].mayPark());
    CHECK (rows[0].mayPark());

    REQUIRE (rig.applied ("object.move", { text ("VD000040"), text ("VD000001"), osc::Value::int32 (3) }));
    rig.parameters.markStale();
    rig.snapshot = rig.parameters.publish (rig.tick, rig.state);
    show = client::model::ShowModel {};
    show.refresh (*rig.snapshot, "VD000001");

    const auto moved = cueRows();
    REQUIRE (moved.size() == 3);
    CHECK_FALSE (moved[0].soundBelow);
    CHECK (moved[2].followsMovie);
    CHECK_FALSE (moved[2].soundOfAbove);
}

TEST_CASE ("video: a movie moved takes the sound locked to it along, in one edit")
{
    Rig rig;

    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000002/source"), text ("movie") }));
    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (1), text ("media"),
                                          text ("Clip sound"), text ("VD000040") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/VD000040/lockedTo"), text ("VD000002") }));

    const auto order = [&rig]
    {
        std::vector<std::string> out;

        for (const auto& child : rig.document.findById ("VD000001"))
            out.push_back (child["id"].toString().toStdString());

        return out;
    };

    REQUIRE (order() == std::vector<std::string> { "VD000002", "VD000040", "VD000003" });

    //  Later in the list: the sound comes too, straight after it.
    REQUIRE (rig.applied ("object.move", { text ("VD000002"), text ("VD000001"), osc::Value::int32 (3) }));
    CHECK (order() == std::vector<std::string> { "VD000003", "VD000002", "VD000040" });

    //  And back to the top, the same.
    REQUIRE (rig.applied ("object.move", { text ("VD000002"), text ("VD000001"), osc::Value::int32 (0) }));
    CHECK (order() == std::vector<std::string> { "VD000002", "VD000040", "VD000003" });

    //  One undo, one move undone - both rows.
    REQUIRE (rig.applied ("undo"));
    CHECK (order() == std::vector<std::string> { "VD000003", "VD000002", "VD000040" });

    //  Into a group: both go in.
    REQUIRE (rig.applied ("cue.create", { text ("VD000001"), osc::Value::int32 (0), text ("group"),
                                          text ("Scene"), text ("VD000050") }));
    REQUIRE (rig.applied ("object.move", { text ("VD000002"), text ("VD000050"), osc::Value::int32 (0) }));

    std::vector<std::string> inside;

    for (const auto& child : rig.document.findById ("VD000050"))
        inside.push_back (child["id"].toString().toStdString());

    CHECK (inside == std::vector<std::string> { "VD000002", "VD000040" });
}

//==============================================================================
TEST_CASE ("video: a canvas's level and its DCA take the whole picture towards black")
{
    /*  Namespace draft §38, WT: a canvas's level, times what its DCA leaves
        along the fader's travel (the picture's law, 37.5 WE), handed to the
        video side on the tick one moves; the region carries it to the
        renderer by the canvas's name; and the composite is taken down once,
        towards black, never layer by layer. */
    VideoRig rig;
    cue::DcaTable dcas;
    rig.runner.setDcas (&dcas);

    rig.tickOnce();
    REQUIRE (! rig.sink.canvasLevelsSent.empty());
    CHECK (rig.sink.canvasLevelsSent.back() == std::vector<std::pair<std::string, double>> { { "VD000011", 1.0 } });

    const auto sentBefore = rig.sink.canvasLevelsSent.size();
    rig.tickOnce();
    CHECK (rig.sink.canvasLevelsSent.size() == sentBefore);      // nothing moved, nothing sent

    REQUIRE (rig.document.setAttribute ("/godot/canvas/VD000011/level", "50").ok);
    rig.tickOnce();
    REQUIRE (rig.sink.canvasLevelsSent.size() == sentBefore + 1);
    CHECK (rig.sink.canvasLevelsSent.back()[0].second == doctest::Approx (0.5));

    const auto dca = rig.document.createDca ("Pictures").id;
    REQUIRE (rig.document.setAttribute ("/godot/canvas/VD000011/dca", dca).ok);
    dcas.set (dca, -6.0);
    rig.tickOnce();
    CHECK (rig.sink.canvasLevelsSent.back()[0].second == doctest::Approx (0.5 * video::opacityForTrim (-6.0)));

    dcas.set (dca, -120.0);
    rig.tickOnce();
    CHECK (rig.sink.canvasLevelsSent.back()[0].second == doctest::Approx (0.0));

    /*  THE COMPOSITE, each channel towards black. */
    CHECK (video::scaledColour (0xFF8040u, 1.0) == 0xFF8040u);
    CHECK (video::scaledColour (0xFF8040u, 0.5) == 0x804020u);
    CHECK (video::scaledColour (0xFF8040u, 0.0) == 0u);
}
