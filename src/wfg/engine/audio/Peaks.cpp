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

#include <wfg/engine/audio/Peaks.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace wfg::audio::peaks
{
    namespace
    {
        constexpr std::array<std::uint8_t, 4> magic { { 'W', 'F', 'G', 'K' } };
        constexpr std::size_t headerSize = 4 + 2 + 2 + 4 + 8 + 4;
        constexpr std::size_t pairSize = 4;

        void putLittleEndian (std::vector<std::uint8_t>& out, std::uint64_t value, int bytes)
        {
            for (int i = 0; i < bytes; ++i)
                out.push_back (static_cast<std::uint8_t> ((value >> (8 * i)) & 0xffu));
        }

        std::uint64_t getLittleEndian (const std::uint8_t* at, int bytes) noexcept
        {
            std::uint64_t value = 0;

            for (int i = 0; i < bytes; ++i)
                value |= static_cast<std::uint64_t> (at[i]) << (8 * i);

            return value;
        }
    }

    std::int16_t toShort (float sample) noexcept
    {
        const auto clamped = std::clamp (static_cast<double> (sample), -1.0, 1.0);
        return static_cast<std::int16_t> (std::lround (clamped * 32767.0));
    }

    double toUnit (std::int16_t value) noexcept
    {
        return std::clamp (static_cast<double> (value) / 32767.0, -1.0, 1.0);
    }

    PeakPair pairOf (const PeakPair& first, const PeakPair& second) noexcept
    {
        return PeakPair { std::min (first.low, second.low), std::max (first.high, second.high) };
    }

    PeakTrack trackOf (std::vector<PeakPair> finest, std::uint32_t sampleRate, std::uint64_t samples)
    {
        PeakTrack track;
        track.sampleRate = sampleRate;
        track.samples = samples;

        if (finest.empty())
            return track;

        track.levels.push_back (std::move (finest));

        while (track.levels.back().size() > coarsestPairs)
        {
            const auto& finer = track.levels.back();
            std::vector<PeakPair> coarser;
            coarser.reserve ((finer.size() + 1) / 2);

            for (std::size_t at = 0; at < finer.size(); at += 2)
                coarser.push_back (at + 1 < finer.size() ? pairOf (finer[at], finer[at + 1]) : finer[at]);

            track.levels.push_back (std::move (coarser));
        }

        return track;
    }

    std::vector<std::uint8_t> write (const PeakTrack& track)
    {
        const auto& finest = track.levels.empty() ? std::vector<PeakPair> {} : track.levels.front();

        std::vector<std::uint8_t> out;
        out.reserve (headerSize + finest.size() * pairSize);

        out.insert (out.end(), magic.begin(), magic.end());
        putLittleEndian (out, formatVersion, 2);
        putLittleEndian (out, static_cast<std::uint64_t> (hop), 2);
        putLittleEndian (out, track.sampleRate, 4);
        putLittleEndian (out, track.samples, 8);
        putLittleEndian (out, finest.size(), 4);

        for (const auto& pair : finest)
        {
            putLittleEndian (out, static_cast<std::uint16_t> (pair.low), 2);
            putLittleEndian (out, static_cast<std::uint16_t> (pair.high), 2);
        }

        return out;
    }

    bool read (const std::uint8_t* bytes, std::size_t size, PeakTrack& out)
    {
        if (bytes == nullptr || size < headerSize)
            return false;

        if (! std::equal (magic.begin(), magic.end(), bytes))
            return false;

        if (getLittleEndian (bytes + 4, 2) != formatVersion
             || getLittleEndian (bytes + 6, 2) != static_cast<std::uint64_t> (hop))
            return false;

        const auto sampleRate = static_cast<std::uint32_t> (getLittleEndian (bytes + 8, 4));
        const auto samples = getLittleEndian (bytes + 12, 8);
        const auto count = static_cast<std::size_t> (getLittleEndian (bytes + 20, 4));

        if (size != headerSize + count * pairSize)
            return false;

        std::vector<PeakPair> finest (count);

        for (std::size_t at = 0; at < count; ++at)
        {
            const auto* pair = bytes + headerSize + at * pairSize;
            finest[at].low = static_cast<std::int16_t> (static_cast<std::uint16_t> (getLittleEndian (pair, 2)));
            finest[at].high = static_cast<std::int16_t> (static_cast<std::uint16_t> (getLittleEndian (pair + 2, 2)));
        }

        out = trackOf (std::move (finest), sampleRate, samples);
        return true;
    }

    //==========================================================================
    void Collector::add (const float* const* channels, int numChannels, int count)
    {
        if (channels == nullptr || numChannels <= 0 || count <= 0)
            return;

        for (int i = 0; i < count; ++i)
        {
            //  The first value a stretch sees starts it, whichever channel has one.
            auto starting = filled == 0;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                if (channels[channel] == nullptr)
                    continue;

                const auto value = channels[channel][i];

                if (starting)
                {
                    low = value;
                    high = value;
                    starting = false;
                }
                else
                {
                    low = std::min (low, value);
                    high = std::max (high, value);
                }
            }

            ++samples;

            if (++filled == hop)
            {
                finest.push_back ({ toShort (low), toShort (high) });
                filled = 0;
            }
        }
    }

    PeakTrack Collector::finish (std::uint32_t sampleRate)
    {
        if (filled > 0)
        {
            finest.push_back ({ toShort (low), toShort (high) });
            filled = 0;
        }

        return trackOf (std::move (finest), sampleRate, samples);
    }
}
