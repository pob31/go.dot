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

/*  BINDINGS (namespace draft §74, AQX-ARD): a control in the room - a MIDI
    controller, a note, an OSC address - wired to a number in the tree, scaled
    into its range, live while the show is open or for each run of a cue. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/binding/BindingIndex.h>
#include <wfg/engine/binding/Scaling.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/client/model/Text.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    struct Rig
    {
        Rig()
        {
            doc::registerDocumentCommands (engine.commands(), document);
            tree::registerTreeCommands (engine.commands(), touches);

            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "media", "Door").id;
            REQUIRE_FALSE (cueId.empty());
        }

        Engine::TickResult send (const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin::cli, command, std::move (args)));
            const auto result = engine.processTick (tick++);
            parameters.markStale();
            return result;
        }

        std::string published (const std::string& address)
        {
            tree::EngineState state;
            state.documentRevision = document.showRevision();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::string listId, cueId;
        std::int64_t tick = 1;
    };

    osc::Value s (const std::string& text) { return osc::Value::string (text); }
}

//==============================================================================
TEST_CASE ("binding: a show has none until one is made, and the first makes the show's collection")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    //  A show without bindings writes none - an old show saves unchanged.
    CHECK (doc::CanonicalXml::write (rig.document).find ("Bindings") == std::string::npos);

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00001"), s ("number"), s ("7"),
                                            s ("target"), s ("/godot/cue/" + rig.cueId + "/level") }).rejected == 0);

    CHECK (rig.published ("/godot/binding/order") == "BND00001");
    CHECK (rig.published ("/godot/binding/BND00001/number") == "7");
    CHECK (rig.published ("/godot/binding/BND00001/target") == "/godot/cue/" + rig.cueId + "/level");
    CHECK (rig.published ("/godot/binding/BND00001/cue").empty());

    //  Its defaults: a MIDI controller, absolute, over the target's own range, picked up.
    CHECK (rig.published ("/godot/binding/BND00001/source") == "midi");
    CHECK (rig.published ("/godot/binding/BND00001/type") == "controlChange");
    CHECK (rig.published ("/godot/binding/BND00001/control") == "absolute");
    CHECK (rig.published ("/godot/binding/BND00001/ownRange") == "true");
    CHECK (rig.published ("/godot/binding/BND00001/pickup") == "true");
    CHECK (rig.published ("/godot/binding/BND00001/feedback") == "false");
    CHECK (rig.published ("/godot/binding/BND00001/enabled") == "true");

    //  Written before the timecode settings, and read back as it was.
    const auto written = doc::CanonicalXml::write (rig.document);
    const auto bindings = written.find ("<Bindings>");
    REQUIRE (bindings != std::string::npos);
    CHECK (bindings < written.find ("<Sync"));

    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (written, reopened).ok);
    CHECK (doc::CanonicalXml::write (reopened) == written);
    CHECK (reopened.getAttribute ("/godot/binding/BND00001/number") == std::optional<std::string> ("7"));
}

TEST_CASE ("binding: one of a cue names it, and what is not a cue, or not there, is refused")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.send ("binding.create", { s (rig.cueId), s ("BND00002"), s ("target"), s ("level") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/BND00002/cue") == rig.cueId);
    CHECK (rig.published ("/godot/binding/BND00002/target") == "level");

    CHECK (rig.send ("binding.create", { s ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (rig.listId) }).rejected == 1);

    //  Its cue and its identifier have their own places; a word the row does not take is refused.
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("cue"), s (rig.cueId) }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("control"), s ("sideways") }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("source") }).rejected == 1);
}

TEST_CASE ("binding: an OSC address is one arriving on Go.dot's port, never one of the tree's")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00003"), s ("source"), s ("osc"),
                                            s ("address"), s ("/desk/fader/1") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/BND00003/address") == "/desk/fader/1");

    CHECK (rig.send ("binding.create", { s (""), s (""), s ("address"), s ("/godot/cue/x/level") }).rejected == 1);
    CHECK_FALSE (rig.document.setAttribute ("/godot/binding/BND00003/address", "/godot").ok);
    CHECK_FALSE (rig.document.setAttribute ("/godot/binding/BND00003/address", "fader").ok);
    CHECK (rig.document.setAttribute ("/godot/binding/BND00003/address", "").ok);

    //  And it goes as anything goes.
    CHECK (rig.send ("object.delete", { s ("BND00003") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/order").empty());
}

TEST_CASE ("binding: a locked show gains none, nor an empty collection")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.send ("binding.create", { s ("") }).rejected == 1);
    CHECK (doc::CanonicalXml::write (rig.document).find ("Bindings") == std::string::npos);
}

//==============================================================================
/*  BD.8: THE LAWS AND THE MATCHERS, pure. */
TEST_CASE ("binding scaling: absolute, over the target's own range by its own law, or a straight line")
{
    binding::Scale scale;
    binding::TargetRange level;
    level.hasMinimum = level.hasMaximum = true;
    level.minimum = -120.0;
    level.maximum = 12.0;
    level.unit = "dB";

    //  A level along the fader's travel: the bottom silence, the top the range's.
    CHECK (binding::absoluteValue (scale, 0.0, level) <= -119.0);
    CHECK (binding::absoluteValue (scale, 1.0, level) <= 12.0);
    CHECK (binding::absoluteValue (scale, 1.0, level) > 0.0);

    //  And back, for feedback: the same place on the travel.
    const auto threeQuarters = binding::absoluteValue (scale, 0.75, level);
    CHECK (std::abs (binding::travelFor (scale, threeQuarters, level) - 0.75) < 1.0e-6);

    //  A frequency in ratios: the middle of 20..20000 is 632, not 10010.
    const binding::TargetRange frequency { false, true, 20.0, true, 20000.0, "Hz" };
    CHECK (std::abs (binding::absoluteValue (scale, 0.5, frequency) - std::sqrt (20.0 * 20000.0)) < 1.0e-6);
    CHECK (std::abs (binding::travelFor (scale, std::sqrt (20.0 * 20000.0), frequency) - 0.5) < 1.0e-9);

    //  A whole number rounded; a straight line between outLow and outHigh when asked.
    const binding::TargetRange count { true, true, 0.0, true, 10.0, "" };
    CHECK (std::abs (binding::absoluteValue (scale, 0.34, count) - 3.0) < 1.0e-9);

    auto line = scale;
    line.ownRange = false;
    line.outLow = -6.0;
    line.outHigh = 0.0;
    CHECK (std::abs (binding::absoluteValue (line, 0.5, level) - (-3.0)) < 1.0e-9);
    CHECK (std::abs (binding::travelFor (line, -3.0, level) - 0.5) < 1.0e-9);

    //  The control's travel: inLow and inHigh, clamped, and inverted.
    auto part = scale;
    part.inLow = 0.25;
    part.inHigh = 0.75;
    CHECK (std::abs (binding::travelOf (part, 0.5) - 0.5) < 1.0e-9);
    CHECK (std::abs (binding::travelOf (part, 0.0)) < 1.0e-9);
    CHECK (std::abs (binding::travelOf (part, 1.0) - 1.0) < 1.0e-9);
    part.invert = true;
    CHECK (std::abs (binding::travelOf (part, 0.25) - 1.0) < 1.0e-9);
    CHECK (std::abs (binding::inputFor (part, 1.0) - 0.25) < 1.0e-9);
}

TEST_CASE ("binding scaling: relative turns as the master dial does, and rate moves by its push and dead zone")
{
    binding::Scale scale;
    const binding::TargetRange wait { false, true, 0.0, true, 3600.0, "s" };

    CHECK (binding::midiSteps (1) == 1);
    CHECK (binding::midiSteps (63) == 63);
    CHECK (binding::midiSteps (65) == -1);
    CHECK (binding::midiSteps (127) == -63);
    CHECK (binding::midiSteps (0) == 0);
    CHECK (binding::midiSteps (64) == 0);

    CHECK (std::abs (binding::relativeValue (scale, 0.5, 1, wait) - 0.6) < 1.0e-9);
    auto inverted = scale;
    inverted.invert = true;
    CHECK (std::abs (binding::relativeValue (inverted, 0.5, 1, wait) - 0.4) < 1.0e-9);

    //  Rate: the middle is rest, the dead zone moves nothing, a full push moves `speed` of the span a second.
    CHECK (std::abs (binding::deflectionOf (scale, 0.5)) < 1.0e-12);
    CHECK (std::abs (binding::deflectionOf (scale, 0.54)) < 1.0e-12);
    CHECK (std::abs (binding::deflectionOf (scale, 1.0) - 1.0) < 1.0e-9);
    CHECK (std::abs (binding::deflectionOf (scale, 0.0) + 1.0) < 1.0e-9);
    CHECK (binding::deflectionOf (scale, 0.6) > 0.0);
    CHECK (binding::deflectionOf (scale, 0.6) < 0.2);

    const binding::TargetRange gain { false, true, -24.0, true, 24.0, "dB" };
    CHECK (std::abs (binding::rateValue (scale, 0.0, 1.0, 1.0, gain) - 24.0) < 1.0e-9);
    CHECK (std::abs (binding::rateValue (scale, 0.0, -1.0, 0.1, gain) - (-2.4)) < 1.0e-9);
    CHECK (std::abs (binding::rateValue (scale, 20.0, 1.0, 1.0, gain) - 24.0) < 1.0e-9);
    CHECK (std::abs (binding::rateValue (scale, 3.0, 0.0, 1.0, gain) - 3.0) < 1.0e-9);
}

TEST_CASE ("binding scaling: a motorless control picks the value up before it moves it")
{
    binding::Pickup pickup;

    //  Far from the value: nothing, however it moves on its own side.
    CHECK_FALSE (binding::pickUp (pickup, -40.0, -6.0, 0.5));
    CHECK_FALSE (binding::pickUp (pickup, -30.0, -6.0, 0.5));

    //  Crossing it: caught, and from then on it moves the value.
    CHECK (binding::pickUp (pickup, -2.0, -6.0, 0.5));
    CHECK (binding::pickUp (pickup, -50.0, -6.0, 0.5));

    //  Landing within a step of it catches it too.
    pickup.reset();
    CHECK (binding::pickUp (pickup, -6.3, -6.0, 0.5));
}

TEST_CASE ("binding index: read from the show, and matched by port, channel, kind and number")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDCC007"), s ("number"), s ("7"), s ("channel"), s ("2"),
                                            s ("target"), s ("/godot/cue/x/level") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDCC014"), s ("type"), s ("controlChange14"),
                                            s ("number"), s ("1") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDPB001"), s ("type"), s ("pitchBend") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (rig.cueId), s ("BNDCQ001"), s ("source"), s ("osc"),
                                            s ("address"), s ("/desk/fader/1"), s ("target"), s ("level"),
                                            s ("inLow"), s ("0.25") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDXX001"), s ("number"), s ("7"),
                                            s ("enabled"), s ("false") }).rejected == 0);

    const auto index = binding::BindingIndex::build (rig.document);
    REQUIRE (index->bindings.size() == 5u);

    //  Its rows as the show holds them.
    const auto* cc = index->find ("BNDCC007");
    REQUIRE (cc != nullptr);
    CHECK (cc->channel == 2);
    CHECK (cc->number == 7);
    CHECK (cc->scale.ownRange);
    CHECK (cc->pickup);
    CHECK_FALSE (cc->feedback);
    CHECK (cc->control == binding::Control::absolute);
    CHECK (cc->targetAddress() == "/godot/cue/x/level");

    //  A row of its cue, by name; a number written in either locale's way read the one way.
    const auto* ofCue = index->find ("BNDCQ001");
    REQUIRE (ofCue != nullptr);
    CHECK (ofCue->source == binding::Source::osc);
    CHECK (ofCue->targetAddress() == "/godot/cue/" + rig.cueId + "/level");
    CHECK (std::abs (ofCue->scale.inLow - 0.25) < 1.0e-12);
    CHECK (index->find ("BNDXX001")->enabled == false);

    const auto ids = [] (const std::vector<const binding::Binding*>& found)
    {
        std::vector<std::string> out;

        for (const auto* each : found)
            out.push_back (each->id);

        return out;
    };

    const std::uint8_t controllerOnTwo[] { 0xB1, 7, 100 };
    const std::uint8_t controllerOnOne[] { 0xB0, 7, 100 };
    const std::uint8_t fineHalf[] { 0xB0, 33, 5 };
    const std::uint8_t bend[] { 0xE3, 0x00, 0x40 };
    const std::uint8_t pressure[] { 0xD0, 90 };
    const std::uint8_t noteOff[] { 0x90, 60, 0 };

    const auto arrival = binding::decodeMidi (controllerOnTwo, 3);
    REQUIRE (arrival.has_value());
    CHECK (arrival->channel == 2);
    CHECK (arrival->value == 100);
    CHECK (ids (binding::matchMidi (*index, "PORT0001", *arrival)) == std::vector<std::string> { "BNDCC007" });
    CHECK (ids (binding::matchMidi (*index, "PORT0001", *binding::decodeMidi (controllerOnOne, 3))).empty());

    //  A 14-bit pair on its second controller; pitch bend's whole value; channel pressure.
    CHECK (ids (binding::matchMidi (*index, "PORT0001", *binding::decodeMidi (fineHalf, 3)))
             == std::vector<std::string> { "BNDCC014" });

    const auto bent = binding::decodeMidi (bend, 3);
    REQUIRE (bent.has_value());
    CHECK (bent->value == 8192);
    CHECK (std::abs (binding::midiFraction (binding::MidiType::pitchBend, bent->value) - 8192.0 / 16383.0) < 1.0e-12);
    CHECK (ids (binding::matchMidi (*index, "PORT0001", *bent)) == std::vector<std::string> { "BNDPB001" });

    const auto pressed = binding::decodeMidi (pressure, 2);
    REQUIRE (pressed.has_value());
    CHECK (pressed->kind == binding::MidiArrival::Kind::channelPressure);
    CHECK (pressed->value == 90);

    //  A note-on of velocity nought is a release, and moves nothing.
    CHECK_FALSE (binding::decodeMidi (noteOff, 3).has_value());

    //  A port named: only that one.
    REQUIRE (rig.document.setAttribute ("/godot/binding/BNDCC007/port", "PORT0002").ok);
    const auto named = binding::BindingIndex::build (rig.document);
    CHECK (binding::matchMidi (*named, "PORT0001", *arrival).empty());
    CHECK (ids (binding::matchMidi (*named, "PORT0002", *arrival)) == std::vector<std::string> { "BNDCC007" });

    //  OSC by its address.
    CHECK (ids (binding::matchOsc (*index, "/desk/fader/1")) == std::vector<std::string> { "BNDCQ001" });
    CHECK (binding::matchOsc (*index, "/desk/fader/2").empty());
}
