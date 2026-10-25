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

#pragma once

/*
    THE MTC LANE: MIDI TIME CODE SENT AT ITS MOMENT (namespace draft §59, TC.5;
    AHO, AHP, AHQ, AHR).

    WHY A THREAD OF ITS OWN. MTC is a full frame at a start or a jump and then
    quarter frames, four a frame - every 8 to 10 ms. The tick is 20 ms, so it
    cannot pace them, and the MIDI sender's one queue sends whatever it pops
    as soon as it pops it, at an ordinary priority, behind any SysEx a cue
    queued first - on Windows a hundred-byte SysEx holds that thread for about
    thirty milliseconds. So MTC has a lane beside the queue: one thread, raised
    as the tick thread is, that sleeps until each message's moment and sends it
    then.

    INSIDE THE MIDI SENDER, NOT BESIDE IT, because the sender owns the devices
    and Windows lets one owner have a port: the lane sends on the device
    objects the sender already holds, looked up per message (so a port rebound
    while the code runs is followed), through `Wire` - the three things it
    needs of its owner and nothing more.

    A SAMPLE BECOMES A MOMENT through the clock line (clock/ClockEstimate.h).
    Each message is due on a SAMPLE - MtcSchedule.h's one rule, so a quarter
    frame and the LTC frame it starts can never disagree - and the tick hands
    the lane, once a tick, where Go.dot's samples were and when (`feedClock`).
    The lane keeps its OWN estimate from those pairs, as the video renderer
    keeps its own: one reader to a line, never the tick thread's. Then the
    moment is held back by the interface's output latency, so the code leaves
    when its sample is HEARD, not when it is computed (AHP).

    ONE PROGRAM AT A TIME (AHA: one MTC output a show). A start REPLACES the
    program sending from its zero sample on - a takeover or a jump (AHL) - and
    what the old one had due before that sample still leaves; a new program
    always opens with a full frame. A stop ends it at a frame boundary (AHK),
    a cut at once.

    THE LAST DOOR, per message, in this order: the machine HOLDS and the port
    is not a surface's - the message is counted as sent and nothing leaves
    (§58, AGG), and when the hold lets go the code is re-seated at the next
    cycle with a full frame, which is what a backup taking over needs (RD.7);
    the lane is MUTED (tx off, AHR) - nothing leaves, and unmuting re-seats it
    the same way; the port has NO DEVICE - dropped, counted, and the code runs
    on; and only then is it sent, under the PORT'S GATE (AHQ), the lock the
    sender's queue also takes around every send, so a quarter frame - F1, a
    System Common message - is never sent inside a SysEx, which it would end.

    LATE, AND HOW LATE (AHQ). A message more than a quarter frame late is not
    sent: the code is re-seated at the next cycle without a full frame (a
    receiver given pieces from a broken cycle puts together a label that never
    was), or with one when the full frame itself was late or the lane stalled
    for more than a quarter of a second, which a receiver takes as lost lock.
    What leaves more than 2 ms after its moment is counted (`sync/mtcLate`),
    and the latest of anything is kept (`sync/mtcWorst`). Nothing held or muted
    is ever counted late: it never left.

    THE MONITOR sees the full frames and not a hundred quarter frames a second
    (AHQ).

    THE TICK THREAD IS NEVER MADE TO WAIT. What it calls - start, stop, mute,
    the clock's pairs, the latency - pushes into a small inbox under a mutex
    held for a push_back, stores a pair under another held for a copy, or
    stores an atomic, and notifies. Everything else is the lane thread's alone:
    the programs, the cursors, the estimate.

    TESTABLE WITHOUT TIME: `pump (now)` is the whole of what the thread does
    once it has woken, so a test hands it a moment of a clock of its own and
    looks at what reached a fake port. A lane given a test's clock never starts
    its thread: the test IS its thread.
*/

#include <wfg/engine/clock/ClockEstimate.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/timecode/MtcSchedule.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wfg::midi
{
    class Output;

    class MtcLane
    {
    public:
        /*  THE CLOCK SEAM: the machine's steady clock in nanoseconds, and the
            short last wait before a moment. The product's is the steady clock
            itself; a test's is a number it moves by hand. */
        class Clock
        {
        public:
            virtual ~Clock() = default;

            virtual std::int64_t now() const noexcept = 0;

            /*  Returns at or soon after `nanos`: the last stretch before a
                message, after the long sleep has stopped about a millisecond
                and a half short of it. */
            virtual void sleepUntil (std::int64_t nanos) const noexcept = 0;
        };

        /** The steady clock, shared: what every lane uses unless told otherwise. */
        static std::shared_ptr<const Clock> steadyClock();

        /*  WHAT THE LANE NEEDS OF THE SENDER THAT OWNS IT. Each called on the
            lane's thread, outside every lock the lane holds. */
        class Wire
        {
        public:
            /*  The device behind a port and the gate every send to it is made
                under; both null when nothing is bound. */
            struct Door
            {
                std::shared_ptr<Output> device;
                std::shared_ptr<std::mutex> gate;
            };

            virtual Door doorOf (const std::string& portId) const = 0;

            /*  Whether the machine holds this port's traffic back: held, and
                the port not a control surface's (AGH). */
            virtual bool holdsBack (const std::string& portId) const = 0;

            /*  A message has left on `device`, outside its gate: counted, and
                shown to the network monitor when `shown`. */
            virtual void noteLeft (Output& device, const Bytes& bytes, bool shown) = 0;

        protected:
            ~Wire() = default;
        };

        explicit MtcLane (Wire& wireToUse);
        ~MtcLane();

        MtcLane (const MtcLane&) = delete;
        MtcLane& operator= (const MtcLane&) = delete;

        /*  A clock of a test's own, before the thread would start: the lane's
            thread is then never started, and the test calls `pump` itself. */
        void useClock (std::shared_ptr<const Clock> clockToUse);

        /*  Starts the thread (MidiSender::start) and stops it (MidiSender::stop,
            before the queue's closing flush). Both safe to call twice, and
            the stop when never started. */
        void beginThread();
        void endThread();

        //==============================================================================
        /*  TICK THREAD. Each pushes one command into the inbox and wakes the
            lane; what it does is the lane's, the next time it looks. */
        void start (const std::string& runId, const std::string& port, std::uint64_t program,
                    timecode::Rate rate, int sampleRate, std::int64_t startFrame, std::int64_t zeroSample);

        void stop (std::uint64_t program, std::int64_t atSample, bool cut);

        void mute (bool on);

        /*  A pair of the clock line: where Go.dot's samples were (`sample`, at
            `sampleRate`) when the steady clock read `nanos`. Stored for the
            lane's next look; nothing waits. `heldAt` is the sample the show
            will stand at once held (PRD §3.34), -1 while it is not: nothing
            due at or after it leaves, and the code goes on after Play with a
            full frame. */
        void feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate, std::int64_t heldAt = -1);

        //  The interface's output latency, in samples (AHP).
        void setOutputLatency (int samples) noexcept { latencySamples.store (std::max (0, samples), std::memory_order_relaxed); }

        //==============================================================================
        /*  THE LANE'S THREAD, or a test standing for it: every message due by
            `nowNanos` - a moment of the lane's clock - handled in order, each
            through the last door. */
        void pump (std::int64_t nowNanos);

        /*  The moment the next message is due, latency included; nothing when
            no program is waiting, or the clock line has had no pair yet. The
            lane's thread, or a test standing for it. */
        std::optional<std::int64_t> nextMoment();

        //==============================================================================
        //  The readouts. Any thread.
        std::uint32_t lateCount() const noexcept      { return late.load (std::memory_order_relaxed); }
        double worstMs() const noexcept               { return static_cast<double> (worstNanos.load (std::memory_order_relaxed)) * 1.0e-6; }
        std::uint64_t currentProgram() const noexcept { return sending.load (std::memory_order_relaxed); }

        //  What actually left, and what found no device behind its port.
        std::uint64_t sentCount() const noexcept      { return sentMessages.load (std::memory_order_relaxed); }
        std::uint32_t unsentCount() const noexcept    { return noDevice.load (std::memory_order_relaxed); }

        //  How late a message may leave before it is counted, and before it is not sent at all.
        static constexpr std::int64_t countedLateNanos = 2'000'000;
        static constexpr std::int64_t stallNanos = 250'000'000;

    private:
        static constexpr std::int64_t never = std::numeric_limits<std::int64_t>::max();
        static constexpr std::size_t pendingSize = 4;

        enum class Verb { start, stop, cut, mute, unmute };

        struct Command
        {
            Verb verb = Verb::start;
            std::uint64_t program = 0;
            std::int64_t sample = 0;            // a start's zero; a stop's or a cut's sample
            std::int64_t startFrame = 0;
            timecode::Rate rate = timecode::Rate::fps25;
            int sampleRate = 0;
            std::string runId;
            std::string port;
        };

        /*  ONE PROGRAM: what it sends, from where, and how far it has got. The
            lane thread's alone. */
        struct Program
        {
            std::uint64_t id = 0;
            std::string runId;
            std::string port;
            timecode::Rate rate = timecode::Rate::fps25;
            int sampleRate = 0;
            std::int64_t zero = 0;
            timecode::MtcCursor cursor;

            //  Nothing due at or after this leaves.
            std::int64_t endSample = never;

            //  The due sample of the last message handled, whatever became of it.
            std::int64_t lastDue = std::numeric_limits<std::int64_t>::min();
            bool begun = false;

            //  Held back since the last message that left: a full frame at the next cycle, once let go.
            bool wasHeld = false;

            //  Unmuted since: the same.
            bool reseatOnNext = false;
        };

        //  Into the inbox, under its lock for a push_back, and the lane woken.
        void post (Command command);

        //  Commands from the tick thread, applied in the order they came.
        void drain();
        void apply (Command& command);

        /*  Where a stop or a cut of a program ends it: nothing due at or after
            the sample this answers leaves. */
        std::int64_t endOf (const Program& p, std::int64_t atSample, bool cut) const noexcept;

        //  A waiting start taken out of the list, the rest closing up behind it.
        void removeWaiting (std::size_t index);

        //  The first waiting start becomes the program sending: its zero has come.
        void promote();

        //  The newest pair, fed to the lane's own estimate if it has not been.
        void refreshClock();

        /*  Which program's message is next: the running one's, or a waiting
            start whose zero has come before it. Ends a running program whose
            next message is past its end. */
        enum class Next { none, active, pending };
        Next whatIsNext();

        std::int64_t latencyNanosOf (const Program& p) const noexcept;
        std::int64_t momentOf (const Program& p, std::int64_t sample) const noexcept;

        //  A message handled - sent, held, muted, dropped - and the program begun with it.
        void mark (const timecode::MtcMessage& message);

        //  The same, and the cursor moved past it.
        void pass (const timecode::MtcMessage& message);

        //  Taken off the lane: nothing more of it leaves.
        void finish();

        void counted (std::int64_t lateBy) noexcept;

        void run();

        Wire& wire;
        std::shared_ptr<const Clock> clock;
        bool testClock = false;

        //  The inbox, the thread's sleep and its stop: `inboxMutex`, a leaf.
        std::mutex inboxMutex;
        std::condition_variable wake;
        std::vector<Command> inbox;
        bool running = false;
        std::thread worker;

        /*  How many commands have been posted, so a send waiting on its gate
            can tell, without a lock, that something changed while it waited. */
        std::atomic<std::uint64_t> posted { 0 };
        std::uint64_t seenPosted = 0;

        //  The newest clock pair: `pairMutex`, a leaf held for a copy.
        struct Pair
        {
            std::int64_t sample = -1;
            std::int64_t nanos = 0;
            int sampleRate = 0;
            std::uint64_t serial = 0;
            std::int64_t heldAt = -1;
        };

        std::mutex pairMutex;
        Pair newest;
        std::uint64_t fedSerial = 0;

        std::atomic<int> latencySamples { 0 };

        //  The lane thread's alone.
        ClockEstimate estimate;

        //  Where the show stands while held (PRD §3.34), or -1: the last pair's.
        std::int64_t heldAt = -1;

        //  Whether a message due at `sample` waits for Play.
        bool heldBack (std::int64_t sample) const noexcept { return heldAt >= 0 && sample >= heldAt; }
        std::vector<Command> drained;
        Program active;
        std::array<Program, pendingSize> pending;
        std::size_t pendingCount = 0;
        bool muted = false;
        Bytes scratch;

        std::atomic<std::uint32_t> late { 0 };
        std::atomic<std::int64_t> worstNanos { 0 };
        std::atomic<std::uint64_t> sending { 0 };
        std::atomic<std::uint64_t> sentMessages { 0 };
        std::atomic<std::uint32_t> noDevice { 0 };
    };
}
