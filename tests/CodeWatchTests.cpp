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
    A code's watcher and its records (namespace draft §60, TG.5; AIL, AIO,
    AIP, AIQ, AIR).

    What is pinned here, fed made-up observations on a clock of 960 samples a
    tick (48 kHz at 50 Hz) - LTC-like, one frame at a time with its fraction of
    a sample, handed over at the first tick after it was heard whole; MTC-like,
    eight pieces a cycle, each with a driver's jitter, handed over when the
    cycle's last piece has come:
      - a code locks with ONE `jumped` and says nothing more while it runs, at
        all six rates, the map on the frames' own samples;
      - fifty parts per million fast for an hour writes a handful of
        `running` - counted - each map within half a millisecond of the code,
        and the speed learnt; MTC with half a millisecond of jitter likewise
        within a millisecond over ten minutes;
      - the count's line MTC is now placed through (CountLine.h): a young
        line read at the nominal slope, a pair far off it not fitted through,
        a count running late and catching up no slip, a slip told and the
        slope kept across it - and, held as MTC's reader holds it, a count
        late at one distance for a second no slip either;
      - and MTC made as serve makes it - Go.dot's own sender's messages
        arriving on the steady clock, read through the MTC input, placed on a
        count read a block at a time by a tick that runs late and stalls - a
        handful of records in ten minutes, each map within a millisecond
        (TG.11's faults, part 2);
      - nothing written while the show holds no time group on a code, its
        map kept all the same, and at once when one appears: `jumped` while
        it runs, `stopped` where it stopped;
      - a dropout shorter than four frames is invisible, a longer one is
        `stopped` where the code stopped, and the code coming back `jumped`;
      - a locate believed only once two frames agree; one corrupt frame, and
        two that disagree, never locate;
      - backwards, and at half speed;
      - the latency shift, and a latency changed re-anchoring;
      - the rate: 29.97 told from 30, and 23.976 from 24, by the frames'
        period, the show's rate not the code's;
      - a takeover's re-say;
      - and `code.heard` through the engine: the Runner's maps and the
        readouts from them, a record that does not read refused, a replay of
        the log reaching the same maps with no watcher at all.

    Every case runs under fr_FR as well as C: a record's speed is text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/midi/MtcInbox.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/CodeWatch.h>
#include <wfg/engine/timecode/CountLine.h>
#include <wfg/engine/timecode/LtcDecoder.h>
#include <wfg/engine/timecode/MtcReader.h>
#include <wfg/engine/timecode/MtcSchedule.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::timecode;

namespace
{
    constexpr int sampleRate = 48000;
    constexpr std::int64_t perTick = 960;

    double framesPerSecond (Rate rate)
    {
        return static_cast<double> (numOf (rate)) / static_cast<double> (denOf (rate));
    }

    std::int64_t frameNamed (const char* text, Rate rate)
    {
        const auto label = parseLabel (text);
        REQUIRE (label.has_value());
        return frameOf (*label, rate);
    }

    bool exactlyOne (double value)
    {
        return std::bit_cast<std::uint64_t> (value) == std::bit_cast<std::uint64_t> (1.0);
    }

    //==========================================================================
    /*  LTC AS THE DECODER HANDS IT OVER: a code counting at `rate`, its frames
        `fps` a second at `speed` (negative backwards), the frame `first`
        beginning at sample `zero` - or, backwards, ending there - each placed
        with up to `jitter` samples of error either way and handed over at the
        first tick after it was heard whole. `missing` frames are never heard,
        and `frameFor` says which frame the k-th one is, when not the next. */
    struct LtcLine
    {
        Rate rate = Rate::fps25;
        double fps = 25.0;
        double speed = 1.0;
        std::int64_t first = 0;
        double zero = 1000.3;
        double jitter = 0.0;
        std::mt19937 random { 7 };
        std::int64_t next = 0;

        std::function<bool (std::int64_t)> missing;
        std::function<std::int64_t (std::int64_t)> frameFor;

        double period() const { return static_cast<double> (sampleRate) / (fps * std::abs (speed)); }

        //  Where the k-th frame's start falls: its first edge forwards, its last backwards.
        double startOf (std::int64_t k) const
        {
            return zero + static_cast<double> (speed > 0.0 ? k : k + 1) * period();
        }

        double heardWholeAt (std::int64_t k) const
        {
            return speed > 0.0 ? startOf (k) + period() : startOf (k);
        }

        std::int64_t frameOfK (std::int64_t k) const
        {
            if (frameFor)
                return frameFor (k);

            return speed > 0.0 ? first + k : first - k;
        }

        LtcHeard frameAt (std::int64_t k)
        {
            LtcHeard heard;
            heard.label = labelOf (frameOfK (k), rate);
            heard.drop = dropsFrames (rate);
            heard.reverse = speed < 0.0;
            heard.parityOk = true;

            auto at = startOf (k);

            if (jitter > 0.0)
                at += std::uniform_real_distribution<double> (-jitter, jitter) (random);

            heard.startSample = static_cast<std::int64_t> (std::floor (at));
            heard.startFraction = static_cast<float> (at - std::floor (at));
            heard.bitPeriod = period() / 80.0;
            return heard;
        }

        void feed (CodeWatch& watch, std::int64_t now)
        {
            while (heardWholeAt (next) <= static_cast<double> (now))
            {
                if (! missing || ! missing (next))
                    watch.hear (frameAt (next));

                ++next;
            }
        }
    };

    /*  MTC AS THE READER HANDS IT OVER: a code counting by `rateCode`, its
        frames `fps` a second, cycles of eight pieces from the frame `first`
        (even), piece q sent q quarter frames after `zero` and arriving up to
        `jitter` seconds either side; each cycle handed over at the first tick
        after its last piece arrived, every piece an observation of its own. */
    struct MtcLine
    {
        int rateCode = 1;
        double fps = 25.0;
        double speed = 1.0;
        std::int64_t first = 0;
        double zero = 5000.0;
        double jitter = 0.0;
        std::mt19937 random { 11 };
        std::int64_t nextCycle = 0;
        std::array<double, 8> arrivals {};
        bool drawn = false;

        double quarter() const { return static_cast<double> (sampleRate) / (fps * speed * 4.0); }

        void draw()
        {
            for (int p = 0; p < 8; ++p)
            {
                auto at = zero + static_cast<double> (nextCycle * 8 + p) * quarter();

                if (jitter > 0.0)
                    at += std::uniform_real_distribution<double> (-jitter, jitter) (random) * sampleRate;

                arrivals[static_cast<std::size_t> (p)] = at;
            }

            drawn = true;
        }

        void feed (CodeWatch& watch, std::int64_t now)
        {
            const auto rate = rateOfMtcCode (rateCode);
            const auto perDay = framesPerDay (rate);

            for (;;)
            {
                if (! drawn)
                    draw();

                if (arrivals[7] > static_cast<double> (now))
                    return;

                const auto cycleFrame = first + 2 * nextCycle;

                for (int p = 0; p < 8; ++p)
                {
                    MtcObservation observation;
                    observation.frame = (cycleFrame + p / 4) % perDay;
                    observation.quarter = p % 4;
                    observation.label = labelOf (observation.frame, rate);
                    observation.rateCode = rateCode;
                    observation.sample = static_cast<std::int64_t> (std::llround (arrivals[static_cast<std::size_t> (p)]));
                    watch.hear (observation);
                }

                ++nextCycle;
                drawn = false;
            }
        }
    };

    struct Said
    {
        std::int64_t tick = 0;
        CodeSaid said;
    };

    //  A watcher on the test's clock, its grace a tick, and everything it said with the tick it said it on.
    struct Watching
    {
        explicit Watching (CodeWatch::Kind kind, Rate showRate = Rate::fps25)
            : watch (kind)
        {
            watch.setSampleRate (sampleRate);
            watch.setShowRate (showRate);
            watch.setGrace (perTick);
        }

        template <typename Line>
        void run (Line& line, std::int64_t fromTick, std::int64_t toTick)
        {
            for (auto n = fromTick; n < toTick; ++n)
            {
                line.feed (watch, n * perTick);
                sayAt (n);
            }
        }

        void sayAt (std::int64_t n)
        {
            if (auto said = watch.update (n * perTick))
                heard.push_back ({ n, *said });
        }

        std::size_t count (CodeTurn turn) const
        {
            return static_cast<std::size_t> (std::count_if (heard.begin(), heard.end(),
                                                            [turn] (const Said& s) { return s.said.what == turn; }));
        }

        std::string spelled() const
        {
            std::string out;

            for (const auto& s : heard)
                out += "tick " + std::to_string (s.tick) + ": " + std::string (wordOf (s.said.what)) + " "
                     + unitWordOf (s.said.map.unit) + " frame " + std::to_string (s.said.map.frame) + " sample "
                     + std::to_string (s.said.map.sample) + " speed " + std::to_string (s.said.map.speed) + "\n";

            return out;
        }

        CodeWatch watch;
        std::vector<Said> heard;
    };
}

//==============================================================================
TEST_CASE ("code watch: LTC locks with one jumped and says nothing more while it runs, at every rate, its map on the frames' own samples")
{
    for (const auto rate : { Rate::fps23976, Rate::fps24, Rate::fps25, Rate::fps2997df, Rate::fps2997, Rate::fps30 })
    {
        INFO ("rate " << std::string (wordOf (rate)));

        LtcLine line;
        line.rate = rate;
        line.fps = framesPerSecond (rate);
        line.first = frameNamed ("10:00:00:00", rate);

        Watching watching { CodeWatch::Kind::ltc, rate };
        watching.run (line, 1, 500);

        INFO (watching.spelled());
        REQUIRE (watching.heard.size() == 1u);

        const auto& lock = watching.heard.front();
        CHECK (lock.said.what == CodeTurn::jumped);
        CHECK (lock.said.map.state == CodeState::running);
        CHECK (unitWordOf (lock.said.map.unit) == std::string (wordOf (rate)));
        CHECK (exactlyOne (lock.said.map.speed));
        CHECK (watching.watch.state() == CodeState::running);

        //  Locked on the second frame heard: its tick is the first after that frame was whole.
        CHECK (lock.tick == static_cast<std::int64_t> (std::ceil (line.heardWholeAt (1) / static_cast<double> (perTick))));

        //  Every frame heard since falls, on the map, on its own first sample.
        for (std::int64_t k = 2; k < line.next; ++k)
        {
            const auto at = lock.said.map.sampleOf (line.frameOfK (k));
            REQUIRE (at.has_value());

            if (std::abs (static_cast<double> (*at) - std::ceil (line.startOf (k))) > 1.0)
            {
                FAIL_CHECK ("frame " << k << " placed on " << *at << ", heard from " << line.startOf (k));
                break;
            }
        }
    }
}

TEST_CASE ("code watch: fifty parts per million fast for an hour - a handful of running, each map within half a millisecond, the speed learnt")
{
    LtcLine line;
    line.speed = 1.00005;
    line.first = frameNamed ("01:00:00:00", Rate::fps25);
    line.jitter = 0.02;

    Watching watching { CodeWatch::Kind::ltc };

    double worst = 0.0;

    for (std::int64_t n = 1; n < 50 * 3600; ++n)
    {
        line.feed (watching.watch, n * perTick);
        watching.sayAt (n);

        //  Once a second, the map last said against where the code really is.
        if (n % 50 == 0 && ! watching.heard.empty() && line.next > 2)
        {
            const auto k = line.next - 1;
            const auto at = watching.heard.back().said.map.sampleOf (line.frameOfK (k));
            REQUIRE (at.has_value());
            worst = std::max (worst, std::abs (static_cast<double> (*at) - line.startOf (k)));
        }
    }

    INFO (watching.spelled());
    MESSAGE ("an hour at +50 ppm: " << watching.count (CodeTurn::running) << " running, the map at worst "
             << worst / 48.0 << " ms from the code");

    CHECK (watching.count (CodeTurn::jumped) == 1u);
    CHECK (watching.count (CodeTurn::stopped) == 0u);
    CHECK (watching.count (CodeTurn::running) >= 1u);
    CHECK (watching.count (CodeTurn::running) <= 5u);

    //  Half a millisecond is 24 samples; a sample more for the anchor's rounding and the jitter.
    CHECK (worst <= 26.0);

    //  The speed learnt to a fifth of a part per million.
    CHECK (watching.heard.back().said.map.speed == doctest::Approx (1.00005).epsilon (2.0e-7));
    CHECK (watching.watch.jitterSamples() < 0.05);
}

TEST_CASE ("code watch: MTC with half a millisecond of jitter - a handful of running in ten minutes, each map within a millisecond")
{
    MtcLine line;
    line.speed = 1.00005;
    line.first = frameNamed ("02:00:00:00", Rate::fps25);
    line.jitter = 0.0005;

    Watching watching { CodeWatch::Kind::mtc };

    double worst = 0.0;

    for (std::int64_t n = 1; n < 50 * 600; ++n)
    {
        line.feed (watching.watch, n * perTick);
        watching.sayAt (n);

        //  Once a second, the map's place for the last cycle's frame against when it was sent.
        if (n % 50 == 0 && ! watching.heard.empty() && line.nextCycle > 2)
        {
            const auto cycle = line.nextCycle - 1;
            const auto at = watching.heard.back().said.map.sampleOf (line.first + 2 * cycle);
            REQUIRE (at.has_value());
            worst = std::max (worst, std::abs (static_cast<double> (*at) - (line.zero + static_cast<double> (cycle * 8) * line.quarter())));
        }
    }

    INFO (watching.spelled());
    MESSAGE ("ten minutes of MTC at +50 ppm, jitter 0.5 ms: " << watching.count (CodeTurn::running)
             << " running, the map at worst " << worst / 48.0 << " ms from the code, jitter measured "
             << watching.watch.jitterSamples() / 48.0 << " ms");

    CHECK (watching.count (CodeTurn::jumped) == 1u);
    CHECK (watching.count (CodeTurn::stopped) == 0u);
    CHECK (watching.count (CodeTurn::running) <= 8u);
    CHECK (worst <= 48.0 + 24.0);
    CHECK (watching.heard.back().said.map.speed == doctest::Approx (1.00005).epsilon (2.0e-5));

    //  The jitter measured: half a millisecond uniform either way is 0.29 ms about the line.
    CHECK (watching.watch.jitterSamples() / 48.0 == doctest::Approx (0.29).epsilon (0.2));
}

//==============================================================================
namespace
{
    /*  MTC FROM A CABLE, THROUGH SERVE'S HOOK (namespace draft §60.7, TG.11's
        faults, part 2). The case above hands the watcher observations already
        on Go.dot's samples; this one makes them as serve does. Go.dot's own
        sender (MtcSchedule.h) writes the code, and each of its messages
        arrives on the steady clock with a MIDI driver's half millisecond
        either way; Go.dot's samples run on an interface's crystal of their
        own, twenty parts per million slow, counted a `block` at a time; and
        the hook is ticked fifty times a second, up to `lateMs` late - and, with
        `stalls`, held back every eight seconds by 30 to 130 ms and then run
        through the ticks it owes back to back, a third of a millisecond apart,
        as a Debug engine's tick thread is. Each tick does what serve's hook
        does: the tick's clock pair to the MTC input, everything the port
        brought since taken and read, the observations and the last piece's
        sample to the watcher, and its turn asked for at the count. The code
        runs `codeSpeed` against Go.dot's samples. */
    struct CableMtc
    {
        int block = 256;
        double lateMs = 3.0;
        bool stalls = false;
        double codeSpeed = 1.00005;
        double crystal = 1.0 - 20.0e-6;

        //  Where the code stops coming, in the steady clock's milliseconds; never by default.
        double stopMs = 1.0e300;

        std::mt19937 random { 23 };

        static constexpr double sampleZero = 3'000'000.0;
        static constexpr double codeStartMs = 1500.0;

        CableMtc()
        {
            input.configure ({ portId, true });
            first = frameNamed ("02:00:00:00", Rate::fps25);
            cursor = MtcCursor { Rate::fps25, sampleRate, first, 0 };
        }

        //  The engine's draws, the same on every standard library.
        double uniform (double low, double high)
        {
            return low + (high - low) * (static_cast<double> (random()) / 4294967296.0);
        }

        static std::int64_t nanos (double ms) { return 9'000'000'000LL + static_cast<std::int64_t> (std::llround (ms * 1.0e6)); }

        double trueSample (double ms) const { return sampleZero + ms * 48.0 * crystal; }

        double counted (double ms) const
        {
            return std::floor (trueSample (ms) / static_cast<double> (block)) * static_cast<double> (block);
        }

        //  The steady moment the code passes `frames` frames after its first.
        double momentOf (double frames) const { return codeStartMs + frames * 40.0 / (codeSpeed * crystal); }

        /*  WHERE A MAP SHOULD PUT `frames` frames after the first: the sample
            playing as the code passed it - less how far the count, read a
            block at a time, stood behind the samples on average, which is
            where any line through the pairs has to stand. */
        double expected (double frames) const
        {
            return trueSample (momentOf (frames)) - (pairs > 0 ? behind / static_cast<double> (pairs) : 0.0);
        }

        //  How far a map puts the frame `frames` after the first from where it should, in samples.
        double offBy (const CodeMap& map, std::int64_t frames) const
        {
            const auto at = map.sampleOf (first + frames);
            REQUIRE (at.has_value());
            return static_cast<double> (*at) - expected (static_cast<double> (frames));
        }

        //  The code's frame just heard at a steady moment, counted from the first.
        std::int64_t frameHeardAt (double ms) const
        {
            return static_cast<std::int64_t> (std::floor ((std::min (ms, stopMs) - codeStartMs)
                                                          * codeSpeed * crystal / 40.0)) - 2;
        }

        //  The moment the tick `n` is taken at, its stall and its lateness drawn.
        double momentOfTick (std::int64_t n)
        {
            const auto due = static_cast<double> (n) * 20.0 / crystal;

            if (stalls && n % 400 == 200)
                stallUntil = due + uniform (30.0, 130.0);

            const auto at = std::max ({ due + (lateMs > 0.0 ? uniform (0.0, lateMs) : 0.0), stallUntil, lastTick + 0.3 });
            lastTick = at;
            return at;
        }

        //  One tick of serve's hook, at the steady moment `ms`: what it said, if anything.
        std::optional<CodeSaid> tick (CodeWatch& watch, double ms)
        {
            const auto count = counted (ms);
            behind += trueSample (ms) - count;
            ++pairs;

            input.feedClock (static_cast<std::int64_t> (count), nanos (ms), sampleRate);

            for (;;)
            {
                const auto message = cursor.next();
                const auto due = codeStartMs + static_cast<double> (message.dueSample) / 48.0 / (codeSpeed * crystal);
                const auto arrival = std::max (lastArrival, due + uniform (-0.5, 0.5));

                if (arrival > ms)
                    break;

                if (due < stopMs)
                    inbox.push (portId, message.bytes.data(), static_cast<std::size_t> (message.size), nanos (arrival));

                lastArrival = arrival;
                cursor.advance();
            }

            input.drain (inbox);

            for (const auto& observation : input.observations())
                watch.hear (observation);

            watch.heardAt (input.lastHeardSample());
            return watch.update (static_cast<std::int64_t> (count));
        }

        inline static const std::string portId { "P1" };

        midi::MtcInbox inbox;
        midi::MtcInput input;
        MtcCursor cursor;
        std::int64_t first = 0;
        double stallUntil = 0.0;
        double lastTick = 0.0;
        double lastArrival = 0.0;
        double behind = 0.0;
        std::int64_t pairs = 0;
    };

    //  A watcher on MTC as serve sets it: its grace a tick and a block.
    CodeWatch cableWatch (int block)
    {
        CodeWatch watch { CodeWatch::Kind::mtc };
        watch.setSampleRate (sampleRate);
        watch.setShowRate (Rate::fps25);
        watch.setGrace (perTick + block);
        return watch;
    }
}

TEST_CASE ("count line: clock pairs on one straight line - a young line at the nominal slope, one pair far off not fitted through, a count late and catching up no slip, a slip told and the slope kept across it")
{
    /*  An interface twenty parts per million slow against the steady clock,
        counted 512 samples at a time, its pairs taken by a tick fifty times a
        second up to 3 ms late; `slip` samples lost for good from the count. */
    constexpr double crystal = 1.0 - 20.0e-6;
    constexpr double block = 512.0;
    std::mt19937 random { 29 };
    double slip = 0.0;
    double ms = 0.0;

    const auto uniform = [&random] (double low, double high)
    {
        return low + (high - low) * (static_cast<double> (random()) / 4294967296.0);
    };

    const auto nanos = [] (double at) { return 9'000'000'000LL + static_cast<std::int64_t> (std::llround (at * 1.0e6)); };
    const auto trueSample = [&slip] (double at) { return 5'000'000.0 + at * 48.0 * crystal - slip; };
    const auto counted = [&trueSample] (double at) { return static_cast<std::int64_t> (std::floor (trueSample (at) / block) * block); };

    //  The slope read off a line over a second, against the nominal: -20e-6 is the crystal.
    const auto slopeOf = [&nanos, &ms] (const CountLine& line)
    {
        return (line.sampleAt (nanos (ms + 1000.0)) - line.sampleAt (nanos (ms))) / 48000.0 - 1.0;
    };

    //  Where the line puts the moment `ms` against the truth, less the half block a count stands behind it.
    const auto placed = [&] (const CountLine& line) { return line.sampleAt (nanos (ms)) - (trueSample (ms) - block / 2.0); };

    CountLine line;
    CountLine twin;

    const auto tick = [&] (std::int64_t n, bool both)
    {
        ms = static_cast<double> (n) * 20.0 / crystal + uniform (0.0, 3.0);

        if (both)
            twin.feed (counted (ms), nanos (ms), sampleRate);

        return line.feed (counted (ms), nanos (ms), sampleRate);
    };

    //  A SECOND OF PAIRS: the line's own slope is anywhere within a part in a thousand; what is read is the nominal's.
    for (std::int64_t n = 1; n <= 50; ++n)
        CHECK_FALSE (tick (n, true));

    MESSAGE ("a second of 512-sample pairs: the line's own slope " << (line.line().slope / 48.0 - 1.0) * 1.0e6
             << " ppm, the slope read " << slopeOf (line) * 1.0e6 << " ppm, the crystal -20");
    CHECK (std::abs (slopeOf (line) + 20.0e-6) <= 30.0e-6);
    CHECK (std::abs (placed (line)) <= 48.0);

    //  TWO MINUTES ON: the crystal's slope to a part per million, the moment within half a millisecond.
    for (std::int64_t n = 51; n <= 6050; ++n)
        CHECK_FALSE (tick (n, true));

    CHECK (std::abs (slopeOf (line) + 20.0e-6) <= 1.0e-6);
    CHECK (std::abs (placed (line)) <= 24.0);

    /*  ONE PAIR FAR OFF - its count read 30 ms before its moment, a thread
        preempted between the two - is not fitted through: the twin, never fed
        it, reads the same to the last bit two seconds on. */
    ms = 6051.0 * 20.0 / crystal;
    CHECK_FALSE (line.feed (counted (ms - 30.0), nanos (ms), sampleRate));

    for (std::int64_t n = 6052; n <= 6150; ++n)
        CHECK_FALSE (tick (n, true));

    CHECK (std::bit_cast<std::uint64_t> (line.sampleAt (nanos (ms))) == std::bit_cast<std::uint64_t> (twin.sampleAt (nanos (ms))));

    /*  A COUNT RUNNING LATE - the hosted pump held back 600 ms on a loaded
        machine, then delivering its blocks eight times faster than they play
        until it is back on time, the tick taking a pair each time the count
        passes another tick's worth, as the tick thread does - is no slip, and
        the moment in the middle of it is read where the crystal puts it. */
    {
        const auto stallFrom = 6151.0 * 20.0 / crystal;
        const auto frozen = static_cast<double> (counted (stallFrom));
        auto due = frozen + 960.0;
        auto lateTold = false;
        auto latest = 0.0;

        auto at = stallFrom;

        for (; at < stallFrom + 2000.0; at += 0.25)
        {
            const auto catching = frozen + std::max (0.0, at - stallFrom - 600.0) * 48.0 * crystal * 8.0;
            const auto onTime = static_cast<double> (counted (at));
            const auto count = std::min (onTime, std::floor (catching / block) * block);

            //  Back on time: the tick goes on as before.
            if (at > stallFrom + 600.0 && count >= onTime)
                break;

            if (count >= due)
            {
                lateTold = lateTold || line.feed (static_cast<std::int64_t> (count), nanos (at), sampleRate);
                latest = std::min (latest, count - (trueSample (at) - block / 2.0));
                due = count + 960.0;
            }
        }

        MESSAGE ("a count 600 ms late, caught up eight times faster than it plays: " << -latest / 48.0
                 << " ms behind at worst, " << std::string (lateTold ? "told a slip" : "no slip"));
        CHECK (latest < -400.0 * 48.0);
        CHECK_FALSE (lateTold);

        ms = stallFrom + 300.0;
        CHECK (std::abs (placed (line)) <= 24.0);

        for (auto n = static_cast<std::int64_t> (std::ceil (at * crystal / 20.0)); n <= 6300; ++n)
            CHECK_FALSE (tick (n, false));
    }

    CHECK (std::abs (placed (line)) <= 24.0);

    //  A SLIP - a block lost for good - is told within eight pairs, and the line after it reads at the slope it had.
    slip = block;
    auto told = 0;

    for (std::int64_t n = 6301; n <= 6308 && told == 0; ++n)
        if (tick (n, false))
            told = static_cast<int> (n - 6300);

    CHECK (told > 0);

    for (std::int64_t n = 6309; n <= 6334; ++n)
        CHECK_FALSE (tick (n, false));

    MESSAGE ("a slip told on its pair " << told << "; half a second after, the slope read "
             << slopeOf (line) * 1.0e6 << " ppm, the moment " << placed (line) / 48.0 << " ms off");
    CHECK (std::abs (slopeOf (line) + 20.0e-6) <= 1.0e-6);
    CHECK (std::abs (placed (line)) <= 72.0);
}

TEST_CASE ("count line, held as MTC's reader holds it: a count 70 ms late at one distance for a second is no slip where a line that does not hold tells one; a buffer lost for good is told two seconds on")
{
    /*  An interface thirty parts per million fast, counted 128 samples at a
        time - the driver's blocks - by a tick up to 3 ms late; the count `late`
        milliseconds behind its deadlines (the hosted pump of a loaded machine)
        and `lost` samples short for good (a dropped buffer). */
    constexpr double crystal = 1.0 + 30.0e-6;
    constexpr double block = 128.0;
    std::mt19937 random { 31 };
    double late = 0.0;
    double lost = 0.0;
    double ms = 0.0;

    const auto uniform = [&random] (double low, double high)
    {
        return low + (high - low) * (static_cast<double> (random()) / 4294967296.0);
    };

    const auto nanos = [] (double at) { return 9'000'000'000LL + static_cast<std::int64_t> (std::llround (at * 1.0e6)); };
    const auto trueSample = [] (double at) { return 7'000'000.0 + at * 48.0 * crystal; };

    CountLine held { CountLine::lateHoldSeconds };
    CountLine prompt;
    auto heldTold = std::int64_t { 0 };
    auto promptTold = std::int64_t { 0 };

    const auto tick = [&] (std::int64_t n)
    {
        ms = static_cast<double> (n) * 20.0 / crystal + uniform (0.0, 3.0);
        const auto count = static_cast<std::int64_t> (std::floor ((trueSample (ms - late) - lost) / block) * block);

        if (held.feed (count, nanos (ms), sampleRate) && heldTold == 0)
            heldTold = n;

        if (prompt.feed (count, nanos (ms), sampleRate) && promptTold == 0)
            promptTold = n;
    };

    //  Where the held line puts the moment `ms` against where the crystal does, less what was lost and half a block.
    const auto placed = [&] { return held.sampleAt (nanos (ms)) - (trueSample (ms) - lost - block / 2.0); };

    for (std::int64_t n = 1; n <= 6000; ++n)
        tick (n);

    REQUIRE (heldTold == 0);
    REQUIRE (promptTold == 0);
    CHECK (std::abs (placed()) <= 24.0);

    //  LATE AT ONE DISTANCE for a second, then on time: the held line stands where it was.
    late = 70.0;

    for (std::int64_t n = 6001; n <= 6050; ++n)
    {
        tick (n);
        CHECK (std::abs (placed()) <= 24.0);
    }

    late = 0.0;

    for (std::int64_t n = 6051; n <= 6100; ++n)
        tick (n);

    MESSAGE ("70 ms late for a second: the line that does not hold told a slip on tick " << promptTold
             << ", the held one " << heldTold);
    CHECK (promptTold > 0);
    CHECK (heldTold == 0);
    CHECK (std::abs (placed()) <= 24.0);

    //  A DEVICE'S BUFFER LOST FOR GOOD, four blocks: told two seconds on and a few pairs, and read where it now is.
    lost = 4.0 * block;
    promptTold = 0;

    for (std::int64_t n = 6101; n <= 6300; ++n)
        tick (n);

    MESSAGE ("512 samples lost on tick 6101: told on tick " << heldTold << " by the held line, " << promptTold
             << " by the other");
    CHECK (heldTold >= 6101 + 100);
    CHECK (heldTold <= 6101 + 110);
    CHECK (std::abs (placed()) <= 24.0);
}

TEST_CASE ("code watch: MTC through the input, on a count read a block at a time by a tick up to 3 ms late and through its stalls - ten minutes at +50 ppm are a handful of records, each map within a millisecond")
{
    /*  TG.11'S FAULT (3): the MTC reader placed each moment on Go.dot's
        samples through a `ClockEstimate`, whose slope follows the tick's
        pairs one by one - and pairs a block apart in their noise, or a few
        back to back after a stall, each a few hundred microseconds after the
        last, walk it to its clamp of half a percent. The watcher's line through
        MTC wandered with it by a millisecond and more, and wrote a `running`
        each time. */
    for (const auto block : { 256, 512 })
        for (const auto stalls : { false, true })
        {
            INFO (block << "-sample blocks" << std::string (stalls ? ", the tick stalled every eight seconds" : ""));

            CableMtc cable;
            cable.block = block;
            cable.stalls = stalls;

            auto watch = cableWatch (block);
            std::vector<Said> heard;
            double worst = 0.0;

            for (std::int64_t n = 1; n < 50 * 600; ++n)
            {
                const auto ms = cable.momentOfTick (n);

                if (auto said = cable.tick (watch, ms))
                    heard.push_back ({ n, *said });

                //  Once a second, from the fifth: the map last said against where the code is.
                if (n % 50 == 0 && n > 250 && ! heard.empty())
                    worst = std::max (worst, std::abs (cable.offBy (heard.back().said.map, cable.frameHeardAt (ms))));
            }

            const auto count = [&heard] (CodeTurn turn)
            {
                return static_cast<std::size_t> (std::count_if (heard.begin(), heard.end(),
                                                                [turn] (const Said& s) { return s.said.what == turn; }));
            };

            std::string spelled;

            for (const auto& s : heard)
                spelled += "tick " + std::to_string (s.tick) + ": " + std::string (wordOf (s.said.what)) + " frame "
                         + std::to_string (s.said.map.frame) + " sample " + std::to_string (s.said.map.sample)
                         + " speed " + std::to_string (s.said.map.speed) + "\n";

            INFO (spelled);
            MESSAGE ("ten minutes of MTC through the input at +50 ppm, " << block << "-sample blocks"
                     << std::string (stalls ? ", stalls" : "") << ": " << heard.size() << " records, "
                     << count (CodeTurn::running) << " running, the map at worst " << worst / 48.0 << " ms from the code");

            CHECK (count (CodeTurn::jumped) == 1u);
            CHECK (count (CodeTurn::stopped) == 0u);
            CHECK (count (CodeTurn::running) <= 6u);

            //  A millisecond is 48 samples; half a millisecond more for the driver's jitter at the line's end.
            CHECK (worst <= 48.0 + 24.0);
            CHECK (heard.back().said.map.speed == doctest::Approx (1.00005).epsilon (2.0e-5));
        }
}

TEST_CASE ("code watch: nothing written while the show holds no time group on the code - its map kept all the same - and at once when one appears, jumped while it runs and stopped where it stopped")
{
    CableMtc cable;
    auto watch = cableWatch (cable.block);
    std::vector<Said> heard;

    const auto run = [&] (std::int64_t from, std::int64_t to)
    {
        for (auto n = from; n < to; ++n)
            if (auto said = cable.tick (watch, cable.momentOfTick (n)))
                heard.push_back ({ n, *said });
    };

    //  No group: half a minute of code, nothing said, nothing to say at a takeover - but the line kept.
    watch.setWriting (false);
    run (1, 1500);
    CHECK (heard.empty());
    CHECK_FALSE (watch.resay().has_value());
    REQUIRE (watch.map().state == CodeState::running);
    CHECK (std::abs (cable.offBy (watch.map(), cable.frameHeardAt (cable.lastTick))) <= 48.0);
    CHECK (watch.map().speed == doctest::Approx (1.00005).epsilon (5.0e-5));

    //  A group appears: `jumped` on the very next tick, on the line it kept.
    watch.setWriting (true);
    run (1500, 1501);
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].said.what == CodeTurn::jumped);
    CHECK (heard[0].said.map == watch.map());
    CHECK (std::abs (cable.offBy (heard[0].said.map, cable.frameHeardAt (cable.lastTick))) <= 48.0);

    //  Then as any code: little to say for half a minute.
    run (1501, 3000);
    CHECK (heard.size() <= 3u);

    //  The group gone and the code stopped: nothing said, the map stopped all the same.
    const auto said = heard.size();
    watch.setWriting (false);
    cable.stopMs = cable.lastTick + 100.0;
    run (3000, 3500);
    CHECK (heard.size() == said);
    REQUIRE (watch.map().state == CodeState::stopped);

    //  A group again: `stopped` at once, where it stopped.
    watch.setWriting (true);
    run (3500, 3501);
    REQUIRE (heard.size() == said + 1);
    CHECK (heard.back().said.what == CodeTurn::stopped);
    CHECK (heard.back().said.map == watch.map());

    //  And nothing again until the code comes back.
    run (3501, 3600);
    CHECK (heard.size() == said + 1);

    //  A takeover's re-say on the tick a group appears says it once, not twice.
    watch.setWriting (false);
    watch.setWriting (true);
    const auto resaid = watch.resay();
    REQUIRE (resaid.has_value());
    CHECK (resaid->what == CodeTurn::stopped);
    run (3600, 3601);
    CHECK (heard.size() == said + 1);
}

TEST_CASE ("code watch: a dropout shorter than four frames is invisible; a longer one is stopped where it stopped, and the code back is jumped")
{
    LtcLine line;
    line.first = frameNamed ("00:30:00:00", Rate::fps25);
    line.missing = [] (std::int64_t k) { return (k >= 100 && k < 103) || (k >= 200 && k < 212); };

    Watching watching { CodeWatch::Kind::ltc };
    watching.run (line, 1, 600);

    INFO (watching.spelled());
    REQUIRE (watching.heard.size() == 3u);

    const auto& stop = watching.heard[1];
    CHECK (watching.heard[0].said.what == CodeTurn::jumped);
    CHECK (stop.said.what == CodeTurn::stopped);
    CHECK (watching.heard[2].said.what == CodeTurn::jumped);

    //  Stopped on the last frame heard, standing, after four frames past the next one due and a tick.
    CHECK (stop.said.map.state == CodeState::stopped);
    CHECK (stop.said.map.frame == line.first + 199);
    CHECK (stop.said.map.sample == static_cast<std::int64_t> (std::ceil (line.startOf (199))));
    CHECK (stop.said.map.speed == doctest::Approx (0.0));

    const auto silentFrom = line.heardWholeAt (199);
    CHECK (static_cast<double> (stop.tick * perTick) > silentFrom + 5.0 * line.period());
    CHECK (static_cast<double> (stop.tick * perTick) <= silentFrom + 5.0 * line.period() + 2.0 * perTick);

    //  Back: believed on its second frame, 213.
    const auto& back = watching.heard[2];
    CHECK (back.said.map.frame == line.first + 213);
    CHECK (std::abs (static_cast<double> (back.said.map.sample) - std::ceil (line.startOf (213))) <= 1.0);
    CHECK (exactlyOne (back.said.map.speed));
}

TEST_CASE ("code watch: a locate is believed once two frames agree; one corrupt frame, or two that disagree, never locates")
{
    LtcLine line;
    line.first = frameNamed ("05:00:00:00", Rate::fps25);
    const auto first = line.first;

    line.frameFor = [first] (std::int64_t k)
    {
        if (k == 100)
            return first + 5000;                // one corrupt frame

        if (k == 150)
            return first + 7000;                // two that disagree with each other

        if (k == 151)
            return first + 9000;

        if (k >= 200)
            return first + 30'000 + k;          // a locate, and the code runs on from there

        return first + k;
    };

    Watching watching { CodeWatch::Kind::ltc };
    watching.run (line, 1, 600);

    INFO (watching.spelled());
    REQUIRE (watching.heard.size() == 2u);

    const auto& locate = watching.heard[1];
    CHECK (locate.said.what == CodeTurn::jumped);
    CHECK (locate.said.map.frame == first + 30'201);
    CHECK (std::abs (static_cast<double> (locate.said.map.sample) - std::ceil (line.startOf (201))) <= 1.0);

    //  Said on the tick the second frame was heard whole, and not the first's.
    CHECK (locate.tick == static_cast<std::int64_t> (std::ceil (line.heardWholeAt (201) / static_cast<double> (perTick))));
    CHECK (exactlyOne (locate.said.map.speed));
}

TEST_CASE ("code watch: a code played backwards, and one at half speed")
{
    SUBCASE ("backwards")
    {
        LtcLine line;
        line.speed = -1.0;
        line.first = frameNamed ("01:00:00:00", Rate::fps25);

        Watching watching { CodeWatch::Kind::ltc };
        watching.run (line, 1, 500);

        INFO (watching.spelled());
        REQUIRE (watching.heard.size() == 1u);

        const auto& map = watching.heard.front().said.map;
        CHECK (watching.heard.front().said.what == CodeTurn::jumped);
        CHECK (map.speed == doctest::Approx (-1.0).epsilon (1.0e-6));

        //  Frame 200 back is reached from above on its own start sample.
        const auto at = map.sampleOf (line.frameOfK (200));
        REQUIRE (at.has_value());
        CHECK (std::abs (static_cast<double> (*at) - std::ceil (line.startOf (200))) <= 1.0);
    }

    SUBCASE ("half speed")
    {
        LtcLine line;
        line.speed = 0.5;
        line.first = frameNamed ("01:00:00:00", Rate::fps25);

        Watching watching { CodeWatch::Kind::ltc };
        watching.run (line, 1, 1000);

        INFO (watching.spelled());
        REQUIRE (watching.heard.size() == 1u);

        const auto& map = watching.heard.front().said.map;
        CHECK (map.speed == doctest::Approx (0.5).epsilon (1.0e-5));
        CHECK (unitWordOf (map.unit) == "25");

        const auto at = map.sampleOf (line.frameOfK (200));
        REQUIRE (at.has_value());
        CHECK (std::abs (static_cast<double> (*at) - std::ceil (line.startOf (200))) <= 1.0);
    }
}

TEST_CASE ("code watch: the latency shift - LTC heard its input's and output's latency before it came in, and a latency changed re-anchors")
{
    LtcLine line;
    line.first = frameNamed ("01:00:00:00", Rate::fps25);

    Watching plain { CodeWatch::Kind::ltc };
    Watching shifted { CodeWatch::Kind::ltc };
    shifted.watch.setLatency (100 + 156);

    LtcLine again = line;
    plain.run (line, 1, 100);
    shifted.run (again, 1, 100);

    REQUIRE (plain.heard.size() == 1u);
    REQUIRE (shifted.heard.size() == 1u);

    //  The same frame, heard 256 samples earlier than it came in.
    CHECK (shifted.heard[0].said.map.frame == plain.heard[0].said.map.frame);
    CHECK (shifted.heard[0].said.map.sample == plain.heard[0].said.map.sample - 256);

    //  The interface changed: the next tick re-anchors on the new shift, the line the same.
    shifted.watch.setLatency (512);
    shifted.run (again, 100, 101);
    REQUIRE (shifted.heard.size() == 2u);
    CHECK (shifted.heard[1].said.what == CodeTurn::running);

    const auto frame = line.frameOfK (90);
    const auto before = shifted.heard[0].said.map.sampleOf (frame);
    const auto after = shifted.heard[1].said.map.sampleOf (frame);
    REQUIRE (before.has_value());
    REQUIRE (after.has_value());
    CHECK (*after - *before == -256);
}

TEST_CASE ("code watch: the rate - 29.97 told from 30 and 23.976 from 24 by the frames' period, whatever the show's rate")
{
    struct Case
    {
        const char* name;
        Rate counting;
        double fps;
        const char* expected;
    };

    for (const auto& c : { Case { "29.97 counting to 30", Rate::fps30, 30000.0 / 1001.0, "29.97" },
                           Case { "30", Rate::fps30, 30.0, "30" },
                           Case { "23.976 counting to 24", Rate::fps24, 24000.0 / 1001.0, "23.976" },
                           Case { "24", Rate::fps24, 24.0, "24" } })
    {
        INFO (c.name);

        LtcLine line;
        line.rate = c.counting;
        line.fps = c.fps;
        line.first = frameNamed ("00:00:10:00", c.counting);

        //  The show says 25: the labels and the period, not the show, decide.
        Watching watching { CodeWatch::Kind::ltc, Rate::fps25 };
        watching.run (line, 1, 300);

        INFO (watching.spelled());
        REQUIRE (! watching.heard.empty());

        const auto& last = watching.heard.back().said.map;
        CHECK (unitWordOf (last.unit) == c.expected);
        CHECK (exactlyOne (last.speed));
        CHECK (watching.count (CodeTurn::stopped) == 0u);
        CHECK (watching.heard.size() <= 3u);
    }

    SUBCASE ("MTC: rate code 3 at 29.97, through half a millisecond of jitter")
    {
        MtcLine line;
        line.rateCode = 3;
        line.fps = 30000.0 / 1001.0;
        line.first = frameNamed ("03:00:00:00", Rate::fps30);
        line.jitter = 0.0005;

        Watching watching { CodeWatch::Kind::mtc, Rate::fps25 };
        watching.run (line, 1, 50 * 60);

        INFO (watching.spelled());
        REQUIRE (! watching.heard.empty());
        CHECK (unitWordOf (watching.heard.back().said.map.unit) == "29.97");
        CHECK (watching.heard.back().said.map.speed == doctest::Approx (1.0).epsilon (1.0e-4));
    }
}

TEST_CASE ("code watch: a takeover's re-say - the line it runs on as jumped, where it stopped as stopped, nothing never heard")
{
    //  The code runs for 150 frames - heard whole by tick 302 - and stops.
    LtcLine line;
    line.first = frameNamed ("07:00:00:00", Rate::fps25);
    line.missing = [] (std::int64_t k) { return k >= 150; };

    Watching watching { CodeWatch::Kind::ltc };
    CHECK_FALSE (watching.watch.resay().has_value());

    watching.run (line, 1, 120);
    REQUIRE (watching.heard.size() == 1u);

    const auto resaid = watching.watch.resay();
    REQUIRE (resaid.has_value());
    CHECK (resaid->what == CodeTurn::jumped);
    CHECK (resaid->map.state == CodeState::running);
    CHECK (exactlyOne (resaid->map.speed));

    //  The same line: any frame falls on the same sample by either map.
    const auto frame = line.frameOfK (140);
    const auto lockedAt = watching.heard[0].said.map.sampleOf (frame);
    const auto resaidAt = resaid->map.sampleOf (frame);
    REQUIRE (lockedAt.has_value());
    REQUIRE (resaidAt.has_value());
    CHECK (std::abs (*lockedAt - *resaidAt) <= 1);

    //  Nothing more to say on the ticks after: the re-say is its line.
    watching.run (line, 120, 140);
    CHECK (watching.heard.size() == 1u);

    watching.run (line, 140, 400);
    REQUIRE (watching.heard.size() == 2u);
    REQUIRE (watching.heard[1].said.what == CodeTurn::stopped);

    const auto stopped = watching.watch.resay();
    REQUIRE (stopped.has_value());
    CHECK (stopped->what == CodeTurn::stopped);
    CHECK (stopped->map == watching.heard[1].said.map);
}

//==============================================================================
namespace
{
    /*  THE ENGINE AS SERVE WIRES THE RUNNER, with no audio side: what applies
        `code.heard`, and what a replay of its log is. */
    struct CodeRig
    {
        CodeRig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            runner.setSamplesPerTick (static_cast<int> (perTick));
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult send (const std::string& origin, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin, codeheard::command, std::move (args)));
            return tickOnce();
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (65);
        cue::Runner runner { document, runs, runIds, focus };
        std::int64_t tick = 1;
    };

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }
}

TEST_CASE ("code.heard: the Runner keeps each source's map and the readouts read it; a record that does not read is refused; a replay reaches the same maps")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const std::string ltc = "ltc:IN000001";
    const std::string mtc = "mtc:P0000001";

    /*  A WATCHER'S TURNS, AS SERVE SUBMITS THEM: an LTC code locks, runs a
        while, and stops; each turn said goes in from the source's origin. */
    LtcLine line;
    line.first = frameNamed ("09:59:50:00", Rate::fps25);
    line.missing = [] (std::int64_t k) { return k >= 250; };

    CodeWatch watch { CodeWatch::Kind::ltc };
    watch.setSampleRate (sampleRate);
    watch.setShowRate (Rate::fps25);
    watch.setGrace (perTick);

    std::vector<CodeSaid> said;
    std::optional<std::int64_t> lockedAt;

    for (std::int64_t n = 1; n < 700; ++n)
    {
        line.feed (watch, n * perTick);

        if (auto turn = watch.update (n * perTick))
        {
            said.push_back (*turn);
            REQUIRE (rig.engine.submit (ltc, codeheard::command, codeHeardArgs (ltc, *turn)));
        }

        const auto result = rig.tickOnce();
        CHECK (result.rejected == 0u);

        //  While it runs: the Runner's map is the one said, and the readout reads the code now.
        if (! lockedAt.has_value() && ! said.empty())
        {
            lockedAt = n;
            const auto* map = rig.runner.codeMapOf (ltc);
            REQUIRE (map != nullptr);
            CHECK (*map == said.back().map);
            CHECK (map->sampleRate == sampleRate);
        }

        if (n == 300)
        {
            const auto readout = rig.runner.codeReadout (ltc, n * perTick);
            CHECK (readout.state == "running");
            CHECK (readout.rate == "25");
            CHECK (std::bit_cast<std::uint64_t> (readout.speed) == std::bit_cast<std::uint64_t> (1.0));

            //  The frame the code is in at that sample: counted from the frames heard.
            const auto inFrame = static_cast<std::int64_t> (std::floor ((static_cast<double> (n * perTick) - line.zero) / line.period()));
            CHECK (readout.code == formatLabel (labelOf (line.first + inFrame, Rate::fps25), Rate::fps25));
        }
    }

    REQUIRE (said.size() == 2u);
    CHECK (said[1].what == CodeTurn::stopped);

    {
        const auto readout = rig.runner.codeReadout (ltc, 700 * perTick);
        CHECK (readout.state == "stopped");
        CHECK (readout.code == formatLabel (labelOf (line.first + 249, Rate::fps25), Rate::fps25));
        CHECK (readout.speed == doctest::Approx (0.0));
    }

    //  The show names no source: none. It names one never heard: absent.
    CHECK (rig.runner.codeReadout ("", 0).state == "none");
    CHECK (rig.runner.codeReadout ("ltc:IN000002", 0).state == "absent");

    //  AN MTC MAP BESIDE IT, by hand, at 29.97 drop-frame and a speed of its own.
    REQUIRE (rig.send (mtc, { s (mtc), s ("jumped"), s ("29.97df"), osc::Value::int64 (100'000),
                              osc::Value::int64 (480'000), osc::Value::float64 (0.99995) }).rejected == 0u);
    {
        const auto* map = rig.runner.codeMapOf (mtc);
        REQUIRE (map != nullptr);
        CHECK (map->state == CodeState::running);
        CHECK (map->frame == 100'000);
        CHECK (map->sample == 480'000);
        CHECK (map->unit == unitOf (Rate::fps2997df));
        CHECK (rig.runner.codeReadout (mtc, 480'000).code == formatLabel (labelOf (100'000, Rate::fps2997df), Rate::fps2997df));
    }

    //  REFUSED, bad-value, and no map moved: a turn, a source, a rate, a frame, a unit that is not one.
    const auto mapsBefore = rig.runner.codeMaps();

    for (const auto& args : std::vector<std::vector<osc::Value>> {
             { s (mtc), s ("paused"), s ("25"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s ("smpte:1"), s ("jumped"), s ("25"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s ("ltc:"), s ("jumped"), s ("25"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s (mtc), s ("jumped"), s ("26"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s (mtc), s ("jumped"), s ("25"), osc::Value::int64 (86'400 * 25), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s (mtc), s ("jumped"), s ("ms"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) },
             { s ("clock"), s ("jumped"), s ("25"), osc::Value::int64 (0), osc::Value::int64 (0), osc::Value::float64 (1.0) } })
    {
        INFO ("refused: " << args[0].getString() << " " << args[1].getString() << " " << args[2].getString());
        const auto result = rig.send (mtc, args);
        CHECK (result.rejected == 1u);
        CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);
    }

    CHECK (rig.runner.codeMaps() == mapsBefore);

    /*  AND A REPLAY OF THE LOG, with no watcher and no code: the same maps,
        record for record. */
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    CodeRig fresh;
    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);
    CHECK (fresh.runner.codeMaps() == rig.runner.codeMaps());
    CHECK (fresh.runner.codeMaps().size() == 2u);
}
