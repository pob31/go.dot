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

#include <wfg/engine/midi/MidiSender.h>

#include <algorithm>

namespace wfg::midi
{
    MidiSender::~MidiSender()
    {
        stop();
    }

    std::vector<Device> MidiSender::availableDevices()
    {
        std::vector<Device> out;

        for (const auto& info : juce::MidiOutput::getAvailableDevices())
            out.push_back ({ info.name.toStdString(), info.identifier.toStdString() });

        return out;
    }

    void MidiSender::unbind (const std::string& portId)
    {
        const std::lock_guard<std::mutex> lock { queueMutex };

        for (auto at = bound.begin(); at != bound.end(); ++at)
            if (at->port == portId)
            {
                bound.erase (at);
                return;
            }
    }

    bool MidiSender::bind (const std::string& portId, const std::string& label,
                           const std::string& deviceName, const std::string& wantedId,
                           std::string& matchedId, std::string& why)
    {
        const auto devices = availableDevices();

        /*  ONE RULE FOR BOTH SIDES OF THE CABLE, in `PortTable::match`: the
            identifier first, the name second, and a name that fits two devices
            refused rather than guessed. */
        const auto* found = PortTable::match (devices, deviceName, wantedId, why);

        if (found == nullptr)
        {
            /*  NAMED, AND THE ALTERNATIVES NAMED WITH IT. A port bound to a
                device that is not there is a cue that will fail at half past
                seven for a reason nobody can see from the show file, so the
                sentence has to be enough to fix it by. */
            std::string line = "the port \"" + label + "\": " + why;

            if (devices.empty())
            {
                line += "; this machine has no MIDI outputs";
            }
            else
            {
                line += "; this machine has";

                for (const auto& info : devices)
                    line += " \"" + info.name + "\"";
            }

            refusals.push_back (line);
            why = line;
            return false;
        }

        matchedId = found->identifier;

        auto device = juce::MidiOutput::openDevice (juce::String (found->identifier));

        if (device == nullptr)
        {
            why = "the MIDI output \"" + found->name + "\" is there and would not open";
            refusals.push_back ("the port \"" + label + "\": " + why);
            return false;
        }

        /*  A SECOND BINDING REPLACES THE FIRST rather than being refused. Two
            `--midi-out=Lights=...` on one command line is somebody correcting
            themselves, and the last one is what they meant. */
        const auto existing = std::find_if (bound.begin(), bound.end(),
                                            [&portId] (const Bound& b)
                                            { return b.port == portId; });

        if (existing != bound.end())
            existing->device = std::move (device);
        else
            bound.push_back ({ portId, std::move (device) });

        return true;
    }

    bool MidiSender::isBound (const std::string& portId) const
    {
        return std::any_of (bound.begin(), bound.end(),
                            [&portId] (const Bound& b)
                            { return b.port == portId && b.device != nullptr; });
    }

    void MidiSender::start()
    {
        if (running.exchange (true))
            return;

        worker = std::thread ([this] { run(); });
    }

    void MidiSender::stop()
    {
        if (! running.exchange (false))
            return;

        wakeUp.notify_all();

        if (worker.joinable())
            worker.join();
    }

    std::string MidiSender::send (const std::string& port, const Bytes& bytes)
    {
        if (bytes.empty())
            return sendError::badMessage;

        /*  ASKED BEFORE IT IS QUEUED, so that a cue naming a port nobody bound
            fails on the tick it fired rather than silently going into a queue
            that will drop it. The run wants to say `no-port` while the operator
            is still looking at the cue that did it. */
        if (! isBound (port))
            return sendError::noPort;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            queue.push_back ({ port, bytes });
        }

        wakeUp.notify_one();
        return {};
    }

    void MidiSender::run()
    {
        while (running.load (std::memory_order_relaxed))
        {
            Queued next;

            {
                std::unique_lock<std::mutex> lock { queueMutex };

                wakeUp.wait (lock, [this]
                {
                    return ! queue.empty() || ! running.load (std::memory_order_relaxed);
                });

                if (queue.empty())
                    continue;

                next = std::move (queue.front());
                queue.pop_front();
            }

            const auto device = std::find_if (bound.begin(), bound.end(),
                                              [&next] (const Bound& b)
                                              { return b.port == next.port; });

            if (device == bound.end() || device->device == nullptr)
                continue;

            /*  THE BLOCKING CALL, on the thread this class exists to give it.
                A hundred-byte dump holds this for about thirty milliseconds on
                Windows and nothing above it notices. */
            const juce::MidiMessage message { next.bytes.data(),
                                              static_cast<int> (next.bytes.size()) };

            device->device->sendMessageNow (message);
            delivered.fetch_add (1, std::memory_order_relaxed);
        }

        /*  WHATEVER IS STILL QUEUED GOES, because a show that is closing has
            usually just sent the blackout. Bounded by what is in hand rather
            than by the queue, so a producer that never stopped cannot hold the
            shutdown open. */
        std::deque<Queued> remaining;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            remaining.swap (queue);
        }

        for (const auto& item : remaining)
        {
            const auto device = std::find_if (bound.begin(), bound.end(),
                                              [&item] (const Bound& b)
                                              { return b.port == item.port; });

            if (device == bound.end() || device->device == nullptr)
                continue;

            const juce::MidiMessage message { item.bytes.data(),
                                              static_cast<int> (item.bytes.size()) };

            device->device->sendMessageNow (message);
            delivered.fetch_add (1, std::memory_order_relaxed);
        }
    }
}
