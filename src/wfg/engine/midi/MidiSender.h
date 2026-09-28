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

/*  THE THREAD THAT PUTS A MIDI CUE ON THE WIRE.

    WHY A THREAD AT ALL, and it is one measured fact rather than caution. On
    Windows `juce::MidiOutput::sendMessageNow` for a system-exclusive message
    BUSY-WAITS the calling thread until the port has taken every byte
    (juce_Midi_windows.cpp) - about thirty milliseconds for a hundred bytes at
    MIDI baud, which is a tick and a half of the thread that owns the model and
    also publishes the tree. §4.1 says GO never blocks; a hundred-byte dump on
    the GO path is exactly how it would.

    So the tick thread enqueues at its flush and this thread sends, which is the
    shape `MountProbe` already has for the same reason.

    THE PORT IS THE SHOW'S NAME AND THE DEVICE IS THE BUILDING'S. `bind` takes
    the two and joins them; a cue naming a port nothing was bound to fails its
    RUN with `no-port` and never the load, because a show travels to a rig that
    has not been patched yet and still has to open (§4.10).

    THIS FILE AND MidiInputs.h ARE WHERE JUCE'S MIDI HEADERS LIVE. The cue layer
    sees `MidiSink`, which names no vendor type, so a MIDI cue is tested with a
    recording fake on every platform - including the two where JUCE cannot make
    a virtual port, and every CI runner, which have no ports at all.
*/

#pragma once

#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/midi/PortTable.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::midi
{
    class MidiSender final : public MidiSink
    {
    public:
        MidiSender() = default;
        ~MidiSender() override;

        /*  Binds one of the show's declared ports to one of this machine's
            devices. False, with a line in `problems()`, when there is no such
            device.

            THE PORT IS ITS IDENTIFIER, because that is what a cue carries -
            `--midi-out` is given the NAME, which is what a person reads, and
            whoever parses the command line resolves the one to the other
            against the document. `label` is that name, kept only so that a
            refusal is a sentence somebody can act on.

            BOTH HALVES OF THE DEVICE, and the identifier is tried FIRST
            (author, 2026-09-22, asked as "what is best if switching USB
            ports?"). An identifier is the only thing that tells two identical
            interfaces apart and the first thing to break when a cable moves
            to another socket; a name is the other way round. `wantedId` may be
            empty, which is an ordinary first run. What was actually matched
            comes back in `matchedId` so the caller can write it down for next
            time, and `why` carries the sentence when nothing was. */
        bool bind (const std::string& portId, const std::string& label,
                   const std::string& deviceName, const std::string& wantedId,
                   std::string& matchedId, std::string& why);

        /** Closes a port's device and forgets it. Safe while the sending thread runs. */
        void unbind (const std::string& portId);

        /** This machine's outputs, name and identifier, for a menu to offer. */
        static std::vector<Device> availableDevices();

        /** Starts the sending thread. Nothing leaves before this. */
        void start();

        /** Stops it, after whatever is queued has gone. */
        void stop();

        /*  Queues one message. Tick thread; takes a mutex for a push_back and
            never waits on a port. */
        std::string send (const std::string& port, const Bytes& bytes) override;

        const std::vector<std::string>& problems() const noexcept { return refusals; }

        /** How many messages have actually left, for anybody watching. */
        std::size_t sent() const noexcept { return delivered.load (std::memory_order_relaxed); }

        /** Whether a port name has a device behind it. */
        bool isBound (const std::string& portName) const;

    private:
        void run();

        /*  SHARED, SO A PORT CAN BE REBOUND WHILE THE SHOW RUNS (2026-09-25):
            the sending thread takes its own reference under `boundMutex` and
            sends outside it, so a device closed by a rebind in the middle of a
            thirty-millisecond dump is closed by whichever of the two lets go
            last - and never while the lock is held, which the tick thread's
            `isBound` also takes. */
        struct Bound
        {
            std::string port;
            std::shared_ptr<juce::MidiOutput> device;
        };

        std::shared_ptr<juce::MidiOutput> deviceFor (const std::string& portId) const;

        struct Queued
        {
            std::string port;
            Bytes bytes;
        };

        mutable std::mutex boundMutex;
        std::vector<Bound> bound;
        std::vector<std::string> refusals;

        mutable std::mutex queueMutex;
        std::condition_variable wakeUp;
        std::deque<Queued> queue;

        std::atomic<bool> running { false };
        std::atomic<std::size_t> delivered { 0 };
        std::thread worker;
    };
}
