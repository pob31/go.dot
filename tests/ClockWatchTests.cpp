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
    The wall clock watched as a code (namespace draft §60, TG.6; AIO, AIQ).

    What is pinned here, on a made-up world of three clocks - a steady clock,
    a wall clock on it that may be slewed and stepped, and Go.dot's sample
    count, which may run on a crystal of its own and is read, as an interface
    reads it, a block at a time - ticked fifty times a second as serve's hook
    is, the tick up to a few milliseconds late:
      - the first reading is `jumped` - the first a second after the clock
        pairs begin, the count's line settled - at its own millisecond, on its
        own sample, at a speed of exactly one; a world on one crystal says
        nothing more;
      - a wall clock slewed fifty parts per million for an hour, against an
        interface twenty slow and read through 256-sample blocks, writes a
        handful of `running`, each map within a millisecond and a half of the
        clock, the speed learnt;
      - a step of a second back, and an hour forward (summer time), each
        `jumped` at the new time, at the speed the clock had; a nudge of
        twenty milliseconds a `running`;
      - midnight crossed is no turn, the map written before it placing the
        morning;
      - a slow tick, three seconds with no reading, is time passing: nothing
        said with one crystal, and an hour of gaps through blocks no different
        from an hour without;
      - Go.dot's own clock moved is `jumped`, and a dropped buffer one
        `running` at the speed the clock had; forgotten, the next reading is a
        first;
      - the latency taken off, and a latency changed re-anchoring; a re-say;
      - a show watches the clock only while it holds a time group on it;
      - and `code.heard` from the clock through the engine: the Runner's map,
        and a replay of the log reaching the same map with no clock at all.

    Every case runs under fr_FR as well as C: a record's speed is text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/ClockWatch.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/CodeWatch.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::timecode;

namespace
{
    constexpr int sampleRate = 48000;
    constexpr double tickMs = 20.0;
    constexpr std::int64_t day = 86'400'000;

    constexpr std::int64_t timeOfDay (int hours, int minutes, int seconds, int milliseconds = 0)
    {
        return ((static_cast<std::int64_t> (hours) * 60 + minutes) * 60 + seconds) * 1000 + milliseconds;
    }

    bool exactlyOne (double value)
    {
        return std::bit_cast<std::uint64_t> (value) == std::bit_cast<std::uint64_t> (1.0);
    }

    bool sameDouble (double a, double b)
    {
        return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b);
    }

    //==========================================================================
    /*  THE WORLD: steady milliseconds `t` from an origin; the wall clock there,
        `wallAt (t)`, unwrapped, slewed by `wallSpeed` and stepped by
        `wallShift`; Go.dot's true sample, `trueSample (t)`, on a crystal
        `crystal` times nominal, moved by `sampleShift`; the interface's count
        read a `block` at a time (0: the true sample, floored). Each tick hands
        the watcher the clock pair, as serve does at the top of its hook, and a
        reading of the wall clock `readGapMs` later - half a millisecond by
        default, so a world whose clocks agree reads each millisecond at its
        middle and every map is exact. */
    struct World
    {
        double startMs = static_cast<double> (timeOfDay (20, 0, 0));
        double wallSpeed = 1.0;
        double wallShift = 0.0;
        double crystal = 1.0;
        double sampleZero = 1'000'000.0;
        double sampleShift = 0.0;
        int block = 0;
        double lateMs = 0.0;
        double readGapMs = 0.5;
        std::mt19937 random { 5 };

        double wallAt (double t) const { return startMs + wallShift + t * wallSpeed; }
        double trueSample (double t) const { return sampleZero + sampleShift + t * 48.0 * crystal; }

        //  The steady moment the wall clock reaches `wallMs` (unwrapped).
        double steadyWhen (double wallMs) const { return (wallMs - startMs - wallShift) / wallSpeed; }

        //  How far behind the true sample the count reads on average: half a block.
        double countBias() const { return static_cast<double> (block) / 2.0; }

        //  Where a map should place the wall clock's millisecond `wallMs` (unwrapped).
        double expectedSample (double wallMs, std::int64_t latency = 0) const
        {
            return trueSample (steadyWhen (wallMs)) - countBias() - static_cast<double> (latency);
        }

        //  The engine's draws, the same on every standard library.
        double uniform (double low, double high)
        {
            return low + (high - low) * (static_cast<double> (random()) / 4294967296.0);
        }

        static std::int64_t nanos (double t) { return 7'000'000'000LL + static_cast<std::int64_t> (std::llround (t * 1.0e6)); }

        //  The wall clock's millisecond of the day at the tick `n`'s reading, unwrapped.
        double wallAtTick (std::int64_t n) const { return std::floor (wallAt (static_cast<double> (n) * tickMs + readGapMs)); }

        void tick (ClockWatch& watch, double t)
        {
            const auto at = t + (lateMs > 0.0 ? uniform (0.0, lateMs) : 0.0);
            const auto truth = trueSample (at);
            const auto counted = block > 0 ? std::floor (truth / block) * block : std::floor (truth);
            watch.feedClock (static_cast<std::int64_t> (counted), nanos (at), sampleRate);

            const auto readAt = at + readGapMs;
            WallReading reading;
            reading.millisecondOfDay = static_cast<std::int64_t> (std::floor (wallAt (readAt))) % day;
            reading.steadyNanos = nanos (readAt);
            watch.hear (reading);
        }
    };

    struct Said
    {
        std::int64_t tick = 0;
        CodeSaid said;
    };

    //  A watcher in a world, ticked; everything it said with the tick it said it on.
    struct Watching
    {
        void run (std::int64_t fromTick, std::int64_t toTick)
        {
            for (auto n = fromTick; n < toTick; ++n)
                at (n);
        }

        void at (std::int64_t n)
        {
            world.tick (watch, static_cast<double> (n) * tickMs);

            if (auto said = watch.update())
                heard.push_back ({ n, *said });
        }

        std::size_t count (CodeTurn turn) const
        {
            return static_cast<std::size_t> (std::count_if (heard.begin(), heard.end(),
                                                            [turn] (const Said& s) { return s.said.what == turn; }));
        }

        //  How far a map places a millisecond of the wall clock (unwrapped) from where the world has it, in samples.
        double offBy (const CodeMap& map, double wallMs, std::int64_t latency = 0) const
        {
            const auto whole = std::floor (wallMs);
            const auto at = map.sampleOf (codemap::wrapped (static_cast<std::int64_t> (whole), day));
            REQUIRE (at.has_value());
            return static_cast<double> (*at) - world.expectedSample (whole, latency);
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

        World world;
        ClockWatch watch;
        std::vector<Said> heard;
    };
}

//==============================================================================
TEST_CASE ("clock watch: the first reading is jumped - its own millisecond on its own sample, at a speed of exactly one, a second after the clock pairs began - and a world on one crystal says nothing more")
{
    Watching watching;
    watching.run (0, 50 * 600);

    INFO (watching.spelled());
    REQUIRE (watching.heard.size() == 1u);

    //  The readings of the count's line's first second are not heard: the first heard is a second on.
    const auto& first = watching.heard.front();
    CHECK (first.tick == 50);
    CHECK (first.said.what == CodeTurn::jumped);
    CHECK (first.said.map.state == CodeState::running);
    CHECK (unitWordOf (first.said.map.unit) == "ms");
    CHECK (first.said.map.frame == timeOfDay (20, 0, 1));
    CHECK (first.said.map.sample == 1'048'000);
    CHECK (first.said.map.sampleRate == sampleRate);
    CHECK (exactlyOne (first.said.map.speed));
    CHECK (watching.watch.state() == CodeState::running);

    //  Every minute of the ten placed on its own sample by the one map.
    for (int minute = 0; minute <= 10; ++minute)
        CHECK (std::abs (watching.offBy (first.said.map, static_cast<double> (timeOfDay (20, minute, 0)))) <= 1.0);

    //  And what it says of itself: the one rounding of the millisecond about the line.
    CHECK (watching.watch.jitterMilliseconds() < 0.05);
}

TEST_CASE ("clock watch: the wall clock slewed fifty parts per million for an hour - a handful of running, each map within a millisecond and a half, the speed learnt")
{
    for (const auto slew : { 1.00005, 0.99995 })
    {
        INFO ("wall clock at " << slew << " of the steady clock");

        //  The interface twenty parts per million slow, read through 256-sample blocks, the tick up to 3 ms late.
        Watching watching;
        watching.world.wallSpeed = slew;
        watching.world.crystal = 0.99998;
        watching.world.block = 256;
        watching.world.lateMs = 3.0;

        double worst = 0.0;

        for (std::int64_t n = 0; n < 50 * 3600; ++n)
        {
            watching.at (n);

            //  Once a second after the first, the map last said against where the clock really is.
            if (n % 50 == 0 && ! watching.heard.empty())
                worst = std::max (worst, std::abs (watching.offBy (watching.heard.back().said.map, watching.world.wallAtTick (n))));
        }

        const auto speed = slew / 0.99998;

        INFO (watching.spelled());
        MESSAGE ("an hour at " << (slew - 1.0) * 1.0e6 << " ppm, the interface at -20: "
                 << watching.count (CodeTurn::running) << " running, the map at worst " << worst / 48.0
                 << " ms from the clock, the speed learnt to "
                 << std::abs (watching.heard.back().said.map.speed / speed - 1.0) * 1.0e6 << " ppm, jitter "
                 << watching.watch.jitterMilliseconds() << " ms");

        CHECK (watching.count (CodeTurn::jumped) == 1u);
        CHECK (watching.count (CodeTurn::stopped) == 0u);
        CHECK (watching.count (CodeTurn::running) >= 1u);
        CHECK (watching.count (CodeTurn::running) <= 10u);

        //  A millisecond is the line's drift before it is written again; half as much again for the block's leftover.
        CHECK (worst <= 1.5 * 48.0);

        //  The speed: wall-clock milliseconds per Go.dot millisecond, learnt to a part per million.
        CHECK (watching.heard.back().said.map.speed == doctest::Approx (speed).epsilon (1.0e-6));
    }
}

TEST_CASE ("clock watch: a step of a second back and of an hour forward are jumped at the new time, at the speed the clock had; a nudge of 20 ms is running")
{
    //  A clock two hundred parts per million fast, so the speed kept across a step is one learnt, not one.
    Watching watching;
    watching.world.wallSpeed = 1.0002;
    watching.run (0, 15'000);

    INFO (watching.spelled());
    REQUIRE (! watching.heard.empty());
    const auto learnt = watching.heard.back().said.map.speed;
    CHECK (learnt == doctest::Approx (1.0002).epsilon (2.0e-6));
    CHECK (watching.count (CodeTurn::jumped) == 1u);

    const auto saidSince = [&watching] (std::size_t from)
    {
        return std::vector<Said> (watching.heard.begin() + static_cast<std::ptrdiff_t> (from), watching.heard.end());
    };

    //  THE NIGHT A TIME-SYNC SERVICE STEPS IT A SECOND BACK.
    auto before = watching.heard.size();
    watching.world.wallShift -= 1000.0;
    watching.run (15'000, 30'000);

    {
        const auto said = saidSince (before);
        REQUIRE (! said.empty());
        CHECK (said.front().tick == 15'000);
        CHECK (said.front().said.what == CodeTurn::jumped);
        CHECK (said.front().said.map.frame == static_cast<std::int64_t> (watching.world.wallAtTick (15'000)) % day);
        CHECK (sameDouble (said.front().said.map.speed, learnt));
        CHECK (std::abs (watching.offBy (said.front().said.map, watching.world.wallAtTick (15'000))) <= 25.0);
        CHECK (std::count_if (said.begin(), said.end(), [] (const Said& s) { return s.said.what == CodeTurn::jumped; }) == 1);
    }

    //  THE NIGHT SUMMER TIME BEGINS: an hour forward.
    before = watching.heard.size();
    watching.world.wallShift += 3'600'000.0;
    watching.run (30'000, 45'000);

    {
        const auto said = saidSince (before);
        REQUIRE (! said.empty());
        CHECK (said.front().tick == 30'000);
        CHECK (said.front().said.what == CodeTurn::jumped);
        CHECK (said.front().said.map.frame == static_cast<std::int64_t> (watching.world.wallAtTick (30'000)) % day);
        CHECK (said.front().said.map.frame - static_cast<std::int64_t> (watching.world.startMs) > 3'600'000);
        CHECK (sameDouble (said.front().said.map.speed, watching.heard[before - 1].said.map.speed));
        CHECK (std::abs (watching.offBy (said.front().said.map, watching.world.wallAtTick (30'000))) <= 25.0);
        CHECK (std::count_if (said.begin(), said.end(), [] (const Said& s) { return s.said.what == CodeTurn::jumped; }) == 1);
    }

    //  A NUDGE: twenty milliseconds on, a re-anchor and no jump - the map twenty milliseconds earlier.
    before = watching.heard.size();
    const auto lastMap = watching.heard.back().said.map;
    watching.world.wallShift += 20.0;
    watching.run (45'000, 45'001);

    {
        const auto said = saidSince (before);
        REQUIRE (said.size() == 1u);
        CHECK (said.front().said.what == CodeTurn::running);

        const auto frame = static_cast<std::int64_t> (watching.world.wallAtTick (45'100)) % day;
        const auto was = lastMap.sampleOf (frame);
        const auto now = said.front().said.map.sampleOf (frame);
        REQUIRE (was.has_value());
        REQUIRE (now.has_value());
        CHECK (std::abs (static_cast<double> (*now - *was) + 20.0 * 48.0) <= 1.5 * 48.0);
    }
}

TEST_CASE ("clock watch: after a step, read through 512-sample blocks, the young line says nothing for ten seconds but the jump")
{
    //  Steps of a second back and forth every minute, after two minutes to settle.
    Watching watching;
    watching.world.wallSpeed = 1.00005;
    watching.world.block = 512;
    watching.world.lateMs = 3.0;
    watching.run (0, 50 * 120);

    std::vector<std::int64_t> steps;

    for (int k = 0; k < 8; ++k)
    {
        const auto from = 50 * (120 + 60 * k);
        watching.world.wallShift += k % 2 == 0 ? -1000.0 : 1000.0;
        steps.push_back (from);
        watching.run (from, from + 50 * 60);
    }

    INFO (watching.spelled());
    CHECK (watching.count (CodeTurn::jumped) == 1u + steps.size());

    for (const auto from : steps)
    {
        const auto said = std::count_if (watching.heard.begin(), watching.heard.end(), [from] (const Said& s)
                                         { return s.tick >= from && s.tick < from + 50 * 10; });
        CHECK_MESSAGE (said == 1, said << " said in the ten seconds after the step at tick " << from);
    }
}

TEST_CASE ("clock watch: midnight crossed is no turn, and the map written before it places the morning")
{
    SUBCASE ("one crystal: the map of 23:59:00 places 00:00:30")
    {
        Watching watching;
        watching.world.startMs = static_cast<double> (timeOfDay (23, 59, 0));
        watching.run (0, 50 * 120);

        INFO (watching.spelled());
        REQUIRE (watching.heard.size() == 1u);

        const auto& map = watching.heard.front().said.map;
        CHECK (map.frame == timeOfDay (23, 59, 1));

        const auto at = map.sampleOf (timeOfDay (0, 0, 30));
        REQUIRE (at.has_value());
        CHECK (static_cast<double> (*at) == doctest::Approx (watching.world.expectedSample (static_cast<double> (day + timeOfDay (0, 0, 30)))));
        CHECK (map.positionAt (*at).frame == timeOfDay (0, 0, 30));
        CHECK (readoutOf (&map, *at).code == "00:00:30");
    }

    SUBCASE ("slewed and read through blocks: every map, before midnight and after it, places the clock")
    {
        Watching watching;
        watching.world.startMs = static_cast<double> (timeOfDay (23, 50, 0));
        watching.world.wallSpeed = 1.00005;
        watching.world.block = 256;
        watching.world.lateMs = 3.0;

        double worst = 0.0;

        for (std::int64_t n = 0; n < 50 * 1200; ++n)
        {
            watching.at (n);

            if (n % 50 == 0 && ! watching.heard.empty())
                worst = std::max (worst, std::abs (watching.offBy (watching.heard.back().said.map, watching.world.wallAtTick (n))));
        }

        INFO (watching.spelled());
        CHECK (watching.count (CodeTurn::jumped) == 1u);
        CHECK (worst <= 1.5 * 48.0);

        //  Every map's anchor a millisecond of the day, wrapped, whichever side of midnight it was written on.
        for (const auto& s : watching.heard)
        {
            CHECK (s.said.map.frame >= 0);
            CHECK (s.said.map.frame < day);
        }
    }
}

TEST_CASE ("clock watch: a slow tick - three seconds with no reading - is time passing, no turn")
{
    SUBCASE ("one crystal")
    {
        Watching watching;
        watching.run (0, 3000);
        watching.run (3150, 6000);

        INFO (watching.spelled());
        CHECK (watching.heard.size() == 1u);
    }

    SUBCASE ("slewed and read through 512-sample blocks: an hour with a gap of three seconds or ten every five minutes is an hour without them")
    {
        Watching watching;
        watching.world.wallSpeed = 1.00005;
        watching.world.block = 512;
        watching.world.lateMs = 3.0;

        std::vector<std::int64_t> gapsEnded;
        double worst = 0.0;
        std::int64_t n = 0;

        while (n < 50 * 3600)
        {
            if (n > 0 && n % (50 * 300) == 0)
            {
                n += gapsEnded.size() % 2 == 0 ? 150 : 500;
                gapsEnded.push_back (n);
            }

            watching.at (n);

            //  The map against the clock at every second, and at the first tick after each gap.
            if (! watching.heard.empty()
                  && (n % 50 == 0 || std::find (gapsEnded.begin(), gapsEnded.end(), n) != gapsEnded.end()))
                worst = std::max (worst, std::abs (watching.offBy (watching.heard.back().said.map, watching.world.wallAtTick (n))));

            ++n;
        }

        INFO (watching.spelled());
        REQUIRE (gapsEnded.size() == 11u);

        /*  No jump, a handful of re-anchors as the drift alone writes them, and
            the map as close to the clock across every gap as anywhere else: a
            drift record that falls due at a gap's end is the drift's, which is
            why no tick is held to silence here, and the one crystal above is. */
        CHECK (watching.count (CodeTurn::jumped) == 1u);
        CHECK (watching.count (CodeTurn::running) <= 10u);
        CHECK (worst <= 1.5 * 48.0);
    }
}

TEST_CASE ("clock watch: Go.dot's own clock moved under the wall clock is jumped; forgotten, the next reading is a first")
{
    Watching watching;
    watching.run (0, 3000);
    REQUIRE (watching.heard.size() == 1u);

    /*  The interface's count went back half a second: eight pairs tell the
        count's line it slipped, it starts again, and once it has had its
        second the clock is elsewhere on it. */
    watching.world.sampleShift -= 24'000.0;
    watching.run (3000, 6000);

    INFO (watching.spelled());
    REQUIRE (watching.heard.size() == 2u);

    const auto& moved = watching.heard[1];
    CHECK (moved.tick >= 3050);
    CHECK (moved.tick <= 3060);
    CHECK (moved.said.what == CodeTurn::jumped);
    CHECK (exactlyOne (moved.said.map.speed));
    CHECK (std::abs (watching.offBy (moved.said.map, watching.world.wallAtTick (5000))) <= 1.0);

    //  Forgotten - the show stopped watching the clock, or Go.dot's clock was away - and read again.
    watching.watch.forget();
    CHECK (watching.watch.state() == CodeState::absent);
    CHECK_FALSE (watching.watch.resay().has_value());

    watching.run (6000, 6100);
    REQUIRE (watching.heard.size() == 3u);
    CHECK (watching.heard[2].tick == 6050);
    CHECK (watching.heard[2].said.what == CodeTurn::jumped);
}

TEST_CASE ("clock watch: a dropped buffer - the count a block behind for good - is one running, at the speed the clock had, and the map follows it")
{
    for (const int block : { 256, 512 })
    {
        INFO ("block " << block);

        //  An hour's quarter learnt first, so the speed kept is a measured one.
        Watching watching;
        watching.world.wallSpeed = 1.00005;
        watching.world.block = block;
        watching.world.lateMs = 3.0;
        watching.run (0, 50 * 900);

        REQUIRE (! watching.heard.empty());
        const auto before = watching.heard.size();
        const auto speedBefore = watching.heard.back().said.map.speed;

        watching.world.sampleShift -= static_cast<double> (block);
        watching.run (50 * 900, 50 * 960);

        INFO (watching.spelled());
        REQUIRE (watching.heard.size() == before + 1);

        const auto& slipped = watching.heard.back();
        CHECK (slipped.said.what == CodeTurn::running);
        CHECK (slipped.tick > 50 * 900 + 50);
        CHECK (slipped.tick <= 50 * 900 + 60);
        CHECK (sameDouble (slipped.said.map.speed, speedBefore));

        //  The map after it places the clock on the count as it is now.
        CHECK (std::abs (watching.offBy (slipped.said.map, watching.world.wallAtTick (50 * 959))) <= 1.5 * 48.0);
    }
}

TEST_CASE ("clock watch: the output latency taken off, a latency changed re-anchoring, and a takeover's re-say")
{
    Watching watching;
    CHECK_FALSE (watching.watch.resay().has_value());

    watching.watch.setLatency (256);
    watching.run (0, 1000);

    INFO (watching.spelled());
    REQUIRE (watching.heard.size() == 1u);

    //  Heard 256 samples before it is read.
    CHECK (watching.heard[0].said.map.sample == 1'048'000 - 256);
    CHECK (std::abs (watching.offBy (watching.heard[0].said.map, watching.world.wallAtTick (900), 256)) <= 1.0);

    //  The interface changed: the next tick re-anchors, the same line.
    watching.watch.setLatency (512);
    watching.run (1000, 2000);
    REQUIRE (watching.heard.size() == 2u);
    CHECK (watching.heard[1].tick == 1000);
    CHECK (watching.heard[1].said.what == CodeTurn::running);

    const auto frame = static_cast<std::int64_t> (watching.world.wallAtTick (1500));
    const auto before = watching.heard[0].said.map.sampleOf (frame);
    const auto after = watching.heard[1].said.map.sampleOf (frame);
    REQUIRE (before.has_value());
    REQUIRE (after.has_value());
    CHECK (*after - *before == -256);

    //  A TAKEOVER: its own line, said in full as jumped; nothing more after it.
    const auto resaid = watching.watch.resay();
    REQUIRE (resaid.has_value());
    CHECK (resaid->what == CodeTurn::jumped);
    CHECK (exactlyOne (resaid->map.speed));

    const auto resaidAt = resaid->map.sampleOf (frame);
    REQUIRE (resaidAt.has_value());
    CHECK (std::abs (*resaidAt - *after) <= 1);

    watching.run (2000, 3000);
    CHECK (watching.heard.size() == 2u);
}

//==============================================================================
TEST_CASE ("clock watch: a show watches the clock only while it holds a time group whose source is the clock")
{
    doc::ShowDocument document;
    const auto list = document.createList ("Show").id;

    const auto make = [&document] (const std::string& parent, int index, const std::string& kind,
                                   const doc::ShowDocument::Attributes& born = {})
    {
        const auto made = document.createCue (parent, index, kind, kind, {}, born);
        REQUIRE_MESSAGE (made.ok, kind << ": " << made.reason);
        return made.id;
    };

    //  Nothing: no list, then a list of a memo and a plain sequence.
    CHECK_FALSE (document.holdsTimeGroupOn ("clock"));
    make (list, 0, "memo");
    const auto sequence = make (list, 1, "group");
    CHECK_FALSE (document.holdsTimeGroupOn ("clock"));
    CHECK_FALSE (document.holdsTimeGroupOn ("ltc"));

    //  A time group with no source written reads LTC, the table's default.
    const auto timed = make (sequence, 0, "group", { { "mode", "time" } });
    CHECK (document.holdsTimeGroupOn ("ltc"));
    CHECK_FALSE (document.holdsTimeGroupOn ("clock"));

    //  Its source turned to the clock, inside a sequence.
    REQUIRE (document.setAttribute ("/godot/cue/" + timed + "/timeSource", "clock").ok);
    CHECK (document.holdsTimeGroupOn ("clock"));
    CHECK_FALSE (document.holdsTimeGroupOn ("ltc"));

    //  And gone.
    REQUIRE (document.remove (timed).ok);
    CHECK_FALSE (document.holdsTimeGroupOn ("clock"));

    //  One in the Persistent section counts as well.
    const auto section = document.createPersistent (list);
    REQUIRE (section.ok);
    make (section.id, 0, "group", { { "mode", "time" }, { "timeSource", "clock" } });
    CHECK (document.holdsTimeGroupOn ("clock"));
    CHECK_FALSE (document.holdsTimeGroupOn ("mtc"));
}

//==============================================================================
namespace
{
    /*  THE ENGINE AS SERVE WIRES THE RUNNER, with no audio side: what applies
        `code.heard`, and what a replay of its log is. */
    struct ClockRig
    {
        ClockRig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            runner.setSamplesPerTick (960);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (65);
        cue::Runner runner { document, runs, runIds, focus };
        std::int64_t tick = 1;
    };
}

TEST_CASE ("code.heard from the clock: the Runner keeps the wall clock's map, and a replay reaches the same map")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    ClockRig rig;
    const std::string clock = "clock";

    /*  THE WATCHER'S TURNS, AS SERVE SUBMITS THEM, from origin `clock`: the
        first reading, then a step back of a second. */
    Watching watching;
    std::vector<CodeSaid> said;

    for (std::int64_t n = 0; n < 1000; ++n)
    {
        if (n == 500)
            watching.world.wallShift -= 1000.0;

        const auto before = watching.heard.size();
        watching.at (n);

        if (watching.heard.size() > before)
        {
            said.push_back (watching.heard.back().said);
            REQUIRE (rig.engine.submit (clock, codeheard::command, codeHeardArgs (clock, said.back())));
        }

        const auto result = rig.tickOnce();
        CHECK (result.rejected == 0u);

        if (! said.empty())
        {
            const auto* map = rig.runner.codeMapOf (clock);
            REQUIRE (map != nullptr);
            CHECK (*map == said.back().map);
        }
    }

    INFO (watching.spelled());
    REQUIRE (said.size() == 2u);
    CHECK (said[1].what == CodeTurn::jumped);

    //  The map the Runner keeps places a time of day where the world has it: 20:00:30, a second late now.
    {
        const auto* map = rig.runner.codeMapOf (clock);
        REQUIRE (map != nullptr);
        CHECK (map->unit == wallClockUnit());
        CHECK (map->sampleRate == sampleRate);

        const auto at = map->sampleOf (timeOfDay (20, 0, 30));
        REQUIRE (at.has_value());
        CHECK (static_cast<double> (*at) == doctest::Approx (watching.world.expectedSample (static_cast<double> (timeOfDay (20, 0, 30)))));

        //  No readout of its own: the window shows the clock itself.
        CHECK (rig.runner.codeReadout ("", 0).state == "none");
    }

    /*  AND A REPLAY OF THE LOG, with no clock read at all: the same map,
        record for record. */
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    ClockRig fresh;
    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);
    CHECK (fresh.runner.codeMaps() == rig.runner.codeMaps());
    CHECK (fresh.runner.codeMaps().size() == 1u);
}

