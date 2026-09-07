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

        /*  Binds one of the show's port names to one of this machine's devices.
            False, with a line in `problems()`, when there is no such device. */
        bool bind (const std::string& portName, const std::string& deviceName);

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

        struct Bound
        {
            std::string port;
            std::unique_ptr<juce::MidiOutput> device;
        };

        struct Queued
        {
            std::string port;
            Bytes bytes;
        };

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
