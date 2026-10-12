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

#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstddef>

namespace wfg::timecode
{
    namespace
    {
        /*  ONE ROW PER RATE, in the enumeration's order, so a rate's facts are
            read off one line rather than out of six switches that could be
            edited apart. */
        struct Facts
        {
            Rate rate;
            std::int64_t num;
            std::int64_t den;
            int nominal;
            bool drop;
            int mtcCode;
            std::string_view word;
        };

        constexpr std::array<Facts, 6> facts {{
            { Rate::fps23976,  24000, 1001, 24, false, 0, "23.976" },
            { Rate::fps24,        24,    1, 24, false, 0, "24" },
            { Rate::fps25,        25,    1, 25, false, 1, "25" },
            { Rate::fps2997df, 30000, 1001, 30, true,  2, "29.97df" },
            { Rate::fps2997,   30000, 1001, 30, false, 3, "29.97" },
            { Rate::fps30,        30,    1, 30, false, 3, "30" },
        }};

        const Facts& factsOf (Rate rate) noexcept
        {
            for (const auto& row : facts)
                if (row.rate == rate)
                    return row;

            return facts[2];        // 25, the default (AHG); unreachable for a real Rate
        }

        /*  THE DROP-FRAME BLOCKS. Two names dropped a minute, nine minutes in
            ten: a minute that drops holds 30 * 60 - 2 frames, and ten minutes
            hold 30 * 600 - 9 * 2. */
        constexpr std::int64_t droppedPerMinute = 2;
        constexpr std::int64_t framesPerDroppingMinute = 30 * 60 - droppedPerMinute;
        constexpr std::int64_t framesPerTenMinutes = 30 * 600 - 9 * droppedPerMinute;

        /*  A digit pair at `at`, or -1. */
        int twoDigits (std::string_view text, std::size_t at) noexcept
        {
            if (at + 2 > text.size())
                return -1;

            const auto high = text[at];
            const auto low = text[at + 1];

            if (high < '0' || high > '9' || low < '0' || low > '9')
                return -1;

            return (high - '0') * 10 + (low - '0');
        }

        /*  A FRAME'S PLACE IN ITS DAY, never below nought: `%` keeps the sign
            of what it divides, so a frame counted back past midnight would
            otherwise come out negative and name nothing. */
        std::int64_t wrapped (std::int64_t frame, std::int64_t day) noexcept
        {
            const auto rest = frame % day;
            return rest < 0 ? rest + day : rest;
        }
    }

    std::int64_t numOf (Rate rate) noexcept     { return factsOf (rate).num; }
    std::int64_t denOf (Rate rate) noexcept     { return factsOf (rate).den; }
    int nominalOf (Rate rate) noexcept          { return factsOf (rate).nominal; }
    bool dropsFrames (Rate rate) noexcept       { return factsOf (rate).drop; }
    int mtcCodeOf (Rate rate) noexcept          { return factsOf (rate).mtcCode; }
    std::string_view wordOf (Rate rate) noexcept { return factsOf (rate).word; }

    std::optional<Rate> rateFromWord (std::string_view word) noexcept
    {
        for (const auto& row : facts)
            if (row.word == word)
                return row.rate;

        return std::nullopt;
    }

    std::optional<Label> parseLabel (std::string_view text) noexcept
    {
        /*  EXACTLY ELEVEN CHARACTERS, `HH:MM:SS:FF`. A label is the one thing
            a timecode reader shows, and a looser reading - one digit for the
            hours, spaces, a frame count - would store a guess at what somebody
            meant where AHF promises what they decided. */
        if (text.size() != 11)
            return std::nullopt;

        const auto between = [&text] (std::size_t at, bool beforeFrames)
        {
            const auto c = text[at];
            return c == ':' || c == ';' || (beforeFrames && c == '.');
        };

        if (! between (2, false) || ! between (5, false) || ! between (8, true))
            return std::nullopt;

        Label label;
        label.hours = twoDigits (text, 0);
        label.minutes = twoDigits (text, 3);
        label.seconds = twoDigits (text, 6);
        label.frames = twoDigits (text, 9);

        if (label.hours < 0 || label.hours > 23 || label.minutes < 0 || label.minutes > 59
              || label.seconds < 0 || label.seconds > 59 || label.frames < 0 || label.frames >= 30)
            return std::nullopt;

        return label;
    }

    std::string formatLabel (const Label& label, Rate rate, bool driftSeparator)
    {
        const auto two = [] (int value)
        {
            const auto clamped = value < 0 ? 0 : value % 100;
            return std::string { static_cast<char> ('0' + clamped / 10), static_cast<char> ('0' + clamped % 10) };
        };

        const auto last = driftSeparator && dropsFrames (rate) ? ";" : ":";

        return two (label.hours) + ":" + two (label.minutes) + ":" + two (label.seconds) + last
                 + two (label.frames);
    }

    std::int64_t framesPerDay (Rate rate) noexcept
    {
        if (dropsFrames (rate))
            return 24 * 6 * framesPerTenMinutes;

        return std::int64_t { 86400 } * nominalOf (rate);
    }

    Label normalise (const Label& label, Rate rate) noexcept
    {
        auto out = label;
        const auto nominal = nominalOf (rate);

        /*  FRAMES PAST THE RATE CARRY into the next second: 00:00:10:27 at 25
            is a label the rate does not have, and the one after it is
            00:00:11:00. The carry runs up through the minutes and the hours
            and round the clock, as everything here does. */
        if (out.frames >= nominal)
        {
            out.frames = 0;
            ++out.seconds;
        }

        if (out.seconds >= 60)
        {
            out.seconds = 0;
            ++out.minutes;
        }

        if (out.minutes >= 60)
        {
            out.minutes = 0;
            ++out.hours;
        }

        if (out.hours >= 24)
            out.hours = 0;

        //  THE TWO NAMES A DROPPING MINUTE LEAVES OUT read as its third.
        if (dropsFrames (rate) && out.seconds == 0 && out.frames < droppedPerMinute && out.minutes % 10 != 0)
            out.frames = static_cast<int> (droppedPerMinute);

        return out;
    }

    std::int64_t frameOf (const Label& label, Rate rate) noexcept
    {
        const auto named = normalise (label, rate);
        const auto nominal = static_cast<std::int64_t> (nominalOf (rate));

        const auto seconds = std::int64_t { named.hours } * 3600 + std::int64_t { named.minutes } * 60
                               + named.seconds;
        auto frame = seconds * nominal + named.frames;

        /*  AT DROP-FRAME, LESS THE NAMES LEFT OUT before it: two for every
            minute so far that was not a tenth one. */
        if (dropsFrames (rate))
        {
            const auto minutes = std::int64_t { named.hours } * 60 + named.minutes;
            frame -= droppedPerMinute * (minutes - minutes / 10);
        }

        return wrapped (frame, framesPerDay (rate));
    }

    Label labelOf (std::int64_t frameIn, Rate rate) noexcept
    {
        auto frame = wrapped (frameIn, framesPerDay (rate));
        const auto nominal = static_cast<std::int64_t> (nominalOf (rate));

        /*  AT DROP-FRAME, THE NAMES LEFT OUT ARE PUT BACK before the count is
            split: eighteen for every whole ten minutes, and two for every
            dropping minute begun in the block this frame is in. The block's
            first minute keeps its :00 and :01, which is why the first two
            frames of a block are counted apart (`inBlock < 2`). */
        if (dropsFrames (rate))
        {
            const auto blocks = frame / framesPerTenMinutes;
            const auto inBlock = frame % framesPerTenMinutes;

            frame += 9 * droppedPerMinute * blocks;

            if (inBlock >= droppedPerMinute)
                frame += droppedPerMinute * ((inBlock - droppedPerMinute) / framesPerDroppingMinute);
        }

        Label label;
        label.frames = static_cast<int> (frame % nominal);
        label.seconds = static_cast<int> ((frame / nominal) % 60);
        label.minutes = static_cast<int> ((frame / (nominal * 60)) % 60);
        label.hours = static_cast<int> ((frame / (nominal * 3600)) % 24);
        return label;
    }
}
