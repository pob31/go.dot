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

/*  THE SEAM BETWEEN JUCE'S MIDI AND THE ENGINE'S.

    One function - `eventFrom` - and it is the only place in the engine that
    knows what a `juce::MidiMessage` is. Everything above it works on the
    engine's own `MidiEvent`, which is what lets the matching be tested on a
    machine with no MIDI interface.

    So this file tests the conversion, which needs no port either: a
    `juce::MidiMessage` can be built from bytes. What cannot be tested without
    hardware is opening a real device, and that is on the hardware checklist.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/midi/MidiInputs.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace wfg;

TEST_CASE ("midi: a message becomes the event the matchers take")
{
    const auto noteOn = midi::eventFrom (juce::MidiMessage::noteOn (3, 60, (juce::uint8) 100),
                                         "Desk");

    CHECK (noteOn.port == "Desk");
    CHECK (noteOn.type == cue::triggerType::noteOn);
    CHECK (noteOn.channel == 3);
    CHECK (noteOn.number == 60);
    CHECK (noteOn.data == 100);

    const auto noteOff = midi::eventFrom (juce::MidiMessage::noteOff (3, 60, (juce::uint8) 64),
                                          "Desk");

    CHECK (noteOff.type == cue::triggerType::noteOff);
    CHECK (noteOff.number == 60);

    const auto controller = midi::eventFrom (juce::MidiMessage::controllerEvent (7, 11, 64),
                                             "Desk");

    CHECK (controller.type == cue::triggerType::controlChange);
    CHECK (controller.channel == 7);
    CHECK (controller.number == 11);
    CHECK (controller.data == 64);

    const auto program = midi::eventFrom (juce::MidiMessage::programChange (2, 12), "Desk");

    CHECK (program.type == cue::triggerType::programChange);
    CHECK (program.channel == 2);
    CHECK (program.number == 12);
}

TEST_CASE ("midi: a note-on of velocity nought is reported as a note-ON")
{
    /*  WHICH IS NOT WHAT JUCE SAYS BY DEFAULT, and the difference matters here.

        `MidiMessage::isNoteOn()` answers false for a note-on carrying velocity
        nought, because on a synthesiser that message means "release" - and for
        a synthesiser that is the right answer. Here it is the wrong one: §3.7
        lets a trigger ask for a velocity, and matching `data = 0` on a `noteOn`
        is precisely how somebody catches the release from the very many
        surfaces that spell it that way.

        So the classification is by the status byte. What the wire said is what
        is reported; what it MEANS is the trigger's business. */
    const auto message = juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0);

    REQUIRE_FALSE (message.isNoteOn());          // JUCE's reading
    REQUIRE (message.isNoteOff());

    const auto event = midi::eventFrom (message, "Desk");

    CHECK (event.type == cue::triggerType::noteOn);
    CHECK (event.data == 0);
}

TEST_CASE ("midi: an event nothing listens for has no type, and fires nothing")
{
    /*  Clock, active sensing, pitch bend, aftertouch, system exclusive: a
        surface sends a great deal that is not a trigger, and the four types
        §3.7 lists are the four a surface uses to say "this button". Anything
        else converts to an event with no type, and the caller drops it before
        the matcher is troubled - which matters at MIDI clock's twenty-four
        messages a beat. */
    for (const auto& message : { juce::MidiMessage::midiClock(),
                                 juce::MidiMessage::midiStart(),
                                 juce::MidiMessage::pitchWheel (1, 8192),
                                 juce::MidiMessage::aftertouchChange (1, 60, 64),
                                 juce::MidiMessage::channelPressureChange (1, 64) })
    {
        const auto event = midi::eventFrom (message, "Desk");
        CHECK (event.type.empty());
    }

    /*  AND ALL NOTES OFF IS NOT ONE OF THEM, which is worth knowing rather than
        guessing: on the wire it is controller 123, so it converts to a control
        change and a trigger listening for controller 123 matches it. That is
        the wire's truth and it is the right answer - a desk's panic button is a
        perfectly reasonable thing to hang a cue on. */
    const auto panic = midi::eventFrom (juce::MidiMessage::allNotesOff (1), "Desk");

    CHECK (panic.type == cue::triggerType::controlChange);
    CHECK (panic.number == 123);
}

TEST_CASE ("midi: this machine's ports are a list, and an empty one is an answer")
{
    /*  A port list is a fact about the machine. An empty one is a fact too, and
        making it an error would mean the only build that could run this is one
        with hardware plugged into it - which is no CI runner. */
    const auto inputs = midi::availableInputs();
    const auto outputs = midi::availableOutputs();

    MESSAGE ("this machine has " << inputs.size() << " MIDI input(s) and "
             << outputs.size() << " output(s)");

    for (const auto& name : inputs)
        MESSAGE ("  in:  " << name);

    for (const auto& name : outputs)
        MESSAGE ("  out: " << name);

    CHECK (true);
}

TEST_CASE ("midi: a device that is not there is refused, and the message names what is")
{
    /*  A trigger that never fires because a cable is in the wrong socket is the
        failure this exists to make loud, and the moment to say so is while
        somebody is still looking at the terminal they typed it into. The answer
        is almost always one of the names this machine does have, spelled
        differently - so they are in the message. */
    midi::MidiInputs inputs;

    CHECK_FALSE (inputs.open ("no such port, surely"));
    REQUIRE (inputs.problems().size() == 1u);
    CHECK (inputs.problems().front().find ("--midi-in") != std::string::npos);
    CHECK (inputs.problems().front().find ("no such port, surely") != std::string::npos);
    CHECK (inputs.count() == 0u);
}

//==============================================================================
/*  MIDI CUES: THE BYTES, AND THE CUE THAT SENDS THEM.

    §3.10 asks for every MIDI event type, which means the interesting half of a
    MIDI cue is arithmetic on seven document fields - a status byte, a channel
    that is one-based on the page and nought-based on the wire, and a pitch bend
    that is fourteen bits in two halves. All of that is a pure function and is
    checked here byte for byte.

    THE OTHER HALF IS THAT NOTHING REAL IS NEEDED TO CHECK IT. JUCE makes
    virtual MIDI ports on macOS and Linux and not on Windows, so a test that
    wanted to HEAR a cue would run on two platforms of three - and on no CI
    runner, none of which has a MIDI interface. A recording sink runs
    everywhere, and what it records is exactly what would have left.
*/
#include <wfg/engine/midi/MidiMessages.h>

namespace
{
    /** The sink as a notebook: what was sent, to which port, in order. */
    struct RecordingSink final : midi::MidiSink
    {
        std::string send (const std::string& port, const midi::Bytes& bytes) override
        {
            if (! bound.empty() && bound.find (port) == std::string::npos)
                return midi::sendError::noPort;

            sent.push_back ({ port, bytes });
            return {};
        }

        struct Message
        {
            std::string port;
            midi::Bytes bytes;
        };

        /** Empty accepts any port; otherwise only this one. */
        std::string bound;
        std::vector<Message> sent;
    };

    /** A cue's bytes as a readable string, for a failure message worth having. */
    std::string hexOf (const midi::Bytes& bytes)
    {
        static const char* digits = "0123456789ABCDEF";
        std::string out;

        for (const auto byte : bytes)
        {
            if (! out.empty())
                out += ' ';

            out += digits[byte >> 4];
            out += digits[byte & 0x0f];
        }

        return out;
    }

    /*  Declares a <Port> and answers with its identifier, which is what a cue
        carries. Hand-built because there is no `port.create` command: a show's
        ports are authored, not made by a client at half past seven. */
    std::string declarePort (doc::ShowDocument& document, const std::string& name)
    {
        auto ports = document.root().getChildWithName ("MidiPorts");

        if (! ports.isValid())
        {
            ports = juce::ValueTree { "MidiPorts" };
            document.root().appendChild (ports, nullptr);
        }

        const auto id = document.ids().generate();

        juce::ValueTree port { "Port" };
        port.setProperty (juce::Identifier ("id"), juce::String (id), nullptr);
        port.setProperty (juce::Identifier ("name"), juce::String (name), nullptr);
        ports.appendChild (port, nullptr);

        return id;
    }

    midi::Bytes bytesFor (const midi::MessageSpec& spec)
    {
        const auto built = midi::messageFor (spec);
        INFO ("problem: " << built.problem);
        REQUIRE (built.ok());
        return built.bytes;
    }
}

TEST_CASE ("midi cue: every event type PRD 3.10 lists comes out as its own bytes")
{
    /*  The whole table, and each row is a different way to be wrong: a status
        nibble, a channel that is one-based on the page and nought-based on the
        wire, a type with one data byte rather than two, and a bend that is
        neither. */
    struct Case
    {
        const char* type;
        int channel, number, data;
        midi::Bytes expected;
    };

    const Case cases[] = {
        { "noteOn",          1,  60, 100, { 0x90, 0x3c, 0x64 } },
        { "noteOn",         16,  60, 100, { 0x9f, 0x3c, 0x64 } },
        { "noteOff",         2,  60,   0, { 0x81, 0x3c, 0x00 } },
        { "controlChange",   3,   7, 127, { 0xb2, 0x07, 0x7f } },
        { "aftertouch",      4,  60,  64, { 0xa3, 0x3c, 0x40 } },
        { "programChange",   5,  12,   0, { 0xc4, 0x0c } },
        { "channelPressure", 6,   0,  90, { 0xd5, 0x5a } },

        /*  FOURTEEN BITS IN TWO SEVEN-BIT HALVES, least significant first, and
            8192 is the centre - which is the one number in MIDI that everybody
            gets the wrong way round at least once. */
        { "pitchBend",       7,   0, 8192, { 0xe6, 0x00, 0x40 } },
        { "pitchBend",       7,   0,    0, { 0xe6, 0x00, 0x00 } },
        { "pitchBend",       7,   0, 16383, { 0xe6, 0x7f, 0x7f } },
    };

    for (const auto& one : cases)
    {
        midi::MessageSpec spec;
        spec.type = one.type;
        spec.channel = one.channel;
        spec.number = one.number;
        spec.data = one.data;

        const auto bytes = bytesFor (spec);

        INFO (one.type << " ch " << one.channel << ": got " << hexOf (bytes)
               << ", wanted " << hexOf (one.expected));
        CHECK (bytes == one.expected);
    }
}

TEST_CASE ("midi cue: a note-on of velocity nought is a note-on, because that is what was asked for")
{
    /*  JUCE's own factory turns this into a note-off, which is right for a
        synthesiser and wrong for a cue engine: the document said noteOn and a
        show that quietly sent something else would be a show nobody could debug
        from the file. The same rule the INPUT side follows for the same
        reason - see MidiInputs' eventFrom. */
    midi::MessageSpec spec;
    spec.type = "noteOn";
    spec.channel = 1;
    spec.number = 60;
    spec.data = 0;

    CHECK (bytesFor (spec) == midi::Bytes { 0x90, 0x3c, 0x00 });
}

TEST_CASE ("midi cue: a value outside its range is refused rather than clamped")
{
    /*  Clamping is how a show goes out wrong quietly: somebody meant something
        this cue cannot do, and the nearest legal message is not it. The schema
        already refuses each of these when the document is written, so this is
        the second net - the one that catches a value that arrived over the
        wire. */
    const auto refused = [] (const char* type, int channel, int number, int data)
    {
        midi::MessageSpec spec;
        spec.type = type;
        spec.channel = channel;
        spec.number = number;
        spec.data = data;

        INFO (type << " ch " << channel << " n " << number << " d " << data);
        CHECK_FALSE (midi::messageFor (spec).ok());
    };

    refused ("noteOn", 0, 60, 100);           // channels are one-based
    refused ("noteOn", 17, 60, 100);
    refused ("noteOn", 1, 128, 100);          // seven bits
    refused ("noteOn", 1, 60, 128);
    refused ("pitchBend", 1, 0, 16384);       // fourteen
    refused ("programChange", 1, 128, 0);
    refused ("nonsense", 1, 60, 100);         // a type nothing sends
}

TEST_CASE ("midi cue: sysex is read as hex the way a manual prints it")
{
    midi::MessageSpec spec;
    spec.type = "sysex";
    spec.sysex = "F0 7E 00 06 01 F7";

    CHECK (bytesFor (spec) == midi::Bytes { 0xf0, 0x7e, 0x00, 0x06, 0x01, 0xf7 });

    /*  Case and spacing are what somebody's fingers did, not what they meant. */
    spec.sysex = "f07e0006 01f7";
    CHECK (bytesFor (spec) == midi::Bytes { 0xf0, 0x7e, 0x00, 0x06, 0x01, 0xf7 });

    /*  THE FRAMING IS ADDED WHEN IT IS ABSENT, because a person copying the
        middle of a table out of a manual has the payload and not the envelope. */
    spec.sysex = "7E 00 06 01";
    CHECK (bytesFor (spec) == midi::Bytes { 0xf0, 0x7e, 0x00, 0x06, 0x01, 0xf7 });
}

TEST_CASE ("midi cue: a sysex that is not a sysex is refused")
{
    const auto refused = [] (const char* hex)
    {
        midi::MessageSpec spec;
        spec.type = "sysex";
        spec.sysex = hex;

        INFO ("sysex \"" << hex << "\"");
        CHECK_FALSE (midi::messageFor (spec).ok());
    };

    refused ("");                        // nothing to send
    refused ("F0 7E 0");                 // half a byte is not a byte
    refused ("F0 7E ZZ F7");             // not hex
    refused ("F0 7E 00 06");             // opened and never closed
    refused ("7E 00 06 F7");             // closed and never opened
    refused ("F0 7E 90 06 F7");          // a status byte inside the dump
}

TEST_CASE ("midi cue: hexBytes is where a typed-in dump is judged")
{
    midi::Bytes out;

    CHECK (midi::hexBytes ("00 7F FF", out));
    CHECK (out == midi::Bytes { 0x00, 0x7f, 0xff });

    CHECK (midi::hexBytes ("", out));
    CHECK (out.empty());

    CHECK_FALSE (midi::hexBytes ("0", out));
    CHECK_FALSE (midi::hexBytes ("0G", out));
}

//==============================================================================
TEST_CASE ("midi cue: firing one puts its bytes on the port the show named")
{
    /*  Through the whole cue layer: a document, a GO, and what a cable would
        have carried. */
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (31);
    cue::Runner runner { document, runs, runIds, focus };

    RecordingSink sink;
    runner.setMidiSink (&sink);

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Show").id;
    const auto cueId = document.createCue (listId, 0, "midi", "House lights").id;

    REQUIRE_FALSE (cueId.empty());

    /*  THE CUE NAMES THE PORT BY IDENTIFIER, the way a route names its bus:
        the name is what a person reads and what `--midi-out` is given, and
        renaming a port must not silence every cue that used it. */
    const auto portId = declarePort (document, "Lights");

    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/port", portId).ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/type", "programChange").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/channel", "3").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/number", "12").ok);

    REQUIRE (engine.submit (origin::cli, "cue.fire", { osc::Value::string (cueId) }));
    engine.processTick (0);

    REQUIRE (sink.sent.size() == 1u);
    CHECK (sink.sent.front().port == portId);
    CHECK (sink.sent.front().bytes == midi::Bytes { 0xc2, 0x0c });

    /*  AND ITS RUN IS A MIDI RUN, so a client watching /godot/run sees which
        kind of thing is happening rather than a cue of no sort at all. */
    REQUIRE (runs.all().size() == 1u);
    CHECK (runs.all().front().kind == "midi");
}

TEST_CASE ("midi cue: a port nothing was bound to fails the run and not the load")
{
    /*  §4.10 again: which cable "Lights" is on is a fact about the building, so
        a show travels to a rig that has not been patched yet and still opens.
        What fails is the cue, at the moment it is fired, saying `no-port`. */
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (37);
    cue::Runner runner { document, runs, runIds, focus };

    RecordingSink sink;
    runner.setMidiSink (&sink);

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Show").id;
    const auto cueId = document.createCue (listId, 0, "midi", "House lights").id;

    const auto declared = declarePort (document, "Lights");
    const auto bound = declarePort (document, "The desk");

    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/port", declared).ok);

    /*  Bound to the OTHER port, so the cue's own is declared and unpatched -
        which is a rig that has not been patched yet rather than a broken show. */
    sink.bound = bound;

    REQUIRE (engine.submit (origin::cli, "cue.fire", { osc::Value::string (cueId) }));
    engine.processTick (0);

    /*  The document loaded and the cue was fired: it is the RUN that failed. */
    CHECK (document.validate().empty());
    CHECK (sink.sent.empty());

    /*  THE FAILURE IS REPORTED FROM THE TICK HOOK, like every other report,
        because a handler that submitted one would produce it twice on replay.
        So the hook has to run for the run to hear about it. */
    runner.beforeTick (engine, 1);
    engine.processTick (1);

    REQUIRE (runs.all().size() == 1u);
    CHECK (runs.all().front().error == cue::runError::noPort);
    CHECK (runs.all().front().state == cue::runState::failed);
}

TEST_CASE ("midi cue: a show that asks to be verified is refused when it is read")
{
    /*  There is no read-back on a MIDI cable, so nothing would ever answer and
        the cue would wait for its timeout and fail - every time, at half past
        seven. Refused at load, like an OSC trigger listening inside /godot and
        a start offset beside a range. */
    doc::ShowDocument document;

    const auto listId = document.createList ("Show").id;
    const auto cueId = document.createCue (listId, 0, "midi", "House lights").id;

    CHECK (document.validate().empty());

    /*  The row's own enum refuses it, which is the first net. */
    CHECK_FALSE (document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified").ok);

    /*  And a hand-edited file gets the second one. */
    auto cue = document.findById (cueId);
    cue.setProperty (juce::Identifier ("wait"), "verified", nullptr);

    const auto problems = document.validate();

    /*  TWO COMPLAINTS AND NOT ONE, which is right: the row's enum does not
        carry `verified` either, so the general check and the specific one both
        answer. The specific one is what a person can act on. */
    REQUIRE_FALSE (problems.empty());

    const auto said = std::any_of (problems.begin(), problems.end(),
                                   [] (const std::string& problem)
                                   {
                                       return problem.find ("no read-back") != std::string::npos;
                                   });

    INFO ("problems: " << problems.size());
    CHECK (said);
}

TEST_CASE ("midi cue: the show declares its ports and the document says nothing about devices")
{
    /*  A <Port> holds a name somebody chose and nothing else. Which cable it is
        is --midi-out's answer, and the two are separate for the reason a bus is
        separate from a hardware channel: a show moved to another rig re-points
        the ports rather than every cue. */
    doc::ShowDocument document;

    auto ports = document.root().getChildWithName ("MidiPorts");

    if (! ports.isValid())
    {
        ports = juce::ValueTree { "MidiPorts" };
        document.root().appendChild (ports, nullptr);
    }

    juce::ValueTree port { "Port" };
    port.setProperty (juce::Identifier ("id"), "PRT00001", nullptr);
    port.setProperty (juce::Identifier ("name"), "Lights", nullptr);
    ports.appendChild (port, nullptr);

    CHECK (document.validate().empty());
    CHECK (doc::ShowDocument::ownerForElement ("Port") == "port");

    /*  And a Port has a name and nothing that could name a device. */
    const auto& schema = doc::Schema::instance();

    REQUIRE (schema.element ("Port") != nullptr);
    CHECK (schema.attribute ("Port", "name") != nullptr);
    CHECK (schema.attribute ("Port", "device") == nullptr);
}
