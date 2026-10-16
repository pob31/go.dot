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

#include <wfg/engine/video/render/SoundPlacer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wfg::video::card
{
    namespace
    {
        constexpr double pi = 3.14159265358979323846;

        /*  How much history is kept behind the oldest sample still to be read:
            a quarter of a second, so a card a little behind reads what it
            needs and the rest is let go. */
        constexpr std::int64_t keptBehind = 12000;

        //  At a card's 48 kHz, eight taps either side.
        constexpr int baseHalfWidth = 8;

        std::int32_t toInteger (double value) noexcept
        {
            const auto clamped = std::clamp (value, -1.0, 1.0);
            return static_cast<std::int32_t> (std::lround (clamped * 2147483647.0));
        }
    }

    void SoundPlacer::setChannels (int cardChannels)
    {
        channelCount = std::max (0, cardChannels);
        history.assign (static_cast<std::size_t> (channelCount), {});
        historyBegun = false;
        historyStart = 0;
        begun = false;
    }

    int SoundPlacer::historyLength() const noexcept
    {
        return history.empty() ? 0 : static_cast<int> (history.front().size());
    }

    void SoundPlacer::take (std::int64_t sample, int frames, int rateOfBlock, const std::vector<const float*>& lane)
    {
        if (channelCount == 0 || frames <= 0 || rateOfBlock <= 0)
            return;

        //  A new rate, or a count that went back: the history begins again.
        if (rateOfBlock != rate || (historyBegun && sample < historyStart + historyLength()))
        {
            for (auto& one : history)
                one.clear();

            historyBegun = false;
            rate = rateOfBlock;
        }

        if (! historyBegun)
        {
            historyStart = sample;
            historyBegun = true;
        }

        //  A block that never came: silence of its length.
        const auto gap = sample - (historyStart + historyLength());

        for (std::size_t channel = 0; channel < history.size(); ++channel)
        {
            auto& one = history[channel];

            if (gap > 0)
                one.insert (one.end(), static_cast<std::size_t> (gap), 0.0f);

            const auto* from = channel < lane.size() ? lane[channel] : nullptr;

            if (from != nullptr)
                one.insert (one.end(), from, from + frames);
            else
                one.insert (one.end(), static_cast<std::size_t> (frames), 0.0f);
        }

        /*  NOTHING READ FOR TWO SECONDS - the card not yet sending, no line to
            read by - and the history keeps its last second, never more. */
        if (historyLength() > 2 * rate)
        {
            const auto drop = historyLength() - rate;

            for (auto& one : history)
                one.erase (one.begin(), one.begin() + drop);

            historyStart += drop;
        }
    }

    void SoundPlacer::startAt (std::int64_t cardSample) noexcept
    {
        cursor = cardSample;
        begun = true;
    }

    void SoundPlacer::makeTable (double cutoff)
    {
        tableCutoff = cutoff;
        halfWidth = static_cast<int> (std::ceil (baseHalfWidth / cutoff));
        const auto taps = 2 * halfWidth;
        table.assign (static_cast<std::size_t> ((phases + 1) * taps), 0.0f);

        //  Row f: the taps for a read a fraction f/phases past a sample, at
        //  offsets -halfWidth+1 .. halfWidth, Blackman-windowed, summing to one.
        for (int f = 0; f <= phases; ++f)
        {
            const auto fraction = static_cast<double> (f) / phases;
            double sum = 0.0;

            for (int k = 0; k < taps; ++k)
            {
                const auto t = static_cast<double> (k - halfWidth + 1) - fraction;
                const auto x = cutoff * t;
                const auto sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                const auto w = 0.5 + 0.5 * (t / static_cast<double> (halfWidth));   // 0..1 across the window
                const auto window = std::abs (t) >= static_cast<double> (halfWidth) ? 0.0
                                                                                   : 0.42 - 0.5 * std::cos (2.0 * pi * w) + 0.08 * std::cos (4.0 * pi * w);
                const auto tap = cutoff * sinc * window;
                table[static_cast<std::size_t> (f * taps + k)] = static_cast<float> (tap);
                sum += tap;
            }

            if (sum > 1.0e-12)
                for (int k = 0; k < taps; ++k)
                    table[static_cast<std::size_t> (f * taps + k)] = static_cast<float> (table[static_cast<std::size_t> (f * taps + k)] / sum);
        }
    }

    double SoundPlacer::readAt (int channel, double position) const
    {
        const auto& samples = history[static_cast<std::size_t> (channel)];
        const auto base = static_cast<std::int64_t> (std::floor (position));
        const auto fraction = position - static_cast<double> (base);
        const auto row = static_cast<int> (std::lround (fraction * phases));
        const auto taps = 2 * halfWidth;
        const auto first = base - historyStart - halfWidth + 1;
        double sum = 0.0;

        for (int k = 0; k < taps; ++k)
        {
            const auto at = first + k;

            if (at >= 0 && at < static_cast<std::int64_t> (samples.size()))
                sum += static_cast<double> (table[static_cast<std::size_t> (row * taps + k)]) * samples[static_cast<std::size_t> (at)];
        }

        return sum;
    }

    int SoundPlacer::place (std::int64_t untilCardSample, const std::function<double (std::int64_t)>& goDotAt,
                            std::vector<std::int32_t>& out)
    {
        out.clear();

        if (! begun || channelCount == 0 || ! historyBegun || rate <= 0 || untilCardSample <= cursor)
            return 0;

        //  The cutoff for this rate: a source faster than the card's is read
        //  through a filter narrowed to the card's half rate.
        const auto ratio = static_cast<double> (rate) / 48000.0;
        const auto cutoff = std::min (1.0, 1.0 / ratio);

        if (std::abs (cutoff - tableCutoff) > 1.0e-6)
            makeTable (cutoff);

        //  The line is straight over a few milliseconds: read at its two ends
        //  and between them, a chunk at a time.
        constexpr std::int64_t chunk = 240;
        int placed = 0;
        auto lowest = std::numeric_limits<double>::max();

        while (cursor < untilCardSample)
        {
            const auto count = std::min<std::int64_t> (chunk, untilCardSample - cursor);
            const auto from = goDotAt (cursor);
            const auto to = goDotAt (cursor + count);
            const auto step = (to - from) / static_cast<double> (count);

            //  Not here yet: what the last of the chunk reads has not come.
            if (to + halfWidth >= static_cast<double> (knownUntil()))
                break;

            for (std::int64_t n = 0; n < count; ++n)
            {
                const auto position = from + step * static_cast<double> (n);
                lowest = std::min (lowest, position);
                const auto known = position - halfWidth + 1 >= static_cast<double> (historyStart);

                if (! known)
                    ++lateSamples;

                for (int channel = 0; channel < channelCount; ++channel)
                    out.push_back (known ? toInteger (readAt (channel, position)) : 0);
            }

            cursor += count;
            placed += static_cast<int> (count);
        }

        //  What no later read will reach is let go, a quarter of a second behind.
        if (placed > 0)
        {
            const auto keepFrom = static_cast<std::int64_t> (std::floor (lowest)) - halfWidth - keptBehind;
            const auto drop = std::clamp<std::int64_t> (keepFrom - historyStart, 0, historyLength());

            if (drop > 0)
            {
                for (auto& one : history)
                    one.erase (one.begin(), one.begin() + static_cast<std::ptrdiff_t> (drop));

                historyStart += drop;
            }
        }

        return placed;
    }
}
