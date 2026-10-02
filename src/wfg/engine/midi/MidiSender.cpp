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

#include <wfg/engine/monitor/TrafficTap.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wfg::midi
{
    namespace
    {
        /*  THE PRODUCT'S OUTPUT: a JUCE device, opened by `bind`. */
        class JuceOutput final : public Output
        {
        public:
            explicit JuceOutput (std::unique_ptr<juce::MidiOutput> deviceToUse)
                : device (std::move (deviceToUse)) {}

            void sendNow (const Bytes& bytes) override
            {
                device->sendMessageNow (juce::MidiMessage { bytes.data(), static_cast<int> (bytes.size()) });
            }

            std::string name() const override { return device->getName().toStdString(); }

        private:
            std::unique_ptr<juce::MidiOutput> device;
        };
    }

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
        //  Closed here, after the lock is let go - or by the sender, if it is mid-send.
        std::shared_ptr<Output> closing;

        {
            const std::lock_guard<std::mutex> lock { boundMutex };

            for (auto at = bound.begin(); at != bound.end(); ++at)
                if (at->port == portId)
                {
                    closing = std::move (at->device);
                    bound.erase (at);
                    break;
                }
        }

        /*  AND ITS NOTES ARE FORGOTTEN (2026-10-02, namespace draft §23.10): the
            device that heard them is closed, and a later note-off for them
            would reach whatever is put behind the name next - a synth Go.dot
            never played. After the bound lock, never inside it. */
        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            outbox.forgetPort (portId);
        }
    }

    std::shared_ptr<Output> MidiSender::deviceFor (const std::string& portId) const
    {
        const std::lock_guard<std::mutex> lock { boundMutex };

        for (const auto& entry : bound)
            if (entry.port == portId)
                return entry.device;

        return {};
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
        attach (portId, std::make_shared<JuceOutput> (std::move (device)));
        return true;
    }

    void MidiSender::attach (const std::string& portId, std::shared_ptr<Output> output)
    {
        std::shared_ptr<Output> replaced;

        /*  A NAME PUT ON ANOTHER DEVICE FORGETS WHAT THE OLD ONE WAS PLAYING,
            as an unbind does: the live rebind releases a port and binds it
            again, and the device now behind it never heard those notes.

            THE SWAP AND THE FORGETTING UNDER BOTH LOCKS, in the order the
            header gives - `queueMutex`, then `boundMutex` (the review of H4,
            2026-10-02). With the queue let go between them, the sending thread
            could find the new device, send a note-on to it and record it, and
            the forgetting would then erase a note the new device is playing.
            The old device is closed after both are let go, as `replaced` goes. */
        {
            const std::lock_guard<std::mutex> queueLock { queueMutex };
            const std::lock_guard<std::mutex> lock { boundMutex };

            const auto existing = std::find_if (bound.begin(), bound.end(),
                                                [&portId] (const Bound& b)
                                                { return b.port == portId; });

            if (existing != bound.end())
            {
                replaced = std::move (existing->device);
                existing->device = std::move (output);
            }
            else
            {
                bound.push_back ({ portId, std::move (output) });
            }

            if (replaced != nullptr)
                outbox.forgetPort (portId);
        }
    }

    bool MidiSender::isBound (const std::string& portId) const
    {
        const std::lock_guard<std::mutex> lock { boundMutex };

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
        /*  CLEARED UNDER THE QUEUE LOCK (2026-10-02, the review of K4). The
            sending thread reads `running` under that lock and then waits on
            it; cleared without the lock, the store and the notify could both
            land between its read and its wait - the wake-up lost, and the join
            below waiting for ever on a thread asleep over an empty queue. Under
            the lock the store is either before the read, which sees it, or
            after the wait has begun, which the notify reaches. */
        {
            const std::lock_guard<std::mutex> lock { queueMutex };

            if (! running.exchange (false))
                return;
        }

        wakeUp.notify_all();

        if (worker.joinable())
            worker.join();
    }

    std::string MidiSender::send (const std::string& port, const Bytes& bytes)
    {
        return enqueue ({ port, bytes, {}, false });
    }

    std::string MidiSender::sendForRun (const std::string& runId, const std::string& port,
                                        const Bytes& bytes)
    {
        return enqueue ({ port, bytes, runId, true });
    }

    std::string MidiSender::enqueue (Outgoing message)
    {
        if (message.bytes.empty())
            return sendError::badMessage;

        /*  ASKED BEFORE IT IS QUEUED, so that a cue naming a port nobody bound
            fails on the tick it fired rather than silently going into a queue
            that will drop it. The run wants to say `no-port` while the operator
            is still looking at the cue that did it. */
        if (! isBound (message.port))
            return sendError::noPort;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            outbox.push (std::move (message));
        }

        wakeUp.notify_one();
        return {};
    }

    std::size_t MidiSender::dropQueued()
    {
        std::size_t dropped = 0;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            dropped = outbox.dropQueued();
        }

        /*  The note-offs are new work for the sending thread, whatever was
            dropped to make room for them. */
        wakeUp.notify_one();
        return dropped;
    }

    void MidiSender::run()
    {
        while (running.load (std::memory_order_relaxed))
        {
            Outgoing next;
            std::shared_ptr<Output> device;

            {
                std::unique_lock<std::mutex> lock { queueMutex };

                wakeUp.wait (lock, [this]
                {
                    return ! outbox.empty() || ! running.load (std::memory_order_relaxed);
                });

                if (! outbox.pop (next))
                    continue;

                /*  THE DEVICE LOOKED UP BEFORE THE QUEUE IS LET GO (2026-10-02,
                    namespace draft §23.10), and the note record told while it
                    is still held: a double Esc on the tick thread then finds a
                    note-on either still in the queue - dropped, never heard - or
                    in the record, its note-off queued behind the send below.
                    Looked up after the lock, a note-on taken in between was
                    neither, and its note would have rung on. A message with no
                    device is not going anywhere, so the record never hears of
                    it. `queueMutex` then `boundMutex`, the order the header
                    gives. */
                device = deviceFor (next.port);

                if (device != nullptr)
                    outbox.markLeaving (next);
            }

            if (device == nullptr)
                continue;

            /*  THE BLOCKING CALL, on the thread this class exists to give it.
                A hundred-byte dump holds this for about thirty milliseconds on
                Windows and nothing above it notices - outside `boundMutex`,
                which the tick thread's `isBound` also takes, and outside the
                queue lock, which its `send` takes. */
            deliver (*device, next.bytes);
        }

        /*  WHAT A CUE STILL HAD QUEUED GOES, because a show that is closing
            has usually just sent the blackout. Bounded by what is in hand
            rather than by the queue, so a producer that never stopped cannot
            hold the shutdown open. A SURFACE'S traffic still queued is dropped
            (2026-10-02, the review of K4): the surface closes with the show,
            and a display's SysEx could hold the thread ahead of the note-offs.

            AND THEN A NOTE-OFF FOR EVERY NOTE A CUE LEFT DOWN (2026-10-02, K4,
            namespace draft §23.15): one `0x8n key 0` a port, channel and key
            the record holds - the double Esc's rule, nothing to a synth Go.dot
            never played - after the queue, so a note-on still in it is ended
            too. Taken in the same hand under the queue lock, the devices looked
            up inside it in the header's order (`queueMutex`, then
            `boundMutex`).

            A BUDGET A PORT, SO A PORT THAT DOES NOT TAKE MESSAGES CANNOT HOLD
            THE QUIT FOR LONG (the review of K4 revised the first cap, KR): one
            pass over what is in hand, nothing retried here, and a port whose
            sends have held the thread `budget` in all is sent nothing more in
            this pass. A device that has gone answers its send with an error at
            once; a driver that says it is not ready is retried by JUCE itself,
            fifty times a millisecond's sleep apart on Windows
            (juce_Midi_windows.cpp) - most of a second for one message at the
            default timer - so a dead port costs its budget and at most one
            message more. A port that is only slow gets every note-off its
            budget covers, which a single-message cap had taken for dead. What
            no budget can bound is one call that never returns: a cue's SysEx
            JUCE waits on for good. */
        std::deque<Outgoing> remaining;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            remaining = outbox.takeAllForClose ([this] (const std::string& portId)
                                                { return deviceFor (portId) != nullptr; });
        }

        constexpr auto budget = std::chrono::milliseconds (250);
        std::map<std::string, std::chrono::steady_clock::duration> spent;

        for (const auto& item : remaining)
        {
            auto& used = spent[item.port];

            if (used >= budget)
                continue;

            const auto device = deviceFor (item.port);

            if (device == nullptr)
                continue;

            const auto began = std::chrono::steady_clock::now();
            deliver (*device, item.bytes);
            used += std::chrono::steady_clock::now() - began;
        }
    }

    void MidiSender::deliver (Output& device, const Bytes& bytes)
    {
        device.sendNow (bytes);
        delivered.fetch_add (1, std::memory_order_relaxed);

        if (auto* watching = tap.load (std::memory_order_acquire); watching != nullptr && watching->isListening())
            watching->record (monitor::Direction::out, monitor::Medium::midi, monitor::Road::midi,
                              device.name(), bytes.data(), bytes.size());
    }
}
