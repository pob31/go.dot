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
    Timecode rates and labels, on their own (namespace draft §59, AHF, AHG):
    what a label is, which frame it names at each rate, and the drop-frame rule
    the whole of 29.97 drop-frame turns on.

    The expected labels are written out by hand from the rule as SMPTE 12M
    states it - :00 and :01 left out at every minute not divisible by ten - and
    not computed by the code under test. Nothing here reads a clock or a locale;
    it runs in both locales because a label is text a show stores.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace wfg::timecode;

namespace
{
    constexpr std::array<Rate, 6> everyRate { Rate::fps23976, Rate::fps24, Rate::fps25,
                                              Rate::fps2997df, Rate::fps2997, Rate::fps30 };

    Label label (int hours, int minutes, int seconds, int frames)
    {
        return Label { hours, minutes, seconds, frames };
    }

    std::string text (const Label& value)
    {
        return formatLabel (value, Rate::fps25);
    }
}

TEST_CASE ("timecode: the six rates, their fractions, their counts and their words")
{
    CHECK (numOf (Rate::fps23976) == 24000);
    CHECK (denOf (Rate::fps23976) == 1001);
    CHECK (numOf (Rate::fps24) == 24);
    CHECK (denOf (Rate::fps24) == 1);
    CHECK (numOf (Rate::fps25) == 25);
    CHECK (denOf (Rate::fps25) == 1);
    CHECK (numOf (Rate::fps2997df) == 30000);
    CHECK (denOf (Rate::fps2997df) == 1001);
    CHECK (numOf (Rate::fps2997) == 30000);
    CHECK (denOf (Rate::fps2997) == 1001);
    CHECK (numOf (Rate::fps30) == 30);
    CHECK (denOf (Rate::fps30) == 1);

    const std::array<int, 6> nominal { 24, 24, 25, 30, 30, 30 };
    const std::array<int, 6> mtc { 0, 0, 1, 2, 3, 3 };
    const std::array<const char*, 6> words { "23.976", "24", "25", "29.97df", "29.97", "30" };

    for (std::size_t n = 0; n < everyRate.size(); ++n)
    {
        INFO ("rate " << words[n]);
        CHECK (nominalOf (everyRate[n]) == nominal[n]);
        CHECK (mtcCodeOf (everyRate[n]) == mtc[n]);
        CHECK (dropsFrames (everyRate[n]) == (everyRate[n] == Rate::fps2997df));
        CHECK (wordOf (everyRate[n]) == words[n]);
        REQUIRE (rateFromWord (words[n]).has_value());
        CHECK (*rateFromWord (words[n]) == everyRate[n]);
    }

    CHECK_FALSE (rateFromWord ("29,97").has_value());
    CHECK_FALSE (rateFromWord ("").has_value());
    CHECK_FALSE (rateFromWord ("50").has_value());
}

TEST_CASE ("timecode: a label reads with colons, a semicolon or a full stop before its frames")
{
    REQUIRE (parseLabel ("01:00:00:00").has_value());
    CHECK (*parseLabel ("01:00:00:00") == label (1, 0, 0, 0));
    CHECK (*parseLabel ("23:59:59:29") == label (23, 59, 59, 29));
    CHECK (*parseLabel ("10:20:30;15") == label (10, 20, 30, 15));
    CHECK (*parseLabel ("10:20:30.15") == label (10, 20, 30, 15));

    //  A drop-frame display's own spelling, semicolons all through.
    REQUIRE (parseLabel ("01;00;00;00").has_value());
    CHECK (*parseLabel ("01;00;00;00") == label (1, 0, 0, 0));

    //  Written back with colons whatever it was read with (AHF).
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps2997df) == "01:02:03:04");
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps2997df, true) == "01:02:03;04");
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps25, true) == "01:02:03:04");
}

TEST_CASE ("timecode: what is not a label is refused, never guessed at")
{
    for (const auto* bad : { "", "1:2:3", "1:2:3:4", "01:00:00", "01:00:00:00:00", "25:00:00:00",
                             "24:00:00:00", "01:60:00:00", "01:00:60:00", "01:00:00:30",
                             "01-00-00-00", "01:00.00:00", "0a:00:00:00", " 1:00:00:00",
                             "01:00:00:00 ", "-1:00:00:00", "01:00:00:5" })
    {
        INFO ("text: \"" << bad << "\"");
        CHECK_FALSE (parseLabel (bad).has_value());
    }
}

TEST_CASE ("timecode: every frame of a day round-trips through its label at every rate")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto day = framesPerDay (rate);

        /*  Every frame of the first ten minutes and a half - two drop-frame
            blocks and the dropping minutes inside them - and then every 997th
            frame to the end of the day, which crosses every hour. */
        for (std::int64_t frame = 0; frame < 19000; ++frame)
        {
            const auto named = labelOf (frame, rate);
            REQUIRE (frameOf (named, rate) == frame);
            REQUIRE (normalise (named, rate) == named);
        }

        for (std::int64_t frame = 19000; frame < day; frame += 997)
            REQUIRE (frameOf (labelOf (frame, rate), rate) == frame);

        CHECK (frameOf (labelOf (day - 1, rate), rate) == day - 1);
    }
}

TEST_CASE ("timecode: the non-drop rates count every label")
{
    CHECK (framesPerDay (Rate::fps25) == 86400 * 25);
    CHECK (framesPerDay (Rate::fps24) == 86400 * 24);
    CHECK (framesPerDay (Rate::fps23976) == 86400 * 24);
    CHECK (framesPerDay (Rate::fps2997) == 86400 * 30);
    CHECK (framesPerDay (Rate::fps30) == 86400 * 30);

    CHECK (frameOf (label (1, 0, 0, 0), Rate::fps25) == 90000);
    CHECK (text (labelOf (90000 + 25 * 61 + 7, Rate::fps25)) == "01:01:01:07");
    CHECK (frameOf (label (0, 1, 0, 0), Rate::fps2997) == 1800);
    CHECK (formatLabel (labelOf (1800, Rate::fps2997), Rate::fps2997, true) == "00:01:00:00");
}

TEST_CASE ("timecode: drop-frame leaves out :00 and :01 at every minute not divisible by ten")
{
    constexpr auto df = Rate::fps2997df;

    //  00:00:59;29 + one frame = 00:01:00;02.
    const auto beforeMinute = frameOf (label (0, 0, 59, 29), df);
    CHECK (beforeMinute == 1799);
    CHECK (labelOf (beforeMinute + 1, df) == label (0, 1, 0, 2));
    CHECK (formatLabel (labelOf (beforeMinute + 1, df), df, true) == "00:01:00;02");

    //  00:09:59;29 + one frame = 00:10:00;00, which keeps its :00.
    const auto beforeTen = frameOf (label (0, 9, 59, 29), df);
    CHECK (beforeTen == 17981);
    CHECK (labelOf (beforeTen + 1, df) == label (0, 10, 0, 0));
    CHECK (labelOf (beforeTen + 2, df) == label (0, 10, 0, 1));
    CHECK (labelOf (beforeTen + 3, df) == label (0, 10, 0, 2));

    //  A ten-minute block holds 17982 frames, a dropping minute 1798.
    CHECK (frameOf (label (0, 10, 0, 0), df) == 17982);
    CHECK (frameOf (label (0, 2, 0, 2), df) - frameOf (label (0, 1, 0, 2), df) == 1798);
    CHECK (frameOf (label (1, 0, 0, 0), df) == 6 * 17982);
    CHECK (framesPerDay (df) == 24 * 6 * 17982);

    //  And the label the rate lacks reads as the next one (AHF).
    CHECK (normalise (label (0, 1, 0, 0), df) == label (0, 1, 0, 2));
    CHECK (normalise (label (0, 1, 0, 1), df) == label (0, 1, 0, 2));
    CHECK (normalise (label (0, 10, 0, 0), df) == label (0, 10, 0, 0));
    CHECK (normalise (label (0, 1, 1, 0), df) == label (0, 1, 1, 0));
    CHECK (frameOf (label (0, 1, 0, 0), df) == frameOf (label (0, 1, 0, 2), df));
}

TEST_CASE ("timecode: frames past a rate's count carry into the next second")
{
    CHECK (normalise (label (0, 0, 10, 27), Rate::fps25) == label (0, 0, 11, 0));
    CHECK (normalise (label (0, 0, 59, 24), Rate::fps24) == label (0, 1, 0, 0));
    CHECK (normalise (label (0, 0, 59, 24), Rate::fps25) == label (0, 0, 59, 24));
    CHECK (normalise (label (23, 59, 59, 27), Rate::fps25) == label (0, 0, 0, 0));

    //  Carried into a dropping minute, and then to its :02.
    CHECK (normalise (label (0, 0, 59, 29), Rate::fps2997df) == label (0, 0, 59, 29));
    CHECK (normalise (label (0, 0, 59, 29), Rate::fps24) == label (0, 1, 0, 0));
}

TEST_CASE ("timecode: the day wraps at 24 hours, both ways")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto day = framesPerDay (rate);
        const auto last = nominalOf (rate) - 1;

        CHECK (labelOf (day, rate) == label (0, 0, 0, 0));
        CHECK (labelOf (day + 5, rate) == labelOf (5, rate));
        CHECK (labelOf (-1, rate) == label (23, 59, 59, last));
        CHECK (labelOf (day - 1, rate) == label (23, 59, 59, last));
        CHECK (frameOf (label (23, 59, 59, last), rate) == day - 1);

        //  The frame after the last of the day is the first.
        CHECK (labelOf (frameOf (label (23, 59, 59, last), rate) + 1, rate) == label (0, 0, 0, 0));
    }
}
