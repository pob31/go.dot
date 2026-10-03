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
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/midi/MidiInputs.h>
#include <wfg/engine/midi/PortTable.h>

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
        spec.data1 = one.number;
        spec.data2 = one.data;

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
    spec.data1 = 60;
    spec.data2 = 0;

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
        spec.data1 = number;
        spec.data2 = data;

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
/*  WHICH CABLE A PORT IS ON, AND WHAT SURVIVES MOVING IT (2026-09-22).

    The author's question was "what is best if switching USB ports?" and the
    answer has two halves, because the two things a MIDI device can be named by
    fail in opposite directions. An identifier is operating-system formatted
    and carries the device's instance path on Windows, so a cable in another
    socket usually has a new one - and it is the only thing that can tell two
    identical interfaces apart. A name survives the move and cannot.

    `PortTable::match` is that rule in one function, so both sides of the cable
    ask the same thing and a case can hand it a list with no hardware in the
    room.
*/
TEST_CASE ("midi: a port matches its device by identifier first and by name second")
{
    const std::vector<midi::Device> machine {
        { "MIDIMATE II", "USB/VID_0763&PID_1001/5&1a2b3c&0&1" },
        { "Dante Midi Port 1", "dante-1" },
    };

    std::string why;

    SUBCASE ("the identifier wins when it is there")
    {
        const auto* found = midi::PortTable::match (machine, "Dante Midi Port 1",
                                                    "USB/VID_0763&PID_1001/5&1a2b3c&0&1", why);

        REQUIRE (found != nullptr);
        CHECK (found->name == "MIDIMATE II");
        CHECK (why.empty());
    }

    SUBCASE ("and the name catches a cable that moved to another socket")
    {
        /*  The identifier it was bound to last time is gone, because the
            device instance path changed with the port. The name is still the
            device's own, so the port finds it. */
        const auto* found = midi::PortTable::match (machine, "MIDIMATE II",
                                                    "USB/VID_0763&PID_1001/5&9z8y7x&0&4", why);

        REQUIRE (found != nullptr);
        CHECK (found->name == "MIDIMATE II");
    }

    SUBCASE ("a name nothing answers to is a sentence and not a guess")
    {
        CHECK (midi::PortTable::match (machine, "A desk nobody owns", {}, why) == nullptr);
        CHECK (why.find ("A desk nobody owns") != std::string::npos);
    }

    SUBCASE ("and two devices of one name are refused rather than picked between")
    {
        /*  THE CASE THE IDENTIFIER EXISTS FOR, and where it has been lost the
            honest answer is none: a MIDI cue arriving at the wrong desk is
            worse than one that does not arrive. */
        const std::vector<midi::Device> twins {
            { "MIDIMATE II", "one" }, { "MIDIMATE II", "two" } };

        CHECK (midi::PortTable::match (twins, "MIDIMATE II", {}, why) == nullptr);
        CHECK (why.find ("2 devices") != std::string::npos);

        //  With the identifier remembered, the same pair is unambiguous.
        const auto* found = midi::PortTable::match (twins, "MIDIMATE II", "two", why);
        REQUIRE (found != nullptr);
        CHECK (found->identifier == "two");
    }

    SUBCASE ("a port that asks for nothing is bound to nothing, and that is not a fault")
    {
        CHECK (midi::PortTable::match (machine, {}, {}, why) == nullptr);
        CHECK (why.empty());
    }

    SUBCASE ("an identifier alone, once the device has gone, says so")
    {
        CHECK (midi::PortTable::match (machine, {}, "a device that left", why) == nullptr);
        CHECK_FALSE (why.empty());
    }
}

TEST_CASE ("midi: the show declares a port and the machine says which cable it is")
{
    doc::ShowDocument document;

    const auto made = document.createPort ("Lights");
    REQUIRE (made.ok);

    const auto base = "/godot/port/" + made.id + "/";

    /*  THE NAME IS THE SHOW'S AND TRAVELS; the identifier is this machine's
        and does not dirty the show, which is what `persist=state` buys. */
    REQUIRE (document.setAttribute (base + "outputDevice", "MIDIMATE II").ok);

    const auto before = document.showRevision();
    REQUIRE (document.setAttribute (base + "outputDeviceId", "usb-1").ok);

    CHECK (document.showRevision() == before);
    CHECK (document.getAttribute (base + "outputDeviceId") == std::string ("usb-1"));

    /*  AND A LOCKED SHOW STILL LETS THE ENGINE WRITE IT DOWN, which is what
        makes the write-back work during a performance: the lock refuses what
        somebody DECIDES, and where the cable is is not that. */
    REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);

    CHECK (document.setAttribute (base + "outputDeviceId", "usb-2").ok);
    CHECK_FALSE (document.setAttribute (base + "outputDevice", "Another desk").ok);
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
    /*  `data1` AND NOT `number` (2026-09-22). This line said `number` until the
        two rows were told apart, and it worked by accident: `cue,number` and
        `midi,number` were one attribute, so writing the cue's place in the
        list also wrote the program. Now they are separate and this writes the
        program, which is what the case is about. */
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/data1", "12").ok);

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

TEST_CASE ("midi cue: a port switched off sends nothing, and its run ends saying not-sent")
{
    /*  THE PROMISE THE PORT'S ROW HAS MADE ALL ALONG (2026-10-01, J3, namespace
        draft §23.7): "Off, the cue still runs and finishes carrying the warning
        not-sent, exactly as a network device's tx does". A network device kept
        it; a MIDI port did not, and a cue on a bound port switched off went out
        on the cable. Doh! asks whether a MIDI cue reached a synth, and a port
        that sends whatever its switch says would answer that wrongly. */
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (41);
    cue::Runner runner { document, runs, runIds, focus };

    RecordingSink sink;
    runner.setMidiSink (&sink);

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Show").id;
    const auto cueId = document.createCue (listId, 0, "midi", "House lights").id;
    const auto portId = declarePort (document, "Lights");

    REQUIRE_FALSE (cueId.empty());
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/port", portId).ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/type", "programChange").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/data1", "12").ok);

    const auto fireAndSettle = [&]
    {
        REQUIRE (engine.submit (origin::cli, "cue.fire", { osc::Value::string (cueId) }));
        engine.processTick (0);

        runner.beforeTick (engine, 1);
        engine.processTick (1);

        REQUIRE (runs.all().size() == 1u);
        return runs.all().front();
    };

    SUBCASE ("off: nothing reaches the cable, and the run ends done carrying not-sent")
    {
        REQUIRE (document.setAttribute ("/godot/port/" + portId + "/tx", "false").ok);

        const auto run = fireAndSettle();

        CHECK (sink.sent.empty());
        CHECK (run.state == cue::runState::done);
        CHECK (run.warning == cue::runWarning::notSent);
        CHECK (run.error.empty());
    }

    SUBCASE ("on, said: one message, and no warning")
    {
        REQUIRE (document.setAttribute ("/godot/port/" + portId + "/tx", "true").ok);

        const auto run = fireAndSettle();

        CHECK (sink.sent.size() == 1u);
        CHECK (run.state == cue::runState::done);
        CHECK (run.warning.empty());
    }

    SUBCASE ("on, unsaid - the row's default: one message, and no warning")
    {
        const auto run = fireAndSettle();

        CHECK (sink.sent.size() == 1u);
        CHECK (run.state == cue::runState::done);
        CHECK (run.warning.empty());
    }

    SUBCASE ("a message that cannot be built still fails bad-message on a port switched off")
    {
        /*  As a network cue's failed write comes before its `tx` test: what
            is wrong with the cue is said whatever the switch says. */
        REQUIRE (document.setAttribute ("/godot/port/" + portId + "/tx", "false").ok);
        REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/type", "sysex").ok);

        const auto run = fireAndSettle();

        CHECK (sink.sent.empty());
        CHECK (run.state == cue::runState::failed);
        CHECK (run.error == cue::runError::badMessage);
    }
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

//==============================================================================
TEST_CASE ("midi: a surface's port is the surface's, and every other port still reaches the triggers")
{
    /*  PHASE 6'S SEAM, and the test door beside it. A control surface's port
        is offered to a consumer - the bridge - before any trigger sees it, so a
        D700 moving a fader never fires a cue that listens for pitch bend; and
        `inject` puts a message on the same road an arriving one takes, which is
        what lets a surface be tested with no hardware in the room. */
    doc::ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto cueId = document.createCue (listId, 0, "memo", "Doors").id;

    const auto keys = document.createPort ("Keys").id;
    const auto desk = document.createPort ("Desk").id;

    const auto trigger = document.createTrigger (cueId, "midi").id;
    REQUIRE (document.setAttribute ("/godot/trigger/" + trigger + "/type", "noteOn").ok);
    REQUIRE (document.setAttribute ("/godot/trigger/" + trigger + "/number", "60").ok);

    Engine engine;
    midi::MidiInputs inputs;
    inputs.sendTo (engine);
    inputs.publishTriggers (cue::TriggerIndex::build (document));

    std::vector<std::string> taken;

    inputs.setConsumer ([&taken, &desk] (const std::string& portId, const midi::Bytes&)
                        {
                            if (portId != desk)
                                return false;

                            taken.push_back (portId);
                            return true;
                        });

    const midi::Bytes noteOn { 0x90, 60, 100 };

    /*  THE DESK'S NOTE IS THE DESK'S: taken, and no trigger fires. */
    inputs.inject (desk, noteOn);
    CHECK (taken.size() == 1u);

    auto result = engine.processTick (1);
    CHECK (result.applied + result.rejected + result.dropped == 0u);

    /*  THE KEYS' NOTE IS A TRIGGER'S, as it always was. `trigger.fire` is not
        registered on this engine, so it arrives and is refused - which is
        enough to say it was sent. */
    inputs.inject (keys, noteOn);
    CHECK (taken.size() == 1u);

    result = engine.processTick (2);
    CHECK (result.applied + result.rejected + result.dropped == 1u);

    /*  AND WITH NO CONSUMER, every port is the triggers'. */
    inputs.setConsumer ({});
    inputs.inject (desk, noteOn);

    result = engine.processTick (3);
    CHECK (result.applied + result.rejected + result.dropped == 1u);
}

//==============================================================================
#include <wfg/engine/midi/MidiSender.h>
#include <wfg/engine/midi/PortBinder.h>

TEST_CASE ("midi: a port changed while the show runs is put on its device then, not at the next start")
{
    /*  Found by the author on 2026-09-25, with the D700 on the desk: two ports
        added in the MIDI tab, both set to the D700, read "unbound" for the rest
        of the session - with no sentence, because nothing had been tried. The
        binding runs again for what the show changed, and only for that.

        NO HARDWARE: every device named here is one no machine has, so what is
        checked is which ports are tried again and what each one says - the
        sentence a real device would have replaced. */
    midi::MidiInputs inputs;
    midi::MidiSender outputs;
    midi::PortBinder binder { inputs, outputs };

    const auto wish = [] (std::string id, std::string label, std::string device)
    {
        midi::PortWish made;
        made.id = std::move (id);
        made.label = std::move (label);
        made.inputDevice = device;
        made.outputDevice = std::move (device);
        return made;
    };

    std::vector<midi::PortWish> show { wish ("PORT0001", "Desk", "no such desk, surely") };

    const auto atStart = binder.bindAll (show);
    REQUIRE (atStart.size() == 1u);
    CHECK_FALSE (atStart.front().binding.bound);
    CHECK (atStart.front().binding.problem.find ("no such desk, surely") != std::string::npos);

    //  The same show again: nothing to do, and nothing asked of the message thread.
    CHECK_FALSE (binder.want (show));
    CHECK (binder.take().empty());

    SUBCASE ("a port added is tried, and the one already there is left alone")
    {
        show.push_back (wish ("PORT0002", "Surface", "no such surface, surely"));
        REQUIRE (binder.want (show));
        CHECK (binder.take().empty());      // nothing until the message thread has done it

        binder.rebind();

        const auto done = binder.take();
        REQUIRE (done.size() == 1u);
        CHECK (done.front().id == "PORT0002");
        CHECK_FALSE (done.front().gone);
        CHECK_FALSE (done.front().binding.bound);
        CHECK (done.front().binding.problem.find ("no such surface, surely") != std::string::npos);

        CHECK (binder.take().empty());      // taken once
    }

    SUBCASE ("a port put on another device is tried on that one")
    {
        show.front().outputDevice = "another desk, surely";
        REQUIRE (binder.want (show));
        binder.rebind();

        const auto done = binder.take();
        REQUIRE (done.size() == 1u);
        CHECK (done.front().id == "PORT0001");
        CHECK (done.front().binding.problem.find ("another desk, surely") != std::string::npos);
    }

    SUBCASE ("a port only renamed keeps what it had")
    {
        show.front().label = "Lighting desk";
        REQUIRE (binder.want (show));
        binder.rebind();
        CHECK (binder.take().empty());
    }

    SUBCASE ("a port the show no longer declares is forgotten")
    {
        show.clear();
        REQUIRE (binder.want (show));
        binder.rebind();

        const auto done = binder.take();
        REQUIRE (done.size() == 1u);
        CHECK (done.front().id == "PORT0001");
        CHECK (done.front().gone);
    }

    SUBCASE ("two edits before the message thread gets there are one rebinding, of the last")
    {
        show.front().outputDevice = "first try";
        REQUIRE (binder.want (show));
        show.front().outputDevice = "second try";
        REQUIRE (binder.want (show));

        binder.rebind();
        binder.rebind();                    // the second has nothing left to do

        const auto done = binder.take();
        REQUIRE (done.size() == 1u);
        CHECK (done.front().binding.problem.find ("second try") != std::string::npos);
    }
}

//==============================================================================
/*  DOH! AND MIDI (PRD §3.32, namespace draft §24; the author, 2026-10-01).

    Two things the show says about a port, read from the document: whether it
    PLAYS SOUND - a synth, not a desk - which makes a scene that sent to it
    heard, and so paused rather than handed back; and its Doh! setting, which
    leaves what reached it to its operator unless it says take back. Each case
    failed before D1: `go.doh` was an unknown command, and neither row existed. */
namespace
{
    struct MidiDohRig
    {
        MidiDohRig()
        {
            engine.log().openInMemory ({});
            runner.setMidiSink (&sink);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            listId = document.createList ("Show").id;
            port = declarePort (document, "Keys");
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);
        }

        std::string midiCue (const std::string& parent, int index)
        {
            const auto id = document.createCue (parent, index, "midi", "Note").id;
            REQUIRE (document.setAttribute ("/godot/cue/" + id + "/port", port).ok);
            REQUIRE (document.setAttribute ("/godot/cue/" + id + "/type", "programChange").ok);
            REQUIRE (document.setAttribute ("/godot/cue/" + id + "/data1", "5").ok);
            return id;
        }

        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
        }

        Engine::TickResult press (const char* command)
        {
            engine.submit ("cli", command, {});
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        void park (const std::string& cueId)
        {
            REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), cueId).ok);
            tickOnce();
        }

        std::size_t revocations()
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            return static_cast<std::size_t> (std::count_if (parsed.records.begin(), parsed.records.end(),
                                                            [] (const auto& record) { return record.command == "run.revoke"; }));
        }

        const cue::Run* newestRunOf (const std::string& cueId) const
        {
            const cue::Run* out = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    out = &run;

            return out;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (43);
        cue::Runner runner { document, runs, runIds, focus };
        RecordingSink sink;

        std::string listId, port;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("go.doh: a MIDI cue to a port that plays sound makes its scene heard; to any other port it does not")
{
    MidiDohRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    rig.midiCue (scene, 0);
    const auto wait = rig.document.createCue (scene, 1, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + wait + "/preWait", "10").ok);

    auto heard = false;
    auto txOff = false;

    SUBCASE ("plays sound: heard, so its job ends it, nothing given back")
    {
        heard = true;
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
    }

    SUBCASE ("plays sound, and the port's own Doh! setting says take back: still heard")
    {
        heard = true;
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);
    }

    SUBCASE ("does not play sound: not heard, so the scene is given back") {}

    SUBCASE ("plays sound but switched off: nothing left the machine, so nothing was heard")
    {
        txOff = true;
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/tx", "false").ok);
    }

    rig.park (scene);
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK (rig.sink.sent.size() == (txOff ? 0u : 1u));

    const auto sceneRun = rig.newestRunOf (scene)->id;
    REQUIRE (rig.press ("go.doh").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    /*  (2026-10-02, D2: a scene nobody heard, the block the standby made ready
        and the GO adopted, is handed back exactly - prepared again, nothing
        revoked - where D1 gave it back the way a preparation is given back.) */
    if (heard)
        CHECK (rig.runs.find (sceneRun)->isFinished());
    else
        CHECK (rig.runs.find (sceneRun)->state == cue::runState::preparing);

    CHECK (rig.revocations() == 0u);
}

TEST_CASE ("go.doh: a MIDI port left to its operator gets the cue once in all")
{
    MidiDohRig rig;
    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.sink.sent.size() == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.sink.sent.size() == 1u);
    CHECK (rig.newestRunOf (note)->warning == std::string (cue::runWarning::leftToOperator));
}

TEST_CASE ("go.doh: a MIDI port that takes back gets the cue again from the corrected GO")
{
    MidiDohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);

    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();

    CHECK (rig.sink.sent.size() == 2u);
    CHECK (rig.newestRunOf (note)->warning.empty());
}

TEST_CASE ("go.doh: a MIDI cue that found no port sent nothing, so the corrected GO sends it")
{
    MidiDohRig rig;
    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    //  Bound to nothing of this port's at the early GO: `no-port`.
    rig.sink.bound = "SOMEWHERE";

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.newestRunOf (note)->error == cue::runError::noPort);
    REQUIRE (rig.sink.sent.empty());

    //  The cable plugged back in - the live rebind - before the corrected GO.
    rig.sink.bound = rig.port;

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();

    CHECK (rig.sink.sent.size() == 1u);
    CHECK (rig.newestRunOf (note)->warning.empty());
}

TEST_CASE ("go.doh: a scene heard through a synth is carried on - what it sent goes again only where its port takes back")
{
    /*  Doh! D2 (2026-10-02, namespace draft §24.12; the design's test 26, and
        the MIDI half of its test 7). A scene whose only sound so far was a MIDI
        cue to a port that plays sound was heard: paused, and the next GO carries
        it on - the line it was waiting on waits only what was left of its wait.
        What the note sent went to a port left to its operator, so it is not sent
        again; to one that takes back, the corrected GO sends it again, as a
        first GO would. On a port that does not play sound nothing was heard, and
        the scene is handed back exactly - the block the horizon made ready, as
        it was - and the corrected GO runs the note again, sending nothing. */
    MidiDohRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    const auto note = rig.midiCue (scene, 0);
    const auto hold = rig.document.createCue (scene, 1, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "10").ok);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    auto heard = true;
    auto takeBack = false;

    SUBCASE ("plays sound, left to its operator: carried on, the note not sent again")
    {
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
    }

    SUBCASE ("plays sound, and takes back: carried on, the note sent again")
    {
        takeBack = true;
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);
    }

    SUBCASE ("does not play sound: handed back as it was, the note left")
    {
        heard = false;
    }

    rig.park (scene);
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (rig.sink.sent.size() == 1u);

    const auto sceneRun = rig.newestRunOf (scene)->id;
    const auto* waiting = rig.newestRunOf (hold);
    REQUIRE (waiting != nullptr);
    REQUIRE (waiting->state == cue::runState::waiting);
    const auto dueBefore = waiting->dueTick;

    const auto dohTick = rig.tick;
    REQUIRE (rig.press ("go.doh").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    if (! heard)
    {
        CHECK (rig.runs.find (sceneRun)->state == cue::runState::preparing);
        CHECK (rig.revocations() == 0u);
    }

    const auto goTick = rig.tick;
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK (rig.sink.sent.size() == (takeBack ? 2u : 1u));

    if (heard)
    {
        CHECK (rig.newestRunOf (scene)->id != sceneRun);

        const auto* again = rig.newestRunOf (hold);
        REQUIRE (again != nullptr);
        CHECK (again->state == cue::runState::waiting);
        CHECK (again->dueTick == goTick + (dueBefore - dohTick));
        return;
    }

    //  The same block, adopted again; the note run again, sending nothing.
    CHECK (rig.newestRunOf (scene)->id == sceneRun);
    CHECK (rig.newestRunOf (note)->warning == std::string (cue::runWarning::leftToOperator));
}

//==============================================================================
/*  A DOUBLE ESC AND THE MIDI CABLE (2026-10-02, H4, namespace draft §23.10).

    PRD §4.4's immediate level "drops all actions": a cue's message still in the
    sender's queue is dropped. And the author's rule for what was already played
    (2026-09-30): a note-off for each note Go.dot started and has not ended -
    nothing else, no all-notes-off, and nothing to a synth Go.dot never played.
    The surface bridge's traffic shares the queue and is never dropped. Every
    case failed on the code before H4 but the last, a net: there was no queue to
    ask, no drop, and no note-off anywhere in the engine. */
#include <wfg/engine/midi/SendQueue.h>

namespace
{
    /*  The sending thread, by hand: what `MidiSender::run` does with a message
        whose port has a device - taken, shown to the note record, sent. */
    void leaves (midi::SendQueue& queue, midi::Outgoing item)
    {
        queue.push (std::move (item));

        midi::Outgoing next;
        REQUIRE (queue.pop (next));
        queue.markLeaving (next);
    }

    midi::Outgoing cueMessage (const std::string& port, midi::Bytes bytes, const std::string& run = "RUN00001")
    {
        return { port, std::move (bytes), run, true };
    }

    midi::Outgoing noteOffFor (const std::string& port, std::uint8_t status, std::uint8_t key)
    {
        return { port, { status, key, 0 }, {}, false };
    }

    bool same (const midi::Outgoing& a, const midi::Outgoing& b)
    {
        return a.port == b.port && a.bytes == b.bytes && a.run == b.run && a.cue == b.cue;
    }
}

TEST_CASE ("midi send queue: a double Esc drops the cues' messages and ends every note that left, once")
{
    midi::SendQueue queue;
    const std::string port = "PRT00001";

    SUBCASE ("a note that left is ended, and the cue message still waiting is dropped")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        queue.push (cueMessage (port, { 0xB1, 7, 90 }));
        REQUIRE (queue.notes().sounding() == 1u);

        CHECK (queue.dropQueued() == 1u);

        REQUIRE (queue.waiting().size() == 1u);
        CHECK (same (queue.waiting().front(), noteOffFor (port, 0x81, 60)));

        midi::Outgoing next;
        REQUIRE (queue.pop (next));
        CHECK (queue.empty());
        CHECK (queue.notes().sounding() == 0u);
    }

    SUBCASE ("a note a cue already ended gets nothing more - by a note-off, or a note-on at velocity nought")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));

        SUBCASE ("note-off") { leaves (queue, cueMessage (port, { 0x81, 60, 0 })); }
        SUBCASE ("note-on at nought") { leaves (queue, cueMessage (port, { 0x91, 60, 0 })); }

        CHECK (queue.notes().sounding() == 0u);
        CHECK (queue.dropQueued() == 0u);
        CHECK (queue.waiting().empty());
    }

    SUBCASE ("a note-on that never left is dropped, and gets no note-off")
    {
        queue.push (cueMessage (port, { 0x91, 60, 100 }));

        CHECK (queue.dropQueued() == 1u);
        CHECK (queue.waiting().empty());
        CHECK (queue.notes().sounding() == 0u);
    }

    SUBCASE ("a cue's own note-off still waiting is dropped, and the record's one goes in its place")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        queue.push (cueMessage (port, { 0x81, 60, 0 }));

        CHECK (queue.dropQueued() == 1u);
        REQUIRE (queue.waiting().size() == 1u);
        CHECK (same (queue.waiting().front(), noteOffFor (port, 0x81, 60)));
    }

    SUBCASE ("a surface's traffic is never dropped, and its note-ons are no note of the show's")
    {
        /*  A D700's LED colour is a note-on (PRD §3.16). */
        leaves (queue, { port, { 0x90, 0x5D, 0x7F }, {}, false });
        queue.push ({ port, { 0x90, 0x5E, 0x7F }, {}, false });

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 1u);
        CHECK (queue.waiting().front().bytes == midi::Bytes { 0x90, 0x5E, 0x7F });
        CHECK (queue.notes().sounding() == 0u);
    }

    SUBCASE ("the note-offs go ahead of what is still waiting")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        queue.push ({ port, { 0xF0, 0x7E, 0x7F, 0xF7 }, {}, false });   // a surface's SysEx

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 2u);
        CHECK (same (queue.waiting().front(), noteOffFor (port, 0x81, 60)));
    }

    SUBCASE ("a second double Esc before they have gone neither drops nor repeats them")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        leaves (queue, cueMessage (port, { 0x91, 64, 100 }));

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 2u);

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 2u);
        CHECK (same (queue.waiting()[0], noteOffFor (port, 0x81, 60)));
        CHECK (same (queue.waiting()[1], noteOffFor (port, 0x81, 64)));
    }

    SUBCASE ("one note-off per port, channel and key - however many cues pressed it")
    {
        const std::string other = "PRT00002";

        leaves (queue, cueMessage (port, { 0x91, 60, 100 }, "RUN00001"));
        leaves (queue, cueMessage (port, { 0x91, 60, 80 }, "RUN00002"));     // the same key again
        leaves (queue, cueMessage (port, { 0x92, 60, 100 }));                // channel 3
        leaves (queue, cueMessage (other, { 0x91, 60, 100 }));
        leaves (queue, cueMessage (other, { 0x9F, 60, 100 }));               // channel 16

        CHECK (queue.notes().sounding() == 4u);
        CHECK (queue.notes().runsOf (port, 1, 60) == std::vector<std::string> { "RUN00001", "RUN00002" });

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 4u);
        CHECK (same (queue.waiting()[0], noteOffFor (port, 0x81, 60)));
        CHECK (same (queue.waiting()[1], noteOffFor (port, 0x82, 60)));
        CHECK (same (queue.waiting()[2], noteOffFor (other, 0x81, 60)));
        CHECK (same (queue.waiting()[3], noteOffFor (other, 0x8F, 60)));
    }

    SUBCASE ("a cue's All Notes Off or All Sound Off ends that port and channel's notes")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        leaves (queue, cueMessage (port, { 0x91, 64, 100 }));
        leaves (queue, cueMessage (port, { 0x92, 67, 100 }));

        SUBCASE ("All Notes Off") { leaves (queue, cueMessage (port, { 0xB1, 123, 0 })); }
        SUBCASE ("All Sound Off") { leaves (queue, cueMessage (port, { 0xB1, 120, 0 })); }

        CHECK (queue.notes().sounding() == 1u);
        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 1u);
        CHECK (same (queue.waiting().front(), noteOffFor (port, 0x82, 67)));
    }

    SUBCASE ("a port given another device forgets what the old one was playing")
    {
        const std::string other = "PRT00002";

        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        leaves (queue, cueMessage (other, { 0x91, 62, 100 }));

        queue.forgetPort (port);

        CHECK (queue.dropQueued() == 0u);
        REQUIRE (queue.waiting().size() == 1u);
        CHECK (same (queue.waiting().front(), noteOffFor (other, 0x81, 62)));
    }
}

//==============================================================================
/*  A NOTE STILL DOWN WHEN THE SHOW CLOSES (2026-10-02, K4, namespace draft
    §23.15, closing H4's named limit IX). The sending thread's last delivery is
    what is still queued, then one note-off - `0x8n key 0`, as the double Esc's
    - for every key a cue started and nothing ended, the queued messages that
    will leave counted first; nothing to a synth Go.dot never played. With the
    call declared and doing what the close did before - the queue, and nothing
    after it - every subcase with a note left to end failed; those with none
    are guards. */
TEST_CASE ("midi send queue: the show closing ends every note a cue started, once, and nothing else")
{
    midi::SendQueue queue;
    const std::string port = "PRT00001";
    const std::string unbound = "PRT00002";
    const auto everyPort = [] (const std::string&) { return true; };

    SUBCASE ("a held note gets exactly one note-off")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));

        const auto last = queue.takeAllForClose (everyPort);

        REQUIRE (last.size() == 1u);
        CHECK (same (last.front(), noteOffFor (port, 0x81, 60)));
        CHECK (queue.empty());
        CHECK (queue.notes().sounding() == 0u);
    }

    SUBCASE ("a note a cue ended gets nothing")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        leaves (queue, cueMessage (port, { 0x81, 60, 0 }));

        CHECK (queue.takeAllForClose (everyPort).empty());
    }

    SUBCASE ("a surface's note-on is no note of the show's")
    {
        leaves (queue, { port, { 0x90, 0x5D, 0x7F }, {}, false });

        CHECK (queue.takeAllForClose (everyPort).empty());
    }

    SUBCASE ("a note-on still waiting leaves, and its note-off follows it")
    {
        queue.push (cueMessage (port, { 0x91, 62, 100 }));

        const auto last = queue.takeAllForClose (everyPort);

        REQUIRE (last.size() == 2u);
        CHECK (same (last[0], cueMessage (port, { 0x91, 62, 100 })));
        CHECK (same (last[1], noteOffFor (port, 0x81, 62)));
    }

    SUBCASE ("a cue's own note-off still waiting ends its note: nothing more")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        queue.push (cueMessage (port, { 0x81, 60, 0 }));

        const auto last = queue.takeAllForClose (everyPort);

        REQUIRE (last.size() == 1u);
        CHECK (same (last[0], cueMessage (port, { 0x81, 60, 0 })));
    }

    SUBCASE ("a note-on waiting for a port with no device goes nowhere, and is owed nothing")
    {
        queue.push (cueMessage (unbound, { 0x91, 60, 100 }));

        const auto last = queue.takeAllForClose ([&unbound] (const std::string& p) { return p != unbound; });

        REQUIRE (last.size() == 1u);                        // the sender passes it over
        CHECK (last.front().bytes == midi::Bytes { 0x91, 60, 100 });
    }

    SUBCASE ("one note-off a key, after the cues' messages still waiting")
    {
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }, "RUN00001"));
        leaves (queue, cueMessage (port, { 0x91, 60, 80 }, "RUN00002"));
        leaves (queue, cueMessage (port, { 0x92, 64, 100 }));
        queue.push (cueMessage (port, { 0xB1, 7, 0 }));                  // a cue's blackout

        const auto last = queue.takeAllForClose (everyPort);

        REQUIRE (last.size() == 3u);
        CHECK (same (last[0], cueMessage (port, { 0xB1, 7, 0 })));
        CHECK (same (last[1], noteOffFor (port, 0x81, 60)));
        CHECK (same (last[2], noteOffFor (port, 0x82, 64)));
    }

    SUBCASE ("a surface's traffic still waiting is dropped, so nothing holds the note-offs back")
    {
        /*  The review of K4: a surface's display SysEx can hold the thread for
            as long as JUCE waits for its port, and the surface is closing with
            the show. A cue's message still goes. */
        leaves (queue, cueMessage (port, { 0x91, 60, 100 }));
        queue.push ({ port, { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0x41, 0xF7 }, {}, false });
        queue.push ({ port, { 0x90, 0x5D, 0x7F }, {}, false });

        const auto last = queue.takeAllForClose (everyPort);

        REQUIRE (last.size() == 1u);
        CHECK (same (last[0], noteOffFor (port, 0x81, 60)));
    }
}

//==============================================================================
namespace
{
    /*  A sink built on the real queue, with the sending thread done by hand
        (`drain`), so a case can stop between what was handed over and what
        left. */
    struct QueueingSink final : midi::MidiSink
    {
        std::string send (const std::string& port, const midi::Bytes& bytes) override
        {
            outbox.push ({ port, bytes, {}, false });
            return {};
        }

        std::string sendForRun (const std::string& runId, const std::string& port,
                                const midi::Bytes& bytes) override
        {
            outbox.push ({ port, bytes, runId, true });
            return {};
        }

        std::size_t dropQueued() override
        {
            ++drops;
            return outbox.dropQueued();
        }

        void drain()
        {
            midi::Outgoing next;

            while (outbox.pop (next))
            {
                outbox.markLeaving (next);
                wire.push_back (next);
            }
        }

        /*  The sending thread's last delivery, when the sender stops: what is
            still queued, then the note-offs the show closing owes. */
        void close()
        {
            for (auto& message : outbox.takeAllForClose ([] (const std::string&) { return true; }))
                wire.push_back (std::move (message));
        }

        midi::SendQueue outbox;
        std::vector<midi::Outgoing> wire;
        int drops = 0;
    };

    struct MidiStopRig
    {
        MidiStopRig()
        {
            runner.setMidiSink (&sink);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            listId = document.createList ("Show").id;
            port = declarePort (document, "Synth");
        }

        /*  A MIDI cue on channel 2 of the synth. */
        std::string midiCue (const std::string& parent, int index, const char* type, int data1, int data2)
        {
            const auto id = document.createCue (parent, index, "midi", "Synth").id;
            const auto base = "/godot/cue/" + id + "/";

            REQUIRE (document.setAttribute (base + "port", port).ok);
            REQUIRE (document.setAttribute (base + "type", type).ok);
            REQUIRE (document.setAttribute (base + "channel", "2").ok);
            REQUIRE (document.setAttribute (base + "data1", std::to_string (data1)).ok);
            REQUIRE (document.setAttribute (base + "data2", std::to_string (data2)).ok);
            return id;
        }

        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
        }

        void press (const char* command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", command, std::move (args)));
            tickOnce();
        }

        void fire (const std::string& cueId) { press ("cue.fire", { osc::Value::string (cueId) }); }

        const cue::Run* runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return &run;

            return nullptr;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (47);
        cue::Runner runner { document, runs, runIds, focus };
        QueueingSink sink;

        std::string listId, port;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("midi cue: double Esc sends exactly one note-off for a note a cue started, and Esc sends none")
{
    MidiStopRig rig;
    const auto note = rig.midiCue (rig.listId, 0, "noteOn", 60, 100);
    const auto swell = rig.midiCue (rig.listId, 1, "controlChange", 7, 90);

    rig.fire (note);
    rig.sink.drain();

    REQUIRE (rig.sink.wire.size() == 1u);
    CHECK (rig.sink.wire[0].bytes == midi::Bytes { 0x91, 60, 100 });

    /*  THE RUN RIDES WITH IT: what Doh! will ask the note record for. */
    REQUIRE (rig.runOf (note) != nullptr);
    CHECK (rig.sink.wire[0].run == rig.runOf (note)->id);
    CHECK (rig.sink.wire[0].cue);

    SUBCASE ("double Esc: the waiting message dropped, one note-off for the note")
    {
        rig.fire (swell);                                   // handed over, not yet gone
        REQUIRE (rig.sink.outbox.size() == 1u);

        rig.press ("run.killAll");

        CHECK (rig.sink.drops == 1);
        REQUIRE (rig.sink.outbox.size() == 1u);
        CHECK (rig.sink.outbox.waiting().front().port == rig.port);
        CHECK (rig.sink.outbox.waiting().front().bytes == midi::Bytes { 0x81, 60, 0 });

        rig.sink.drain();

        REQUIRE (rig.sink.wire.size() == 2u);
        CHECK (rig.sink.wire.back().bytes == midi::Bytes { 0x81, 60, 0 });
        CHECK (rig.sink.outbox.notes().sounding() == 0u);

        for (const auto& message : rig.sink.wire)
            CHECK (message.bytes.front() != 0xB1);           // the swell never left
    }

    SUBCASE ("double Esc after the cue's own note-off: nothing more")
    {
        const auto release = rig.midiCue (rig.listId, 2, "noteOff", 60, 0);
        rig.fire (release);
        rig.sink.drain();
        REQUIRE (rig.sink.wire.size() == 2u);

        rig.press ("run.killAll");

        CHECK (rig.sink.drops == 1);
        CHECK (rig.sink.outbox.empty());
        rig.sink.drain();
        CHECK (rig.sink.wire.size() == 2u);
    }

    SUBCASE ("Esc drops nothing and ends no note")
    {
        rig.fire (swell);
        REQUIRE (rig.sink.outbox.size() == 1u);

        rig.press ("run.stopAll");

        CHECK (rig.sink.drops == 0);
        REQUIRE (rig.sink.outbox.size() == 1u);
        CHECK (rig.sink.outbox.waiting().front().bytes == midi::Bytes { 0xB1, 7, 90 });
        CHECK (rig.sink.outbox.notes().sounding() == 1u);
    }
}

TEST_CASE ("midi cue: a member a killed scene was about to launch sends nothing after the press")
{
    /*  DROP ALL ACTIONS REACHES WHAT WAS STILL TO BE ASKED FOR (namespace draft
        §23.10). A killed scene ends its members a tick after the press, from its
        own job - and in the press's tick that job had already asked for its
        next member's launch, which drains after the press. The member fired:
        its note-on left after the note-offs, and rang with nothing to end it. */
    MidiStopRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    rig.document.createCue (scene, 0, "memo", "Count in");
    const auto note = rig.midiCue (scene, 1, "noteOn", 60, 100);

    rig.fire (scene);

    for (int n = 0; n < 20 && rig.runOf (note) == nullptr; ++n)
        rig.tickOnce();

    /*  Spawned, and its launch asked for in the next tick's hook - the press's. */
    REQUIRE (rig.runOf (note) != nullptr);
    REQUIRE (rig.runOf (note)->state == cue::runState::armed);
    const auto noteRun = rig.runOf (note)->id;

    rig.press ("run.killAll");

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    rig.sink.drain();

    CHECK (rig.sink.wire.empty());
    CHECK (rig.sink.outbox.notes().sounding() == 0u);
    CHECK (rig.runs.find (noteRun)->isFinished());
    CHECK (rig.runs.find (noteRun)->launchRequestedAtTick == 0);
}

TEST_CASE ("midi cue: closing the show sends exactly one note-off for a note a cue started, and none for one it ended")
{
    /*  K4 (2026-10-02, namespace draft §23.15): through the Runner, the
        sending thread's last delivery done by hand as `MidiSender::run` does
        it when the sender stops. */
    MidiStopRig rig;
    const auto note = rig.midiCue (rig.listId, 0, "noteOn", 60, 100);

    rig.fire (note);
    rig.sink.drain();
    REQUIRE (rig.sink.wire.size() == 1u);

    SUBCASE ("held")
    {
        rig.sink.close();

        REQUIRE (rig.sink.wire.size() == 2u);
        CHECK (rig.sink.wire.back().port == rig.port);
        CHECK (rig.sink.wire.back().bytes == midi::Bytes { 0x81, 60, 0 });
    }

    SUBCASE ("ended by its own note-off")
    {
        rig.fire (rig.midiCue (rig.listId, 1, "noteOff", 60, 0));
        rig.sink.drain();
        REQUIRE (rig.sink.wire.size() == 2u);

        rig.sink.close();

        CHECK (rig.sink.wire.size() == 2u);
    }

    SUBCASE ("ended by a double Esc's note-off: the close owes nothing more")
    {
        rig.press ("run.killAll");
        rig.sink.drain();
        REQUIRE (rig.sink.wire.size() == 2u);

        rig.sink.close();

        CHECK (rig.sink.wire.size() == 2u);
    }
}

TEST_CASE ("midi: a double Esc on a sender with nothing bound is harmless")
{
    /*  The real sender, with no device and no thread - every CI runner's
        machine. A net: it compiled only once the two calls existed, and
        nothing bound leaves nothing to drop. */
    midi::MidiSender outputs;

    CHECK (outputs.sendForRun ("RUN00001", "PRT00001", { 0x90, 60, 100 }) == midi::sendError::noPort);
    CHECK (outputs.dropQueued() == 0u);
}

//==============================================================================
/*  THE SENDING THREAD ITSELF, STARTED AND STOPPED (2026-10-02, the review of
    K4, namespace draft §23.15). `midi::Output` is the seam: the sender's own
    thread sends to a fake port, so the close is driven through `start` and
    `stop` as the product drives it - not by hand. */
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    /*  A port that records what reached it, and can be made slow: each send
        holds the sending thread for `delayMs`, as a driver retrying a port
        that says it is not ready does. */
    struct FakePort final : midi::Output
    {
        void sendNow (const midi::Bytes& bytes) override
        {
            if (const auto ms = delayMs.load(); ms > 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (ms));

            const std::lock_guard<std::mutex> lock { wireLock };
            received.push_back (bytes);
        }

        std::string name() const override { return "Fake synth"; }

        std::vector<midi::Bytes> wire() const
        {
            const std::lock_guard<std::mutex> lock { wireLock };
            return received;
        }

        std::size_t count (const midi::Bytes& bytes) const
        {
            const auto all = wire();
            return static_cast<std::size_t> (std::count (all.begin(), all.end(), bytes));
        }

        std::atomic<int> delayMs { 0 };
        mutable std::mutex wireLock;
        std::vector<midi::Bytes> received;
    };

    /*  Until the sending thread has sent `wanted` messages, or five seconds. */
    bool waitForSent (const midi::MidiSender& outputs, std::size_t wanted)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (5);

        while (outputs.sent() < wanted)
        {
            if (std::chrono::steady_clock::now() > until)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }

        return true;
    }
}

TEST_CASE ("midi sender: stopping it ends a note a cue left down, once, and sends nothing a synth never played")
{
    midi::MidiSender outputs;
    const auto synth = std::make_shared<FakePort>();
    const auto other = std::make_shared<FakePort>();
    outputs.attach ("PRT00001", synth);
    outputs.attach ("PRT00002", other);
    outputs.start();

    REQUIRE (outputs.sendForRun ("RUN00001", "PRT00001", { 0x91, 60, 100 }).empty());   // held
    REQUIRE (outputs.sendForRun ("RUN00002", "PRT00001", { 0x91, 62, 100 }).empty());
    REQUIRE (outputs.sendForRun ("RUN00002", "PRT00001", { 0x81, 62, 0 }).empty());     // ended
    REQUIRE (outputs.send ("PRT00001", { 0x90, 0x5D, 0x7F }).empty());                 // a surface's LED
    REQUIRE (waitForSent (outputs, 4));

    outputs.stop();

    const auto wire = synth->wire();
    REQUIRE (wire.size() == 5u);
    CHECK (wire.back() == midi::Bytes { 0x81, 60, 0 });
    CHECK (synth->count ({ 0x81, 60, 0 }) == 1u);
    CHECK (synth->count ({ 0x81, 62, 0 }) == 1u);                  // the cue's own, and no more
    CHECK (synth->count ({ 0x80, 0x5D, 0 }) == 0u);
    CHECK (other->wire().empty());

    //  A second stop - the destructor's - sends nothing more.
    outputs.stop();
    CHECK (synth->wire().size() == 5u);
}

TEST_CASE ("midi sender: a slow port still gets every note-off at the close")
{
    /*  A port that takes 45 ms a message is slow, not dead: the review's case
        against K4's first cap, which took any short message over 40 ms as a
        dead port and sent it nothing more - one note-off of three. */
    midi::MidiSender outputs;
    const auto synth = std::make_shared<FakePort>();
    outputs.attach ("PRT00001", synth);
    outputs.start();

    /*  Two notes, not three: a 45 ms sleep can last twice that on a shared
        macOS runner, and three of those overran the 250 ms budget (CI,
        2026-10-02). Two still fail the old cap, which sent one. */
    for (const std::uint8_t key : { std::uint8_t { 60 }, std::uint8_t { 64 } })
        REQUIRE (outputs.sendForRun ("RUN00001", "PRT00001", { 0x91, key, 100 }).empty());

    REQUIRE (waitForSent (outputs, 2));
    synth->delayMs = 45;

    outputs.stop();

    CHECK (synth->count ({ 0x81, 60, 0 }) == 1u);
    CHECK (synth->count ({ 0x81, 64, 0 }) == 1u);
}

TEST_CASE ("midi sender: a port that does not take messages holds the close for a bounded time")
{
    /*  Ten notes held on a port that now takes 100 ms a message: the close
        gives that port its budget and moves on, rather than a second. */
    midi::MidiSender outputs;
    const auto synth = std::make_shared<FakePort>();
    outputs.attach ("PRT00001", synth);
    outputs.start();

    for (std::uint8_t key = 60; key < 70; ++key)
        REQUIRE (outputs.sendForRun ("RUN00001", "PRT00001", { 0x91, key, 100 }).empty());

    REQUIRE (waitForSent (outputs, 10));
    synth->delayMs = 100;

    const auto began = std::chrono::steady_clock::now();
    outputs.stop();
    const auto took = std::chrono::steady_clock::now() - began;

    const auto offs = synth->wire().size() - 10u;
    CHECK (offs >= 1u);
    CHECK (offs < 10u);
    CHECK (took < std::chrono::milliseconds (700));
}

TEST_CASE ("midi sender: started and stopped with nothing to send, it always returns")
{
    /*  THE LOST WAKE-UP (the review of K4). `stop` used to clear `running`
        and notify without the queue lock, while the sending thread reads
        `running` under it before it waits: a clear landing between that read
        and the wait was never heard, and the join waited for ever. A net -
        the window is a few instructions wide and no loop of this length is
        sure to hit it; what makes it impossible is the lock (MidiSender.cpp). */
    for (int round = 0; round < 200; ++round)
    {
        midi::MidiSender outputs;
        outputs.attach ("PRT00001", std::make_shared<FakePort>());
        outputs.start();
        outputs.stop();
    }

    midi::MidiSender never;
    never.stop();                                   // never started: returns
    CHECK (never.sent() == 0u);
}

//==============================================================================
/*  DOH! D3 AND MIDI (2026-10-03, PRD §3.32, namespace draft §24.13): the
    report names a port left to its operator by the name the show gives it; a
    MIDI member the GO stopped in its pre-wait is put back with the rest of
    that wait, under the act the Doh brings back to life. Each case was run on
    the engine sources of 5fd0e76 with it, and failed there. */
TEST_CASE ("go.doh: a MIDI cue to a port left to its operator is named by the port's name")
{
    /*  The design's test 25, its MIDI SUBCASE. */
    MidiDohRig rig;
    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.sink.sent.size() == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    const auto& report = rig.runner.listState().dohReport().text;
    INFO (report);
    CHECK (report.find ("Keys: Note - left to its operator, not sent again") != std::string::npos);
}

TEST_CASE ("go.doh: a MIDI member of a running act the GO stopped in its pre-wait is put back under the act with the rest of its wait")
{
    /*  The design's test 29, its MIDI SUBCASE (red team C minor 3). The act's
        line, then its MIDI member - four seconds of pre-wait - GO'd; the GO a
        Doh! takes back is a stop cue after the act, aimed at the member while
        it waits. The act, its last member ended, is brought back to life (HE),
        and the member put back under it with what was left of its wait: it
        sends at its time, whatever its port says, since nothing of it had left. */
    MidiDohRig rig;
    const auto act = rig.document.createCue (rig.listId, 0, "group", "Act").id;
    const auto line = rig.document.createCue (act, 0, "memo", "Line").id;
    const auto note = rig.midiCue (act, 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + note + "/preWait", "4").ok);

    const auto stop = rig.document.createCue (rig.listId, 1, "transport", "Hold the note").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stop + "/target", note).ok);
    rig.document.createCue (rig.listId, 2, "memo", "After");

    rig.park (line);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);               // the note, waiting
    rig.tickOnce();

    const auto old = rig.newestRunOf (note)->id;
    const auto due = rig.newestRunOf (note)->dueTick;
    REQUIRE (rig.newestRunOf (note)->state == cue::runState::waiting);

    for (int n = 0; n < 50; ++n)
        rig.tickOnce();

    REQUIRE (rig.press ("go").rejected == 0);               // the stop, early
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.runs.find (old)->isFinished());

    REQUIRE (rig.press ("go.doh").rejected == 0);

    const auto* again = rig.newestRunOf (note);
    REQUIRE (again->id != old);
    CHECK (again->state == cue::runState::waiting);
    CHECK (again->dueTick == due);
    CHECK (again->parent == rig.newestRunOf (act)->id);
    CHECK (rig.newestRunOf (act)->state == cue::runState::playing);

    while (rig.tick < due + 3)
        rig.tickOnce();

    CHECK (rig.sink.sent.size() == 1u);
}

//==============================================================================
/*  DOH! D4 AND MIDI (2026-10-03, PRD §3.32, namespace draft §24.14): what a
    GO sent to a port that TAKES BACK cannot be called back off the cable - a
    message is an event - so the report names it, and the next GO sends it
    again, as a first GO would (the author, 2026-09-30, (a)). What went to a
    port left to its operator is D3's item, by the port's name. Each case was
    run on the engine of bd25dd5 (D3) first. */
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/log/Replay.h>

namespace
{
    std::size_t sentOn (const RecordingSink& sink, const std::string& port)
    {
        return static_cast<std::size_t> (std::count_if (sink.sent.begin(), sink.sent.end(),
                                                        [&port] (const auto& message) { return message.port == port; }));
    }

    bool reportSays (const MidiDohRig& rig, const std::string& part)
    {
        return rig.runner.listState().dohReport().text.find (part) != std::string::npos;
    }
}

TEST_CASE ("go.doh: a MIDI cue the GO sent to a port that takes back is named as not taken back, and the next GO sends it again")
{
    /*  The design's test 1. The corrected GO is a first GO for it: it sends,
        and its run carries no warning. */
    MidiDohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);

    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.sink.sent.size() == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    INFO (rig.runner.listState().dohReport().text);
    CHECK (reportSays (rig, "Note: MIDI to Keys could not be taken back - the next GO sends it again"));
    CHECK_FALSE (reportSays (rig, "left to its operator"));

    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.sink.sent.size() == 2u);
    CHECK (rig.newestRunOf (note)->warning.empty());
}

TEST_CASE ("go.doh: a MIDI cue that put nothing on a cable is not named as sent")
{
    /*  What counts as sent is what left (HQ): a cue that found no port, or a
        port switched off, put nothing on a cable, so the report has nothing to
        say of it - the next GO sends it as the first GO it is. A net: D3 said
        nothing of MIDI at all. */
    MidiDohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);

    const auto note = rig.midiCue (rig.listId, 0);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    SUBCASE ("no port bound")
    {
        rig.sink.bound = "SOMEWHERE";
    }

    SUBCASE ("the port switched off")
    {
        REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/tx", "false").ok);
    }

    rig.park (note);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.sink.sent.empty());

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.tickOnce();
    rig.tickOnce();

    INFO (rig.runner.listState().dohReport().text);
    CHECK_FALSE (reportSays (rig, "Note: MIDI"));
}

TEST_CASE ("go.doh: a heard scene's MIDI member to a port that takes back is named, and sent again at the resume")
{
    /*  The design's test 3: the resume half is D2's (the scene carried on, the
        note sent again where its port takes back); this adds the naming. */
    MidiDohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/audible", "true").ok);
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    rig.midiCue (scene, 0);
    const auto hold = rig.document.createCue (scene, 1, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "10").ok);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    rig.park (scene);
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (rig.sink.sent.size() == 1u);
    const auto sceneRun = rig.newestRunOf (scene)->id;

    REQUIRE (rig.press ("go.doh").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    INFO (rig.runner.listState().dohReport().text);
    CHECK (reportSays (rig, "Note: MIDI to Keys could not be taken back - the next GO sends it again"));

    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    //  Carried on - a new run of the scene, seated - and the note sent again.
    CHECK (rig.newestRunOf (scene)->id != sceneRun);
    CHECK (rig.sink.sent.size() == 2u);
}

TEST_CASE ("go.doh: the report is one engine record, and a replay rebuilds the same readout - MIDI sent again and MIDI left")
{
    /*  The design's test 4. One scene, two ports: Keys takes back, Lights is
        left to its operator (the default). The report names both - the left
        one by its port - and reaches the readout as ONE engine record,
        `list.dohReport`, which a replay re-injects: the same sentence on the
        same list at the same tick, with no sink and no hook. */
    MidiDohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/port/" + rig.port + "/doh", "takeBack").ok);
    const auto lights = declarePort (rig.document, "Lights");

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);
    rig.midiCue (scene, 0);
    const auto cueToLights = rig.midiCue (scene, 1);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueToLights + "/port", lights).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueToLights + "/name", "Q12").ok);
    const auto hold = rig.document.createCue (scene, 2, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "10").ok);
    rig.document.createCue (rig.listId, 1, "memo", "After");

    //  Parked by a record, so the replay below stands where the session stood.
    REQUIRE (rig.engine.submit ("cli", "standby.set", { osc::Value::string (scene) }));
    rig.tickOnce();
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (sentOn (rig.sink, rig.port) == 1u);
    REQUIRE (sentOn (rig.sink, lights) == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    const auto report = rig.runner.listState().dohReport();
    INFO (report.text);
    CHECK (report.list == rig.listId);
    CHECK (reportSays (rig, "Note: MIDI to Keys could not be taken back - the next GO sends it again"));
    CHECK (reportSays (rig, "Lights: Q12 - left to its operator, not sent again"));

    const auto logged = LogFile::parse (rig.engine.log().contents());
    REQUIRE (logged.errors.empty());
    CHECK (std::count_if (logged.records.begin(), logged.records.end(),
                          [] (const auto& record) { return record.command == "list.dohReport"; }) == 1);

    //  The corrected GO: Keys gets the note again, Lights nothing more.
    REQUIRE (rig.press ("go").rejected == 0);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK (sentOn (rig.sink, rig.port) == 2u);
    CHECK (sentOn (rig.sink, lights) == 1u);

    //  And the session again, from its log alone, with no sink: `wfg replay`'s shape.
    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    MidiDohRig fresh;
    fresh.runner.setMidiSink (nullptr);
    REQUIRE (doc::CanonicalXml::read (show, fresh.document).ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);

    for (const auto& run : rig.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = fresh.runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->state == run.state);
        CHECK (again->takenBack == run.takenBack);
        CHECK (again->sendsLeft == run.sendsLeft);
    }

    CHECK (fresh.runner.listState().dohReport().text == report.text);
    CHECK (fresh.runner.listState().dohReport().list == report.list);
    CHECK (fresh.runner.listState().dohReport().tick == report.tick);
}
