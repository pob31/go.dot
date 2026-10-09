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

#include <wfg/engine/video/Strip.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wfg::video::strip
{
    namespace
    {
        //  What a cut must stand out by: an eighth, and three times its neighbourhood's middle.
        constexpr double hardFloor = 0.12;
        constexpr double hardOverMedian = 3.0;

        //  A change this stark is believed however short the shot before it.
        constexpr double stark = 0.5;
        constexpr double shortestShot = 0.4;

        //  A dissolve: raised distances, frame after frame, whose ends differ by this much, within this long.
        constexpr double raised = 0.01;
        constexpr double dissolveApart = 0.3;
        constexpr double longestDissolve = 2.5;
        constexpr std::size_t shortestDissolve = 3;

        double medianOf (std::vector<double> values)
        {
            if (values.empty())
                return 0.0;

            const auto middle = values.size() / 2;
            std::nth_element (values.begin(), values.begin() + static_cast<std::ptrdiff_t> (middle), values.end());
            return values[middle];
        }

        //  THE BYTES OF THE CACHE, little-endian whatever the machine.
        struct Writer
        {
            std::vector<std::uint8_t> bytes;

            void u8 (std::uint8_t value)    { bytes.push_back (value); }

            void u32 (std::uint32_t value)
            {
                for (int n = 0; n < 4; ++n)
                    bytes.push_back (static_cast<std::uint8_t> ((value >> (8 * n)) & 0xffu));
            }

            void f64 (double value)
            {
                std::uint64_t raw = 0;
                std::memcpy (&raw, &value, sizeof (raw));

                for (int n = 0; n < 8; ++n)
                    bytes.push_back (static_cast<std::uint8_t> ((raw >> (8 * n)) & 0xffu));
            }
        };

        struct Reader
        {
            const std::uint8_t* at;
            std::size_t left;
            bool ok = true;

            bool take (std::size_t count)
            {
                if (! ok || left < count)
                {
                    ok = false;
                    return false;
                }

                return true;
            }

            std::uint8_t u8()
            {
                if (! take (1))
                    return 0;

                const auto value = *at;
                ++at;
                --left;
                return value;
            }

            std::uint32_t u32()
            {
                if (! take (4))
                    return 0;

                std::uint32_t value = 0;

                for (int n = 0; n < 4; ++n)
                    value |= static_cast<std::uint32_t> (at[n]) << (8 * n);

                at += 4;
                left -= 4;
                return value;
            }

            double f64()
            {
                if (! take (8))
                    return 0.0;

                std::uint64_t raw = 0;

                for (int n = 0; n < 8; ++n)
                    raw |= static_cast<std::uint64_t> (at[n]) << (8 * n);

                at += 8;
                left -= 8;

                double value = 0.0;
                std::memcpy (&value, &raw, sizeof (value));
                return value;
            }
        };

        std::uint32_t checksumOf (const std::uint8_t* bytes, std::size_t size) noexcept
        {
            std::uint32_t hash = 2166136261u;

            for (std::size_t n = 0; n < size; ++n)
            {
                hash ^= bytes[n];
                hash *= 16777619u;
            }

            return hash;
        }

        constexpr std::uint32_t magic = 0x53544447u;    // "GDTS", read little-endian
    }

    Signature signatureOf (const Sampler& sample)
    {
        Signature out;
        std::array<double, gridWidth * gridHeight * 3> sums {};
        std::array<double, histogramBins> counts {};

        for (int row = 0; row < sampleHeight; ++row)
            for (int column = 0; column < sampleWidth; ++column)
            {
                const auto u = (column + 0.5) / sampleWidth;
                const auto v = (row + 0.5) / sampleHeight;
                double red = 0.0, green = 0.0, blue = 0.0;

                if (! sample || ! sample (u, v, red, green, blue))
                    red = green = blue = 0.0;

                const auto cell = static_cast<std::size_t> ((row / 4) * gridWidth + column / 4);
                sums[cell * 3] += red;
                sums[cell * 3 + 1] += green;
                sums[cell * 3 + 2] += blue;

                const auto luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
                const auto bin = std::clamp (static_cast<int> (luma / 256.0 * histogramBins), 0, histogramBins - 1);
                counts[static_cast<std::size_t> (bin)] += 1.0;
            }

        for (std::size_t n = 0; n < sums.size(); ++n)
            out.grid[n] = static_cast<std::uint8_t> (std::clamp (std::lround (sums[n] / 16.0), 0L, 255L));

        const auto total = static_cast<double> (sampleWidth * sampleHeight);

        for (std::size_t n = 0; n < counts.size(); ++n)
            out.histogram[n] = static_cast<float> (counts[n] / total);

        return out;
    }

    double distance (const Signature& a, const Signature& b) noexcept
    {
        double grid = 0.0;

        for (std::size_t n = 0; n < a.grid.size(); ++n)
            grid += std::abs (static_cast<double> (a.grid[n]) - static_cast<double> (b.grid[n]));

        grid /= static_cast<double> (a.grid.size()) * 255.0;

        /*  HOW FAR THE BRIGHTNESS MOVED, not whether it changed step: the two
            spreads summed up step by step and compared, so a fade's light
            moving one step reads as one step's worth, and a cut from dark to
            light as most of the way (the earth mover's distance, in one
            dimension). */
        double spread = 0.0, sumA = 0.0, sumB = 0.0;

        for (std::size_t n = 0; n < a.histogram.size(); ++n)
        {
            sumA += static_cast<double> (a.histogram[n]);
            sumB += static_cast<double> (b.histogram[n]);
            spread += std::abs (sumA - sumB);
        }

        return 0.5 * grid + 0.5 * std::min (1.0, spread / static_cast<double> (histogramBins));
    }

    void CutFinder::add (double seconds, const Signature& signature)
    {
        seen.push_back ({ seconds, signature });
    }

    std::vector<Cut> CutFinder::finish() const
    {
        const auto count = seen.size();

        if (count < 3)
            return {};

        std::vector<double> apart (count, 0.0);

        for (std::size_t n = 1; n < count; ++n)
            apart[n] = distance (seen[n - 1].signature, seen[n].signature);

        //  A SECOND EITHER SIDE, in frames as fed.
        const auto step = (seen.back().seconds - seen.front().seconds) / static_cast<double> (count - 1);
        const auto window = step > 0.0 ? std::max<std::size_t> (2, static_cast<std::size_t> (std::lround (1.0 / step)))
                                       : std::size_t { 2 };

        std::vector<double> median (count, 0.0);

        for (std::size_t n = 1; n < count; ++n)
        {
            std::vector<double> around;

            for (auto at = n > window ? n - window : std::size_t { 1 }; at <= std::min (count - 1, n + window); ++at)
                if (at != n)
                    around.push_back (apart[at]);

            median[n] = medianOf (std::move (around));
        }

        const auto hardAt = [&] (std::size_t n)
        {
            return apart[n] > std::max (hardFloor, hardOverMedian * median[n]);
        };

        std::vector<Cut> cuts;
        std::vector<bool> hard (count, false);
        auto lastCut = seen.front().seconds;

        //  THE HARD CUTS: standing out, the largest nearby, not a flash, not too soon.
        for (std::size_t n = 1; n < count; ++n)
        {
            if (! hardAt (n))
                continue;

            auto largest = true;

            for (auto at = n > 2 ? n - 2 : std::size_t { 1 }; at <= std::min (count - 1, n + 2); ++at)
                if (at != n && apart[at] > apart[n])
                    largest = false;

            if (! largest)
                continue;

            //  A FLASH: this frame unlike both neighbours, which are alike - or this the return from one.
            if (n + 1 < count && distance (seen[n - 1].signature, seen[n + 1].signature) < 0.5 * apart[n])
                continue;

            if (n >= 2 && distance (seen[n - 2].signature, seen[n].signature) < 0.5 * apart[n])
                continue;

            if (seen[n].seconds - lastCut < shortestShot && apart[n] < stark)
                continue;

            cuts.push_back ({ seen[n].seconds, apart[n], false });
            hard[n] = true;
            lastCut = seen[n].seconds;
        }

        //  THE DISSOLVES: a run of raised distances, no hard cut in it, whose ends differ.
        std::size_t n = 1;

        while (n < count)
        {
            if (hard[n] || apart[n] <= raised)
            {
                ++n;
                continue;
            }

            const auto start = n;

            while (n < count && ! hard[n] && apart[n] > raised)
                ++n;

            const auto end = n - 1;
            const auto length = end - start + 1;
            const auto from = seen[start - 1].seconds;
            const auto to = seen[end].seconds;

            if (length < shortestDissolve || to - from > longestDissolve)
                continue;

            const auto ends = distance (seen[start - 1].signature, seen[end].signature);

            if (ends <= dissolveApart)
                continue;

            const auto middle = 0.5 * (from + to);
            const auto nearHard = std::any_of (cuts.begin(), cuts.end(),
                                               [middle] (const Cut& cut) { return std::abs (cut.seconds - middle) < shortestShot; });

            if (! nearHard)
                cuts.push_back ({ middle, ends, true });
        }

        std::sort (cuts.begin(), cuts.end(), [] (const Cut& a, const Cut& b) { return a.seconds < b.seconds; });
        return cuts;
    }

    int firstFrameOfShot (int before, int after, const std::function<Signature (int frame)>& at)
    {
        if (after - before <= 1 || ! at)
            return after;

        const auto last = at (before);
        const auto next = at (after);
        auto low = before;
        auto high = after;

        while (high - low > 1)
        {
            const auto middle = low + (high - low) / 2;
            const auto here = at (middle);

            if (distance (here, next) < distance (here, last))
                high = middle;
            else
                low = middle;
        }

        return high;
    }

    const Thumbnail* MovieStrip::thumbnailAt (double seconds) const noexcept
    {
        if (thumbnails.empty())
            return nullptr;

        const auto after = std::upper_bound (thumbnails.begin(), thumbnails.end(), seconds,
                                             [] (double value, const Thumbnail& thumbnail) { return value < thumbnail.seconds; });

        return after == thumbnails.begin() ? &thumbnails.front() : &*std::prev (after);
    }

    std::vector<double> thumbnailTimes (double duration, const std::vector<Cut>& cuts)
    {
        if (! (duration > 0.0))
            return {};

        //  Just inside each new shot, so the picture is the shot and not the change.
        std::vector<double> starts { 0.0 };

        for (const auto& cut : cuts)
        {
            const auto inside = cut.seconds + (cut.gradual ? 0.5 : 0.05);

            if (inside < duration)
                starts.push_back (inside);
        }

        //  TOO MANY SHOTS for the pictures allowed: every so many of them.
        if (starts.size() > maxThumbnails)
        {
            std::vector<double> fewer;
            const auto every = static_cast<double> (starts.size()) / static_cast<double> (maxThumbnails);

            for (std::size_t n = 0; n < maxThumbnails; ++n)
                fewer.push_back (starts[static_cast<std::size_t> (std::floor (static_cast<double> (n) * every))]);

            return fewer;
        }

        auto step = std::max (1.0, duration / 300.0);
        std::vector<double> out;

        for (int tries = 0; tries < 32; ++tries)
        {
            out = starts;

            for (auto at = step; at < duration; at += step)
            {
                const auto nearAStart = std::any_of (starts.begin(), starts.end(),
                                                     [at, step] (double start) { return std::abs (start - at) < 0.5 * step; });

                if (! nearAStart)
                    out.push_back (at);
            }

            if (out.size() <= maxThumbnails)
                break;

            step *= 1.25;
        }

        std::sort (out.begin(), out.end());
        out.erase (std::unique (out.begin(), out.end(), [] (double a, double b) { return std::abs (a - b) < 0.02; }), out.end());

        if (out.size() > maxThumbnails)
            out.resize (maxThumbnails);

        return out;
    }

    int thumbnailHeightFor (int movieWidth, int movieHeight) noexcept
    {
        if (movieWidth <= 0 || movieHeight <= 0)
            return 45;

        return std::clamp (static_cast<int> (std::lround (static_cast<double> (thumbnailWidth) * movieHeight / movieWidth)), 1, 160);
    }

    std::vector<std::uint8_t> encode (const MovieStrip& strip)
    {
        Writer out;
        out.u32 (magic);
        out.u32 (formatVersion);
        out.f64 (strip.duration);
        out.u32 (static_cast<std::uint32_t> (std::max (0, strip.width)));
        out.u32 (static_cast<std::uint32_t> (std::max (0, strip.height)));

        out.u32 (static_cast<std::uint32_t> (strip.cuts.size()));

        for (const auto& cut : strip.cuts)
        {
            out.f64 (cut.seconds);
            out.f64 (cut.strength);
            out.u8 (cut.gradual ? 1 : 0);
        }

        out.u32 (static_cast<std::uint32_t> (strip.thumbnails.size()));

        for (const auto& thumbnail : strip.thumbnails)
        {
            out.f64 (thumbnail.seconds);
            out.u32 (static_cast<std::uint32_t> (std::max (0, thumbnail.width)));
            out.u32 (static_cast<std::uint32_t> (std::max (0, thumbnail.height)));
            out.bytes.insert (out.bytes.end(), thumbnail.rgb.begin(), thumbnail.rgb.end());
        }

        out.u32 (checksumOf (out.bytes.data(), out.bytes.size()));
        return out.bytes;
    }

    bool decode (const std::uint8_t* bytes, std::size_t size, MovieStrip& out)
    {
        if (bytes == nullptr || size < 8)
            return false;

        //  THE CHECKSUM FIRST: the last four bytes, over everything before them.
        std::uint32_t stored = 0;

        for (int n = 0; n < 4; ++n)
            stored |= static_cast<std::uint32_t> (bytes[size - 4 + static_cast<std::size_t> (n)]) << (8 * n);

        if (stored != checksumOf (bytes, size - 4))
            return false;

        Reader in { bytes, size - 4 };

        if (in.u32() != magic || in.u32() != formatVersion)
            return false;

        MovieStrip strip;
        strip.duration = in.f64();
        strip.width = static_cast<int> (in.u32());
        strip.height = static_cast<int> (in.u32());

        const auto cutCount = in.u32();

        if (! in.ok || cutCount > 1000000u)
            return false;

        for (std::uint32_t n = 0; n < cutCount && in.ok; ++n)
        {
            Cut cut;
            cut.seconds = in.f64();
            cut.strength = in.f64();
            cut.gradual = in.u8() != 0;
            strip.cuts.push_back (cut);
        }

        const auto thumbnailCount = in.u32();

        if (! in.ok || thumbnailCount > 100000u)
            return false;

        for (std::uint32_t n = 0; n < thumbnailCount && in.ok; ++n)
        {
            Thumbnail thumbnail;
            thumbnail.seconds = in.f64();
            thumbnail.width = static_cast<int> (in.u32());
            thumbnail.height = static_cast<int> (in.u32());

            const auto bytesOf = static_cast<std::size_t> (thumbnail.width) * static_cast<std::size_t> (thumbnail.height) * 3;

            if (thumbnail.width > 4096 || thumbnail.height > 4096 || ! in.take (bytesOf))
                return false;

            thumbnail.rgb.assign (in.at, in.at + bytesOf);
            in.at += bytesOf;
            in.left -= bytesOf;
            strip.thumbnails.push_back (std::move (thumbnail));
        }

        if (! in.ok || in.left != 0)
            return false;

        out = std::move (strip);
        return true;
    }
}
