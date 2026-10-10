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

/*  WHAT A SOUND WITH AN EDIT PLAYS (namespace draft §55, ADM, ADP).

    While its edit is open a sound cue plays the render of its sections, and
    is as long as they are put together; frozen, it plays its bounce as any
    sound plays its file. The one resolver of that is asked here directly,
    then through a Runner driven by hand with a player that is a notebook -
    the RangeTests' rig - so the arms it asks for, and when, are the facts:
    an edit with no render fails as `rendering` and is asked again once the
    render lands; a cue on standby follows a new render, a freeze and an
    unfreeze; a launched one keeps its file.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/PlayedMedia.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/SectionCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    /*  The audio side as a notebook, as RangeTests has it. */
    struct NotePlayer final : cue::Player
    {
        int trackCount() const override              { return 2; }
        int slotCount() const override               { return 8; }
        int blockSize() const override               { return 128; }
        int channelsPerTrack() const override        { return 1; }
        int sampleRate() const override              { return 48000; }
        std::int64_t samplesElapsed() const override { return samples; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int track, int, std::int64_t) override
        {
            playing.insert (track);
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            return true;
        }

        bool stopAtSample (int, int, std::int64_t) override { return true; }
        void setLevelDb (int, double) override {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override  { return playing.count (track) > 0; }
        bool isArmReady (int) const override       { return armsReady; }
        std::uint64_t placeLoop (int, int, const LoopMove&) override { return 1; }
        std::optional<LoopTaken> loopTaken (int, int) override { return std::nullopt; }

        /** The disk answers: every outstanding arm is reported as ready. */
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });

            arms.clear();
            armsReady = true;
        }

        std::int64_t samples = 0;
        bool armsReady = false;
        std::vector<cue::ArmRequest> arms;
        std::set<int> playing;
    };

    struct EditShow
    {
        EditShow()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            doc::registerSectionCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            runner.setMediaDurations (&lengths);
            runner.setEditRenders ([this] { return renders; });

            listId = document.createList ("Sound").id;
            cueId = document.createCue (listId, 0, "media", "Bed").id;

            document.setAttribute ("/godot/cue/" + cueId + "/file", "night.wav");
            document.setAttribute (cue::standbyAddressOf (listId), cueId);
        }

        juce::ValueTree cue() const { return document.findById (cueId); }

        /** The renderer has made the edit as it now is. */
        void renderLands (const std::string& file)
        {
            auto table = std::make_shared<audio::EditRenders>();
            audio::EditRender render;
            render.cue = cueId;
            render.editText = cue::editTextOf (cue());
            render.file = file;
            render.state = audio::renderState::done;
            render.seconds = cue::editedLengthOf (cue()).value_or (0.0);
            (*table)[cueId] = render;
            renders = table;
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            audio.samples += 960;
            return result;
        }

        void ticks (int howMany)
        {
            for (int i = 0; i < howMany; ++i)
                tickOnce();
        }

        Engine::TickResult submitAndTick (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (origin::cli, name, std::move (args)));
            return tickOnce();
        }

        /** The cue's latest run, finished or not; nothing before any. */
        const cue::Run* lastRun() const
        {
            const cue::Run* last = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    last = &run;

            return last;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (23);
        cue::Runner runner { document, runs, runIds, focus };
        NotePlayer audio;

        std::map<std::string, double> lengths { { "night.wav", 30.0 } };
        std::shared_ptr<const audio::EditRenders> renders;

        std::string listId, cueId;
        std::int64_t tick = 0;
    };
}

//==============================================================================
TEST_CASE ("played media: a plain sound plays its file; an open edit plays its render and is as long as its sections")
{
    doc::ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto cueId = document.createCue (listId, 0, "media", "Rain").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/file", "rain.wav").ok);

    const std::map<std::string, double> lengths { { "rain.wav", 30.0 } };
    const auto cue = document.findById (cueId);

    auto played = cue::playedMediaOf (cue, &lengths, nullptr);
    CHECK (played.name == "rain.wav");
    CHECK (played.lengthKnown);
    CHECK (played.lengthSeconds == doctest::Approx (30.0));
    CHECK_FALSE (played.openEdit);
    CHECK_FALSE (played.frozen);
    CHECK_FALSE (cue::editedLengthOf (cue).has_value());

    /*  A length the show does not know is not known here either. */
    played = cue::playedMediaOf (cue, nullptr, nullptr);
    CHECK (played.name == "rain.wav");
    CHECK_FALSE (played.lengthKnown);

    /*  Cut in two and the second piece gone: an open edit, twenty seconds
        long whatever any file says, playing nothing until rendered. */
    REQUIRE (document.splitSection (cueId, 20.0, 30.0, {}).ok);
    const auto second = document.sectionsOf (cue)[1].id;
    REQUIRE (document.removeSection (second).ok);

    played = cue::playedMediaOf (cue, &lengths, nullptr);
    CHECK (played.openEdit);
    CHECK (played.name.empty());
    CHECK (played.lengthKnown);
    CHECK (played.lengthSeconds == doctest::Approx (20.0));
    CHECK (cue::editedLengthOf (cue) == std::optional<double> (20.0));
    CHECK (cue::playedLengthOf (cue, nullptr) == std::optional<double> (20.0));

    /*  A render of another edit is not this one's; one of this edit is. */
    audio::EditRenders renders;
    audio::EditRender render;
    render.cue = cueId;
    render.editText = "0 20 0 0;0 1 0 0;";
    render.file = ".edits/stale.wav";
    render.state = audio::renderState::done;
    renders[cueId] = render;
    CHECK (cue::playedMediaOf (cue, &lengths, &renders).name.empty());

    renders[cueId].editText = cue::editTextOf (cue);
    renders[cueId].state = audio::renderState::rendering;
    CHECK (cue::playedMediaOf (cue, &lengths, &renders).name.empty());

    renders[cueId].state = audio::renderState::done;
    renders[cueId].file = ".edits/fresh.wav";
    played = cue::playedMediaOf (cue, &lengths, &renders);
    CHECK (played.name == ".edits/fresh.wav");
    CHECK (played.lengthSeconds == doctest::Approx (20.0));

    /*  Frozen, the bounce is the file and nothing here knows an edit from a
        plain sound; the render is not asked for. */
    REQUIRE (document.freezeEdit (cueId, "rain.wav", "rain (edit).wav").ok);
    played = cue::playedMediaOf (cue, &lengths, &renders);
    CHECK (played.name == "rain (edit).wav");
    CHECK (played.frozen);
    CHECK_FALSE (played.openEdit);
    CHECK_FALSE (played.lengthKnown);   // no length in the table for the bounce yet
    CHECK_FALSE (cue::editedLengthOf (cue).has_value());
}

TEST_CASE ("played media: an edit that is the whole file as recorded plays the file, once the file's length is known")
{
    doc::ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto cueId = document.createCue (listId, 0, "media", "Rain").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/file", "rain.wav").ok);
    REQUIRE (document.splitSection (cueId, 20.0, 30.0, {}).ok);
    const auto cue = document.findById (cueId);
    REQUIRE (document.joinSection (document.sectionsOf (cue)[0].id).ok);

    const std::map<std::string, double> lengths { { "rain.wav", 30.0 } };
    auto played = cue::playedMediaOf (cue, &lengths, nullptr);
    CHECK (played.name == "rain.wav");
    CHECK_FALSE (played.openEdit);

    /*  Not known to be the whole file, it waits for a render like any edit. */
    played = cue::playedMediaOf (cue, nullptr, nullptr);
    CHECK (played.openEdit);
    CHECK (played.name.empty());
    CHECK (played.lengthSeconds == doctest::Approx (30.0));
}

//==============================================================================
TEST_CASE ("edit playback: an edit with no render fails its arm as rendering, and is armed once the render lands")
{
    EditShow show;

    REQUIRE (show.document.splitSection (show.cueId, 10.0, 30.0, {}).ok);
    show.ticks (5);

    CHECK (show.audio.arms.empty());
    REQUIRE (show.lastRun() != nullptr);
    CHECK (show.lastRun()->error == "rendering");

    /*  Nothing is asked again while nothing changes. */
    show.ticks (20);
    CHECK (show.audio.arms.empty());

    /*  The render lands: the standby is made ready again, on the render. */
    show.renderLands (".edits/first.wav");
    show.ticks (5);

    REQUIRE (show.audio.arms.size() == 1u);
    CHECK (show.audio.arms.front().mediaFile.find (".edits") != std::string::npos);
    CHECK (show.audio.arms.front().mediaFile.find ("first.wav") != std::string::npos);
    CHECK (show.lastRun()->error.empty());
}

TEST_CASE ("edit playback: a cue on standby follows a new render once the edits stop, and a launched one keeps its file")
{
    EditShow show;

    REQUIRE (show.document.splitSection (show.cueId, 10.0, 30.0, {}).ok);
    show.renderLands (".edits/first.wav");
    show.ticks (5);
    REQUIRE (show.audio.arms.size() == 1u);
    show.audio.completeArms (show.engine);
    show.ticks (2);

    /*  An edge moved: the render is of the old edit, so the cue plays
        nothing new yet and is not armed again. */
    const auto second = show.document.sectionsOf (show.cue())[1].id;
    REQUIRE (show.document.trimSection (second, 12.0, 30.0).ok);
    show.ticks (15);
    CHECK (show.audio.arms.empty());

    /*  The new render: one arm, with it, a fifth of a second on. */
    show.renderLands (".edits/second.wav");
    show.ticks (3);
    CHECK (show.audio.arms.empty());
    show.ticks (12);
    REQUIRE (show.audio.arms.size() == 1u);
    CHECK (show.audio.arms.front().mediaFile.find ("second.wav") != std::string::npos);
    show.audio.completeArms (show.engine);
    show.ticks (2);

    /*  Launched, the run keeps the file it plays whatever lands. */
    show.submitAndTick ("go");
    show.ticks (20);
    REQUIRE (show.lastRun() != nullptr);
    CHECK (show.lastRun()->state == cue::runState::playing);

    REQUIRE (show.document.trimSection (second, 14.0, 30.0).ok);
    show.renderLands (".edits/third.wav");
    show.ticks (20);
    CHECK (show.audio.arms.empty());
}

TEST_CASE ("edit playback: a freeze arms the standby on the bounce, and an unfreeze on the render again")
{
    EditShow show;

    REQUIRE (show.document.splitSection (show.cueId, 10.0, 30.0, {}).ok);
    show.renderLands (".edits/first.wav");
    show.ticks (5);
    REQUIRE (show.audio.arms.size() == 1u);
    show.audio.completeArms (show.engine);
    show.ticks (2);

    REQUIRE (show.document.freezeEdit (show.cueId, "night.wav", "night (edit).wav").ok);
    show.ticks (15);
    REQUIRE (show.audio.arms.size() == 1u);
    CHECK (show.audio.arms.front().mediaFile.find ("night (edit).wav") != std::string::npos);
    show.audio.completeArms (show.engine);
    show.ticks (2);

    REQUIRE (show.document.unfreezeEdit (show.cueId).ok);
    show.ticks (15);
    REQUIRE (show.audio.arms.size() == 1u);
    CHECK (show.audio.arms.front().mediaFile.find ("first.wav") != std::string::npos);
}

TEST_CASE ("edit playback: the tree's duration is the edit's while it is open, the file's otherwise")
{
    EditShow show;

    tree::MountTable mounts;
    tree::ParameterTree parameters { show.document, show.engine.commands(), mounts, show.runs };
    parameters.setMediaDurations (&show.lengths);

    const auto duration = [&]
    {
        parameters.markStale();
        tree::EngineState state;
        state.version = "test";
        const auto snapshot = parameters.publish (0, state);
        const auto* node = snapshot->find ("/godot/cue/" + show.cueId + "/duration");
        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        return node->soleValue()->asDouble();
    };

    CHECK (duration() == doctest::Approx (30.0));

    REQUIRE (show.document.splitSection (show.cueId, 10.0, 30.0, {}).ok);
    CHECK (duration() == doctest::Approx (30.0));

    const auto second = show.document.sectionsOf (show.cue())[1].id;
    REQUIRE (show.document.removeSection (second).ok);
    CHECK (duration() == doctest::Approx (10.0));

    REQUIRE (show.document.freezeEdit (show.cueId, "night.wav", "night (edit).wav").ok);
    CHECK (duration() == doctest::Approx (0.0));   // the bounce's length is not in the table yet

    show.lengths["night (edit).wav"] = 10.0;
    parameters.setMediaDurations (&show.lengths);
    CHECK (duration() == doctest::Approx (10.0));
}
