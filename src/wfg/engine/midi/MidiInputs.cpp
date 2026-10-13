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

#include <wfg/engine/monitor/TrafficTap.h>

#include <wfg/engine/Engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace wfg::midi
{
    std::int64_t steadyNanosNow() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::int64_t arrivalStamp (std::int64_t steadyNanos, double counterNowMs, double messageStampSeconds) noexcept
    {
        /*  HOW LONG JUCE HELD THE MESSAGE: its counter now less the message's
            stamp, which JUCE puts on that same counter's scale. Kept between
            nought - a stamp ahead of now, or one that is not a time at all
            (a CoreMIDI packet stamped nought reads as hours ahead) - and
            100 ms, past which a stamp is not a delay to believe but a driver
            clock that wandered: an arrival is never moved back further than a
            MIDI path plausibly holds a message. */
        constexpr double mostHeldMs = 100.0;

        const auto heldMs = std::clamp (counterNowMs - messageStampSeconds * 1000.0, 0.0, mostHeldMs);

        return steadyNanos - static_cast<std::int64_t> (std::llround (heldMs * 1.0e6));
    }

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

    std::vector<Device> MidiInputs::availableDevices()
    {
        std::vector<Device> out;

        for (const auto& info : juce::MidiInput::getAvailableDevices())
            out.push_back ({ info.name.toStdString(), info.identifier.toStdString() });

        return out;
    }

    bool MidiInputs::openAs (const std::string& deviceName, const std::string& wantedId,
                             const std::string& portId, std::string& matchedId, std::string& why)
    {
        const auto devices = availableDevices();
        const auto* found = PortTable::match (devices, deviceName, wantedId, why);

        if (found == nullptr)
            return false;

        matchedId = found->identifier;

        auto input = juce::MidiInput::openDevice (juce::String (found->identifier), this);

        if (input == nullptr)
        {
            why = "the MIDI input \"" + found->name + "\" is there and would not open";
            return false;
        }

        {
            const std::lock_guard<std::mutex> lock { triggerMutex };
            portOfDevice[found->name] = portId;
        }

        input->start();
        open_.push_back (std::move (input));
        return true;
    }

    bool MidiInputs::open (const std::string& name, const std::string& portId)
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

            /*  REMEMBERED BY THE DEVICE'S OWN NAME, which is what the callback
                below is handed and the only thing it can look this up by:
                `juce::MidiInput::getName()` is the device, and the port is the
                show's word for it. */
            if (! portId.empty())
                portOfDevice[name] = portId;

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

    void MidiInputs::setConsumer (Consumer consumerToUse)
    {
        auto next = consumerToUse ? std::make_shared<const Consumer> (std::move (consumerToUse))
                                  : std::shared_ptr<const Consumer> {};

        const std::lock_guard<std::mutex> lock { triggerMutex };
        consumer = std::move (next);
    }

    void MidiInputs::inject (const std::string& portId, const Bytes& message)
    {
        route (portId, message, steadyNanosNow());
    }

    void MidiInputs::inject (const std::string& portId, const Bytes& message, std::int64_t arrivalNanos)
    {
        route (portId, message, arrivalNanos);
    }

    void MidiInputs::listenForMtcOn (const std::string& portId)
    {
        //  Copied before the lock and swapped under it: the input thread waits for a swap, not an allocation.
        auto next = portId;

        const std::lock_guard<std::mutex> lock { triggerMutex };
        std::swap (mtcPort, next);
    }

    void MidiInputs::closeAll()
    {
        for (auto& input : open_)
            if (input != nullptr)
                input->stop();

        open_.clear();
    }

    //==========================================================================
    void MidiInputs::close (const std::string& portId)
    {
        if (portId.empty())
            return;

        std::vector<std::string> devices;

        {
            const std::lock_guard<std::mutex> lock { triggerMutex };

            for (auto at = portOfDevice.begin(); at != portOfDevice.end();)
            {
                if (at->second == portId)
                {
                    devices.push_back (at->first);
                    at = portOfDevice.erase (at);
                }
                else
                {
                    ++at;
                }
            }
        }

        /*  STOPPED BEFORE IT IS LET GO, as `closeAll` does: a message already
            in flight is finished by the callback, which stamps it with the
            device's own name now that the port has let go of it. */
        for (auto at = open_.begin(); at != open_.end();)
        {
            const auto name = *at != nullptr ? (*at)->getName().toStdString() : std::string {};

            if (std::find (devices.begin(), devices.end(), name) != devices.end())
            {
                (*at)->stop();
                at = open_.erase (at);
            }
            else
            {
                ++at;
            }
        }
    }

    void MidiInputs::handleIncomingMidiMessage (juce::MidiInput* source,
                                                const juce::MidiMessage& message)
    {
        /*  THE ARRIVAL, STAMPED FIRST (namespace draft §60, TG.4): the steady
            clock now, less how long JUCE held the message before this call.
            MIDI Time Code is read from when each piece came as much as from
            what it says, so the moment is taken before anything else here
            spends any time.

            A DURATION ON ONE COUNTER, which is why no common base is
            needed. JUCE stamps a message with
            `getMillisecondCounterHiRes` (or `getMillisecondCounter`, its
            whole-millisecond reading) as it stood when the driver connection
            opened, plus the time the driver itself counted since: on Windows
            the WinMM path adds the driver's milliseconds since `midiInStart`
            to the hi-res counter read when the device was made, and the
            Windows MIDI Services path its own clock's ticks to the millisecond
            counter (juce_Midi_windows.cpp, `convertTimeStamp` and the
            `MessageReceived` handler); CoreMIDI the host time since a start
            read beside the millisecond counter (juce_CoreMidi_mac.mm,
            `TimeConverter`); ALSA the queue's real time since its start, or
            the millisecond counter itself for a direct event
            (juce_Midi_linux.cpp). So the stamp is a reading of JUCE's
            counter plus a duration, and the counter now less the stamp is a
            duration too - the time since the driver took the message - with
            the counter's origin cancelling out; it is subtracted from a
            steady reading, whose own origin is never compared with JUCE's.
            What does not cancel is constant for a device's life: the start
            read in whole milliseconds, the moments between WinMM's device
            being made and started, and on Windows MIDI Services the gap
            between the interrupt-time millisecond counter and the
            performance counter the hi-res one reads - a millisecond or two of
            latency the line takes up as an offset. A stamp further out than
            a MIDI path ever holds a message - a driver clock that wandered, a
            stamp that is no time at all - is not believed (`arrivalStamp`
            keeps the correction within nought and 100 ms). */
        const auto arrival = arrivalStamp (steadyNanosNow(), juce::Time::getMillisecondCounterHiRes(),
                                           message.getTimeStamp());

        /*  THE PORT THE SHOW DECLARES, not the cable it arrived on
            (2026-09-22). A trigger names "Lights"; which socket that is
            changes when somebody moves a cable, and every trigger in the show
            would otherwise have to be edited for it. Where no declared port
            claimed this device - a bare `--midi-in` - the device's own name is
            stamped, which is what a rig with one cable has always had. */
        const auto device = source != nullptr ? source->getName().toStdString() : std::string {};
        auto port = device;

        {
            const std::lock_guard<std::mutex> lock { triggerMutex };
            const auto found = portOfDevice.find (device);

            if (found != portOfDevice.end())
                port = found->second;
        }

        const auto* raw = message.getRawData();
        const auto size = message.getRawDataSize();

        //  Shown to the monitor by the device it came from, which is what a person can read.
        if (auto* watching = tap.load (std::memory_order_acquire); watching != nullptr && watching->isListening())
            watching->record (monitor::Direction::in, monitor::Medium::midi, monitor::Road::midi,
                              device, raw, size > 0 ? static_cast<std::size_t> (size) : 0u);

        route (port, size > 0 ? Bytes (raw, raw + size) : Bytes {}, arrival);
    }

    void MidiInputs::route (const std::string& port, const Bytes& bytes, std::int64_t arrivalNanos)
    {
        /*  SOMEBODY ELSE'S THREAD, so this does what every outside input does:
            reads an immutable index, matches, submits. It never touches the
            document - which belongs to the tick thread - and never reaches into
            the model. `Engine::submit` is the one crossing here, and it is the
            same one the OSC socket already makes. */
        std::shared_ptr<const Consumer> taker;
        std::shared_ptr<const cue::TriggerIndex> index;
        auto onMtcPort = false;

        {
            const std::lock_guard<std::mutex> lock { triggerMutex };
            taker = consumer;
            index = triggers;
            onMtcPort = ! mtcPort.empty() && port == mtcPort;
        }

        /*  MIDI TIME CODE ON ITS PORT IS THE TIME GROUPS' (§60, TG.4): kept
            for the tick with its arrival, ahead of the consumer, so a surface
            or a patch listening on the same port never takes it first - and
            never offered to the triggers, which have no event for it (see
            `eventFrom`). Only the inbox's mutex is taken, for one copy.

            `rx` IS NOT ASKED HERE, nor anywhere on this road. The tick asks
            it for the code (AIM, MtcInbox.h); a port's triggers have never
            asked it, though its row says "whether events arriving on this
            port's input fire triggers" - an older gap than this, left for the
            author to decide, not mended in passing. */
        if (onMtcPort && isMtcMessage (bytes.data(), bytes.size()))
        {
            inbox.push (port, bytes.data(), bytes.size(), arrivalNanos);
            return;
        }

        /*  A SURFACE'S PORT IS THE SURFACE'S, and nothing on it is a trigger. */
        if (taker != nullptr && (*taker) (port, bytes))
            return;

        if (target == nullptr || index == nullptr || bytes.empty())
            return;

        const juce::MidiMessage message (bytes.data(), static_cast<int> (bytes.size()));
        const auto event = eventFrom (message, port);

        if (event.type.empty())
            return;

        for (const auto& id : cue::matchMidi (*index, event))
            target->submit ({ "midi:" + port, "trigger.fire",
                              { osc::Value::string (id) } });
    }
}
