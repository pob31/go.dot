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

#include <wfg/engine/midi/MidiInputs.h>

#include <wfg/engine/Engine.h>

namespace wfg::midi
{
    namespace
    {
        std::vector<std::string> namesOf (const juce::Array<juce::MidiDeviceInfo>& devices)
        {
            std::vector<std::string> out;

            for (const auto& device : devices)
                out.push_back (device.name.toStdString());

            return out;
        }
    }

    std::vector<std::string> availableInputs()
    {
        return namesOf (juce::MidiInput::getAvailableDevices());
    }

    std::vector<std::string> availableOutputs()
    {
        return namesOf (juce::MidiOutput::getAvailableDevices());
    }

    //==========================================================================
    cue::MidiEvent eventFrom (const juce::MidiMessage& message, const std::string& port)
    {
        cue::MidiEvent event;
        event.port = port;
        event.channel = message.getChannel();

        /*  CLASSIFIED BY THE STATUS BYTE, which is why `isNoteOn (true)` comes
            first. JUCE reports a note-on of velocity nought as a note-OFF by
            default, and that is right for a synthesiser and wrong here: §3.7
            lets a trigger ask for a velocity, and matching `data = 0` on a
            `noteOn` is how somebody catches the release from the very many
            surfaces that spell it that way. What the wire said is what is
            reported; what it MEANS is the trigger's business. */
        if (message.isNoteOn (true))
        {
            event.type = cue::triggerType::noteOn;
            event.number = message.getNoteNumber();
            event.data = message.getVelocity();
        }
        else if (message.isNoteOff (false))
        {
            event.type = cue::triggerType::noteOff;
            event.number = message.getNoteNumber();
            event.data = message.getVelocity();
        }
        else if (message.isController())
        {
            event.type = cue::triggerType::controlChange;
            event.number = message.getControllerNumber();
            event.data = message.getControllerValue();
        }
        else if (message.isProgramChange())
        {
            event.type = cue::triggerType::programChange;
            event.number = message.getProgramChangeNumber();
            event.data = 0;
        }

        return event;
    }

    //==========================================================================
    MidiInputs::~MidiInputs()
    {
        closeAll();
    }

    bool MidiInputs::open (const std::string& name)
    {
        const auto wanted = juce::String (name);

        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            if (device.name != wanted)
                continue;

            auto input = juce::MidiInput::openDevice (device.identifier, this);

            if (input == nullptr)
            {
                refusals.push_back ("--midi-in: \"" + name + "\" is there and would not open");
                return false;
            }

            input->start();
            open_.push_back (std::move (input));
            return true;
        }

        /*  A SENTENCE AT STARTUP, never a silence. A trigger that never fires
            because a cable is in the wrong socket is the failure this exists to
            make loud, and the moment to say so is while somebody is still
            looking at the terminal they typed it into. The names this machine
            does have go in the message, because the answer is almost always one
            of them spelled differently. */
        auto problem = "--midi-in: this machine has no MIDI input called \"" + name + "\".";
        const auto have = availableInputs();

        if (have.empty())
        {
            problem += " It has none at all.";
        }
        else
        {
            problem += " It has:";

            for (const auto& other : have)
                problem += "\n    " + other;
        }

        refusals.push_back (std::move (problem));
        return false;
    }

    void MidiInputs::publishTriggers (std::shared_ptr<const cue::TriggerIndex> index)
    {
        const std::lock_guard<std::mutex> lock { triggerMutex };
        triggers = std::move (index);
    }

    void MidiInputs::closeAll()
    {
        for (auto& input : open_)
            if (input != nullptr)
                input->stop();

        open_.clear();
    }

    //==========================================================================
    void MidiInputs::handleIncomingMidiMessage (juce::MidiInput* source,
                                                const juce::MidiMessage& message)
    {
        /*  SOMEBODY ELSE'S THREAD, so this does what every outside input does:
            reads an immutable index, matches, submits. It never touches the
            document - which belongs to the tick thread - and never reaches into
            the model. `Engine::submit` is the one crossing here, and it is the
            same one the OSC socket already makes. */
        if (target == nullptr)
            return;

        std::shared_ptr<const cue::TriggerIndex> index;

        {
            const std::lock_guard<std::mutex> lock { triggerMutex };
            index = triggers;
        }

        if (index == nullptr)
            return;

        const auto port = source != nullptr ? source->getName().toStdString() : std::string {};
        const auto event = eventFrom (message, port);

        if (event.type.empty())
            return;

        for (const auto& id : cue::matchMidi (*index, event))
            target->submit ({ "midi:" + port, "trigger.fire",
                              { osc::Value::string (id) } });
    }
}
