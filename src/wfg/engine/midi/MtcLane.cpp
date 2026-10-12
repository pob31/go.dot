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

#include <wfg/engine/midi/MtcLane.h>

#include <wfg/engine/clock/TickThread.h>
#include <wfg/engine/midi/MidiSender.h>
#include <wfg/engine/timecode/FrameClock.h>

#include <chrono>
#include <utility>

namespace wfg::midi
{
    namespace
    {
        /*  HOW THE THREAD WAITS FOR A MOMENT, and it is TickThread.cpp's shape
            for the same reason: a general-purpose scheduler wakes a thread when
            it gets round to it - a millisecond at best on Windows, where JUCE
            asks for that granularity - so the long sleep stops a millisecond
            and a half short, and the last stretch is short sleeps and then
            yields, the oversleep bounded by the slice rather than by the
            scheduler. A quarter frame is 8 to 10 ms apart from the next, so
            this is a sleep, some short sleeps and a few hundred microseconds of
            yielding a message - not a spinning core.

            EXCEPT ON WINDOWS, where a short sleep lasts a millisecond whatever
            it is asked for - the timer's granularity at its finest - so a
            200 µs slice taken half a millisecond out lands half a millisecond
            late (measured: a median of 1.0 ms, the worst 1.8). There the last
            1.2 ms are yields, which costs a share of one core while a code
            runs and buys a median of about a tenth of a millisecond. */
        constexpr auto wakeGuard = std::chrono::microseconds { 1500 };
        constexpr std::int64_t pollSliceNanos = 200'000;

       #if defined (_WIN32)
        constexpr std::int64_t yieldBelowNanos = 1'200'000;
       #else
        constexpr std::int64_t yieldBelowNanos = 300'000;
       #endif

        /*  The longest single sleep, so a clock line corrected by the tick's
            next pair - a drift followed, an interface changed - is read again
            before a message rather than after it. */
        constexpr auto longestSleep = std::chrono::milliseconds { 20 };

        //  A program waiting for the clock line's first pair looks again this often.
        constexpr auto clocklessLook = std::chrono::milliseconds { 5 };

        /*  At most this many messages handled in one pump: a lane that fell
            seconds behind while it held does not hold its thread for all of
            them at once, and the thread comes straight back for the rest. */
        constexpr int mostInOnePump = 1024;

        class SteadyClock final : public MtcLane::Clock
        {
        public:
            std::int64_t now() const noexcept override
            {
                return std::chrono::duration_cast<std::chrono::nanoseconds> (
                           std::chrono::steady_clock::now().time_since_epoch()).count();
            }

            void sleepUntil (std::int64_t nanos) const noexcept override
            {
                for (auto at = now(); at < nanos; at = now())
                {
                    if (nanos - at > yieldBelowNanos)
                        std::this_thread::sleep_for (std::chrono::nanoseconds (pollSliceNanos));
                    else
                        std::this_thread::yield();
                }
            }
        };

        //  A quarter frame's length on the steady clock: a quarter of `den / num` seconds.
        std::int64_t quarterNanosOf (timecode::Rate rate) noexcept
        {
            return 1'000'000'000LL * timecode::denOf (rate)
                   / (static_cast<std::int64_t> (timecode::mtcQuartersPerFrame) * timecode::numOf (rate));
        }
    }

    std::shared_ptr<const MtcLane::Clock> MtcLane::steadyClock()
    {
        static const std::shared_ptr<const Clock> shared = std::make_shared<const SteadyClock>();
        return shared;
    }

    MtcLane::MtcLane (Wire& wireToUse)
        : wire (wireToUse), clock (steadyClock())
    {
        inbox.reserve (16);
        drained.reserve (16);
        scratch.reserve (static_cast<std::size_t> (timecode::mtcFullFrameSize));
    }

    MtcLane::~MtcLane()
    {
        endThread();
    }

    void MtcLane::useClock (std::shared_ptr<const Clock> clockToUse)
    {
        testClock = clockToUse != nullptr;
        clock = testClock ? std::move (clockToUse) : steadyClock();
    }

    //==============================================================================
    void MtcLane::beginThread()
    {
        /*  A TEST'S CLOCK IS A TEST STANDING FOR THE THREAD: it pumps by hand,
            and a thread pumping beside it would race it for every program. */
        if (testClock)
            return;

        {
            const std::lock_guard<std::mutex> lock { inboxMutex };

            if (running || worker.joinable())
                return;

            running = true;
        }

        worker = std::thread ([this] { run(); });
    }

    void MtcLane::endThread()
    {
        /*  CLEARED UNDER THE INBOX LOCK, the queue's own lesson (MidiSender::stop):
            the thread reads `running` under it before it waits, so the store
            is either before that read or after the wait has begun, which the
            notify then reaches. */
        {
            const std::lock_guard<std::mutex> lock { inboxMutex };
            running = false;
        }

        wake.notify_all();

        if (worker.joinable())
            worker.join();
    }

    //==============================================================================
    void MtcLane::post (Command command)
    {
        {
            const std::lock_guard<std::mutex> lock { inboxMutex };
            inbox.push_back (std::move (command));
            posted.fetch_add (1, std::memory_order_release);
        }

        wake.notify_one();
    }

    void MtcLane::start (const std::string& runId, const std::string& port, std::uint64_t program,
                         timecode::Rate rate, int sampleRate, std::int64_t startFrame, std::int64_t zeroSample)
    {
        Command command;
        command.verb = Verb::start;
        command.program = program;
        command.sample = zeroSample;
        command.startFrame = startFrame;
        command.rate = rate;
        command.sampleRate = sampleRate;
        command.runId = runId;
        command.port = port;
        post (std::move (command));
    }

    void MtcLane::stop (std::uint64_t program, std::int64_t atSample, bool cut)
    {
        Command command;
        command.verb = cut ? Verb::cut : Verb::stop;
        command.program = program;
        command.sample = atSample;
        post (std::move (command));
    }

    void MtcLane::mute (bool on)
    {
        Command command;
        command.verb = on ? Verb::mute : Verb::unmute;
        post (std::move (command));
    }

    void MtcLane::feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate)
    {
        if (sample < 0 || sampleRate <= 0)
            return;

        const std::lock_guard<std::mutex> lock { pairMutex };
        newest = { sample, nanos, sampleRate, newest.serial + 1 };
    }

    //==============================================================================
    void MtcLane::drain()
    {
        {
            const std::lock_guard<std::mutex> lock { inboxMutex };
            drained.swap (inbox);
            seenPosted = posted.load (std::memory_order_acquire);
        }

        for (auto& command : drained)
            apply (command);

        drained.clear();
    }

    void MtcLane::apply (Command& command)
    {
        switch (command.verb)
        {
            case Verb::start:
            {
                if (command.program == 0 || command.sampleRate <= 0)
                    return;

                Program next;
                next.id = command.program;
                next.runId = std::move (command.runId);
                next.port = std::move (command.port);
                next.rate = command.rate;
                next.sampleRate = command.sampleRate;
                next.zero = command.sample;
                next.cursor = timecode::MtcCursor (command.rate, command.sampleRate, command.startFrame, command.sample);

                /*  WAITING, IN THE ORDER OF THE SAMPLES THEY START ON - after
                    any already there for the same one - as LTC's starts wait
                    (LtcGenerator.h): each takes over from its own zero. Four
                    is more than a launch horizon ever holds; a fifth would
                    push out the one that starts last. */
                if (pendingCount == pendingSize)
                    --pendingCount;

                auto index = pendingCount;

                while (index > 0 && pending[index - 1].zero > next.zero)
                {
                    pending[index] = std::move (pending[index - 1]);
                    --index;
                }

                pending[index] = std::move (next);
                ++pendingCount;
                return;
            }

            case Verb::stop:
            case Verb::cut:
            {
                const auto cut = command.verb == Verb::cut;
                const auto asSoonAsCanBe = command.sample <= 0;

                /*  A START THAT HAS NOT COME is given its end like a running
                    one - a length's stop is posted with its start - and taken
                    back when that end is its zero: an Esc pressed inside the
                    launch horizon reaches the lane while the code it ends is
                    still waiting to begin. */
                for (std::size_t n = 0; n < pendingCount; ++n)
                    if (pending[n].id == command.program)
                        pending[n].endSample = std::min (pending[n].endSample,
                                                         endOf (pending[n], command.sample, cut));

                for (std::size_t n = pendingCount; n > 0; --n)
                    if (pending[n - 1].id == command.program && pending[n - 1].endSample <= pending[n - 1].zero)
                        removeWaiting (n - 1);

                if (active.id == 0 || active.id != command.program)
                    return;

                const auto end = endOf (active, command.sample, cut);

                /*  PLACED, AND ARRIVED AFTER WHAT IT WAS PLACED FOR HAD GONE:
                    late, as LTC counts a placed stop late. One that was never
                    placed - as soon as can be - never is. */
                if (! asSoonAsCanBe && active.begun && active.lastDue >= end)
                    late.fetch_add (1, std::memory_order_relaxed);

                active.endSample = std::min (active.endSample, end);
                return;
            }

            case Verb::mute:
            {
                muted = true;
                return;
            }

            case Verb::unmute:
            {
                /*  TX BACK ON: the code picks up at the next cycle with a full
                    frame (AHO), never part way through one. */
                if (muted && active.id != 0)
                    active.reseatOnNext = true;

                muted = false;
                return;
            }
        }
    }

    std::int64_t MtcLane::endOf (const Program& p, std::int64_t atSample, bool cut) const noexcept
    {
        /*  AS SOON AS CAN BE is from the next message still to go - the zero,
            for a program that has not begun - since what was due before now
            has gone. */
        const auto from = atSample > 0 ? atSample : p.cursor.next().dueSample;

        //  A CUT ends on its sample exactly (a double Esc).
        if (cut)
            return from;

        /*  A STOP WAITS FOR THE FRAME (AHK): the first boundary at or after
            its sample, so a receiver is never left half way through one - the
            boundary FrameClock gives, the one LTC stops on. */
        const auto frame = timecode::firstFrameAtOrAfter (from - p.zero, p.sampleRate, p.rate);
        return p.zero + timecode::frameStartSample (frame, p.sampleRate, p.rate);
    }

    void MtcLane::removeWaiting (std::size_t index)
    {
        for (auto n = index + 1; n < pendingCount; ++n)
            pending[n - 1] = std::move (pending[n]);

        --pendingCount;
        pending[pendingCount] = {};
    }

    void MtcLane::promote()
    {
        active = std::move (pending[0]);
        removeWaiting (0);

        /*  A tx switched off before this program came still holds: its first
            message waits for tx back on like any other. */
    }

    void MtcLane::finish()
    {
        active = {};
        sending.store (0, std::memory_order_relaxed);
    }

    void MtcLane::refreshClock()
    {
        Pair pair;

        {
            const std::lock_guard<std::mutex> lock { pairMutex };
            pair = newest;
        }

        if (pair.serial == fedSerial)
            return;

        fedSerial = pair.serial;
        estimate.feed (pair.sample, pair.nanos, pair.sampleRate);
    }

    MtcLane::Next MtcLane::whatIsNext()
    {
        for (;;)
        {
            if (active.id != 0 && active.cursor.next().dueSample >= active.endSample)
            {
                finish();
                continue;
            }

            /*  A WAITING START TAKES OVER ON ITS ZERO: what the running one
                has due before that sample still goes, and from it the new
                one's full frame (AHL). On the same sample, the new one. */
            if (pendingCount > 0 && (active.id == 0 || pending[0].zero <= active.cursor.next().dueSample))
                return Next::pending;

            return active.id != 0 ? Next::active : Next::none;
        }
    }

    std::int64_t MtcLane::latencyNanosOf (const Program& p) const noexcept
    {
        if (p.sampleRate <= 0)
            return 0;

        return static_cast<std::int64_t> (latencySamples.load (std::memory_order_relaxed)) * 1'000'000'000LL / p.sampleRate;
    }

    std::int64_t MtcLane::momentOf (const Program& p, std::int64_t sample) const noexcept
    {
        return estimate.nanosAt (sample) + latencyNanosOf (p);
    }

    void MtcLane::mark (const timecode::MtcMessage& message)
    {
        active.lastDue = message.dueSample;

        if (! active.begun)
        {
            active.begun = true;
            sending.store (active.id, std::memory_order_relaxed);
        }
    }

    void MtcLane::pass (const timecode::MtcMessage& message)
    {
        mark (message);
        active.cursor.advance();
    }

    void MtcLane::counted (std::int64_t lateBy) noexcept
    {
        late.fetch_add (1, std::memory_order_relaxed);

        if (lateBy > worstNanos.load (std::memory_order_relaxed))
            worstNanos.store (lateBy, std::memory_order_relaxed);
    }

    std::optional<std::int64_t> MtcLane::nextMoment()
    {
        refreshClock();

        const auto next = whatIsNext();

        if (next == Next::none || ! estimate.hasStarted())
            return std::nullopt;

        const auto& program = next == Next::pending ? pending[0] : active;
        return momentOf (program, program.cursor.next().dueSample);
    }

    //==============================================================================
    void MtcLane::pump (std::int64_t nowNanos)
    {
        drain();
        refreshClock();

        /*  NO LINE YET, NO MOMENT: nothing can be said to be due before the
            tick has handed over its first pair, and nothing is sent. */
        if (! estimate.hasStarted())
            return;

        for (int handled = 0; handled < mostInOnePump; ++handled)
        {
            const auto next = whatIsNext();

            if (next == Next::none)
                return;

            const auto message = (next == Next::pending ? pending[0] : active).cursor.next();
            const auto moment = momentOf (next == Next::pending ? pending[0] : active, message.dueSample);

            if (moment > nowNanos)
                return;

            if (next == Next::pending)
                promote();

            /*  THE LAST DOOR, FIRST: HELD (§58, AGG). Counted as sent - nothing
                leaves, nothing is late - and the cursor moves on with the
                show's samples, so the code is where it should be the moment
                the hold lets go. A surface's port passes (AGH). */
            if (wire.holdsBack (active.port))
            {
                active.wasHeld = true;
                pass (message);
                continue;
            }

            /*  LET GO: a full frame at the next cycle, so a desk chasing the
                code relocates at once - what a backup taking over needs (RD.7). */
            if (active.wasHeld)
            {
                active.wasHeld = false;
                active.cursor.seek (message.dueSample, true);
                continue;
            }

            //  TX OFF (AHR): nothing leaves, and the code runs on unheard.
            if (muted)
            {
                pass (message);
                continue;
            }

            if (active.reseatOnNext)
            {
                active.reseatOnNext = false;
                active.cursor.seek (message.dueSample, true);
                continue;
            }

            /*  NO DEVICE BEHIND THE PORT - unbound while the code ran: dropped
                and counted, and the code runs on, so a port bound again picks
                it up where it has got to. */
            const auto door = wire.doorOf (active.port);

            if (door.device == nullptr || door.gate == nullptr)
            {
                noDevice.fetch_add (1, std::memory_order_relaxed);
                pass (message);
                continue;
            }

            /*  TOO LATE TO SEND (AHQ). More than a quarter frame: re-seated at
                the next cycle after now, without a full frame - unless this
                was the full frame, which still opens the code - and after a
                stall of a quarter of a second, with one whatever it was. */
            if (const auto lateNow = nowNanos - moment;
                lateNow > quarterNanosOf (active.rate) || lateNow > stallNanos)
            {
                counted (lateNow);

                const auto withFullFrame = lateNow > stallNanos || active.cursor.fullFrameFirst();
                const auto sampleNow = estimate.sampleAt (nowNanos - latencyNanosOf (active));

                mark (message);
                active.cursor.seek (std::max (sampleNow, message.dueSample + 1), withFullFrame);
                continue;
            }

            /*  SENT UNDER THE PORT'S GATE (AHQ), which the queue's thread holds
                for the whole of each of its sends: a quarter frame waits for a
                SysEx to finish rather than land inside it. Nothing else is
                asked for while it is held. A command that arrived while this
                waited - a cut, a mute - is looked at before the message goes:
                the gate let go unsent, and the loop goes round again. */
            std::int64_t leftAt = 0;

            {
                std::unique_lock<std::mutex> gateLock { *door.gate };

                if (posted.load (std::memory_order_acquire) != seenPosted)
                {
                    gateLock.unlock();
                    drain();
                    continue;
                }

                scratch.assign (message.bytes.begin(), message.bytes.begin() + message.size);
                leftAt = clock->now();
                door.device->sendWithDue (scratch, moment);
            }

            sentMessages.fetch_add (1, std::memory_order_relaxed);

            //  THE MONITOR SEES THE FULL FRAMES (AHQ), not a hundred quarters a second.
            wire.noteLeft (*door.device, scratch, message.isFullFrame());

            if (const auto lateBy = leftAt - moment; lateBy > countedLateNanos)
                counted (lateBy);
            else if (lateBy > worstNanos.load (std::memory_order_relaxed))
                worstNanos.store (lateBy, std::memory_order_relaxed);

            pass (message);
        }
    }

    //==============================================================================
    void MtcLane::run()
    {
        /*  RAISED AS THE TICK THREAD IS (clock/TickThread.h): above the window,
            below the sound - TIME_CRITICAL on Windows, the time-constraint
            policy on macOS (told the tick's period; a lane that wakes twice as
            often and works for microseconds sits well inside it), SCHED_FIFO
            where Linux allows it. What did not take effect leaves an ordinary
            thread, which still sends, only less punctually. */
        elevateCurrentThreadForTicking();

        for (;;)
        {
            drain();
            const auto moment = nextMoment();

            std::unique_lock<std::mutex> lock { inboxMutex };

            if (! running)
                return;

            //  Something came in since the look: looked at first.
            if (! inbox.empty())
                continue;

            const auto woken = [this] { return ! running || ! inbox.empty(); };

            //  NO PROGRAM: asleep until a command comes.
            if (active.id == 0 && pendingCount == 0)
            {
                wake.wait (lock, woken);
                continue;
            }

            if (! moment.has_value())
            {
                wake.wait_for (lock, clocklessLook, woken);
                continue;
            }

            if (const auto ahead = std::chrono::nanoseconds (*moment - clock->now()); ahead > wakeGuard)
            {
                wake.wait_for (lock, std::min<std::chrono::nanoseconds> (ahead - wakeGuard, longestSleep), woken);
                continue;
            }

            lock.unlock();
            clock->sleepUntil (*moment);
            pump (clock->now());
        }
    }
}
