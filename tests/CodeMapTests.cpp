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
    A code map (namespace draft §60, TG.5): an incoming code measured against
    Go.dot's own clock - an anchor frame, the sample it fell on, a speed.

    What is pinned here: at a speed of exactly one the map IS FrameClock's rule
    - every frame on `frameStartSample`'s sample and every sample in
    `frameAtSample`'s frame, at all six rates and three sample rates, hours
    from the anchor and before it; `positionAt` and `sampleOf` each other's
    inverse to the sample at speeds near one, at half and one and a half; a
    code 0.1 % slow and one played backwards placed where arithmetic done by
    hand puts them; midnight crossed both ways, a frame half a day off read as
    the past; a stopped map and an absent one; and what the show publishes of
    a map - its state, label, rate word and speed - with the wall clock's unit
    TG.6 will use.

    Every case runs under fr_FR as well as C: the readout's speed is text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

using namespace wfg;
using namespace wfg::timecode;

namespace
{
    constexpr std::array<Rate, 6> everyRate { Rate::fps23976, Rate::fps24, Rate::fps25,
                                               Rate::fps2997df, Rate::fps2997, Rate::fps30 };

    CodeMap runningMap (Rate rate, std::int64_t frame, std::int64_t sample, double speed, int sampleRate)
    {
        CodeMap map;
        map.state = CodeState::running;
        map.unit = unitOf (rate);
        map.frame = frame;
        map.sample = sample;
        map.speed = speed;
        map.sampleRate = sampleRate;
        return map;
    }

    std::int64_t frameNamed (const char* text, Rate rate)
    {
        const auto label = parseLabel (text);
        REQUIRE (label.has_value());
        return frameOf (*label, rate);
    }
}

//==============================================================================
TEST_CASE ("code map: at speed one it is FrameClock's rule, to the sample, at every rate, hours either side of the anchor")
{
    for (const auto rate : everyRate)
    {
        for (const auto sampleRate : { 44100, 48000, 96000 })
        {
            INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate);

            const auto anchor = frameNamed ("10:00:00:00", rate);
            const std::int64_t zero = 7'777'777;
            const auto map = runningMap (rate, anchor, zero, 1.0, sampleRate);
            const auto perDay = framesPerDay (rate);

            //  Every frame of the first minute, and a stretch three hours on and two hours back.
            for (const std::int64_t base : { std::int64_t { 0 }, std::int64_t { 3 * 3600 } * nominalOf (rate),
                                             -std::int64_t { 2 * 3600 } * nominalOf (rate) })
            {
                for (std::int64_t k = base; k < base + 1800; ++k)
                {
                    const auto frame = codemap::wrapped (anchor + k, perDay);
                    const auto expected = zero + frameStartSample (k, sampleRate, rate);

                    const auto at = map.sampleOf (frame);
                    REQUIRE (at.has_value());

                    if (*at != expected)
                    {
                        FAIL_CHECK ("frame " << k << " from the anchor: " << *at << " against FrameClock's " << expected);
                        break;
                    }

                    //  The first sample in the frame, and the one before it in the frame before.
                    const auto here = map.positionAt (expected);
                    const auto before = map.positionAt (expected - 1);

                    if (here.frame != frame || before.frame != codemap::wrapped (frame - 1, perDay))
                    {
                        FAIL_CHECK ("frame " << k << ": read back as " << here.frame << " and " << before.frame);
                        break;
                    }
                }
            }

            //  And any sample, near and far, in the frame `frameAtSample` says, its fraction what is left over.
            for (const std::int64_t delta : { std::int64_t { 0 }, std::int64_t { 1 }, std::int64_t { 1601 },
                                              std::int64_t { 1602 }, std::int64_t { -1 }, std::int64_t { -1602 },
                                              std::int64_t { 123'456'789 }, std::int64_t { -98'765'432 } })
            {
                const auto at = map.positionAt (zero + delta);
                CHECK (at.frame == codemap::wrapped (anchor + frameAtSample (delta, sampleRate, rate), perDay));
                CHECK (at.fraction >= 0.0);
                CHECK (at.fraction < 1.0);

                const auto exact = static_cast<double> (delta) * static_cast<double> (numOf (rate))
                                     / (static_cast<double> (sampleRate) * static_cast<double> (denOf (rate)));
                CHECK (at.fraction == doctest::Approx (exact - std::floor (exact)).epsilon (1e-6));
            }
        }
    }
}

TEST_CASE ("code map: positionAt and sampleOf are each other's inverse to the sample, near one and far from it")
{
    for (const auto speed : { 1.0, 1.00005, 0.99995, 0.999, 1.5, 0.5 })
    {
        for (const auto rate : { Rate::fps25, Rate::fps2997df, Rate::fps23976 })
        {
            INFO ("speed " << speed << ", rate " << std::string (wordOf (rate)));

            const auto anchor = frameNamed ("01:59:58:00", rate);
            const auto map = runningMap (rate, anchor, 4'000'000, speed, 48000);
            const auto perDay = framesPerDay (rate);

            for (std::int64_t k = -500; k < 20'000; k += 7)
            {
                const auto frame = codemap::wrapped (anchor + k, perDay);
                const auto at = map.sampleOf (frame);
                REQUIRE (at.has_value());

                const auto here = map.positionAt (*at);
                const auto before = map.positionAt (*at - 1);

                if (here.frame != frame || before.frame != codemap::wrapped (frame - 1, perDay))
                {
                    FAIL_CHECK ("frame " << k << " from the anchor on sample " << *at << " reads "
                                << here.frame << ", the sample before " << before.frame);
                    break;
                }
            }
        }
    }
}

TEST_CASE ("code map: a code 0.1 % slow and one played backwards, where the arithmetic puts them")
{
    //  At 25 and 48 kHz a second of code is 25 frames: 0.999 takes 1/0.999 s of Go.dot's clock for it.
    {
        const auto map = runningMap (Rate::fps25, 1000, 0, 0.999, 48000);

        //  A thousand seconds of code - 25 000 frames - fall 1000 / 0.999 s on: 48 048 048.05 samples, so the next sample.
        CHECK (map.sampleOf (26'000) == std::optional<std::int64_t> (48'048'049));
        CHECK (map.positionAt (48'048'049).frame == 26'000);
        CHECK (map.positionAt (48'048'048).frame == 25'999);

        //  Ten seconds of Go.dot's clock are 9.99 s of code: 249.75 frames.
        const auto at = map.positionAt (480'000);
        CHECK (at.frame == 1249);
        CHECK (at.fraction == doctest::Approx (0.75).epsilon (1e-9));
        CHECK (map.speed == doctest::Approx (0.999));
    }

    /*  BACKWARDS: a second on, the code is a second back - 25 frames before
        the anchor, on its start exactly; a frame's start is where the code
        reaches it from above, after which it reads the frame before. */
    {
        const auto map = runningMap (Rate::fps25, 1000, 1'000'000, -1.0, 48000);

        CHECK (map.positionAt (1'048'000).frame == 975);
        CHECK (map.positionAt (1'048'000).fraction == doctest::Approx (0.0));
        CHECK (map.positionAt (1'048'001).frame == 974);
        CHECK (map.positionAt (1'047'999).frame == 975);

        CHECK (map.sampleOf (975) == std::optional<std::int64_t> (1'048'000));
        CHECK (map.sampleOf (999) == std::optional<std::int64_t> (1'001'920));

        //  A frame ahead of a code going backwards was passed before the anchor.
        CHECK (map.sampleOf (1001) == std::optional<std::int64_t> (998'080));
    }
}

TEST_CASE ("code map: midnight crossed either way, and half a day off is the past")
{
    const auto rate = Rate::fps25;
    const auto anchor = frameNamed ("23:59:59:20", rate);
    const auto map = runningMap (rate, anchor, 10'000'000, 1.0, 48000);

    //  Ten frames on is past midnight: 00:00:00:05, and two seconds on is 00:00:01:20.
    CHECK (map.sampleOf (frameNamed ("00:00:00:05", rate)) == std::optional<std::int64_t> (10'000'000 + 10 * 1920));
    CHECK (map.positionAt (10'000'000 + 2 * 48000).frame == frameNamed ("00:00:01:20", rate));
    CHECK (formatLabel (labelOf (map.positionAt (10'000'000 + 2 * 48000).frame, rate), rate) == "00:00:01:20");

    //  A frame eleven hours ahead is eleven hours on; thirteen hours ahead is eleven hours back.
    CHECK (map.sampleOf (frameNamed ("10:59:59:20", rate)) == std::optional<std::int64_t> (10'000'000 + std::int64_t { 11 * 3600 } * 48000));
    CHECK (map.sampleOf (frameNamed ("12:59:59:20", rate)) == std::optional<std::int64_t> (10'000'000 - std::int64_t { 11 * 3600 } * 48000));

    //  BACKWARDS across midnight: 00:00:00:10 read backwards into 23:59:59:...
    {
        const auto back = runningMap (Rate::fps2997df, frameNamed ("00:00:00:02", Rate::fps2997df), 0, -1.0, 48000);
        const auto read = back.positionAt (static_cast<std::int64_t> (std::ceil (5.0 * 48000.0 * 1001.0 / 30000.0)) + 1);
        CHECK (formatLabel (labelOf (read.frame, Rate::fps2997df), Rate::fps2997df) == "23:59:59:26");
    }

    //  DROP-FRAME, a minute that drops: frame after 00:00:59;29 is 00:01:00;02.
    {
        const auto dropRate = Rate::fps2997df;
        const auto from = frameNamed ("00:00:59:29", dropRate);
        const auto at = runningMap (dropRate, from, 0, 1.0, 48000);
        const auto next = at.positionAt (*at.sampleOf (from + 1));
        CHECK (formatLabel (labelOf (next.frame, dropRate), dropRate) == "00:01:00:02");
    }
}

TEST_CASE ("code map: stopped stands on its anchor, absent places nothing")
{
    auto map = runningMap (Rate::fps25, 5000, 2'000'000, 0.0, 48000);
    map.state = CodeState::stopped;

    CHECK (map.positionAt (99'000'000).frame == 5000);
    CHECK (map.positionAt (99'000'000).fraction == doctest::Approx (0.0));
    CHECK (map.sampleOf (5000) == std::optional<std::int64_t> (2'000'000));
    CHECK_FALSE (map.sampleOf (5001).has_value());

    //  A map running at no speed is standing too.
    map.state = CodeState::running;
    CHECK (map.positionAt (99'000'000).frame == 5000);
    CHECK_FALSE (map.sampleOf (5001).has_value());

    CodeMap absent;
    CHECK_FALSE (absent.sampleOf (0).has_value());
    CHECK (absent.positionAt (12345).frame == 0);
}

TEST_CASE ("code map: what the show publishes of it - state, label, rate word, speed - and the wall clock's unit")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    //  No map heard for a source the show names: absent, nothing else.
    {
        const auto readout = readoutOf (nullptr, 0);
        CHECK (readout.state == "absent");
        CHECK (readout.code.empty());
        CHECK (readout.rate.empty());
        CHECK (readout.speed == doctest::Approx (0.0));
    }

    //  The show names none: the readout's own default.
    CHECK (CodeReadout {}.state == "none");

    {
        /*  Ten seconds of Go.dot's clock at 1.00005 are 10.0005 s of code, 299.7
            frames at 30000/1001: the frame labelled 01:00:09:29 (01:00 drops none). */
        const auto map = runningMap (Rate::fps2997df, frameNamed ("01:00:00:00", Rate::fps2997df), 0, 1.00005, 48000);
        const auto readout = readoutOf (&map, 48000 * 10);
        CHECK (readout.state == "running");
        CHECK (readout.code == "01:00:09:29");
        CHECK (readout.rate == "29.97df");
        CHECK (std::bit_cast<std::uint64_t> (readout.speed) == std::bit_cast<std::uint64_t> (1.00005));
    }

    {
        auto map = runningMap (Rate::fps24, frameNamed ("00:10:00:12", Rate::fps24), 0, 0.0, 48000);
        map.state = CodeState::stopped;
        const auto readout = readoutOf (&map, 48000 * 100);
        CHECK (readout.state == "stopped");
        CHECK (readout.code == "00:10:00:12");
        CHECK (readout.rate == "24");
        CHECK (readout.speed == doctest::Approx (0.0));
    }

    //  THE UNITS BY THEIR WORDS: every rate's own, and `ms` for the wall clock's millisecond of the day.
    for (const auto rate : everyRate)
    {
        const auto unit = unitFromWord (wordOf (rate));
        REQUIRE (unit.has_value());
        CHECK (*unit == unitOf (rate));
        CHECK (unitWordOf (*unit) == std::string (wordOf (rate)));
    }

    const auto clock = unitFromWord ("ms");
    REQUIRE (clock.has_value());
    CHECK (*clock == wallClockUnit());
    CHECK_FALSE (clock->rate.has_value());
    CHECK_FALSE (unitFromWord ("smpte").has_value());

    //  A wall-clock map: 20:00:00 is its 72 000 000th millisecond, a second on its 48 000th sample.
    {
        CodeMap wall;
        wall.state = CodeState::running;
        wall.unit = wallClockUnit();
        wall.frame = 72'000'000 - 1000;
        wall.sample = 0;
        wall.speed = 1.0;
        wall.sampleRate = 48000;

        CHECK (wall.sampleOf (72'000'000) == std::optional<std::int64_t> (48000));
        CHECK (readoutOf (&wall, 48000).code == "20:00:00");
        CHECK (readoutOf (&wall, 48000).rate == "ms");
    }
}
