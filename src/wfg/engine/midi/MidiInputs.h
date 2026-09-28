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
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/midi/PortTable.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <map>
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
        /** This machine's inputs, name and identifier, for a menu to offer. */
        static std::vector<Device> availableDevices();

        MidiInputs() = default;
        ~MidiInputs() override;

        /*  Opens one input by the name a person typed. False, with a line in
            `problems()`, when this machine has no such device.

            `portId` IS WHAT AN ARRIVING EVENT IS STAMPED WITH, and that is the
            whole of the author's 2026-09-22 decision made operational. A
            trigger names the port the SHOW declares - "Lights" - and never the
            cable, so moving an interface to another socket changes one binding
            in the settings while every trigger goes on firing. Empty is a
            `--midi-in` with no port behind it, which stamps the device's own
            name and is what a rig with one cable and no declared ports has
            always done. */
        bool open (const std::string& name, const std::string& portId = {});

        /*  The same, for a port the SHOW declares: both halves of the device
            are given and matched by `PortTable::match` - the identifier first,
            because it is the only thing that tells two identical interfaces
            apart, and the name second, because it is the only thing that
            survives a cable moving to another socket. What was matched comes
            back in `matchedId` so the caller can write it down, and `why`
            carries the sentence when nothing was. */
        bool openAs (const std::string& deviceName, const std::string& wantedId,
                     const std::string& portId, std::string& matchedId, std::string& why);

        /** Where matched events go. Set before opening anything. */
        void sendTo (Engine& engine) noexcept { target = &engine; }

        /*  The triggers to match against, republished by the tick thread
            whenever the document changes - the same immutable snapshot the OSC
            side reads, and for the same reason. */
        void publishTriggers (std::shared_ptr<const cue::TriggerIndex> index);

        /*  A PORT'S TRAFFIC CAN BELONG TO SOMETHING ELSE (Phase 6). A control
            surface's port is the surface's: its faders, touches and buttons are
            not trigger sources, and a D700 moving a fader must not fire a cue
            that happens to listen for pitch bend. So every arriving message is
            offered to this first, with the port it is stamped with, and a
            message it takes goes no further. Called on the input thread; set
            before anything is opened, and replaced whole. */
        using Consumer = std::function<bool (const std::string& portId, const Bytes& message)>;
        void setConsumer (Consumer consumerToUse);

        /*  THE TEST SEAM: a message as if it had arrived on `portId`, down the
            same road an arriving one takes - the consumer first, then the
            triggers. It is what lets bytes from a surface be followed through
            the bridge, the commands, the Runner and back out to bytes with no
            hardware in the room, which is every CI runner. Nothing in the
            product calls it. */
        void inject (const std::string& portId, const Bytes& message);

        const std::vector<std::string>& problems() const noexcept { return refusals; }
        std::size_t count() const noexcept { return open_.size(); }

        void closeAll();

        /*  CLOSES WHAT ONE DECLARED PORT LISTENS ON, so it can be opened again
            on another device while the show runs (2026-09-25). The message
            thread's, as opening is. */
        void close (const std::string& portId);

    private:
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

        /** The one road every message takes: consumer, then triggers. */
        void route (const std::string& portId, const Bytes& message);

        /*  Swapped whole under `triggerMutex` and called outside it, so a slow
            consumer never holds the lock a republished trigger index waits on. */
        std::shared_ptr<const Consumer> consumer;

        Engine* target = nullptr;

        std::vector<std::unique_ptr<juce::MidiInput>> open_;
        std::vector<std::string> refusals;

        mutable std::mutex triggerMutex;
        std::shared_ptr<const cue::TriggerIndex> triggers;

        /*  Which declared port each opened device belongs to, by the device's
            own name - the only thing the callback is handed. Written while
            opening, read on the input thread, under the mutex beside it rather
            than one of its own: both are read on the same line of the same
            callback and a second lock would buy nothing. */
        std::map<std::string, std::string> portOfDevice;
    };
}
