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

/*  THE MIDI INPUTS A SHOW LISTENS ON.

    §3.7 lets a cue be fired by a MIDI event. What arrives on a port is a fact
    about this machine - which is why the ports to open are a command-line
    argument (`--midi-in=<device>`) and never a thing the document says, exactly
    as the audio device is. A show that named its interfaces would be a show
    that only ran in one building.

    THE CALLBACK IS SOMEBODY ELSE'S THREAD, so it does what every other outside
    input does: it reads an immutable trigger index, matches, and submits. It
    never touches the document, which belongs to the tick thread, and it never
    reaches into the model. `Engine::submit` is the one thing here that crosses
    a thread, and it is the same crossing the OSC socket already makes.

    THIS FILE IS WHERE JUCE'S MIDI HEADERS LIVE and the only place they do. The
    conversion from a `juce::MidiMessage` into the engine's own `MidiEvent`
    happens here, in a dozen lines, so that the cue layer and its tests need no
    audio headers at all - which is what lets the matching be tested on a
    machine with no MIDI interface, and every CI runner is one.
*/

#pragma once

#include <wfg/engine/cue/TriggerIndex.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfg
{
    class Engine;
}

namespace wfg::midi
{
    /** What this machine has, by the names a person would type. */
    std::vector<std::string> availableInputs();
    std::vector<std::string> availableOutputs();

    /*  A `juce::MidiMessage` as the engine says it.

        CLASSIFIED BY THE WIRE rather than by JUCE's convenience. JUCE reports a
        note-on of velocity nought as a note-OFF by default, which is right for
        a synthesiser and wrong here: §3.7 lets a trigger ask for a velocity, and
        `data = 0` on a `noteOn` is a thing somebody will need to match
        deliberately - it is how a great many surfaces spell "released". So what
        the status byte says is what is reported.

        Public so it can be tested without a port. */
    cue::MidiEvent eventFrom (const juce::MidiMessage& message, const std::string& port);

    //==========================================================================
    /*  Opens the named inputs and turns matching events into `trigger.fire`.

        A DEVICE THAT IS NOT THERE IS A SENTENCE AT STARTUP, never a silence:
        a trigger that never fires because a cable is in the wrong socket is
        the failure this class exists to make loud. `problems()` is what the
        caller prints.
    */
    class MidiInputs final : private juce::MidiInputCallback
    {
    public:
        MidiInputs() = default;
        ~MidiInputs() override;

        /*  Opens one input by the name a person typed. False, with a line in
            `problems()`, when this machine has no such device. */
        bool open (const std::string& name);

        /** Where matched events go. Set before opening anything. */
        void sendTo (Engine& engine) noexcept { target = &engine; }

        /*  The triggers to match against, republished by the tick thread
            whenever the document changes - the same immutable snapshot the OSC
            side reads, and for the same reason. */
        void publishTriggers (std::shared_ptr<const cue::TriggerIndex> index);

        const std::vector<std::string>& problems() const noexcept { return refusals; }
        std::size_t count() const noexcept { return open_.size(); }

        void closeAll();

    private:
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

        Engine* target = nullptr;

        std::vector<std::unique_ptr<juce::MidiInput>> open_;
        std::vector<std::string> refusals;

        mutable std::mutex triggerMutex;
        std::shared_ptr<const cue::TriggerIndex> triggers;
    };
}
