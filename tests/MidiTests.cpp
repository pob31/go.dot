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

#include <wfg/engine/midi/MidiInputs.h>

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
