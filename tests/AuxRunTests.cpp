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

/*  THE AUXES IN THE SHOW (namespace draft §93): what the Runner does with an
    aux the show holds - a shared rack channel - against an audio side that
    only writes down what it is told.

    - The show's values reach the aux's track on the first tick, and again when
      somebody edits them: its level, its routing, its EQ.
    - A cue's send into an aux lands on the aux's columns, past the outputs.
    - A fade aimed at an aux moves it for tonight and leaves the show alone.
    - A double Esc lets tonight's values go and shuts the aux; the next GO
      opens it again at the show's values.
    - Doh! takes a GO's fade on an aux back.
    - A send into an aux the graph does not have is silent, and the cue plays.
    - Deleting an aux takes the sends into it with it.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/client/model/Eq.h>
#include <wfg/client/model/FadeMix.h>
#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Fx.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Sends.h>
#include <wfg/client/model/Text.h>

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    constexpr int auxTrack = 9;
    constexpr int auxColumn = 64;

    /*  AN AUDIO SIDE WITH ONE AUX, built as track nine with its input at
        column sixty-four, that writes down everything it is told. It arms and
        never gets ready, so a fired media cue stays armed and still. */
    struct AuxPlayer final : cue::Player
    {
        int trackCount() const override                                  { return 8; }
        void requestArm (const cue::ArmRequest& request) override        { arms.push_back (request); }
        int slotCount() const override                                   { return 1; }
        bool launchAtSample (int, int, std::int64_t) override            { return true; }
        bool stop (int) override                                         { return true; }
        bool stopAtSample (int, int, std::int64_t) override              { return true; }
        void setLevelDb (int track, double levelDb) override             { levels[track] = levelDb; }
        void setRouting (int track, const std::vector<cue::Coefficient>& coefficients) override
        {
            routings[track] = coefficients;
        }
        void setEq (int track, const audio::EqSettings& settings) override { eqs[track] = settings; }
        bool isPlaying (int) const override                              { return false; }
        bool isArmReady (int) const override                             { return false; }
        std::int64_t samplesElapsed() const override                     { return 0; }
        int blockSize() const override                                   { return 128; }
        int sampleRate() const override                                  { return 48000; }
        int channelsPerTrack() const override                            { return 2; }

        int rackTrackOf (const std::string& id) const override           { return id == auxId ? auxTrack : -1; }
        int auxColumnOf (const std::string& id) const override           { return id == auxId ? auxColumn : -1; }
        void openAux (int track) override                                { ++opened[track]; }
        void resetEffects (const std::vector<int>&) override             { ++sweeps; }

        std::string auxId;
        std::vector<cue::ArmRequest> arms;
        std::map<int, double> levels;
        std::map<int, std::vector<cue::Coefficient>> routings;
        std::map<int, audio::EqSettings> eqs;
        std::map<int, int> opened;
        int sweeps = 0;
    };

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       if (cue::isLiveWrite (appliedCommand.name, coerced))
                                           return;

                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            runner.setDcas (&dcas);
            runner.setSamplesPerTick (960);

            listId = document.createList ("Sound").id;

            //  The outputs: a stereo direct out on 0-1, then the aux.
            outId = document.createBus ("direct", 2).id;
            auxId = document.createRackChannel ("stereo").id;
            set ("/godot/slot/" + auxId + "/access", "shared");
            set ("/godot/slot/" + auxId + "/name", "Reverb");
            set ("/godot/slot/" + auxId + "/directOut", outId);
            set ("/godot/slot/" + auxId + "/level", "-6");

            audio.auxId = auxId;
            runner.setPlayer (&audio);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        void apply (const std::string& name, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            const auto result = tickOnce();
            REQUIRE_MESSAGE (result.applied >= 1, name << " was refused: " << engine.lastError());
        }

        void set (const std::string& address, const std::string& text)
        {
            const auto result = document.setAttribute (address, text);
            REQUIRE_MESSAGE (result.ok, address << " = " << text);
        }

        std::string media (const std::string& name)
        {
            const auto id = document.createCue (listId, 0, "media", name).id;
            set ("/godot/cue/" + id + "/file", name + ".wav");
            set ("/godot/cue/" + id + "/channels", "2");
            set ("/godot/cue/" + id + "/directOut", outId);
            return id;
        }

        std::string auxFade (double toDb, double seconds)
        {
            const auto id = document.createCue (listId, 0, "fade", "aux down").id;
            set ("/godot/cue/" + id + "/aux", auxId);
            set ("/godot/cue/" + id + "/level", std::to_string (static_cast<int> (toDb)));
            set ("/godot/cue/" + id + "/duration", osc::formatDouble (seconds));
            return id;
        }

        double auxLevel() const
        {
            const auto found = audio.levels.find (auxTrack);
            REQUIRE_MESSAGE (found != audio.levels.end(), "the aux was given no level");
            return found->second;
        }

        AuxPlayer audio;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (19) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::string listId, outId, auxId;
        std::int64_t tick = 1;

        std::string published (const std::string& address)
        {
            parameters.markStale();
            const auto snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }
    };

    bool near (double a, double b, double within = 1.0e-6) { return std::abs (a - b) < within; }

    float gainTo (const std::vector<cue::Coefficient>& routing, int input, int output)
    {
        for (const auto& c : routing)
            if (c.input == input && c.output == output)
                return c.gain;

        return 0.0f;
    }
}

TEST_CASE ("aux runner: the show's aux reaches its track on the first tick, and again after an edit")
{
    Rig rig;
    rig.tickOnce();

    CHECK (near (rig.auxLevel(), -6.0));

    //  Its direct out: a stereo aux onto the stereo out, side to side.
    REQUIRE (rig.audio.routings.count (auxTrack) == 1);
    CHECK (gainTo (rig.audio.routings[auxTrack], 0, 0) == doctest::Approx (1.0f));
    CHECK (gainTo (rig.audio.routings[auxTrack], 1, 1) == doctest::Approx (1.0f));
    CHECK (rig.audio.eqs.count (auxTrack) == 1);

    //  An edit: the level moved, and its EQ's high-pass switched on.
    rig.set ("/godot/slot/" + rig.auxId + "/level", "-3");
    rig.set ("/godot/slot/" + rig.auxId + "/eqHpf", "true");
    rig.tickOnce();

    CHECK (near (rig.auxLevel(), -3.0));
    CHECK (rig.audio.eqs[auxTrack].hpf);

    //  And it sends into itself - a loop the graph carries a block late.
    REQUIRE (rig.document.createSend (rig.auxId, rig.auxId, {}, "-10").ok);
    rig.tickOnce();

    const auto feedback = static_cast<float> (juce::Decibels::decibelsToGain (-10.0));
    CHECK (gainTo (rig.audio.routings[auxTrack], 0, auxColumn) == doctest::Approx (feedback));
    CHECK (gainTo (rig.audio.routings[auxTrack], 1, auxColumn + 1) == doctest::Approx (feedback));
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("aux runner: a cue's send into an aux lands on the aux's columns, below the cue")
{
    Rig rig;

    const auto voice = rig.media ("voice");
    REQUIRE (rig.document.createSend (voice, rig.auxId, {}, "-6").ok);

    rig.apply ("cue.fire", { osc::Value::string (voice) });
    REQUIRE_FALSE (rig.audio.arms.empty());

    const auto& routing = rig.audio.arms.back().routing;
    const auto half = static_cast<float> (juce::Decibels::decibelsToGain (-6.0));

    CHECK (gainTo (routing, 0, 0) == doctest::Approx (1.0f));
    CHECK (gainTo (routing, 0, auxColumn) == doctest::Approx (half));
    CHECK (gainTo (routing, 1, auxColumn + 1) == doctest::Approx (half));
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("aux runner: a send into an aux the graph was built without is silent, and the cue plays")
{
    Rig rig;

    //  Declared since the graph: the audio side has no column for it.
    const auto later = rig.document.createRackChannel ("stereo").id;
    rig.set ("/godot/slot/" + later + "/access", "shared");

    const auto voice = rig.media ("voice");
    REQUIRE (rig.document.createSend (voice, later, {}, "0").ok);

    rig.apply ("cue.fire", { osc::Value::string (voice) });
    REQUIRE_FALSE (rig.audio.arms.empty());

    const auto& routing = rig.audio.arms.back().routing;
    CHECK (gainTo (routing, 0, 0) == doctest::Approx (1.0f));
    CHECK (std::none_of (routing.begin(), routing.end(), [] (const cue::Coefficient& c) { return c.output >= 2; }));
}

TEST_CASE ("aux runner: a fade on an aux moves it tonight and leaves the show alone")
{
    Rig rig;
    rig.tickOnce();

    const auto fade = rig.auxFade (-26, 1.0);
    rig.apply ("cue.fire", { osc::Value::string (fade) });

    //  Halfway, straight in dB: -6 to -26 is -16 at the middle.
    rig.ticks (24);
    CHECK (near (rig.auxLevel(), -16.0, 1.0));

    rig.ticks (40);
    CHECK (near (rig.auxLevel(), -26.0));

    //  The show still holds -6.
    CHECK (rig.document.getAttribute ("/godot/slot/" + rig.auxId + "/level").value_or ("") == "-6");
}

TEST_CASE ("aux runner: a double Esc lets tonight go and shuts the aux; the next GO opens it at the show's values")
{
    Rig rig;
    rig.tickOnce();

    const auto fade = rig.auxFade (-40, 0.0);
    rig.apply ("cue.fire", { osc::Value::string (fade) });
    rig.ticks (2);
    REQUIRE (near (rig.auxLevel(), -40.0));

    rig.apply ("run.killAll", {});
    rig.ticks (2);
    CHECK (rig.audio.sweeps >= 1);
    CHECK (rig.audio.opened[auxTrack] == 0);

    //  A GO: the standby is a cue that plays nothing worth waiting on.
    const auto next = rig.media ("next");
    rig.set ("/godot/list/" + rig.listId + "/standby", next);
    rig.apply ("go", {});
    rig.ticks (2);

    CHECK (rig.audio.opened[auxTrack] == 1);
    CHECK (near (rig.auxLevel(), -6.0));
}

TEST_CASE ("aux runner: deleting an aux takes the sends into it away, in one step")
{
    Rig rig;

    const auto voice = rig.media ("voice");
    const auto send = rig.document.createSend (voice, rig.auxId, {}, "-6").id;
    REQUIRE_FALSE (send.empty());

    REQUIRE (rig.document.remove (rig.auxId).ok);
    CHECK_FALSE (rig.document.findById (send).isValid());
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("aux runner: Doh! takes a GO's fade on an aux back to where it stood")
{
    Rig rig;
    rig.tickOnce();
    REQUIRE (near (rig.auxLevel(), -6.0));

    const auto fade = rig.auxFade (-30, 0.5);
    rig.set ("/godot/list/" + rig.listId + "/standby", fade);
    rig.apply ("go", {});
    rig.ticks (40);
    REQUIRE (near (rig.auxLevel(), -30.0));

    rig.apply ("go.doh", {});
    rig.ticks (80);

    CHECK (near (rig.auxLevel(), -6.0, 0.01));
}

TEST_CASE ("aux runner: an aux's own sound, sends and inserts are published where a client reads them")
{
    Rig rig;

    const auto send = rig.document.createSend (rig.auxId, rig.auxId, {}, "-12").id;
    REQUIRE_FALSE (send.empty());

    CHECK (rig.published ("/godot/slot/" + rig.auxId + "/level") == "-6");
    CHECK (rig.published ("/godot/slot/" + rig.auxId + "/directOut") == rig.outId);
    CHECK (rig.published ("/godot/slot/" + rig.auxId + "/access") == "shared");
    CHECK (rig.published ("/godot/slot/" + rig.auxId + "/sends") == send);
    CHECK (rig.published ("/godot/send/" + send + "/cue") == rig.auxId);
    CHECK (rig.published ("/godot/send/" + send + "/level") == "-12");
}

//==============================================================================
/*  THE WINDOW'S READINGS OF AN AUX (namespace draft §93, A.5): the models the
    panels draw from, over the snapshot the engine publishes. */
namespace
{
    std::shared_ptr<const tree::TreeSnapshot> snapshotOf (Rig& rig)
    {
        rig.parameters.markStale();
        return rig.parameters.publish (rig.tick, rig.state);
    }

    const client::model::Field* fieldNamed (const client::model::Inspection& inspection, const std::string& name)
    {
        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return &field;

        return nullptr;
    }
}

TEST_CASE ("aux window: a cue's mixer carries a strip for the aux, after the mixes, and says it is an aux")
{
    Rig rig;

    const auto mix = rig.document.createBus ("mix", 2).id;
    const auto voice = rig.media ("voice");
    REQUIRE (rig.document.createSend (voice, rig.auxId, {}, "-8").ok);

    const auto snapshot = snapshotOf (rig);
    const auto strips = client::model::readSends (*snapshot, voice);

    REQUIRE (strips.size() == 2);
    CHECK (strips[0].busId == mix);
    CHECK (strips[1].busId == rig.auxId);
    CHECK (strips[1].name == "Reverb");
    CHECK (strips[1].channelWord == "Aux");
    CHECK (strips[1].present());
    CHECK (strips[1].levelDb == doctest::Approx (-8.0));

    //  The aux's own mixer: its level where its rows are, and a strip into itself.
    const auto foot = client::model::readFoot (*snapshot, { client::model::Subject::Kind::sends, rig.auxId }, {});
    CHECK (foot.cueKind == "aux");
    CHECK (foot.base == "/godot/slot/" + rig.auxId + "/");
    CHECK (foot.cueName == "Reverb");
    CHECK (foot.cueLevel == doctest::Approx (-6.0));
    CHECK (foot.notice.empty());
    CHECK (std::any_of (foot.sends.begin(), foot.sends.end(),
                        [&rig] (const client::model::SendStrip& strip) { return strip.busId == rig.auxId; }));
}

TEST_CASE ("aux window: an aux's EQ and inserts are read where its rows are")
{
    Rig rig;
    rig.set ("/godot/slot/" + rig.auxId + "/eqHpf", "true");
    rig.set ("/godot/slot/" + rig.auxId + "/eqHpfFreq", "180");

    const auto snapshot = snapshotOf (rig);

    const auto eq = client::model::readEq (*snapshot, rig.auxId);
    REQUIRE (eq.present);
    CHECK (eq.settings.hpf);
    CHECK (eq.settings.hpfFreq == doctest::Approx (180.0f));

    //  No plugins on its chain yet: said, and where to add one.
    const auto fx = client::model::readFx (*snapshot, rig.auxId);
    CHECK (fx.notice.empty() == false);
    CHECK (fx.notice.find ("Reverb carries no plugins yet") != std::string::npos);
}

TEST_CASE ("aux window: a fade aimed at an aux draws the aux's level, and the inspector offers the auxes")
{
    Rig rig;

    const auto fade = rig.auxFade (-20, 2.0);
    const auto snapshot = snapshotOf (rig);

    const auto mix = client::model::readFadeMix (*snapshot, fade);
    REQUIRE (mix.present);
    CHECK (mix.targetKind == "aux");
    CHECK (mix.targetName == "Reverb");
    CHECK (mix.hasEq);

    const auto inspection = client::model::inspect (*snapshot, fade);
    const auto* aux = fieldNamed (inspection, "aux");
    REQUIRE (aux != nullptr);
    CHECK (aux->value == rig.auxId);
    CHECK (std::any_of (aux->choices.begin(), aux->choices.end(),
                        [&rig] (const auto& choice) { return choice.first == rig.auxId; }));
}

TEST_CASE ("aux window: a mic cue's channel menu leaves the auxes out")
{
    Rig rig;

    const auto mine = rig.document.createRackChannel ("mono").id;
    const auto mic = rig.document.createCue (rig.listId, 0, "mic", "voice").id;
    REQUIRE_FALSE (mic.empty());

    const auto snapshot = snapshotOf (rig);
    const auto inspection = client::model::inspect (*snapshot, mic);
    const auto* channel = fieldNamed (inspection, "channel");
    REQUIRE (channel != nullptr);

    CHECK (std::any_of (channel->choices.begin(), channel->choices.end(),
                        [&mine] (const auto& choice) { return choice.first == mine; }));
    CHECK (std::none_of (channel->choices.begin(), channel->choices.end(),
                         [&rig] (const auto& choice) { return choice.first == rig.auxId; }));
}
