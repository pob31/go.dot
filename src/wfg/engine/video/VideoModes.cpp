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

#include <wfg/engine/video/VideoModes.h>

#include <array>
#include <numeric>
#include <utility>

namespace wfg::video::modes
{
    namespace
    {
        struct Size
        {
            int width;
            int height;
            const char* prefix;   ///< what the word says before the height
        };

        //  The sizes a word names by their height alone, or `dci` and it.
        constexpr std::array<Size, 7> named {{
            { 1280, 720, "" }, { 1920, 1080, "" }, { 3840, 2160, "" }, { 7680, 4320, "" },
            { 2048, 1080, "dci" }, { 4096, 2160, "dci" }, { 8192, 4320, "dci" },
        }};

        bool isDigit (char c) noexcept  { return c >= '0' && c <= '9'; }

        /*  Digits from `at`, as a number; nothing when there are none or too
            many to be a size or a rate. */
        std::optional<std::int64_t> number (const std::string& text, std::size_t& at)
        {
            const auto start = at;
            std::int64_t value = 0;

            while (at < text.size() && isDigit (text[at]) && at - start < 6)
                value = value * 10 + (text[at++] - '0');

            if (at == start || (at < text.size() && isDigit (text[at])))
                return std::nullopt;

            return value;
        }
    }

    double Format::frameRate() const noexcept
    {
        return frameDuration > 0 ? static_cast<double> (timeScale) / static_cast<double> (frameDuration) : 0.0;
    }

    std::string wordFor (const Format& format)
    {
        if (format.width <= 0 || format.height <= 0 || format.frameDuration <= 0 || format.timeScale <= 0)
            return {};

        std::string word;

        for (const auto& size : named)
            if (size.width == format.width && size.height == format.height)
                word = std::string (size.prefix) + std::to_string (size.height);

        if (word.empty())
            word = std::to_string (format.width) + "x" + std::to_string (format.height);

        word += format.interlaced ? 'i' : 'p';

        //  The rate in hundredths, in integers: no locale writes this word
        //  (PRD §3.20), and 60000/1001 is 59.94 on every machine.
        const auto perSecond = format.timeScale * (format.interlaced ? 2 : 1);

        if (perSecond % format.frameDuration == 0)
            return word + std::to_string (perSecond / format.frameDuration);

        const auto hundredths = (perSecond * 100 + format.frameDuration / 2) / format.frameDuration;
        const auto fraction = hundredths % 100;
        return word + std::to_string (hundredths / 100) + (fraction < 10 ? ".0" : ".") + std::to_string (fraction);
    }

    std::optional<Format> parse (const std::string& word)
    {
        Format format;
        std::size_t at = 0;
        const bool dci = word.rfind ("dci", 0) == 0;

        if (dci)
            at = 3;

        const auto first = number (word, at);

        if (! first)
            return std::nullopt;

        if (! dci && at < word.size() && word[at] == 'x')
        {
            ++at;
            const auto height = number (word, at);

            if (! height || *first <= 0 || *height <= 0)
                return std::nullopt;

            format.width = static_cast<int> (*first);
            format.height = static_cast<int> (*height);
        }
        else
        {
            for (const auto& size : named)
                if (size.height == *first && (std::string (size.prefix) == "dci") == dci)
                {
                    format.width = size.width;
                    format.height = size.height;
                }

            if (format.width == 0)
                return std::nullopt;
        }

        if (at >= word.size() || (word[at] != 'p' && word[at] != 'i'))
            return std::nullopt;

        format.interlaced = word[at++] == 'i';

        const auto whole = number (word, at);

        if (! whole || *whole <= 0)
            return std::nullopt;

        //  A rate a second - pictures, or fields when interlaced - as a frame
        //  duration in a time scale: whole rates exactly, and the broadcast
        //  ones a thousandth slow (23.98, 29.97, 59.94 ...) as N*1000/1001.
        std::int64_t duration = 1;
        std::int64_t scale = *whole;

        if (at < word.size())
        {
            if (word[at] != '.' || at + 3 != word.size() || ! isDigit (word[at + 1]) || ! isDigit (word[at + 2]))
                return std::nullopt;

            const auto hundredths = *whole * 100 + (word[at + 1] - '0') * 10 + (word[at + 2] - '0');
            const auto upTo = *whole + 1;
            const auto slow = (upTo * 1000 * 100 + 1001 / 2) / 1001;   // N*1000/1001 in hundredths

            if (slow == hundredths)
            {
                duration = 1001;
                scale = upTo * 1000;
            }
            else
            {
                duration = 100;
                scale = hundredths;
            }
        }

        //  Interlaced: a frame is two fields.
        if (format.interlaced)
            duration *= 2;

        const auto common = std::gcd (duration, scale);
        format.frameDuration = duration / common;
        format.timeScale = scale / common;

        //  Only the word this format makes is this format's word: "1080p050"
        //  and "1080p50.00" are not.
        if (wordFor (format) != word)
            return std::nullopt;

        return format;
    }
}
