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
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/CodeWatch.h>
#include <wfg/engine/timecode/LtcDecoder.h>
#include <wfg/engine/timecode/MtcReader.h>
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
