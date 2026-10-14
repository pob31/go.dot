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
    THE SAME SAMPLE (namespace draft §58, RD.9; decisions AFS, AGF, AGI, AGQ).

    What is pinned here: the clock map (pair/ClockMap.h) - two simulated
    machines whose host clocks are hours apart and drift, whose sound clocks
    run 50 parts per million apart and stamp their blocks of 64, 256 and 512
    samples with jitter, joined by pings with jitter and the odd queue - maps a
    leader's sample onto the follower's within a quarter of a millisecond for
    ten simulated minutes, reads the drift within a part per million, and
    settles again without ringing after a 50 ms slip; the map measured by the
    leader, which pings when it dials, is the same map. The audio side's stamp
    (clock/BlockStamp.h) is never read torn, and the dummy clock and the hosted
    pump write one a block, at the block's deadline; an audio host driven by
    hand stamps only a block it is told the time of; the session clock moves a
    stamp onto its count. The wire: a pong carries NTP's middle two stamps and
    a pinger gets all four, a pong of two still times the trip; what a machine
    says of its clocks reads back exactly, rides a leader's bundle after its
    records and before its beat, reaches a follower welcomed later, and from a
    follower reaches the leader's half. The Runner's one function: a launch
    placed as it always was, on a leader's tick grid with the extra lead, on
    an unmapped follower with its leader's extra lead, on a mapped follower on
    the leader's sample - and one whose sample has gone moved into its file
    and counted; a summary on the record clock. And two engines joined by the
    real stream and receiver, each on its own simulated clock - another start,
    another rate of the crystal, host clocks hours apart: a seated sound, a GO
    and a timeline member start on the leader's sample, to within a block, the
    summaries in step on the record clock; with the map never trusted, a GO
    starts Δ behind, as before RD.9.

    Every case runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/audio/SettingsPump.h>
#include <wfg/engine/clock/BlockStamp.h>
#include <wfg/engine/clock/DummyAudioClock.h>
#include <wfg/engine/clock/SampleClock.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/UndoStep.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/ClockMap.h>
#include <wfg/engine/pair/Follow.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/pair/PeerLink.h>
#include <wfg/engine/pair/Position.h>
#include <wfg/engine/pair/Summary.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using Clock = pair::LinkClock;
    using namespace std::chrono_literals;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }

    std::int64_t nanosOf (Clock::time_point at)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (at.time_since_epoch()).count();
    }

    osc::Packet packetOf (const std::vector<std::uint8_t>& bytes)
    {
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        REQUIRE (decoded.ok);
        return decoded.packet;
    }

    //==========================================================================
    /*  ONE SIMULATED MACHINE'S TWO CLOCKS, against true time `tau` in seconds:
        its host clock - nanoseconds from a base, running `hostDrift` fast -
        and its sound clock - a count of samples from `soundStart`, running
        `soundDrift` fast, stamped a block at a time. A pause of the count (an
        interface that dropped a buffer: the count stands still while time
        goes on) from `slipAt` for `slipSeconds`. */
    struct SimClocks
    {
        double hostBase = 0.0;
        double hostDrift = 0.0;
        double soundStart = 0.0;
        double soundDrift = 0.0;
        int rate = 48000;
        int block = 256;
        double slipAt = 1.0e30;
        double slipSeconds = 0.0;

        double hostAt (double tau) const { return hostBase + tau * 1.0e9 * (1.0 + hostDrift); }

        double startAt (double tau) const
        {
            return soundStart + (tau >= slipAt ? std::min (tau - slipAt, slipSeconds) : 0.0);
        }

        double countAt (double tau) const { return (tau - startAt (tau)) * rate * (1.0 + soundDrift); }

        //  The moment the count reached `count`: after the pause for a count it reached after it.
        double tauOfCount (double count) const
        {
            const auto before = soundStart + count / (rate * (1.0 + soundDrift));

            if (before < slipAt)
                return before;

            return soundStart + slipSeconds + count / (rate * (1.0 + soundDrift));
        }

        //  The last block begun by `tau`: its first sample, stamped with its moment and some jitter.
        BlockStamp::Reading stampAt (double tau, double jitterNanos) const
        {
            const auto count = countAt (tau);
            const auto first = std::floor (count / block) * block;
            BlockStamp::Reading out;
            out.sample = static_cast<std::int64_t> (first);
            out.nanos = static_cast<std::int64_t> (std::llround (hostAt (tauOfCount (first)) + jitterNanos));
            return out;
        }
    };

    /*  ONE PING ON A SIMULATED WIRE: sent at `tau` by `pinger`, a way there and
        a way back each a base and some jitter, now and then a queue on one way
        - answered by `other` twenty microseconds after it arrived. */
    struct SimWire
    {
        explicit SimWire (unsigned seed) : random (seed) {}

        double oneWay()
        {
            auto seconds = 150.0e-6 + jitter (random);

            if (queued (random) < 0.05)
                seconds += queue (random);

            return seconds;
        }

        pair::HostSample ping (double tau, const SimClocks& pinger, const SimClocks& other)
        {
            const auto there = oneWay();
            const auto back = oneWay();
            pair::HostSample out;
            out.sent = static_cast<std::int64_t> (std::llround (pinger.hostAt (tau)));
            out.received = static_cast<std::int64_t> (std::llround (other.hostAt (tau + there)));
            out.left = static_cast<std::int64_t> (std::llround (other.hostAt (tau + there + 20.0e-6)));
            out.back = static_cast<std::int64_t> (std::llround (pinger.hostAt (tau + there + 20.0e-6 + back)));
            return out;
        }

        std::mt19937 random;
        std::exponential_distribution<double> jitter { 1.0 / 80.0e-6 };
        std::uniform_real_distribution<double> queued { 0.0, 1.0 };
        std::uniform_real_distribution<double> queue { 1.0e-3, 4.0e-3 };
    };
}

//==============================================================================
TEST_CASE ("same sample: the clock map puts a leader's sample on the follower's clock within a quarter of a millisecond for ten minutes, reads the drift within a part per million, and settles again after a slip")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    int block = 256;
    double followerDrift = 50.0e-6;
    auto slips = false;

    SUBCASE ("blocks of 64, the follower's crystal 50 ppm fast")    { block = 64;  followerDrift = 50.0e-6; }
    SUBCASE ("blocks of 256, the follower's crystal 50 ppm slow")   { block = 256; followerDrift = -50.0e-6; }
    SUBCASE ("blocks of 512, and the follower slips 50 ms at five minutes") { block = 512; followerDrift = 50.0e-6; slips = true; }

    /*  HOURS APART AND DRIFTING: the leader up for six days, the follower for
        nine hours, their host clocks twelve parts per million fast and seven
        slow; the leader's sound clock started twelve seconds before the
        watching, the follower's five. */
    SimClocks leader;
    leader.hostBase = 5.2e14;
    leader.hostDrift = 12.0e-6;
    leader.soundStart = -12.3;
    leader.block = block;

    SimClocks follower;
    follower.hostBase = 3.3e13;
    follower.hostDrift = -7.0e-6;
    follower.soundStart = -4.71;
    follower.soundDrift = followerDrift;
    follower.block = block;

    if (slips)
    {
        follower.slipAt = 300.0;
        follower.slipSeconds = 0.05;
    }

    std::mt19937 random { static_cast<unsigned> (block) * 7919u };
    std::uniform_real_distribution<double> callbackJitter { -150.0e3, 150.0e3 };
    SimWire wire { static_cast<unsigned> (block) + 11u };

    pair::SoundLineFit leaderFit, followerFit;
    pair::HostOffset followerPings, leaderPings;

    const auto trueDrift = ((1.0 + follower.soundDrift) / (1.0 + leader.soundDrift) - 1.0) * 1.0e6;

    double worst = 0.0, worstAt = 0.0, worstAfterSlip = 0.0, worstBeforeSlip = 0.0, worstOtherWay = 0.0;
    double worstDrift = 0.0;
    int compared = 0, slipsTold = 0;
    double firstTrusted = -1.0;
    auto trustLost = false;
    auto trusted = false;

    //  Ten minutes of ticks; a ping every fifth; the map laid against the truth once a second.
    for (int tick = 0; tick <= 30'000; ++tick)
    {
        const auto tau = tick * 0.02;

        leaderFit.feed (leader.stampAt (tau, callbackJitter (random)), leader.rate);

        if (followerFit.feed (follower.stampAt (tau, callbackJitter (random)), follower.rate))
            ++slipsTold;

        if (tick % 5 == 0)
        {
            followerPings.add (wire.ping (tau, follower, leader));
            leaderPings.add (wire.ping (tau + 0.007, leader, follower));
        }

        if (tick % 50 != 0)
            continue;

        const auto map = pair::ClockMap::compose (leaderFit.line(), followerFit.line(), followerPings.line(),
                                                  pair::ClockMap::MeasuredBy::follower, trusted);

        if (trusted && ! map.trusted())
            trustLost = true;

        trusted = map.trusted();

        if (! trusted)
            continue;

        if (firstTrusted < 0.0)
            firstTrusted = tau;

        /*  A LAUNCH A TENTH OF A SECOND AHEAD on the leader's clock, and where
            the follower's count truly is at that moment. */
        const auto leaderSample = std::floor (leader.countAt (tau + 0.1));
        const auto truth = follower.countAt (leader.tauOfCount (leaderSample));
        const auto error = std::abs (map.followerSampleOf (leaderSample) - truth) / follower.rate;

        //  And back.
        CHECK (std::abs (map.leaderSampleOf (map.followerSampleOf (leaderSample)) - leaderSample) < 0.01);

        ++compared;

        if (tau >= follower.slipAt && tau < follower.slipAt + 2.0)
            continue;

        //  The same map, the host clocks measured by the leader.
        const auto theirs = pair::ClockMap::compose (leaderFit.line(), followerFit.line(), leaderPings.line(),
                                                     pair::ClockMap::MeasuredBy::leader);
        worstOtherWay = std::max (worstOtherWay, std::abs (theirs.followerSampleOf (leaderSample) - truth) / follower.rate);

        if (error > worst)
        {
            worst = error;
            worstAt = tau;
        }

        if (tau > follower.slipAt)
            worstAfterSlip = std::max (worstAfterSlip, error);
        else
            worstBeforeSlip = std::max (worstBeforeSlip, error);

        //  The drift, once two minutes of stamps and exchanges say it.
        if (tau >= 180.0)
            worstDrift = std::max (worstDrift, std::abs (map.driftPartsPerMillion() - trueDrift));
    }

    MESSAGE ("blocks of " << block << ": trusted from " << firstTrusted << " s, " << compared
             << " comparisons, worst " << worst * 1000.0 << " ms at " << worstAt << " s (before a slip " << worstBeforeSlip * 1000.0
             << ", after " << worstAfterSlip * 1000.0 << "), measured by the leader " << worstOtherWay * 1000.0
             << " ms; drift true " << trueDrift << " ppm, read within " << worstDrift << " ppm; slips told " << slipsTold);

    CHECK (firstTrusted >= 0.0);
    CHECK (firstTrusted <= 2.0);
    CHECK (compared > 550);
    CHECK (worst < 0.25e-3);
    CHECK (worstOtherWay < 0.25e-3);
    CHECK (worstDrift < 1.0);
    CHECK_FALSE (trustLost);

    if (slips)
    {
        //  TOLD ONCE, AND SETTLED: no comparison two seconds after it is out by more than before it.
        CHECK (slipsTold == 1);
        CHECK (worstAfterSlip < 0.25e-3);
    }
    else
    {
        CHECK (slipsTold == 0);
    }
}

TEST_CASE ("same sample: the map is not trusted until it has its three lines, a few exchanges, sane clocks and an error under five milliseconds - and an untrusted map leaves the record clock")
{
    pair::SoundLine line;
    line.valid = true;
    line.rate = 48000;
    line.sample = 480'000;
    line.nanos = 1'000'000'000'000;
    line.nanosPerSample = 1.0e9 / 48000.0;
    line.errorNanos = 20'000.0;

    pair::OffsetLine hosts;
    hosts.valid = true;
    hosts.at = 1'000'000'000'000;
    hosts.offset = 2.5e6;
    hosts.samples = 3;
    hosts.errorNanos = 100'000.0;

    //  Everything in hand: trusted, and the leader's sample is the same moment 2.5 ms on.
    auto map = pair::ClockMap::compose (line, line, hosts, pair::ClockMap::MeasuredBy::follower);
    CHECK (map.trusted());
    CHECK (map.onThisClock (480'000) == 480'000 - 120);
    CHECK (map.sampleOnThisClock (480'000, 3, 48000) == 480'000 - 120);
    CHECK (std::abs (map.driftPartsPerMillion()) < 1.0e-6);
    CHECK (map.residualMicroseconds() == doctest::Approx (std::sqrt (2.0 * 20.0 * 20.0 + 100.0 * 100.0)));

    //  Two exchanges are too few.
    auto fewer = hosts;
    fewer.samples = 2;
    CHECK_FALSE (pair::ClockMap::compose (line, line, fewer, pair::ClockMap::MeasuredBy::follower).trusted());

    //  A line not yet fitted.
    auto young = line;
    young.valid = false;
    map = pair::ClockMap::compose (young, line, hosts, pair::ClockMap::MeasuredBy::follower);
    CHECK_FALSE (map.trusted());

    //  An untrusted map is the record clock: three ticks of the follower's samples.
    CHECK (map.sampleOnThisClock (480'000, 3, 48000) == 480'000 + 3 * 960);
    CHECK (pair::ClockMap {}.sampleOnThisClock (2'400'000, -3, 44100) == 2'400'000 - 3 * 882);
    CHECK (pair::ClockMap {}.sampleOnThisClock (2'400'000, 5, 0) == 2'400'000);

    //  Two crystals a percent apart are no crystals.
    auto fast = line;
    fast.nanosPerSample *= 1.01;
    CHECK_FALSE (pair::ClockMap::compose (fast, line, hosts, pair::ClockMap::MeasuredBy::follower).trusted());

    //  Five milliseconds: under it trusted, over it not - and once trusted, kept to ten.
    auto rough = hosts;
    rough.errorNanos = 4.9e6;
    CHECK (pair::ClockMap::compose (line, line, rough, pair::ClockMap::MeasuredBy::follower).trusted());
    rough.errorNanos = 7.0e6;
    CHECK_FALSE (pair::ClockMap::compose (line, line, rough, pair::ClockMap::MeasuredBy::follower).trusted());
    CHECK (pair::ClockMap::compose (line, line, rough, pair::ClockMap::MeasuredBy::follower, true).trusted());
    rough.errorNanos = 11.0e6;
    CHECK_FALSE (pair::ClockMap::compose (line, line, rough, pair::ClockMap::MeasuredBy::follower, true).trusted());
}

//==============================================================================
TEST_CASE ("same sample: a block's stamp is never read torn - one block's sample with another's moment")
{
    BlockStamp stamp;
    std::atomic<bool> done { false };

    BlockStamp::Reading nothing;
    CHECK_FALSE (stamp.read (nothing));

    std::thread writer ([&stamp, &done]
                        {
                            for (std::int64_t k = 1; ! done.load (std::memory_order_relaxed); ++k)
                                stamp.write (k * 256, k * 5333 + 17);
                        });

    std::int64_t reads = 0, torn = 0;
    const auto until = std::chrono::steady_clock::now() + 200ms;

    while (std::chrono::steady_clock::now() < until)
    {
        BlockStamp::Reading reading;

        if (! stamp.read (reading))
            continue;

        ++reads;

        if (reading.sample % 256 != 0 || reading.nanos != (reading.sample / 256) * 5333 + 17)
            ++torn;
    }

    done.store (true);
    writer.join();

    MESSAGE ("reads " << reads << ", torn " << torn << ", writes " << stamp.writes());
    CHECK (reads > 100);
    CHECK (torn == 0);
}

namespace
{
    /*  WAITED FOR, NOT SLEPT FOR: a block is stamped once the clock's own
        thread has run it, and a loaded runner can take longer than any fixed
        sleep to start that thread (macOS CI on 06725f3 had no stamp 60 ms
        after the start). Polls until a block past `after` has been stamped,
        or five seconds have gone. */
    template <typename ReadsStamps>
    bool stampAfter (ReadsStamps&& lastBlock, std::int64_t after, BlockStamp::Reading& out)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (5);

        while (std::chrono::steady_clock::now() < until)
        {
            if (lastBlock (out) && out.sample > after)
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }

        return false;
    }
}

TEST_CASE ("same sample: the dummy clock stamps every block with its first sample and the moment it was due")
{
    DummyAudioClock clock { 48000, 480 };

    BlockStamp::Reading reading;
    CHECK_FALSE (clock.clock().lastBlock (reading));

    clock.start();
    const auto stamped = [&clock] (BlockStamp::Reading& out) { return clock.clock().lastBlock (out); };

    BlockStamp::Reading first;
    REQUIRE (stampAfter (stamped, -1, first));

    //  A few blocks on, so the two stamps are some way apart.
    BlockStamp::Reading later;
    REQUIRE (stampAfter (stamped, first.sample + 3 * 480, later));
    clock.stop();

    BlockStamp::Reading last;
    REQUIRE (clock.clock().lastBlock (last));

    //  The last block's first sample, and moments a block's length apart to the nanosecond.
    CHECK (last.sample == (clock.blocksDelivered() - 1) * 480);
    CHECK (first.sample % 480 == 0);
    REQUIRE (last.sample > first.sample);

    const auto apart = static_cast<double> (last.nanos - first.nanos);
    const auto expected = static_cast<double> (last.sample - first.sample) * 1.0e9 / 48000.0;
    CHECK (std::abs (apart - expected) <= 2.0);

    //  And on the steady clock: due before now.
    CHECK (last.nanos <= nanosOf (Clock::now()));
}

namespace
{
    struct ScopedFolder
    {
        ScopedFolder()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-same-sample-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScopedFolder() { folder.deleteRecursively(); }

        juce::File folder;
    };
}

TEST_CASE ("same sample: the hosted pump stamps every block at its deadline; an audio host driven by hand stamps only a block it was told the time of")
{
    ScopedFolder storage;

    SUBCASE ("the hosted pump")
    {
        audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

        audio::HostedAudioDriver::Settings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 256;
        settings.outputChannels = 2;
        REQUIRE (driver.open (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        REQUIRE (driver.host().buildEdit (spec));

        BlockStamp::Reading reading;
        CHECK_FALSE (driver.clock().lastBlock (reading));

        REQUIRE (driver.start());
        const auto stamped = [&driver] (BlockStamp::Reading& out) { return driver.clock().lastBlock (out); };

        BlockStamp::Reading first;
        REQUIRE (stampAfter (stamped, -1, first));

        BlockStamp::Reading later;
        REQUIRE (stampAfter (stamped, first.sample + 3 * 256, later));
        driver.stop();

        BlockStamp::Reading last;
        REQUIRE (driver.clock().lastBlock (last));

        CHECK (last.sample == (driver.blocksDelivered() - 1) * 256);
        REQUIRE (last.sample > first.sample);

        const auto apart = static_cast<double> (last.nanos - first.nanos);
        const auto expected = static_cast<double> (last.sample - first.sample) * 1.0e9 / 48000.0;
        CHECK (std::abs (apart - expected) <= 2.0);
    }

    SUBCASE ("an audio host driven by hand")
    {
        audio::AudioHost host { storage.folder.getFullPathName().toStdString() };

        audio::HostSettings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 512;
        settings.outputChannels = 2;
        REQUIRE (host.start (settings));

        BlockStamp::Reading reading;
        host.processBlock();
        CHECK_FALSE (host.clock().lastBlock (reading));

        //  Told the moment: the block's first sample - one block in - and that moment.
        host.setBlockTime (123'456'789);
        host.processBlock();
        REQUIRE (host.clock().lastBlock (reading));
        CHECK (reading.sample == 512);
        CHECK (reading.nanos == 123'456'789);

        //  Told once: the next block, told nothing, leaves the stamp as it was.
        host.processBlock();
        REQUIRE (host.clock().lastBlock (reading));
        CHECK (reading.sample == 512);
        CHECK (host.clock().samplesElapsed() == 3 * 512);

        host.stop();
    }
}

TEST_CASE ("same sample: the session's clock moves a device's stamp onto the session's count")
{
    ManualClock device;
    device.stampBlock (960, 5'000);

    audio::SessionAudioClock session { device };
    BlockStamp::Reading reading;
    REQUIRE (session.lastBlock (reading));
    CHECK (reading.sample == 960);

    //  A device opened again under a running session: its count from nought, the session's from where it was.
    ManualClock next;
    next.stampBlock (0, 9'000);
    session.use (next, 48'000);
    CHECK (session.sessionOffset() == 48'000);
    CHECK (&session.counting() == &next);
    REQUIRE (session.lastBlock (reading));
    CHECK (reading.sample == 48'000);
    CHECK (reading.nanos == 9'000);
}

//==============================================================================
TEST_CASE ("same sample: a pong carries NTP's middle two stamps and the pinger gets all four; a pong of two still times the trip")
{
    pair::Stream stream { "S1" };
    std::vector<std::vector<std::uint8_t>> sent;
    stream.setSend ([&sent] (const std::string&, const std::vector<std::uint8_t>& bytes)
                    {
                        sent.push_back (bytes);
                        return true;
                    });

    const auto pinged = Clock::now();
    const auto t0 = nanosOf (pinged) - 150'000;
    const auto arrived = pinged;

    stream.received ("ws:10.0.0.2:50001",
                     osc::Packet::message (pair::wire::ping, { osc::Value::int64 (t0), osc::Value::float32 (0.0004f),
                                                               osc::Value::int64 (77) }),
                     arrived);

    REQUIRE (sent.size() == 1);
    const auto pong = packetOf (sent[0]);
    CHECK (pong.address == pair::wire::pong);
    REQUIRE (pong.args.size() == 4);
    CHECK (pong.args[0].getInt64() == t0);
    CHECK (pong.args[2].getInt64() == nanosOf (arrived));
    CHECK (pong.args[3].getInt64() >= pong.args[2].getInt64());

    pair::Receiver receiver;
    std::vector<pair::HostSample> samples;
    receiver.setOnHostSample ([&samples] (const pair::HostSample& sample) { samples.push_back (sample); });

    const auto home = Clock::now() + 200us;
    receiver.deliver ("10.0.0.1:5010", pong, home);
    receiver.drain (home, 1.0);

    REQUIRE (samples.size() == 1);
    CHECK (samples[0].sent == t0);
    CHECK (samples[0].received == pong.args[2].getInt64());
    CHECK (samples[0].left == pong.args[3].getInt64());
    CHECK (samples[0].back == nanosOf (home));
    CHECK (receiver.roundTripToSay() > 0.0);

    //  RD.5's pong of two: a round trip, and nothing to measure the clocks by.
    pair::Receiver older;
    std::vector<pair::HostSample> none;
    older.setOnHostSample ([&none] (const pair::HostSample& sample) { none.push_back (sample); });
    older.deliver ("10.0.0.1:5010", osc::Packet::message (pair::wire::pong, { osc::Value::int64 (t0), osc::Value::int64 (3) }), home);
    older.drain (home, 1.0);
    CHECK (none.empty());
    CHECK (older.roundTripToSay() > 0.0);
}

namespace
{
    pair::wire::ClockSaid someClock()
    {
        pair::wire::ClockSaid said;
        said.tick = 4'242;
        said.samplesPerTick = 960;
        said.tickZero = -1'234'567;
        said.leadTicks = 4;
        said.onTicks = true;
        said.extraTicks = 2;
        said.delay = 0;
        said.outputLatency = 0.0103125;
        said.line.valid = true;
        said.line.rate = 48000;
        said.line.sample = 9'876'543'210;
        said.line.nanos = 523'456'789'012'345;
        said.line.nanosPerSample = 20833.3359375 + 1.0e-7;
        said.line.errorNanos = 12.5;
        said.line.seconds = 119.98;
        said.hosts.valid = true;
        said.hosts.at = 523'456'000'000'000;
        said.hosts.offset = -4.87e14 + 0.25;
        said.hosts.slope = 1.9e-5;
        said.hosts.errorNanos = 61.5;
        said.hosts.bestTripNanos = 123.0;
        said.hosts.samples = 1199;
        return said;
    }
}

TEST_CASE ("same sample: what a machine says of its clocks reads back exactly, in any locale")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto said = someClock();
    const auto read = pair::wire::readClock (packetOf (pair::wire::clockBytes (said)));
    REQUIRE (read.has_value());

    CHECK (read->tick == said.tick);
    CHECK (read->samplesPerTick == said.samplesPerTick);
    CHECK (read->tickZero == said.tickZero);
    CHECK (read->leadTicks == said.leadTicks);
    CHECK (read->onTicks == said.onTicks);
    CHECK (read->extraTicks == said.extraTicks);
    CHECK (read->delay == said.delay);
    CHECK (std::bit_cast<std::uint64_t> (read->outputLatency) == std::bit_cast<std::uint64_t> (said.outputLatency));
    CHECK (read->line.valid);
    CHECK (read->line.rate == said.line.rate);
    CHECK (read->line.sample == said.line.sample);
    CHECK (read->line.nanos == said.line.nanos);
    CHECK (std::bit_cast<std::uint64_t> (read->line.nanosPerSample) == std::bit_cast<std::uint64_t> (said.line.nanosPerSample));
    CHECK (std::bit_cast<std::uint64_t> (read->hosts.offset) == std::bit_cast<std::uint64_t> (said.hosts.offset));
    CHECK (std::bit_cast<std::uint64_t> (read->hosts.slope) == std::bit_cast<std::uint64_t> (said.hosts.slope));
    CHECK (read->hosts.at == said.hosts.at);
    CHECK (read->hosts.samples == said.hosts.samples);

    //  Another shape is another build's, and says nothing; a bundle is not one.
    CHECK_FALSE (pair::wire::readClock (osc::Packet::message (pair::wire::clockLine, { osc::Value::int64 (1) })).has_value());

    //  Alone it is a follower's word to its leader's half.
    CHECK (pair::wire::boundFor (packetOf (pair::wire::clockBytes (said))) == pair::wire::Bound::leader);
}

TEST_CASE ("same sample: a leader's word on its clocks rides its tick's bundle after the records and before the beat, reaches a follower welcomed later, and a follower's reaches the leader's half")
{
    pair::Stream stream { "S1" };
    stream.setLeading (true);

    std::map<std::string, std::vector<osc::Packet>> frames;
    stream.setSend ([&frames] (const std::string& origin, const std::vector<std::uint8_t>& bytes)
                    {
                        frames[origin].push_back (packetOf (bytes));
                        return true;
                    });

    const auto now = Clock::now();

    pair::wire::Hello hello;
    hello.machine = "backup";
    hello.role = "backup";
    hello.lastCounter = pair::wire::fromNow;
    hello.stream = "S1";
    hello.state = "following";
    hello.ownStream = "S2";
    hello.described = true;
    stream.received ("ws:a", packetOf (pair::wire::helloBytes (hello)), now);

    LogRecord go;
    go.kind = LogRecord::Kind::applied;
    go.tick = 10;
    go.origin = "cli";
    go.command = "go";

    //  Welcomed on tick 9, from now; then said on tick 10, with a record of tick 10.
    stream.afterTick (9, 1.0);
    REQUIRE (frames["ws:a"].size() == 1);

    auto said = someClock();
    said.tick = 10;
    stream.keep (go, nullptr);
    stream.sayClock (pair::wire::clockBytes (said));
    stream.afterTick (10, 1.0);

    REQUIRE (frames["ws:a"].size() == 2);
    const auto first = frames["ws:a"].back();
    REQUIRE (first.isBundle());

    std::vector<std::string> order;

    for (const auto& element : first.elements)
        order.push_back (element.address);

    const auto at = [&order] (const char* address)
    {
        return std::find (order.begin(), order.end(), address) - order.begin();
    };

    CHECK (at (pair::wire::record) < at (pair::wire::clockLine));
    CHECK (at (pair::wire::clockLine) < at (pair::wire::beat));

    //  Not said again on a tick that says nothing...
    stream.afterTick (11, 1.0);
    REQUIRE (frames["ws:a"].size() == 3);
    CHECK (std::none_of (frames["ws:a"].back().elements.begin(), frames["ws:a"].back().elements.end(),
                         [] (const osc::Packet& element) { return element.address == pair::wire::clockLine; }));

    //  ...but in the first bundle of a follower welcomed later.
    stream.received ("ws:b", packetOf (pair::wire::helloBytes (hello)), now);
    stream.afterTick (12, 1.0);
    REQUIRE (frames["ws:b"].size() == 1);
    CHECK (std::any_of (frames["ws:b"].back().elements.begin(), frames["ws:b"].back().elements.end(),
                        [] (const osc::Packet& element) { return element.address == pair::wire::clockLine; }));

    //  The follower's receiver hands it on in the stream's order, after the record.
    pair::Receiver receiver;
    std::vector<std::string> heard;
    std::optional<pair::wire::ClockSaid> got;
    receiver.setOnRecord ([&heard] (std::uint64_t, const LogRecord&) { heard.push_back ("record"); });
    receiver.setOnClock ([&heard, &got] (const pair::wire::ClockSaid& clock)
                         {
                             heard.push_back ("clock");
                             got = clock;
                         });
    receiver.setOnBeat ([&heard] (std::int64_t, Clock::time_point) { heard.push_back ("beat"); });
    receiver.deliver ("10.0.0.1:5010", first, now);
    receiver.drain (now, 1.0);

    REQUIRE (got.has_value());
    CHECK (got->tick == 10);
    CHECK (got->leadTicks == said.leadTicks);
    CHECK (heard == std::vector<std::string> { "record", "clock", "beat" });

    //  A FOLLOWER'S WORD, alone, reaches the leader's half: its Δ, read on the next tick.
    CHECK_FALSE (stream.followerClock().has_value());
    pair::wire::ClockSaid fromFollower;
    fromFollower.delay = 2;
    fromFollower.outputLatency = 0.0015;
    stream.received ("ws:a", packetOf (pair::wire::clockBytes (fromFollower)), now);
    stream.afterTick (13, 1.0);
    REQUIRE (stream.followerClock().has_value());
    CHECK (stream.followerClock()->delay == 2);
    CHECK (stream.followerClock()->outputLatency == doctest::Approx (0.0015));
}

//==============================================================================
namespace
{
    /*  THE AUDIO SIDE WITH NOBODY LISTENING, as SeatTests has it: an arm
        completes the tick after it is asked, a launched voice plays until it is
        stopped. Each arm asked for kept, with the offset it asked. */
    struct QuietPlayer final : cue::Player
    {
        int trackCount() const override                     { return 64; }
        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); asked.push_back (request); }
        int slotCount() const override                      { return 2; }
        bool launchAtSample (int track, int, std::int64_t) override { playing.insert (track); return true; }
        bool stop (int track) override                      { playing.erase (track); return true; }
        bool stopAtSample (int track, int, std::int64_t) override { playing.erase (track); return true; }
        void setLevelDb (int, double) override              {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override           { return playing.count (track) > 0; }
        bool isArmReady (int track) const override          { return ready.count (track) > 0; }
        std::int64_t samplesElapsed() const override        { return samples; }
        int blockSize() const override                      { return 128; }
        int sampleRate() const override                     { return 48000; }
        int channelsPerTrack() const override               { return 2; }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed", { s (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        std::vector<cue::ArmRequest> arms, asked;
        std::set<int> ready, playing;
        std::int64_t samples = 0;
    };

    //  ONE MACHINE, as SeatTests' is: its engine, its show from one text, its runs and its sound.
    struct Machine
    {
        explicit Machine (const std::string& showText)
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::CanonicalXml::read (showText, document).ok);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            pair::registerPairCommands (engine.commands(), pairing);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            runner.setDcas (&dcas);
            runner.setLanes (&lanes);

            engine.setBeforeApply ([this] (const Command& command, const Event& event,
                                           const std::vector<osc::Value>& args, std::int64_t tick)
                                   { cue::beginUndoStep (document, nullptr, command, event, args, tick); });
        }

        /*  One tick, as serve runs one, its count at the tick's sample and
            `within` past it - the count moves a block at a time. */
        void tickOnce (std::int64_t tick, bool hooks = true, std::int64_t within = 0)
        {
            audio.samples = tick * 960 + within;

            if (hooks)
                runner.beforeTick (engine, tick);

            engine.processTick (tick);
            audio.completeArms (engine);
        }

        void standby (const std::string& listId, const std::string& cueId)
        {
            REQUIRE (engine.submit (std::string (origin::cli), "node.set", { s (cue::standbyAddressOf (listId)), s (cueId) }));
        }

        void go() { REQUIRE (engine.submit (std::string (origin::cli), "go")); }

        //  The run of a cue whose launch was placed, the last made; null for none.
        const cue::Run* launchedOf (const std::string& cueId) const
        {
            const cue::Run* found = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId && run.launchedAtSample > 0)
                    found = &run;

            return found;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
        QuietPlayer audio;
        cue::DcaTable dcas;
        cue::LaneTable lanes;
        pair::Pairing pairing;
    };

    /*  A bed with no ranges, a click, and a timeline scene: rain at once,
        thunder two seconds in. */
    struct Show
    {
        Show()
        {
            doc::ShowDocument show;
            list = show.createList ("Sound").id;

            bed = show.createCue (list, 0, "media", "Bed").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + bed + "/file", "bed.wav").ok);
            click = show.createCue (list, 1, "media", "Click").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + click + "/file", "click.wav").ok);

            scene = show.createCue (list, 2, "group", "Scene").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
            rain = show.createCue (scene, 0, "media", "Rain").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);
            thunder = show.createCue (scene, 1, "media", "Thunder").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + thunder + "/file", "thunder.wav").ok);
            REQUIRE (show.setAttribute ("/godot/cue/" + thunder + "/preWait", "2").ok);

            REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0").ok);
            text = doc::CanonicalXml::write (show);
        }

        std::string text, list, bed, click, scene, rain, thunder;
    };

    /*  TWO SOUND LINES OF ONE CRYSTAL, the follower's sample nought
        `earlierMs` before the leader's, the host clocks agreed: a trusted map
        that puts the follower's count that many milliseconds' samples on. */
    pair::ClockMap mapAhead (double earlierMs)
    {
        pair::SoundLine leaderLine;
        leaderLine.valid = true;
        leaderLine.rate = 48000;
        leaderLine.sample = 0;
        leaderLine.nanos = 1'000'000'000'000;
        leaderLine.nanosPerSample = 1.0e9 / 48000.0;
        leaderLine.errorNanos = 10.0;
        leaderLine.seconds = 100.0;

        auto followerLine = leaderLine;
        followerLine.nanos -= static_cast<std::int64_t> (earlierMs * 1.0e6);

        pair::OffsetLine hosts;
        hosts.valid = true;
        hosts.at = 1'000'000'000'000;
        hosts.samples = 5;
        hosts.errorNanos = 100.0;

        const auto map = pair::ClockMap::compose (leaderLine, followerLine, hosts, pair::ClockMap::MeasuredBy::follower);
        REQUIRE (map.trusted());
        return map;
    }
}

TEST_CASE ("same sample: one function places every launch - as it always was alone, on the tick grid with the extra lead when leading, with the leader's extra lead unmapped, on the leader's sample mapped, and moved into its file when that sample has gone")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine machine { show.text };
    CHECK (machine.runner.latencyTicks() == 2);

    cue::Runner::LaunchPlacing placing;
    auto expectedIntoFile = false;
    std::function<std::int64_t (std::int64_t launchTick)> expected;

    SUBCASE ("alone: the launch latency ahead of the count as the tick found it")
    {
        expected = [] (std::int64_t at) { return at * 960 + 37 + 2 * 960; };
    }

    SUBCASE ("leading with a follower: on the tick grid, the latency and the extra lead on, wherever the count stood")
    {
        placing.onTicks = true;
        placing.extraLeadTicks = 1;
        expected = [] (std::int64_t at) { return (at + 2 + 1) * 960; };
    }

    SUBCASE ("following, not mapped: the leader's extra lead added, on the record clock")
    {
        placing.extraLeadTicks = 1;
        expected = [] (std::int64_t at) { return at * 960 + 37 + 3 * 960; };
    }

    SUBCASE ("following, mapped: on the leader's sample, read through the map")
    {
        placing.mapped = true;
        placing.map = mapAhead (40.0);
        placing.offsetTicks = 3;
        placing.leaderSamplesPerTick = 960;
        placing.leaderLeadTicks = 3;
        expected = [] (std::int64_t at) { return (at - 3 + 3) * 960 + 1920; };
    }

    SUBCASE ("following, mapped, the leader's sample already gone: moved into its file and launched a seat's lead on")
    {
        placing.mapped = true;
        placing.map = mapAhead (-5.0);
        placing.offsetTicks = 3;
        placing.leaderSamplesPerTick = 960;
        placing.leaderLeadTicks = 3;
        expectedIntoFile = true;
        expected = [] (std::int64_t at) { return (at + 3 + 10 - 3) * 960 - 240; };
    }

    machine.runner.setLaunchPlacing (placing);

    const auto within = placing.mapped ? std::int64_t { 0 } : std::int64_t { 37 };

    machine.standby (show.list, show.click);
    machine.tickOnce (10, true, within);
    machine.go();

    std::int64_t firstTry = -1, launchedAt = -1;

    for (std::int64_t tick = 11; tick < 60 && launchedAt < 0; ++tick)
    {
        machine.tickOnce (tick, true, within);

        if (firstTry < 0 && machine.runner.lateLaunches() > 0)
            firstTry = tick;

        if (machine.launchedOf (show.click) != nullptr)
            launchedAt = tick;
    }

    REQUIRE (launchedAt >= 0);
    const auto* run = machine.launchedOf (show.click);
    REQUIRE (run != nullptr);

    if (expectedIntoFile)
    {
        //  COUNTED, MOVED INTO ITS FILE BY THE LATENESS, and launched where the leader's is then.
        REQUIRE (firstTry >= 0);
        CHECK (machine.runner.lateLaunches() == 1);
        CHECK (run->launchedAtSample == expected (firstTry));
        REQUIRE (! machine.audio.asked.empty());
        CHECK (machine.audio.asked.back().startOffset == doctest::Approx (0.2).epsilon (1.0e-9));
    }
    else
    {
        CHECK (machine.runner.lateLaunches() == 0);
        CHECK (run->launchedAtSample == expected (launchedAt));
    }

    /*  ON THE RECORD CLOCK: a mapped sound sounds the record lag early - here
        a tick, the leader's tick of the same record moment 40 ms before this
        one's and its lead a tick longer - and a summary to lay against the
        leader's takes it back off. */
    if (placing.mapped && ! expectedIntoFile)
    {
        CHECK (run->recordLag == doctest::Approx (0.02));
        const auto id = run->id;
        machine.tickOnce (launchedAt + 20);

        const auto playheadOf = [&id] (const pair::Summary& summary)
        {
            for (const auto& one : summary.runs)
                if (one.id == id)
                    return one.playhead;

            return std::int64_t { -999 };
        };

        CHECK (playheadOf (machine.runner.summaryNow (launchedAt + 20, 0)) == 18);
        CHECK (playheadOf (machine.runner.summaryNow (launchedAt + 20, 0, true)) == 17);
    }
    else if (! expectedIntoFile)
    {
        CHECK (std::abs (run->recordLag) < 1.0e-12);
    }
}

//==============================================================================
namespace
{
    /*  THE WIRE AS A TCP CONNECTION IS, SeatTests' own: in order, each frame
        no sooner than the one before it, up to 3 ms after it left. */
    struct JitteryWire
    {
        struct Frame
        {
            Clock::time_point at;
            osc::Packet packet;
        };

        explicit JitteryWire (unsigned seed) : random (seed) {}

        pair::Stream::Send send()
        {
            return [this] (const std::string&, const std::vector<std::uint8_t>& bytes)
            {
                const auto decoded = osc::decode (bytes.data(), bytes.size());

                if (! decoded.ok)
                    return false;

                auto at = sentAt + std::chrono::microseconds (jitter (random));
                at = std::max (at, lastArrival);
                lastArrival = at;
                frames.push_back ({ at, decoded.packet });
                return true;
            };
        }

        void deliverUpTo (Clock::time_point now, pair::Receiver& receiver)
        {
            while (! frames.empty() && frames.front().at <= now)
            {
                receiver.deliver ("127.0.0.1:5010", frames.front().packet, frames.front().at);
                frames.pop_front();
            }
        }

        std::mt19937 random;
        std::uniform_int_distribution<int> jitter { 0, 3000 };
        Clock::time_point sentAt {}, lastArrival {};
        std::deque<Frame> frames;
    };
}

TEST_CASE ("same sample: two engines on their own clocks - a seated bed, a GO and a timeline member start on the leader's sample within a block, in step on the record clock; never mapped, a GO starts a fixed delay behind")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    auto pinged = true;
    SUBCASE ("the map trusted")                { pinged = true; }
    SUBCASE ("the map never trusted: no ping") { pinged = false; }

    const Show show;
    Machine main { show.text }, backup { show.text };
    backup.runIds.drawSeed();
    backup.runner.runNames().setSalt (backup.runner.runNames().salt() ^ 0x0f0f0f0fu);

    /*  THE TWO MACHINES' CLOCKS: the main's sound clock counting from true
        time nought, the backup's from 3.71 s and 30 parts per million fast;
        their host clocks days apart, twelve parts per million fast and seven
        slow. Each machine's tick k is when its own count reaches k ticks. */
    SimClocks mainClocks;
    mainClocks.hostBase = 5.2e14;
    mainClocks.hostDrift = 12.0e-6;
    mainClocks.block = 128;

    SimClocks backupClocks;
    backupClocks.hostBase = 3.3e13;
    backupClocks.hostDrift = -7.0e-6;
    backupClocks.soundStart = 3.71;
    backupClocks.soundDrift = 30.0e-6;
    backupClocks.block = 128;

    const auto mainTau = [&mainClocks] (std::int64_t tick) { return mainClocks.tauOfCount (static_cast<double> (tick) * 960.0); };
    const auto backupTau = [&backupClocks] (std::int64_t tick) { return backupClocks.tauOfCount (static_cast<double> (tick) * 960.0); };

    //  The link's own clock, each machine's steady clock as the link reads it: true time, here.
    const auto t0 = Clock::time_point {} + std::chrono::hours { 1000 };
    const auto linkAt = [t0] (double tau)
    {
        return t0 + std::chrono::duration_cast<Clock::duration> (std::chrono::duration<double> (tau));
    };

    //  THE LEADER'S HALF, as serve wires it.
    pair::Stream stream { "S1" };
    stream.setLeading (true);
    main.engine.setAfterRecord ([&stream] (const LogRecord& record, const Command* command) { stream.keep (record, command); });
    stream.setPositionMaker ([&main] (std::int64_t at, std::uint64_t counter)
                             { return pair::toText (main.runner.positionNow (at, counter)); });

    JitteryWire wire { 31 };
    stream.setSend (wire.send());

    //  THE FOLLOWER'S, as serve wires it - its clocks too.
    pair::Receiver receiver;
    pair::Follower following { [&backup] (Event event) { return backup.engine.submit (std::move (event)); } };
    pair::SoundLineFit mainLine, backupLine;
    pair::HostOffset backupPings;
    pair::SoundLine leaderLine;
    SimWire pings { 5 };

    receiver.setOnRecord ([&following] (std::uint64_t, const LogRecord& record) { following.record (record); });
    receiver.setOnBeat ([&following] (std::int64_t leaderTick, Clock::time_point at) { following.beat (leaderTick, at); });
    receiver.setOnPosition ([&following] (std::uint64_t, std::int64_t leaderTick, std::string text)
                            { following.position (leaderTick, std::move (text)); });
    receiver.setOnClock ([&following, &leaderLine] (const pair::wire::ClockSaid& said)
                         {
                             leaderLine = said.line;
                             following.clockSaid (said);
                         });

    pair::FollowGate gate { backup.engine.commands(), [] (std::vector<std::uint8_t>) { return true; } };
    backup.engine.setGate ([&gate] (Event& event) { return gate.take (event); });
    backup.engine.setAfterRecord ([&following] (const LogRecord& record, const Command*) { following.applied (record); });

    std::int64_t mainTick = 0, localTick = 0;
    auto joined = false, trusted = false;
    pair::ClockMap map;
    std::int64_t mainSaidAt = -1000, backupSaidAt = -1000;
    int mainSaidLead = -1, backupSaidDelay = -1;

    std::map<std::int64_t, pair::Summary> mainSummaries;
    std::int64_t summariesCompared = 0, summariesApart = 0, seatedAt = -1;
    std::string firstApart;

    while (mainTick <= 1100)
    {
        if (mainTau (mainTick) <= backupTau (localTick))
        {
            const auto tau = mainTau (mainTick);

            mainLine.feed (mainClocks.stampAt (tau, 0.0), 48000);

            //  ITS LAUNCHES ON ITS GRID, THE FOLLOWER'S Δ ON, while it has one.
            cue::Runner::LaunchPlacing placing;

            if (stream.followerCount() > 0)
            {
                const auto& heard = stream.followerClock();
                placing.onTicks = true;
                placing.extraLeadTicks = heard.has_value() && heard->delay > 0 ? heard->delay : 1;
            }

            main.runner.setLaunchPlacing (placing);

            if (mainTick == 60)  main.standby (show.list, show.bed);
            if (mainTick == 61)  main.go();
            if (mainTick == 600) main.standby (show.list, show.click);
            if (mainTick == 601) main.go();
            if (mainTick == 800) main.standby (show.list, show.scene);
            if (mainTick == 801) main.go();

            main.tickOnce (mainTick);
            mainSummaries[mainTick] = main.runner.summaryNow (mainTick, 0);

            //  WHAT IT SAYS OF ITS CLOCKS, in this tick's bundle.
            pair::wire::ClockSaid said;
            said.tick = mainTick;
            said.samplesPerTick = 960;
            said.leadTicks = main.runner.latencyTicks() + placing.extraLeadTicks;
            said.onTicks = placing.onTicks;
            said.extraTicks = placing.extraLeadTicks;
            said.outputLatency = 0.0103;
            said.line = mainLine.line();

            if (said.leadTicks != mainSaidLead || mainTick - mainSaidAt >= 25)
            {
                stream.sayClock (pair::wire::clockBytes (said));
                mainSaidLead = said.leadTicks;
                mainSaidAt = mainTick;
            }

            wire.sentAt = linkAt (tau) + 2ms;
            stream.afterTick (mainTick, 1.0);
            ++mainTick;
            continue;
        }

        const auto tau = backupTau (localTick);
        const auto now = linkAt (tau);

        //  THE BACKUP JOINS while the bed plays: hello from now, and follows.
        if (! joined && mainTick >= 250)
        {
            pair::wire::Hello hello;
            hello.machine = "backup";
            hello.role = "backup";
            hello.lastCounter = pair::wire::fromNow;
            hello.stream = "S1";
            hello.state = "following";
            hello.ownStream = "S2";
            hello.described = true;
            stream.received ("ws:127.0.0.1:50001", packetOf (pair::wire::helloBytes (hello)), now);
            receiver.startAfresh ("S1");
            following.start();
            backup.engine.setGated (true);
            joined = true;
        }

        wire.deliverUpTo (now, receiver);
        receiver.drain (now, 1.0);

        backupLine.feed (backupClocks.stampAt (tau, 0.0), 48000);

        //  ITS PINGS, the dialler's, from the moment it starts.
        if (pinged && localTick % 5 == 0)
            backupPings.add (pings.ping (tau, backupClocks, mainClocks));

        //  THE MAP, before the leader's records of this tick go in.
        if (joined)
        {
            map = pair::ClockMap::compose (leaderLine, backupLine.line(), backupPings.line(),
                                           pair::ClockMap::MeasuredBy::follower, trusted);
            trusted = map.trusted();
            following.setClockMap (map);
        }

        const auto seatsBefore = following.seats();
        following.beforeTick (localTick, now, 0.0004);

        //  WHERE ITS LAUNCHES GO, as serve says it.
        cue::Runner::LaunchPlacing placing;

        if (following.isFollowing() && following.delayFixed())
            if (const auto* said = following.leaderClock(); said != nullptr && said->onTicks)
            {
                placing.extraLeadTicks = said->extraTicks;

                if (trusted && said->samplesPerTick > 0)
                {
                    placing.mapped = true;
                    placing.map = map;
                    placing.offsetTicks = following.offset();
                    placing.leaderTickZero = said->tickZero;
                    placing.leaderSamplesPerTick = said->samplesPerTick;
                    placing.leaderLeadTicks = said->leadTicks;
                }
            }

        backup.runner.setLaunchPlacing (placing);
        backup.tickOnce (localTick, following.seats() == seatsBefore);

        if (seatedAt < 0 && following.seats() > 0)
            seatedAt = localTick;

        //  ITS Δ, SAID TO THE LEADER.
        if (joined && following.delayFixed()
              && (following.delay() != backupSaidDelay || localTick - backupSaidAt >= 25))
        {
            pair::wire::ClockSaid said;
            said.tick = localTick;
            said.delay = following.delay();
            said.outputLatency = 0.0015;
            stream.received ("ws:127.0.0.1:50001", packetOf (pair::wire::clockBytes (said)), now);
            backupSaidDelay = following.delay();
            backupSaidAt = localTick;
        }

        /*  IN STEP ON THE RECORD CLOCK, every tick once the seat has settled:
            the leader's summary of the record moment this tick applies, and
            this machine's made to be laid against it. */
        if (seatedAt >= 0 && localTick > seatedAt + backup.runner.seatSettleTicks())
            if (const auto theirs = mainSummaries.find (localTick - following.offset()); theirs != mainSummaries.end())
            {
                ++summariesCompared;

                if (const auto apart = pair::differenceOf (theirs->second, backup.runner.summaryNow (localTick, 0, true),
                                                           pair::Labels {});
                    ! apart.empty())
                {
                    ++summariesApart;

                    if (firstApart.empty())
                        firstApart = "local tick " + std::to_string (localTick) + ": " + apart;
                }
            }

        ++localTick;
    }

    //  A SAMPLE OF THE BACKUP'S, as the main's count truly was at that moment.
    const auto onMain = [&mainClocks, &backupClocks] (std::int64_t backupSample)
    {
        return mainClocks.countAt (backupClocks.tauOfCount (static_cast<double> (backupSample)));
    };

    const auto* mainClick = main.launchedOf (show.click);
    const auto* backupClick = backup.launchedOf (show.click);
    REQUIRE (mainClick != nullptr);
    REQUIRE (backupClick != nullptr);
    CHECK (mainClick->id == backupClick->id);

    const auto clickApart = onMain (backupClick->launchedAtSample) - static_cast<double> (mainClick->launchedAtSample);

    MESSAGE ("pinged " << pinged << ": offset " << following.offset() << ", delay " << following.delay()
             << ", the main's extra lead " << main.runner.launchPlacing().extraLeadTicks << ", mapped "
             << backup.runner.launchPlacing().mapped << ", drift " << map.driftPartsPerMillion() << " ppm, residual "
             << map.residualMicroseconds() << " us; the click " << clickApart << " samples apart; summaries "
             << summariesCompared << " compared, " << summariesApart << " apart " << firstApart);

    CHECK (following.delayFixed());
    CHECK (main.runner.launchPlacing().onTicks);
    CHECK (main.runner.launchPlacing().extraLeadTicks == following.delay());
    CHECK (seatedAt >= 0);

    //  IN STEP ON THE RECORD CLOCK, mapped or not - a launch a tick apart read once or twice at most.
    CHECK (summariesCompared > 500);
    CHECK (summariesApart <= 2);

    if (pinged)
    {
        REQUIRE (backup.runner.launchPlacing().mapped);
        CHECK (map.driftPartsPerMillion() == doctest::Approx (30.0).epsilon (0.1));

        //  A GO: the same sample, within a block.
        CHECK (std::abs (clickApart) <= 128.0);

        //  A TIMELINE MEMBER two seconds in, and the one at once.
        for (const auto* cueId : { &show.thunder, &show.rain })
        {
            const auto* theirs = main.launchedOf (*cueId);
            const auto* ours = backup.launchedOf (*cueId);
            REQUIRE (theirs != nullptr);
            REQUIRE (ours != nullptr);
            const auto apart = onMain (ours->launchedAtSample) - static_cast<double> (theirs->launchedAtSample);
            INFO ("cue " << *cueId << ": " << apart << " samples apart");
            CHECK (std::abs (apart) <= 128.0);
        }

        /*  THE SEATED BED: launched where the main's bed has reached the second
            it was armed at. */
        const auto* mainBed = main.launchedOf (show.bed);
        const auto* backupBed = backup.launchedOf (show.bed);
        REQUIRE (mainBed != nullptr);
        REQUIRE (backupBed != nullptr);
        CHECK (mainBed->id == backupBed->id);

        const auto mainThere = static_cast<double> (mainBed->launchedAtSample)
                             + (backupBed->positionOrigin - mainBed->positionOrigin) * 48000.0;
        const auto bedApart = onMain (backupBed->launchedAtSample) - mainThere;
        INFO ("the bed seated at " << backupBed->positionOrigin << " s, " << bedApart << " samples apart");
        CHECK (backupBed->positionOrigin > 3.0);
        CHECK (std::abs (bedApart) <= 128.0);
    }
    else
    {
        /*  NEVER MAPPED: the record clock - the GO behind the main's by the
            ticks the records are applied behind, Δ to the nearest tick, as
            before RD.9: the slot's offset less the two tick clocks' own, as
            the beats measured it. */
        CHECK_FALSE (backup.runner.launchPlacing().mapped);
        const auto behindTicks = clickApart / 960.0;
        const auto recordLag = static_cast<double> (following.offset()) - following.clockOffset();
        INFO ("behind " << behindTicks << " ticks, the record lag " << recordLag << ", delay " << following.delay());
        CHECK (behindTicks >= 0.5);
        CHECK (std::abs (behindTicks - recordLag) <= 0.35);
        CHECK (std::abs (behindTicks - following.delay()) <= 0.75);
    }
}
