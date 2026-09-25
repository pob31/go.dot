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
    A FILE'S LEVEL, FINER THAN ITS COLOURS.

    The author, 2026-09-25: "Can the waveform be more precise in level, not
    colour when zooming in." The waveform's height came from the timbre frames:
    one per 1024 samples, its peak in eight bits - so a zoomed-in bar was a
    staircase of 21 ms steps, and a quiet stretch had a handful of heights to
    choose from. The colour of a stretch is worth one frame; its SHAPE is not.

    So the analysis keeps a second, plain answer beside the pyramid: for every
    `hop` samples, the lowest and the highest sample of every channel, in
    sixteen bits - the lowest so the drawing can be the wave's real shape and
    not a mirror. Halved into coarser levels as the pyramid is, down to
    `coarsestPairs`, so a bar picks the level with a pair per column and every
    zoom costs the same to draw.

    CACHED BESIDE THE PYRAMID, as `<hash>.tpk` next to `<hash>.tpy`, and a file
    missing either is analysed again - one pass makes both. Its own format and
    not a section of the `.tpy`, which is served to the page as it is and read
    by a black-box decoder that refuses a byte past its last level.

    std only, like Timbre.h: a track can be built in a test and read back with
    no audio library in the room.
*/

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wfg::audio
{
    /** The lowest and the highest sample of a stretch, in sixteen bits. */
    struct PeakPair
    {
        std::int16_t low = 0;
        std::int16_t high = 0;

        bool operator== (const PeakPair&) const = default;
    };

    struct PeakTrack
    {
        std::uint32_t sampleRate = 0;
        std::uint64_t samples = 0;

        /*  Level 0 is one pair per `peaks::hop` samples; each level after it
            halves the one below, down to `peaks::coarsestPairs` or fewer. */
        std::vector<std::vector<PeakPair>> levels;

        /** The finest level's pair count, or nought for an empty track. */
        std::size_t pairs() const noexcept { return levels.empty() ? 0 : levels.front().size(); }
    };

    namespace peaks
    {
        /*  A pair per 64 samples: 1.3 ms at 48 kHz, eight a colour frame - a
            megabyte for six minutes of audio. */
        constexpr int hop = 64;

        constexpr std::size_t coarsestPairs = 64;

        /*  BUMPED WHENEVER THE TRACK CHANGES, as the pyramid's version is: a
            file carrying another is refused and built again. */
        constexpr std::uint16_t formatVersion = 1;

        /** A sample in [-1, 1] as sixteen bits, clamped. */
        std::int16_t toShort (float sample) noexcept;

        /** And back, to [-1, 1]. */
        double toUnit (std::int16_t value) noexcept;

        /** The pair of two pairs: the lower low and the higher high. */
        PeakPair pairOf (const PeakPair& first, const PeakPair& second) noexcept;

        /*  A whole track over a finest level: halvings until a level has
            `coarsestPairs` or fewer, a level with an odd count pairing its last
            with itself. */
        PeakTrack trackOf (std::vector<PeakPair> finest, std::uint32_t sampleRate, std::uint64_t samples);

        /*  THE FILE: "WFGK", the version and the hop (two bytes each), the rate
            (four), the samples (eight), the pair count (four), then the finest
            level's pairs, low then high, little-endian. The coarser levels are
            built again on reading - they are arithmetic on the finest. */
        std::vector<std::uint8_t> write (const PeakTrack& track);

        /*  False, leaving `out` alone, for anything that is not exactly that:
            another magic, version or hop, or a length that does not match. */
        bool read (const std::uint8_t* bytes, std::size_t size, PeakTrack& out);

        /*  THE PAIRS, a stretch at a time as the file is read, of any length
            and any number of channels - the analysis hands it the same
            stretches it colours. */
        class Collector
        {
        public:
            void add (const float* const* channels, int numChannels, int count);

            /** The track, the last short stretch included. The collector is spent. */
            PeakTrack finish (std::uint32_t sampleRate);

        private:
            std::vector<PeakPair> finest;
            float low = 0.0f;
            float high = 0.0f;
            int filled = 0;
            std::uint64_t samples = 0;
        };
    }
}
