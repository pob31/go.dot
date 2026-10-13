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
    WHERE AN INCOMING CODE IS AGAINST GO.DOT'S OWN CLOCK: a code map
    (namespace draft §60, TG.5; AII, AIO).

    The tick never follows a code (AII). Go.dot keeps its own clock, the
    interface's sample count, and an incoming code - LTC on an input, MTC on a
    port, and from TG.6 the wall clock - is MEASURED against it: this frame of
    the code fell on this Go.dot sample, and from there the code runs at this
    speed. That is all a map is - an anchor and a slope - and it is the one
    thing the Runner learns about a code: `code.heard` records carry one, a
    replay rebuilds it from them with no decoder, and a time group's member
    (TG.7) is started on the sample `sampleOf` gives for its label.

    THE ANCHOR is a WHOLE frame of the day, counted from 00:00:00:00 at the
    map's unit, and the Go.dot sample it falls on - the first sample at or
    after the frame's moment, FrameClock's convention. THE SPEED is code
    seconds per Go.dot second: 1 nominal, 1.00005 for a code fifty parts per
    million fast, negative for a code played backwards, 0 for one standing.

    EXACT AT SPEED ONE. A code locked to Go.dot's clock - the same word clock,
    or Go.dot's own LTC looped back - has a speed of exactly 1 (the watcher
    writes 1 when what it measured cannot be told from it), and then the map
    is FrameClock's rule itself, in whole numbers: frame `anchor + k` falls on
    `sample + frameStartSample (k)`, and a sample lies in the frame
    `frameAtSample` says. Nothing drifts from the sound beside it however long
    the code runs, and a member started on its label's sample starts on the
    very sample the code's frame does. Any other speed is read in doubles,
    which over a day at 192 kHz are good to a millionth of a sample.

    EVERYTHING WRAPS AT 24 HOURS, as timecode does: the frame after the day's
    last is 00:00:00:00, and the sample a frame falls on is the one where the
    code reaches it NEAREST the anchor - a frame half a day or more away is
    taken as the past - so a code running across midnight reads on.

    THE UNIT IS GENERIC so that the wall clock can be a map too (TG.6): a
    "frame" is `den / num` seconds of the source and a day holds `perDay` of
    them. A timecode's unit is its rate's - 25/1, 30000/1001 - with the rate
    kept for its labels; THE WALL CLOCK'S IS THE MILLISECOND OF THE DAY,
    1000/1 and 86 400 000 a day, with no labels of a rate (`wallClockUnit`,
    the word `ms`). TG.6 reads a clock time as its millisecond and asks the
    same `sampleOf`.

    PURE AND INLINE: no JUCE, no clock of its own, nothing allocated - the tick
    thread reads it every tick, and the tests on made-up numbers.
*/

#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/Timecode.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::timecode
{
    //==========================================================================
    /*  WHAT A CODE IS DOING: never heard since its source was named (or since
        the line restarted), moving, or silent for four frames (AIR). */
    enum class CodeState
    {
        absent,
        running,
        stopped
    };

    inline std::string_view wordOf (CodeState state) noexcept
    {
        switch (state)
        {
            case CodeState::running:  return "running";
            case CodeState::stopped:  return "stopped";
            case CodeState::absent:   break;
        }

        return "absent";
    }

    inline std::optional<CodeState> codeStateFromWord (std::string_view word) noexcept
    {
        if (word == "absent")   return CodeState::absent;
        if (word == "running")  return CodeState::running;
        if (word == "stopped")  return CodeState::stopped;
        return std::nullopt;
    }

    //==========================================================================
    /*  WHAT ONE OF A MAP'S FRAMES IS: `den / num` seconds, `perDay` of them a
        day, and the timecode rate whose labels name them - none for the wall
        clock, whose frames are milliseconds. */
    struct CodeUnit
    {
        std::int64_t num = 25;
        std::int64_t den = 1;
        std::int64_t perDay = 86'400 * 25;
        std::optional<Rate> rate = Rate::fps25;

        bool operator== (const CodeUnit&) const = default;
    };

    inline CodeUnit unitOf (Rate rate) noexcept
    {
        return { numOf (rate), denOf (rate), framesPerDay (rate), rate };
    }

    //  The wall clock's (TG.6): the millisecond of the day.
    inline CodeUnit wallClockUnit() noexcept
    {
        return { 1000, 1, 86'400'000, std::nullopt };
    }

    inline constexpr std::string_view wallClockWord = "ms";

    /*  The unit's word, as `code.heard` and a position carry it: the rate's
        own (`25`, `29.97df` - what `sync/frameRate` takes) or `ms`. And back;
        a word that is neither reads as nothing. */
    inline std::string unitWordOf (const CodeUnit& unit)
    {
        return unit.rate.has_value() ? std::string (wordOf (*unit.rate)) : std::string (wallClockWord);
    }

    inline std::optional<CodeUnit> unitFromWord (std::string_view word) noexcept
    {
        if (word == wallClockWord)
            return wallClockUnit();

        if (const auto rate = rateFromWord (word))
            return unitOf (*rate);

        return std::nullopt;
    }

    //==========================================================================
    /*  A frame of the day, and how far into it: where a code is at a sample. */
    struct CodePosition
    {
        std::int64_t frame = 0;
        double fraction = 0.0;
    };

    namespace codemap
    {
        //  Whether a double is exactly one, read by its bits: the exact path's test.
        inline bool isExactlyOne (double value) noexcept
        {
            return std::bit_cast<std::uint64_t> (value) == std::bit_cast<std::uint64_t> (1.0);
        }

        //  A frame count wrapped into the day, a negative one counted back from midnight.
        inline std::int64_t wrapped (std::int64_t frame, std::int64_t perDay) noexcept
        {
            if (perDay <= 0)
                return frame;

            const auto rest = frame % perDay;
            return rest < 0 ? rest + perDay : rest;
        }

        /*  A difference of two frames of the day as the nearer way round the
            dial: [-perDay / 2, perDay / 2). */
        inline std::int64_t nearestOffset (std::int64_t difference, std::int64_t perDay) noexcept
        {
            const auto rest = wrapped (difference, perDay);
            return rest >= perDay - perDay / 2 ? rest - perDay : rest;
        }
    }

    //==========================================================================
    struct CodeMap
    {
        CodeState state = CodeState::absent;
        CodeUnit unit {};

        //  The anchor: a whole frame of the day, and the Go.dot sample it falls on.
        std::int64_t frame = 0;
        std::int64_t sample = 0;

        //  Code seconds per Go.dot second: 1 nominal, negative backwards, 0 standing.
        double speed = 0.0;

        //  Go.dot's samples a second, which the anchor's sample counts.
        int sampleRate = 0;

        //  The code's frames run past since the anchor at `atSample`, not wrapped. Doubles.
        double framesSince (std::int64_t atSample) const noexcept
        {
            if (sampleRate <= 0 || unit.den <= 0)
                return 0.0;

            return static_cast<double> (atSample - sample) * speed * static_cast<double> (unit.num)
                     / (static_cast<double> (sampleRate) * static_cast<double> (unit.den));
        }

        /*  WHERE THE CODE IS AT A GO.DOT SAMPLE: its frame of the day and how
            far into it. A map that is not running is where its anchor says,
            whatever the sample. */
        CodePosition positionAt (std::int64_t atSample) const noexcept
        {
            CodePosition at { codemap::wrapped (frame, unit.perDay), 0.0 };

            if (state != CodeState::running || sampleRate <= 0 || unit.num <= 0 || unit.den <= 0)
                return at;

            if (codemap::isExactlyOne (speed))
            {
                /*  FRAMECLOCK'S RULE, IN WHOLE NUMBERS: the frame is
                    `frameAtSample` of the samples since the anchor, and the
                    fraction is what that division left over. The samples are
                    split into whole periods of the divisor first, so only a
                    remainder smaller than it meets the multiplier. */
                const auto divisor = static_cast<std::int64_t> (sampleRate) * unit.den;
                const auto delta = atSample - sample;
                auto whole = delta / divisor;
                auto rest = delta % divisor;

                if (rest < 0)
                {
                    rest += divisor;
                    --whole;
                }

                const auto scaled = rest * unit.num;
                at.frame = codemap::wrapped (frame + whole * unit.num + scaled / divisor, unit.perDay);
                at.fraction = static_cast<double> (scaled % divisor) / static_cast<double> (divisor);
                return at;
            }

            const auto since = framesSince (atSample);
            const auto whole = std::floor (since);
            at.frame = codemap::wrapped (frame + static_cast<std::int64_t> (whole), unit.perDay);
            at.fraction = since - whole;
            return at;
        }

        /*  THE GO.DOT SAMPLE A FRAME OF THE DAY FALLS ON: the first sample at
            or after the moment the code reaches the frame's start, nearest the
            anchor round the dial. A code played backwards reaches a frame's
            start at its other end, as LTC read backwards places a frame
            (LtcDecoder.h): the sample where the code goes below the frame's
            start, after which it reads the frame before. A standing or stopped
            code reaches only its own frame, on its anchor's sample; an absent
            one none. */
        std::optional<std::int64_t> sampleOf (std::int64_t frameOfDay) const noexcept
        {
            if (state == CodeState::absent || sampleRate <= 0 || unit.num <= 0 || unit.den <= 0)
                return std::nullopt;

            const auto ahead = codemap::nearestOffset (frameOfDay - frame, unit.perDay);

            if (state != CodeState::running || ! std::isfinite (speed) || ! (std::abs (speed) > 0.0))
                return ahead == 0 ? std::optional<std::int64_t> (sample) : std::nullopt;

            if (codemap::isExactlyOne (speed))
                return sample + frameclock::scaledCeil (ahead, static_cast<std::int64_t> (sampleRate) * unit.den, unit.num);

            /*  IN DOUBLES, then mended by a sample either way: the moment is
                worked out in floating point, and the sample is the one the
                map's own `positionAt` agrees is the first in the frame - so
                the two are each other's inverse to the sample. */
            const auto target = static_cast<double> (ahead);
            const auto moment = target * static_cast<double> (sampleRate) * static_cast<double> (unit.den)
                                  / (static_cast<double> (unit.num) * speed);
            auto at = sample + static_cast<std::int64_t> (std::ceil (moment));
            const auto forwards = speed > 0.0;

            //  Reached: at or past the frame's start in the direction the code runs.
            const auto reached = [this, target, forwards] (std::int64_t candidate)
            {
                const auto since = framesSince (candidate);
                return forwards ? since >= target : since <= target;
            };

            for (int step = 0; step < 4 && ! reached (at); ++step)
                ++at;

            for (int step = 0; step < 4 && reached (at - 1); ++step)
                --at;

            return at;
        }

        bool operator== (const CodeMap& other) const noexcept
        {
            return state == other.state && unit == other.unit && frame == other.frame && sample == other.sample
                     && std::bit_cast<std::uint64_t> (speed) == std::bit_cast<std::uint64_t> (other.speed)
                     && sampleRate == other.sampleRate;
        }
    };

    //==========================================================================
    /*  A FRAME OF THE DAY AS SOMEBODY READS IT: `HH:MM:SS:FF` at a timecode
        rate, with colons as the show stores labels (AHF), and `HH:MM:SS` for
        the wall clock's millisecond. */
    inline std::string labelTextOf (const CodeUnit& unit, std::int64_t frameOfDay)
    {
        if (unit.rate.has_value())
            return formatLabel (labelOf (frameOfDay, *unit.rate), *unit.rate);

        const auto second = codemap::wrapped (frameOfDay, unit.perDay) / 1000;
        const auto two = [] (std::int64_t value)
        {
            return std::string (value < 10 ? "0" : "") + std::to_string (value);
        };

        return two (second / 3600) + ":" + two (second / 60 % 60) + ":" + two (second % 60);
    }

    /*  WHAT THE SHOW PUBLISHES OF AN INCOMING CODE (`sync/ltcIn*`,
        `sync/mtcIn*`; TG.2's rows): what it is doing, the label it reads now,
        its rate in the words `sync/frameRate` takes, and its speed. `none`
        when the show names no source for it - the default here, and what the
        caller leaves when there is none - `absent` when it is named and no map
        has been heard, and otherwise the map's: a running code's label at the
        sample now, a stopped one's where it stopped, nought for its speed. */
    struct CodeReadout
    {
        std::string state = "none";
        std::string code;
        std::string rate;
        double speed = 0.0;
    };

    inline CodeReadout readoutOf (const CodeMap* map, std::int64_t now)
    {
        CodeReadout out;

        if (map == nullptr || map->state == CodeState::absent)
        {
            out.state = "absent";
            return out;
        }

        out.state = std::string (wordOf (map->state));
        out.rate = unitWordOf (map->unit);

        if (map->state == CodeState::running)
        {
            out.code = labelTextOf (map->unit, map->positionAt (now).frame);
            out.speed = map->speed;
        }
        else
        {
            out.code = labelTextOf (map->unit, map->frame);
        }

        return out;
    }
}
