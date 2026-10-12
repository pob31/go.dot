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
#include <wfg/engine/midi/MtcLane.h>
#include <wfg/engine/midi/PortTable.h>
#include <wfg/engine/midi/SendQueue.h>

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::monitor { class TrafficTap; }
namespace wfg::pair { class Hold; }

namespace wfg::midi
{
    /*  ONE DEVICE BEHIND A PORT, as the sending thread sees it: a JUCE output
        in the product (`bind` opens one), a fake in a test - the seam that lets
        the thread itself, its start and its stop, be driven on a machine with
        no MIDI port (2026-10-02, the review of K4). Called on the sending
        thread only, outside every lock. */
    class Output
    {
    public:
        virtual ~Output() = default;

        /*  Sends now, and may block: a SysEx holds the thread until the port
            has taken it, and a driver that is not ready is retried. */
        virtual void sendNow (const Bytes& bytes) = 0;

        /*  THE SAME, FROM THE MTC LANE (namespace draft §59, TC.5), which
            knows the moment the message was due - on the steady clock, in
            nanoseconds. A device has no use for it and sends now; a render
            writes it down beside when the message actually left (AHV). Its own
            name and not an overload of `sendNow`, which the strict build would
            take for a hidden virtual in every fake (-Woverloaded-virtual). */
        virtual void sendWithDue (const Bytes& bytes, std::int64_t dueNanos)
        {
            (void) dueNanos;
            sendNow (bytes);
        }

        /*  The sender has stopped, its last message gone: what an output
            keeps buffered is written out. Nothing for a device. */
        virtual void finish() {}

        /** The device's name, for the network monitor. */
        virtual std::string name() const = 0;
    };

    /*  WHAT A PORT WOULD HAVE SENT, WRITTEN TO A FILE (namespace draft §59,
        AHV): `wfg serve --midi-render=<port>=<file>` puts one of these behind
        a port, for a driver on a machine with no MIDI port at all - a CI
        runner - to read what MTC did and when.

        ONE LINE A MESSAGE, `sent_ns due_ns hex bytes`: when it left on the
        steady clock, when it was due (nought for the queue's messages, which
        are sent as soon as they are taken), and its bytes, two hex digits
        each, a space between. Handed to the system line by line, so a serve
        stopped by TerminateProcess - the black-box harness's way - still
        leaves every line; `finish` at the sender's stop and the output's end
        flushes once more. Each send is made under the port's gate, so two
        threads never write at once. */
    class RenderOutput final : public Output
    {
    public:
        explicit RenderOutput (const std::string& path);
        ~RenderOutput() override;

        RenderOutput (const RenderOutput&) = delete;
        RenderOutput& operator= (const RenderOutput&) = delete;

        /** Whether the file could be opened for writing. */
        bool isOpen() const noexcept { return stream != nullptr; }

        void sendNow (const Bytes& bytes) override;
        void sendWithDue (const Bytes& bytes, std::int64_t dueNanos) override;
        void finish() override;
        std::string name() const override { return label; }

    private:
        void write (const Bytes& bytes, std::int64_t dueNanos);

        std::string label;
        std::unique_ptr<juce::FileOutputStream> stream;
    };

    class MidiSender final : public MidiSink, private MtcLane::Wire
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

        /*  Puts an output behind a port's name - what `bind` does with the
            device it opened, and what a test does with a fake. A port that had
            another output forgets the notes the old one was playing. Safe
            while the sending thread runs. */
        void attach (const std::string& portId, std::shared_ptr<Output> output);

        /** Closes a port's device and forgets it. Safe while the sending thread runs. */
        void unbind (const std::string& portId);

        /** This machine's outputs, name and identifier, for a menu to offer. */
        static std::vector<Device> availableDevices();

        /** Starts the sending thread. Nothing leaves before this. */
        void start();

        /*  Stops it, after what the cues still had queued has gone - a
            surface's traffic is dropped - and then ends every note a cue left
            sounding, one note-off a key, as a double Esc does (2026-10-02, K4,
            namespace draft §23.15; §23.10 had named it a gap). Each port is
            given a budget of time for that last delivery. `serve` calls it as
            soon as the tick thread has stopped, and the destructor again,
            which then does nothing. Safe to call when never started. */
        void stop();

        /*  Queues one message. Tick thread; takes a mutex for a push_back and
            never waits on a port. */
        std::string send (const std::string& port, const Bytes& bytes) override;

        /*  The same for a cue's message, marked as the show's and carrying its
            run: the kind a double Esc drops, and the only kind whose notes are
            recorded once they leave. */
        std::string sendForRun (const std::string& runId, const std::string& port,
                                const Bytes& bytes) override;

        /*  DOUBLE ESC: the queue's cue messages dropped and the note-offs put
            at its front (`SendQueue::dropQueued`), under the queue lock and
            nothing else - a push_back's worth of waiting, never a port's.
            The MTC lane is not touched: the Runner cuts its code (TC.6). */
        std::size_t dropQueued() override;

        //==============================================================================
        /*  MIDI TIME CODE (namespace draft §59, TC.5), sent by the lane this
            sender owns (MtcLane.h) on the devices it owns. `startTimecode`
            answers `no-port` when the port has no device and the machine does
            not hold it back - held answers as taken (§58, AGG) and runs the
            program held. The rest go to the lane as they are. Tick thread;
            nothing here waits on a port. */
        std::string startTimecode (const std::string& runId, const std::string& port, std::uint64_t program,
                                   timecode::Rate rate, int sampleRate, std::int64_t startFrame,
                                   std::int64_t zeroSample) override;
        void stopTimecode (std::uint64_t program, std::int64_t atSample, bool cut) override;
        void muteTimecode (bool on) override;
        void feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate) override;
        void setTimecodeOutputLatency (int samples) override;
        std::uint32_t timecodeLate() const override;
        double timecodeWorstMs() const override;
        std::uint64_t timecodeProgram() const override;

        /*  A CLOCK OF A TEST'S OWN for the lane, before `start`: its thread is
            then never started, and the test pumps it by hand through
            `timecodeLane`. */
        void setTimecodeClock (std::shared_ptr<const MtcLane::Clock> clockToUse) { lane.useClock (std::move (clockToUse)); }
        MtcLane& timecodeLane() noexcept { return lane; }

        const std::vector<std::string>& problems() const noexcept { return refusals; }

        /** How many messages have actually left, for anybody watching. */
        std::size_t sent() const noexcept { return delivered.load (std::memory_order_relaxed); }

        /** Whether a port name has a device behind it. */
        bool isBound (const std::string& portName) const;

        /*  THE NETWORK MONITOR'S TAP: every message as it actually leaves, on
            the sending thread, named by the device it went to. Null records
            nothing. */
        void setTap (monitor::TrafficTap* tapToUse) noexcept { tap.store (tapToUse, std::memory_order_release); }

        /*  THE MACHINE'S HOLD (namespace draft §58, RD.1; AGG): while it holds,
            `send` and `sendForRun` answer as taken and queue nothing - checked
            after a message that cannot be built is refused, which is the
            cue's fault on every machine, and BEFORE the port is asked whether
            it is bound, which is this machine's alone. So a MIDI cue, a
            patch's [noteout], a device on the MIDI wire and Doh!'s rollback all
            end on a backup with nothing plugged in as they end on the main. A
            held message never reaches the queue, so the note record - fed only
            by what leaves - never hears of it, and no note-off follows it at a
            double Esc or the close. Set before the tick thread starts; null is
            never held. */
        void setHold (const pair::Hold* holdToUse) noexcept { hold = holdToUse; }

        /*  THE PORTS A CONTROL SURFACE IS ON, whose traffic the hold lets by
            (AGH): a surface is a client, not a device, and the one plugged into
            this machine is driven by this machine - its LEDs, motor faders and
            displays answer what this machine's show is doing, whichever machine
            sends to the devices. Every byte to such a port passes, a cue's
            included. Handed in whenever the show's surfaces are declared again,
            on the tick thread; read under `boundMutex`. */
        void setSurfacePorts (std::vector<std::string> ports);

    private:
        void run();

        /*  `send` and `sendForRun`'s one road: the same refusals, then a
            push_back under the queue lock. */
        std::string enqueue (Outgoing message);

        /*  What `run` sends: under the port's gate, then counted and shown to
            the monitor on its way - outside the gate, which is held for the
            send and nothing else. */
        void deliver (const Door& door, const Bytes& bytes);
        std::atomic<monitor::TrafficTap*> tap { nullptr };

        //  What the MTC lane asks of its owner (MtcLane::Wire).
        Door doorOf (const std::string& portId) const override;
        bool holdsBack (const std::string& portId) const override;
        void noteLeft (Output& device, const Bytes& bytes, bool shown) override;

        /*  SHARED, SO A PORT CAN BE REBOUND WHILE THE SHOW RUNS (2026-09-25):
            the sending thread takes its own reference under `boundMutex` and
            sends outside it, so a device closed by a rebind in the middle of a
            thirty-millisecond dump is closed by whichever of the two lets go
            last - and never while the lock is held, which the tick thread's
            `isBound` also takes.

            AND ITS GATE (namespace draft §59, AHQ), made with the binding and
            kept when another device is put behind the same name: every send to
            the port is made under it, the queue's and the MTC lane's alike, so
            a quarter frame - F1, a System Common message - never lands inside
            a SysEx, which it would end. Shared like the device, so a sender
            holds it as long as it holds the device. */
        struct Bound
        {
            std::string port;
            std::shared_ptr<Output> device;
            std::shared_ptr<std::mutex> gate;
        };

        /*  Whether a message to this port goes on while the machine holds: a
            surface's port, and nothing else. */
        bool passesHold (const std::string& portId) const;

        const pair::Hold* hold = nullptr;

        mutable std::mutex boundMutex;
        std::vector<Bound> bound;
        std::vector<std::string> surfacePorts;      // under boundMutex
        std::vector<std::string> refusals;

        /*  THE ORDER THE LOCKS ARE TAKEN IN, when one is held while another
            is asked for: `queueMutex`, then `boundMutex` - the sending thread
            looks a message's device up before it lets the queue go, so the
            note record hears of a note-on only once it is sure to leave.
            `bind` takes both in that order around a device it puts behind a
            name, so a note the new device plays is never forgotten with the
            old one's. Never the other way round: `send` and `unbind` take the
            two one after the other, never one inside the other.

            A PORT'S GATE IS A LEAF (namespace draft §59, TC.5): taken with no
            other lock held - by the queue's thread after it has let both go,
            by the MTC lane after `doorOf` has let `boundMutex` go - and while
            it is held nothing is asked for but the device's send and an
            atomic. So neither the tick thread, which never takes a gate, nor
            the queue or the lane can wait on each other in a circle. The
            lane's own two locks (MtcLane.h, its inbox and its clock pair) are
            leaves too, held for a push_back or a copy and never around a gate
            or a send. */
        mutable std::mutex queueMutex;
        std::condition_variable wakeUp;
        SendQueue outbox;

        std::atomic<bool> running { false };
        std::atomic<std::size_t> delivered { 0 };
        std::thread worker;

        /*  THE MTC LANE, last: its thread calls back into everything above,
            and is stopped in `stop` - the destructor's first - before any of
            it goes. */
        MtcLane lane { *this };
    };
}
