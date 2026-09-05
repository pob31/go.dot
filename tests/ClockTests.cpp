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

/*
    The 50 Hz tick: where it sits, when it comes due, and the thread that
    notices.

    ALMOST EVERYTHING HERE IS TESTED WITHOUT A THREAD, and that is deliberate
    rather than convenient. The interesting claims - a tick straddling a block,
    six ticks coming due at once after a stall, a sample rate changing under a
    running show - are arithmetic. Driving them through a real thread would
    measure the operating system's scheduler at the same time, and a test that
    fails when a CI runner is busy teaches everyone to re-run it rather than to
    read it.

    So nextTickAction() is a pure function and this file calls it directly, one
    simulated block at a time. The two cases that genuinely need a thread get
    one, and they assert only what no scheduler can change.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/AudioClockSource.h>
#include <wfg/engine/clock/DummyAudioClock.h>
#include <wfg/engine/clock/SampleTime.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/clock/TickThread.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    /*  Every rate anybody actually runs, and every buffer size the presets
        offer. The matrix is the point: a tick that is exact at 48 kHz and drifts
        at 44.1 kHz would be a bug nobody found until a show. */
    constexpr int sampleRates[] = { 22050, 44100, 48000, 88200, 96000, 192000 };
    constexpr int blockSizes[]  = { 32, 64, 128, 256, 512 };

    /*  Runs the schedule forward one block at a time, exactly as the tick
        thread does, and returns every tick it produced with how late each one
        was. No threads, no waiting, no clock. */
    struct Run
    {
        std::vector<std::int64_t> ticks;
        std::vector<std::int64_t> lateness;

        std::int64_t worstLateness() const
        {
            return ticks.empty() ? 0 : *std::max_element (lateness.begin(), lateness.end());
        }
    };

    Run runBlocks (const TickClock& clock, int blockSize, int numBlocks)
    {
        Run result;
        std::int64_t samples = 0;
        std::int64_t lastProcessed = -1;

        for (int b = 0; b <= numBlocks; ++b)
        {
            /*  Drain everything due at this position before advancing, which is
                what the thread does: several ticks that came due together are
                processed back to back with no waiting between them. */
            for (;;)
            {
                const auto action = nextTickAction (clock, lastProcessed, samples);

                if (action.kind != TickAction::Kind::process)
                    break;

                result.ticks.push_back (action.tick);
                result.lateness.push_back (action.lateness);
                lastProcessed = action.tick;
            }

            samples += blockSize;
        }

        return result;
    }

    /*  Waits for something to become true, or gives up. The timeout exists so a
        broken thread fails the test instead of hanging the suite; it is never
        the thing being measured. */
    template <typename Predicate>
    bool waitUntil (Predicate predicate,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds { 5000 })
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds { 1 });
        }

        return predicate();
    }
}

//==============================================================================
TEST_CASE ("tick clock: a rate 50 does not divide exactly is refused, not rounded")
{
    /*  A tick at 882.02 samples drifts a whole sample every fifty ticks, so an
        hour-long show ends 3600 samples away from where its log says it was.
        Refusing costs nothing: every rate anybody uses divides exactly. */
    for (const int rate : sampleRates)
    {
        INFO ("rate " << rate);
        const auto perTick = TickClock::samplesPerTickFor (rate);

        REQUIRE (perTick.has_value());
        CHECK (*perTick == rate / TickClock::rateHz);
        CHECK (*perTick * TickClock::rateHz == rate);       // exact, by construction
        CHECK (TickClock::create (rate).has_value());
    }

    for (const int rate : { 44101, 47999, 1, 49, 51, 0, -48000 })
    {
        INFO ("rate " << rate);
        CHECK_FALSE (TickClock::samplesPerTickFor (rate).has_value());
        CHECK_FALSE (TickClock::create (rate).has_value());
    }
}

TEST_CASE ("tick clock: every rate and block size gives every tick, in order, within one block")
{
    /*  The claim this file exists for. For each of thirty combinations: no tick
        is skipped, none is out of order, none is repeated, and none is
        processed more than one block after its own position - which is the
        bound the design promises, since the thread can only observe the counter
        between blocks. */
    for (const int rate : sampleRates)
    {
        const auto clock = TickClock::create (rate);
        REQUIRE (clock.has_value());

        for (const int block : blockSizes)
        {
            INFO ("rate " << rate << ", block " << block
                          << ", samples per tick " << clock->samplesPerTick());

            const auto run = runBlocks (*clock, block, 200);

            REQUIRE (! run.ticks.empty());

            for (std::size_t i = 0; i < run.ticks.size(); ++i)
            {
                CHECK (run.ticks[i] == static_cast<std::int64_t> (i));   // in order, gapless, from 0
                CHECK (run.lateness[i] >= 0);
            }

            /*  Strictly less than one block: if it were a whole block late, the
                previous block end would already have reached the boundary and
                the tick would have run then. */
            CHECK (run.worstLateness() < block);

            // And each tick sits exactly where the arithmetic says it does.
            for (const auto tick : run.ticks)
                CHECK (clock->sampleForTick (tick)
                         == tick * static_cast<std::int64_t> (clock->samplesPerTick()));
        }
    }
}

TEST_CASE ("tick clock: at 44.1k with 512-sample blocks every tick straddles a block")
{
    /*  The awkward case named in the plan, called out on its own because it is
        the one where the two numbers share no common factor: 882 and 512 mean a
        tick boundary almost never lands on a block end, so lateness is nearly
        always non-zero and is nearly always under a block. */
    const auto clock = TickClock::create (44100);
    REQUIRE (clock.has_value());
    CHECK (clock->samplesPerTick() == 882);

    const auto run = runBlocks (*clock, 512, 400);

    CHECK (run.worstLateness() < 512);
    CHECK (run.worstLateness() > 0);            // they really do straddle

    // Over four hundred blocks, only tick 0 lands exactly on a boundary.
    const auto exact = std::count (run.lateness.begin(), run.lateness.end(), 0);
    CHECK (exact == 1);
}

TEST_CASE ("tick clock: several ticks due at once come back to back, in order")
{
    /*  One long block, or one scheduling stall, and the counter jumps past
        several boundaries. Every one of them is still produced, in order: the
        tick index is the event log's ordering key and a gap in it would be a
        gap in the record of the show. */
    const auto clock = TickClock::create (44100);       // 882 samples per tick
    REQUIRE (clock.has_value());

    std::vector<std::int64_t> produced;
    std::int64_t lastProcessed = -1;

    constexpr std::int64_t stalledAt = 5000;            // 5 x 882 = 4410, 6 x 882 = 5292

    for (;;)
    {
        const auto action = nextTickAction (*clock, lastProcessed, stalledAt);

        if (action.kind != TickAction::Kind::process)
            break;

        produced.push_back (action.tick);
        lastProcessed = action.tick;
    }

    CHECK (produced == std::vector<std::int64_t> { 0, 1, 2, 3, 4, 5 });

    // The next one is not due, and the wait says by how much.
    const auto next = nextTickAction (*clock, lastProcessed, stalledAt);
    CHECK (next.kind == TickAction::Kind::wait);
    CHECK (next.tick == 6);
    CHECK (next.shortfall == 6 * 882 - stalledAt);
}

TEST_CASE ("tick clock: a rate change keeps the index monotonic and the boundary fixed")
{
    /*  PRD §6.2's Dante clock domain moving under a running show. The tick the
        change lands on must not move - the log already says where it was - and
        the sequence must stay gapless and increasing across it. */
    auto clock = TickClock::create (48000);             // 960
    REQUIRE (clock.has_value());
    CHECK (clock->samplesPerTick() == 960);

    const auto boundary = clock->sampleForTick (100);
    CHECK (boundary == 96000);

    REQUIRE (clock->rebase (100, 44100));               // 882 from here on

    CHECK (clock->sampleRate() == 44100);
    CHECK (clock->samplesPerTick() == 882);
    CHECK (clock->anchorTick() == 100);
    CHECK (clock->anchorSample() == boundary);

    CHECK (clock->sampleForTick (100) == boundary);             // has not moved
    CHECK (clock->sampleForTick (101) == boundary + 882);       // new ratio from here

    for (std::int64_t n = 100; n < 300; ++n)
        CHECK (clock->sampleForTick (n + 1) > clock->sampleForTick (n));

    // And the reverse lookup agrees on both sides of the boundary.
    CHECK (clock->ticksReachedAt (boundary) == 100);
    CHECK (clock->ticksReachedAt (boundary + 881) == 100);
    CHECK (clock->ticksReachedAt (boundary + 882) == 101);
    CHECK (clock->ticksReachedAt (boundary - 1) == 99);
}

TEST_CASE ("tick clock: a rebase that would rewrite the past is refused")
{
    auto clock = TickClock::create (48000);
    REQUIRE (clock.has_value());
    REQUIRE (clock->rebase (100, 44100));

    // A rate that does not divide.
    CHECK_FALSE (clock->rebase (150, 44101));

    /*  And a tick before the anchor: that moment has already happened and the
        log already records where it sat. */
    CHECK_FALSE (clock->rebase (99, 48000));

    // Neither attempt changed anything.
    CHECK (clock->samplesPerTick() == 882);
    CHECK (clock->anchorTick() == 100);

    // Re-anchoring at the same rate is allowed: PRD §3.14 will want it.
    const auto sample = clock->sampleForTick (150);
    CHECK (clock->rebase (150, 44100));
    CHECK (clock->anchorTick() == 150);
    CHECK (clock->sampleForTick (150) == sample);
}

TEST_CASE ("tick clock: ticksReachedAt before the anchor says nothing has come due")
{
    const auto clock = TickClock::create (48000);
    REQUIRE (clock.has_value());

    CHECK (clock->ticksReachedAt (0) == 0);
    CHECK (clock->ticksReachedAt (959) == 0);
    CHECK (clock->ticksReachedAt (960) == 1);

    /*  Negative positions truncate towards zero in C++, which would round the
        wrong way and report a tick reached one boundary early. The guard makes
        it say "not yet" instead. */
    CHECK (clock->ticksReachedAt (-1) == -1);
    CHECK (clock->ticksReachedAt (-100000) == -1);
}

//==============================================================================
TEST_CASE ("sample time: samples and duration convert both ways")
{
    CHECK (samplesToDuration (48000, 48000) == std::chrono::seconds { 1 });
    CHECK (samplesToDuration (960, 48000) == std::chrono::milliseconds { 20 });
    CHECK (samplesToDuration (882, 44100) == std::chrono::milliseconds { 20 });

    CHECK (durationToSamples (std::chrono::seconds { 1 }, 48000) == 48000);
    CHECK (durationToSamples (std::chrono::milliseconds { 20 }, 48000) == 960);

    // Early is on time: a negative lateness would only invite averaging it away.
    CHECK (durationToSamples (std::chrono::nanoseconds { -5 }, 48000) == 0);

    // And a rate of zero divides by nothing rather than crashing.
    CHECK (samplesToDuration (1000, 0) == std::chrono::nanoseconds { 0 });
    CHECK (durationToSamples (std::chrono::seconds { 1 }, 0) == 0);
}

TEST_CASE ("sample time: a hundred days does not overflow the conversion")
{
    /*  The obvious spelling, `samples * 1'000'000'000 / rate`, passes 2^63 after
        about fifty hours at 48 kHz and comes back NEGATIVE - so a deadline lands
        in the past and the clock free-runs. Fifty hours is the kind of limit
        that never shows up in a test and does show up in a rig somebody left on
        over a weekend. */
    constexpr std::int64_t rate = 48000;
    constexpr std::int64_t days = 100;
    constexpr std::int64_t samples = days * 24 * 60 * 60 * rate;

    const auto duration = samplesToDuration (samples, static_cast<int> (rate));

    CHECK (duration.count() > 0);
    CHECK (duration == std::chrono::hours { days * 24 });
    CHECK (durationToSamples (duration, static_cast<int> (rate)) == samples);
}

//==============================================================================
TEST_CASE ("audio clock source: it only ever goes forwards")
{
    AudioClockSource source;
    CHECK (source.samplesElapsed() == 0);

    source.advance (512);
    source.advance (512);
    CHECK (source.samplesElapsed() == 1024);

    /*  There is no way to write anything else here, and that is the whole
        reason this type exists rather than the audio side holding a ManualClock:
        the counter the tick index is derived from cannot be rewound by anything
        anyone writes inside a device callback. */
    const SampleClock& asClock = source;
    CHECK (asClock.samplesElapsed() == 1024);
}

//==============================================================================
TEST_CASE ("dummy clock: it is paced, and it reports how late it ran")
{
    /*  Nothing here asserts that the dummy clock is FAST. A general-purpose
        scheduler wakes a thread when it gets round to it, and a loaded CI
        runner is worse; an assertion about speed would be an assertion about
        the machine.

        What is asserted is that it is PACED, and no amount of jitter can
        disguise that: an unpaced loop delivers hundreds of thousands of blocks
        in 300 ms where this delivers tens. */
    constexpr int rate = 48000;
    constexpr int block = 512;                      // 10.67 ms per block
    constexpr int runFor = 300;                     // ms

    DummyAudioClock clock { rate, block };
    CHECK_FALSE (clock.isRunning());
    CHECK (clock.sampleRate() == rate);
    CHECK (clock.blockSize() == block);

    clock.start();
    CHECK (clock.isRunning());

    std::this_thread::sleep_for (std::chrono::milliseconds { runFor });
    clock.stop();

    CHECK_FALSE (clock.isRunning());

    const auto delivered = clock.blocksDelivered();
    const auto ideal = static_cast<std::int64_t> (runFor) * rate / (1000 * block);

    INFO ("delivered " << delivered << " blocks, unloaded would be about " << ideal);
    INFO ("worst block lateness " << clock.worstBlockLateness() << " samples");

    CHECK (delivered > 0);
    CHECK (delivered < ideal * 5);

    // The counter and the block count agree, which they must after a join.
    CHECK (clock.clock().samplesElapsed() == delivered * block);

    // Lateness is measured rather than assumed away.
    CHECK (clock.blockLateness() >= 0);
    CHECK (clock.worstBlockLateness() >= clock.blockLateness());
}

TEST_CASE ("dummy clock: start and stop are safe to repeat")
{
    DummyAudioClock clock { 48000, 512 };

    clock.stop();               // never started
    clock.start();
    clock.start();              // already running
    clock.stop();
    clock.stop();               // already stopped

    CHECK_FALSE (clock.isRunning());
}

//==============================================================================
TEST_CASE ("tick thread: it processes every tick in order, driven by the counter")
{
    /*  One of the two cases that needs a real thread. What it asserts is
        deterministic anyway: the counter is a ManualClock this test moves by
        hand, so where the ticks land does not depend on when the thread woke. */
    Engine engine;
    ManualClock counter;

    const auto schedule = TickClock::create (48000);        // 960 samples per tick
    REQUIRE (schedule.has_value());

    TickThread thread { engine, counter, *schedule };
    CHECK_FALSE (thread.isRunning());

    thread.start();
    CHECK (thread.isRunning());

    // Tick 0 sits at sample 0, so it is due before anything has elapsed at all.
    REQUIRE (waitUntil ([&thread] { return thread.lastTick() >= 0; }));
    CHECK (thread.lastTick() == 0);
    CHECK (thread.lateness() == 0);

    // Ten ticks' worth in one jump. It must catch up one at a time, no gaps.
    counter.advance (960 * 10);
    REQUIRE (waitUntil ([&thread] { return thread.lastTick() >= 10; }));

    thread.stop();
    CHECK_FALSE (thread.isRunning());

    CHECK (thread.lastTick() == 10);
    CHECK (thread.ticksProcessed() == 11);
    CHECK (engine.currentTick() == 10);

    /*  And the catch-up is visible in the numbers rather than smoothed over.
        Tick 1 sits at sample 960 and did not run until the counter read 9600,
        so it was 8640 samples late - which is exactly what an operator should
        see after a stall. */
    CHECK (thread.latenessMax() == 8640);
    CHECK (thread.lateness() == 0);          // tick 10 landed on its own boundary
}

TEST_CASE ("tick thread: it says honestly that it has no priority guarantee")
{
    /*  Phase 1's shim does nothing, and reports that it did nothing. A shim
        that returned true would let the first person investigating a late show
        rule out the right cause on the strength of it. */
    CHECK_FALSE (elevateCurrentThreadForTicking());

    Engine engine;
    ManualClock counter;
    const auto schedule = TickClock::create (48000);
    REQUIRE (schedule.has_value());

    TickThread thread { engine, counter, *schedule };
    thread.start();
    REQUIRE (waitUntil ([&thread] { return thread.lastTick() >= 0; }));

    CHECK_FALSE (thread.hasElevatedPriority());
    CHECK (thread.sampleRate() == 48000);
    CHECK (thread.samplesPerTick() == 960);

    thread.stop();
}

TEST_CASE ("tick thread: a dummy clock drives it end to end")
{
    /*  The other case that needs threads, and the only place the two are wired
        together the way `serve` will wire them. Loose bounds on purpose: this
        asks whether the parts connect, not how quickly. */
    Engine engine;
    DummyAudioClock audio { 48000, 512 };

    const auto schedule = TickClock::create (48000);
    REQUIRE (schedule.has_value());

    TickThread thread { engine, audio.clock(), *schedule };

    audio.start();
    thread.start();

    REQUIRE (waitUntil ([&thread] { return thread.lastTick() >= 5; }));

    thread.stop();
    audio.stop();

    INFO ("ticks " << thread.lastTick() << ", worst lateness "
                   << thread.latenessMax() << " samples");

    CHECK (thread.lastTick() >= 5);
    CHECK (engine.currentTick() == thread.lastTick());

    /*  Every tick up to that index really was processed - the engine's own
        index is the last one it was handed, and the thread never skips, so the
        two agreeing is the gapless claim checked end to end. */
    CHECK (thread.ticksProcessed() == thread.lastTick() + 1);
}
