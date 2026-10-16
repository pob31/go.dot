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
#include <wfg/engine/binding/BindingBridge.h>
#include <wfg/engine/binding/BindingIndex.h>
#include <wfg/engine/binding/Scaling.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/oscquery/EngineNamespace.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/client/model/Bindings.h>
#include <wfg/client/model/MidiPorts.h>
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
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDPB001"), s ("type"), s ("pitchBend"), s ("invert"), s ("true"),
                                            s ("ownRange"), s ("false"), s ("feedback"), s ("true") }).rejected == 0);
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

    //  A flag the show turned over reads as it was written, either way.
    CHECK (index->find ("BNDPB001")->scale.invert);
    CHECK_FALSE (index->find ("BNDPB001")->scale.ownRange);
    CHECK (index->find ("BNDPB001")->feedback);

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

//==============================================================================
/*  BD.9: THE BRIDGE. Bytes and datagrams in, `node.set` out, in the order serve
    ticks it: the bridge's writes before the tick, the index and the tree after. */
namespace
{
    struct BridgeRig : Rig
    {
        BridgeRig()
        {
            cue::registerRunCommands (engine.commands(), runs);
            binding::hearCommands (engine.commands(), bridge);
            bridge.sendTo (&feedback);
            settle();
        }

        void settle()
        {
            parameters.markStale();
            tree::EngineState state;
            state.documentRevision = document.showRevision();
            snapshot = parameters.publish (tick, state);
            bridge.publish (binding::BindingIndex::build (document));
            bridge.afterTick (snapshot);
        }

        //  One tick: what the bridge writes, applied; then the index and the tree again.
        std::vector<Event> step()
        {
            std::vector<Event> sent;
            bridge.beforeTick ([this, &sent] (Event event)
                               {
                                   sent.push_back (event);
                                   return engine.submit (std::move (event));
                               });
            engine.processTick (tick++);
            settle();
            return sent;
        }

        bool cc (int controller, int value)
        {
            return bridge.midiArrived ("PORT0001", { std::uint8_t { 0xB0 }, static_cast<std::uint8_t> (controller),
                                                     static_cast<std::uint8_t> (value) });
        }

        bool ccOn (const std::string& port, int channel, int controller, int value)
        {
            return bridge.midiArrived (port, { static_cast<std::uint8_t> (0xB0 | (channel - 1)),
                                               static_cast<std::uint8_t> (controller), static_cast<std::uint8_t> (value) });
        }

        double number (const std::string& address)
        {
            return osc::parseDouble (published (address)).value_or (-999.0);
        }

        //  What feedback sends, in order.
        struct Sink final : midi::MidiSink
        {
            struct Message
            {
                std::string port;
                midi::Bytes bytes;
            };

            std::string send (const std::string& port, const midi::Bytes& bytes) override
            {
                sent.push_back ({ port, bytes });
                return {};
            }

            std::vector<Message> sent;
        };

        Sink feedback;
        binding::BindingBridge bridge;
    };
}

TEST_CASE ("binding bridge: a controller moves a number by its law, one write a tick, as the binding")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00001"), s ("number"), s ("7"), s ("target"), s (level),
                                            s ("pickup"), s ("false") }).rejected == 0);
    rig.settle();

    //  The top of the travel is the top of the level's range, written by the binding.
    CHECK (rig.cc (7, 127));
    auto sent = rig.step();
    REQUIRE (sent.size() == 1u);
    CHECK (sent.front().origin == "binding:BND00001");
    CHECK (sent.front().command == "node.set");
    CHECK (std::abs (rig.number (level) - 12.0) < 1.0e-9);

    //  Three places in one tick: one write, the last, along the fader's law.
    rig.cc (7, 0);
    rig.cc (7, 40);
    rig.cc (7, 108);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (level) - surface::dbForFraction (108.0 / 127.0)) < 1.0e-3);

    //  Another controller moves nothing; nothing heard, nothing written.
    CHECK_FALSE (rig.cc (8, 20));
    CHECK (rig.step().empty());
    CHECK (rig.step().empty());

    //  Switched off, it hears nothing.
    REQUIRE (rig.document.setAttribute ("/godot/binding/BND00001/enabled", "false").ok);
    rig.settle();
    CHECK_FALSE (rig.cc (7, 0));
    CHECK (rig.step().empty());
}

TEST_CASE ("binding bridge: a knob with no motor picks the value up, and again once something else has moved it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00002"), s ("number"), s ("7"), s ("target"), s (level) }).rejected == 0);
    rig.settle();

    //  The level is at nought, most of the way up: the knob near the bottom moves nothing.
    rig.cc (7, 0);
    CHECK (rig.step().empty());
    rig.cc (7, 30);
    CHECK (rig.step().empty());
    CHECK (std::abs (rig.number (level)) < 1.0e-9);

    //  Swept past it: caught, and from there it moves the level either way.
    rig.cc (7, 120);
    CHECK (rig.step().size() == 1u);
    CHECK (rig.number (level) > 0.0);
    rig.cc (7, 100);
    CHECK (rig.step().size() == 1u);
    CHECK (rig.number (level) < 0.0);

    //  Moved from the window, the level is the knob's no more until it crosses it again.
    REQUIRE (rig.send ("node.set", { s (level), osc::Value::float64 (-30.0) }).rejected == 0);
    rig.settle();
    rig.cc (7, 101);
    CHECK (rig.step().empty());
    CHECK (std::abs (rig.number (level) + 30.0) < 1.0e-9);
    rig.cc (7, 50);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (level) - surface::dbForFraction (50.0 / 127.0)) < 1.0e-3);
}

TEST_CASE ("binding bridge: an encoder turns as the master dial does, a pedal moves by its push, and a double Esc lets go of the push")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto preWait = "/godot/cue/" + rig.cueId + "/preWait";
    const auto postWait = "/godot/cue/" + rig.cueId + "/postWait";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDREV01"), s ("number"), s ("8"), s ("control"), s ("relative"),
                                            s ("target"), s (preWait) }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDRATE1"), s ("number"), s ("9"), s ("control"), s ("rate"),
                                            s ("target"), s (postWait), s ("outHigh"), s ("50"), s ("speed"), s ("1") }).rejected == 0);
    rig.settle();

    //  Three detents and three more in one tick: one write, six tenths of a second; one back.
    rig.cc (8, 3);
    rig.cc (8, 3);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (preWait) - 0.6) < 1.0e-9);
    rig.cc (8, 65);
    rig.step();
    CHECK (std::abs (rig.number (preWait) - 0.5) < 1.0e-9);

    //  Pushed all the way: fifty seconds' span a second, a second a tick, every tick it is held.
    rig.cc (9, 127);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (postWait) - 1.0) < 1.0e-9);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (postWait) - 2.0) < 1.0e-9);

    //  A double Esc: held over, it writes nothing more, however hard it is pushed.
    REQUIRE (rig.send ("run.killAll", {}).rejected == 0);
    rig.settle();
    CHECK (rig.step().empty());
    rig.cc (9, 127);
    CHECK (rig.step().empty());
    CHECK (std::abs (rig.number (postWait) - 2.0) < 1.0e-9);

    //  Back to rest and pushed again: a hand moving a control.
    rig.cc (9, 64);
    CHECK (rig.step().empty());
    rig.cc (9, 127);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (postWait) - 3.0) < 1.0e-9);

    //  And the encoder was never let go of.
    rig.cc (8, 1);
    rig.step();
    CHECK (std::abs (rig.number (preWait) - 0.6) < 1.0e-9);
}

TEST_CASE ("binding bridge: a binding of a cue is live while a run of that cue is going, and only then")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (rig.cueId), s ("BNDCQE02"), s ("number"), s ("7"), s ("target"), s ("level"),
                                            s ("pickup"), s ("false") }).rejected == 0);
    rig.settle();

    //  No run: what it hears is dropped.
    CHECK (rig.cc (7, 127));
    CHECK (rig.step().empty());

    //  A run made ready ahead of a GO is not one going.
    rig.runs.create ("RUNBND01", rig.cueId, "media");
    auto* run = rig.runs.find ("RUNBND01");
    REQUIRE (run != nullptr);
    run->state = cue::runState::armed;
    rig.settle();
    rig.cc (7, 127);
    CHECK (rig.step().empty());

    //  Playing: its own cue's level, by the row's name.
    run->state = cue::runState::playing;
    rig.settle();
    rig.cc (7, 127);
    const auto sent = rig.step();
    REQUIRE (sent.size() == 1u);
    CHECK (sent.front().args.front().getString() == level);
    CHECK (std::abs (rig.number (level) - 12.0) < 1.0e-9);

    //  Ended: let go.
    run->state = cue::runState::done;
    rig.settle();
    rig.cc (7, 0);
    CHECK (rig.step().empty());
    CHECK (std::abs (rig.number (level) - 12.0) < 1.0e-9);
}

TEST_CASE ("binding bridge: an OSC address moves a number by its argument, and is written to no node")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDSC001"), s ("source"), s ("osc"), s ("address"), s ("/desk/fader/1"),
                                            s ("argument"), s ("1"), s ("target"), s (level), s ("pickup"), s ("false") }).rejected == 0);
    rig.settle();

    CHECK (rig.bridge.oscArrived ("/desk/fader/1", { s ("strip"), osc::Value::float32 (1.0f) }));
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (level) - 12.0) < 1.0e-9);

    //  Another address is not the binding's; its own with no such argument is, and moves nothing.
    CHECK_FALSE (rig.bridge.oscArrived ("/desk/fader/2", { s ("strip"), osc::Value::float32 (0.0f) }));
    CHECK (rig.bridge.oscArrived ("/desk/fader/1", {}));
    CHECK (rig.step().empty());

    //  Down the namespace's road: a datagram is the binding's and no write of its own ...
    osc::UdpEndpoint udp;
    oscquery::EngineNamespace nameSpace { rig.engine, rig.parameters, rig.touches, udp };
    nameSpace.bindTo (rig.bridge);
    nameSpace.write ("udp:10.0.0.5:9000", osc::Packet::message ("/desk/fader/1", { s ("strip"), osc::Value::float32 (0.0f) }));
    CHECK (rig.engine.processTick (rig.tick++).total() == 0u);
    CHECK (rig.step().size() == 1u);
    CHECK (rig.number (level) <= -119.0);

    //  ... and a WebSocket client's message to it is a client's write, which finds no node.
    nameSpace.write ("ws:1", osc::Packet::message ("/desk/fader/1", { s ("strip"), osc::Value::float32 (1.0f) }));
    CHECK (rig.engine.processTick (rig.tick++).rejected == 1u);
    CHECK (rig.step().empty());
}

TEST_CASE ("binding bridge: feedback sends a control where its value is, when it moved, and never back what it sent")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDFB001"), s ("port"), s ("PORT0002"), s ("channel"), s ("3"),
                                            s ("number"), s ("7"), s ("target"), s (level), s ("pickup"), s ("false"),
                                            s ("feedback"), s ("true") }).rejected == 0);

    //  Live: the fader is sent where the level is - nought decibels, 108 of 127 - on its port and channel.
    rig.feedback.sent.clear();
    rig.settle();
    REQUIRE (rig.feedback.sent.size() == 1u);
    CHECK (rig.feedback.sent[0].port == "PORT0002");
    CHECK (rig.feedback.sent[0].bytes == midi::Bytes { 0xB2, 7, 108 });

    //  Nothing moved, nothing sent.
    rig.feedback.sent.clear();
    rig.step();
    CHECK (rig.feedback.sent.empty());

    //  The fader moved it: the level is where the fader is, which is not sent back.
    rig.ccOn ("PORT0002", 3, 7, 60);
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (level) - surface::dbForFraction (60.0 / 127.0)) < 1.0e-3);
    CHECK (rig.feedback.sent.empty());

    //  Moved from the window: the fader is sent there.
    REQUIRE (rig.send ("node.set", { s (level), osc::Value::float64 (-6.0) }).rejected == 0);
    rig.settle();
    REQUIRE (rig.feedback.sent.size() == 1u);
    CHECK (rig.feedback.sent[0].bytes
             == midi::Bytes { 0xB2, 7, static_cast<std::uint8_t> (std::lround (surface::fractionForDb (-6.0) * 127.0)) });

    //  A 14-bit pair: its coarse half, then its fine half on the number plus 32.
    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDFB014"), s ("port"), s ("PORT0002"), s ("type"), s ("controlChange14"),
                                            s ("number"), s ("1"), s ("target"), s (level), s ("feedback"), s ("true") }).rejected == 0);
    rig.feedback.sent.clear();
    rig.settle();
    const auto fine = static_cast<int> (std::lround (surface::fractionForDb (-6.0) * 16383.0));
    REQUIRE (rig.feedback.sent.size() == 2u);
    CHECK (rig.feedback.sent[0].bytes == midi::Bytes { 0xB0, 1, static_cast<std::uint8_t> (fine >> 7) });
    CHECK (rig.feedback.sent[1].bytes == midi::Bytes { 0xB0, 33, static_cast<std::uint8_t> (fine & 0x7F) });

    //  Asked for none, or with no port to go back to: nothing.
    REQUIRE (rig.document.setAttribute ("/godot/binding/BNDFB014/feedback", "false").ok);
    REQUIRE (rig.document.setAttribute ("/godot/binding/BNDFB001/port", "").ok);
    rig.feedback.sent.clear();
    REQUIRE (rig.send ("node.set", { s (level), osc::Value::float64 (-12.0) }).rejected == 0);
    rig.settle();
    CHECK (rig.feedback.sent.empty());
}

TEST_CASE ("binding bridge: Learn takes the next control that moves as the endpoint, in one step")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    BridgeRig rig;
    const auto level = "/godot/cue/" + rig.cueId + "/level";
    const auto row = [] (const char* name) { return std::string ("/godot/binding/BNDTCH01/") + name; };

    REQUIRE (rig.send ("binding.create", { s (""), s ("BNDTCH01"), s ("target"), s (level), s ("pickup"), s ("false") }).rejected == 0);
    rig.settle();

    REQUIRE (rig.send ("binding.learn", { s ("BNDTCH01") }).rejected == 0);
    CHECK (rig.bridge.learning() == "BNDTCH01");

    {
        tree::EngineState state;
        state.documentRevision = rig.document.showRevision();
        state.bindingLearn = rig.bridge.learning();
        const auto snapshot = rig.parameters.publish (rig.tick, state);
        CHECK (client::model::text (*snapshot, "/godot/engine/bindingLearn") == "BNDTCH01");
    }

    //  A note on channel 5 of another port: learned as one edit by the binding, and it moves nothing.
    CHECK (rig.bridge.midiArrived ("PORT0003", { 0x94, 60, 100 }));
    const auto sent = rig.step();
    REQUIRE (sent.size() == 1u);
    CHECK (sent[0].command == "node.setMany");
    CHECK (sent[0].origin == "binding:BNDTCH01");
    CHECK (rig.published (row ("source")) == "midi");
    CHECK (rig.published (row ("port")) == "PORT0003");
    CHECK (rig.published (row ("channel")) == "5");
    CHECK (rig.published (row ("type")) == "noteVelocity");
    CHECK (rig.published (row ("number")) == "60");
    CHECK (rig.bridge.learning().empty());
    CHECK (std::abs (rig.number (level)) < 1.0e-9);

    //  From now on that note moves the level.
    CHECK (rig.bridge.midiArrived ("PORT0003", { 0x94, 60, 127 }));
    CHECK (rig.step().size() == 1u);
    CHECK (std::abs (rig.number (level) - 12.0) < 1.0e-9);

    //  An OSC address: never the tree's; the argument the first that carries a number.
    REQUIRE (rig.send ("binding.learn", { s ("BNDTCH01") }).rejected == 0);
    CHECK_FALSE (rig.bridge.oscArrived ("/godot/cue/" + rig.cueId + "/level", { osc::Value::float32 (1.0f) }));
    CHECK (rig.bridge.oscArrived ("/desk/x", { s ("a"), osc::Value::float32 (0.5f) }));
    CHECK (rig.step().size() == 1u);
    CHECK (rig.published (row ("source")) == "osc");
    CHECK (rig.published (row ("address")) == "/desk/x");
    CHECK (rig.published (row ("argument")) == "1");

    //  Asked again and then for nothing: it stops listening, and learns nothing.
    REQUIRE (rig.send ("binding.learn", { s ("BNDTCH01") }).rejected == 0);
    REQUIRE (rig.send ("binding.learn", {}).rejected == 0);
    CHECK (rig.bridge.learning().empty());
    CHECK_FALSE (rig.bridge.oscArrived ("/desk/y", { osc::Value::float32 (0.5f) }));
    CHECK (rig.step().empty());

    //  The door: a binding of this show, which may be edited.
    CHECK (rig.send ("binding.learn", { s ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.send ("binding.learn", { s (rig.cueId) }).rejected == 1);
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.send ("binding.learn", { s ("BNDTCH01") }).rejected == 1);
    CHECK (rig.bridge.learning().empty());
}

//==============================================================================
/*  BD.11: THE WORDS THE WINDOW SAYS A BINDING IN. */
TEST_CASE ("binding words: what moves it, what it moves and when, as the window says them")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;
    namespace model = client::model;

    const auto port = rig.document.createPort ("Desk").id;
    const auto level = "/godot/cue/" + rig.cueId + "/level";

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00001"), s ("port"), s (port), s ("channel"), s ("2"),
                                            s ("number"), s ("7"), s ("target"), s (level) }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (rig.cueId), s ("BND00002"), s ("source"), s ("osc"), s ("address"),
                                            s ("/desk/fader/1"), s ("argument"), s ("1"), s ("target"), s ("level") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00003"), s ("type"), s ("controlChange14"), s ("number"), s ("1") }).rejected == 0);
    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00004"), s ("type"), s ("noteVelocity"), s ("number"), s ("60"),
                                            s ("control"), s ("rate"), s ("feedback"), s ("true") }).rejected == 0);

    rig.published ("/godot/binding/order");
    const auto& snapshot = *rig.snapshot;
    const auto rows = model::readBindings (snapshot);
    const auto ports = model::readPorts (snapshot);
    const std::string dot { "\xc2\xb7" };

    REQUIRE (rows.size() == 4u);
    CHECK (rows[0].id == "BND00001");
    CHECK (rows[3].control == "rate");
    CHECK (rows[3].feedback);
    CHECK (rows[0].pickup);

    //  What moves it: a port by the show's name for it, any port, an OSC address and its argument from one.
    CHECK (model::movedByWords (rows[0], ports) == "Desk " + dot + " ch 2 " + dot + " CC 7");
    CHECK (model::movedByWords (rows[1], ports) == "OSC /desk/fader/1 " + dot + " argument 2");
    CHECK (model::movedByWords (rows[2], ports) == "any port " + dot + " CC 1+33 (14 bits)");
    CHECK (model::movedByWords (rows[3], ports) == "any port " + dot + " note 60");

    //  What it moves: a cue's row by the cue's name, a row of its own cue, nothing yet.
    const auto moves = model::movesWords (rows[0], snapshot);
    CHECK (moves.find ("Door " + dot + " level") != std::string::npos);
    CHECK (model::movesWords (rows[1], snapshot) == "its level");
    CHECK (model::movesWords (rows[2], snapshot) == "nothing yet");

    //  When it is live, and the address it writes.
    CHECK (model::liveWords (rows[0], snapshot) == "all show");
    CHECK (model::liveWords (rows[1], snapshot).rfind ("while ", 0) == 0);
    CHECK (model::liveWords (rows[1], snapshot).find ("Door runs") != std::string::npos);
    CHECK (model::targetAddressOf (rows[1]) == level);
    CHECK (model::targetAddressOf (rows[2]).empty());
    CHECK (model::bindingsOn (rows, level).size() == 2u);

    //  How, and the way round a click takes it.
    CHECK (model::howWords ("rate") == "Rate");
    CHECK (model::nextControl ("absolute") == "relative");
    CHECK (model::nextControl ("relative") == "rate");
    CHECK (model::nextControl ("rate") == "absolute");
    CHECK (model::bindingLearning (snapshot).empty());
}
