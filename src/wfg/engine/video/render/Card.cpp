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

#include <wfg/engine/video/render/Card.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <objbase.h>
#endif

namespace wfg::video::card
{
    System& system()
    {
        //  Made once, from whichever thread asks first; a function-local
        //  static is made once whatever the threads (C++11).
        static const std::unique_ptr<System> made = []
        {
            if (juce::SystemStats::getEnvironmentVariable ("WFG_DECKLINK_FAKE", {}) != "1")
                return makeDeckLinkSystem();

            FakeOptions options;
            options.driftPpm = juce::SystemStats::getEnvironmentVariable ("WFG_DECKLINK_FAKE_DRIFT", "0").getDoubleValue();
            options.dropEvery = juce::SystemStats::getEnvironmentVariable ("WFG_DECKLINK_FAKE_DROP", "0").getIntValue();
            return makeFakeSystem (options);
        }();

        return *made;
    }

    ThreadScope::ThreadScope()
    {
       #if defined (_WIN32)
        //  The multithreaded apartment: a card's callbacks come on the
        //  driver's own threads. A thread already in a single-threaded one
        //  (JUCE's message thread) is refused, and is not where cards are
        //  opened.
        entered = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
       #endif
    }

    ThreadScope::~ThreadScope()
    {
       #if defined (_WIN32)
        if (entered)
            CoUninitialize();
       #endif
    }

    const char* keyingWord (Keying keying) noexcept
    {
        switch (keying)
        {
            case Keying::external: return "external";
            case Keying::internal: return "internal";
            case Keying::off:      break;
        }

        return "off";
    }

    Keying keyingFrom (const std::string& word) noexcept
    {
        if (word == "external")  return Keying::external;
        if (word == "internal")  return Keying::internal;
        return Keying::off;
    }

    const char* referenceWord (Reference reference) noexcept
    {
        switch (reference)
        {
            case Reference::locked:   return "locked";
            case Reference::unlocked: return "unlocked";
            case Reference::none:     break;
        }

        return "none";
    }

    std::string describe (const Found& found)
    {
        if (! found.problem.empty())
            return "problem\t" + found.problem;

        std::string out;

        for (const auto& card : found.cards)
        {
            std::string can;
            const auto add = [&can] (bool has, const std::string& word)
            {
                if (has)
                    can += (can.empty() ? "" : " ") + word;
            };

            add (card.output, "output");
            add (card.input, "input");
            add (card.internalKey, "internal");
            add (card.externalKey, "external");
            add (card.tenBit, "10bit");
            add (card.audioChannels > 0, "sound" + std::to_string (card.audioChannels));

            std::string sends, takes;

            for (const auto& mode : card.modes)
            {
                if (mode.output)
                    sends += (sends.empty() ? "" : " ") + mode.word;

                if (mode.input)
                    takes += (takes.empty() ? "" : " ") + mode.word;
            }

            out += (out.empty() ? "" : "\n") + card.id + "\t" + card.name + "\t" + can + "\t" + sends + "\t" + takes;
        }

        return out;
    }

    std::vector<std::uint8_t> packV210 (const float* rgba, int width, int height, int rowBytes)
    {
        std::vector<std::uint8_t> out (static_cast<std::size_t> (rowBytes) * static_cast<std::size_t> (height), 0);

        if (width <= 0 || height <= 0)
            return out;

        const auto clamp01 = [] (float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        const auto words = rowBytes / 4;

        for (int y = 0; y < height; ++y)
        {
            const auto at = [&] (int x, int c)
            {
                const auto px = x < width ? x : width - 1;
                return clamp01 (rgba[(static_cast<std::size_t> (y) * static_cast<std::size_t> (width) + static_cast<std::size_t> (px)) * 4
                                     + static_cast<std::size_t> (c)]);
            };

            const auto luma = [] (float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };

            const auto sample = [&] (int group, int i) -> std::uint32_t
            {
                const auto first = group * 6;

                if ((i & 1) == 1)
                {
                    const auto x = first + (i - 1) / 2;
                    const auto l = luma (at (x, 0), at (x, 1), at (x, 2));
                    return static_cast<std::uint32_t> (std::min (1019.0f, std::max (4.0f, std::floor (64.0f + 876.0f * l + 0.5f))));
                }

                const auto pair = i / 4;
                const auto x = first + 2 * pair;
                const auto r = (at (x, 0) + at (x + 1, 0)) * 0.5f;
                const auto g = (at (x, 1) + at (x + 1, 1)) * 0.5f;
                const auto b = (at (x, 2) + at (x + 1, 2)) * 0.5f;
                const auto l = luma (r, g, b);
                const auto d = (i % 4) == 0 ? (b - l) / 1.8556f : (r - l) / 1.5748f;
                return static_cast<std::uint32_t> (std::min (1019.0f, std::max (4.0f, std::floor (512.0f + 896.0f * d + 0.5f))));
            };

            for (int w = 0; w < words; ++w)
            {
                const auto group = w / 4;
                const auto k = w % 4;
                const auto word = sample (group, 3 * k) | (sample (group, 3 * k + 1) << 10) | (sample (group, 3 * k + 2) << 20);
                auto* to = out.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (rowBytes) + static_cast<std::size_t> (w) * 4;
                to[0] = static_cast<std::uint8_t> (word & 0xffu);
                to[1] = static_cast<std::uint8_t> ((word >> 8) & 0xffu);
                to[2] = static_cast<std::uint8_t> ((word >> 16) & 0xffu);
                to[3] = static_cast<std::uint8_t> ((word >> 24) & 0xffu);
            }
        }

        return out;
    }

    int v210Sample (const std::vector<std::uint8_t>& packed, int rowBytes, int y, int i) noexcept
    {
        const auto word = static_cast<std::size_t> (i / 3);
        const auto at = static_cast<std::size_t> (y) * static_cast<std::size_t> (rowBytes) + word * 4;

        if (at + 4 > packed.size())
            return -1;

        const std::uint32_t value = static_cast<std::uint32_t> (packed[at]) | (static_cast<std::uint32_t> (packed[at + 1]) << 8)
                                  | (static_cast<std::uint32_t> (packed[at + 2]) << 16) | (static_cast<std::uint32_t> (packed[at + 3]) << 24);
        return static_cast<int> ((value >> (10 * (i % 3))) & 0x3ffu);
    }

    int rowBytesFor (PixelFormat format, int width) noexcept
    {
        switch (format)
        {
            case PixelFormat::bgra8: return width * 4;
            case PixelFormat::yuv8:  return width * 2;
            case PixelFormat::yuv10: return ((width + 47) / 48) * 128;
        }

        return width * 4;
    }
}
