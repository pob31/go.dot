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
    THE TESTS' OWN LTC READER (namespace draft §59, TC.3): what a timecode
    reader on the end of the line would make of the sound the generator draws.

    WRITTEN FROM THE STANDARD'S DESCRIPTION AND NOTHING OF GO.DOT'S. It
    includes no Go.dot header - LtcEncoder.h least of all - and shares no code
    with the encoder or the generator, so a generator that drew the wrong bits
    the same wrong way its encoder laid them out is caught here rather than
    agreed with. What it knows is SMPTE 12M as it is publicly described (the
    "Linear timecode" article's bit table, libltc's `LTCFrame`):

    - BIPHASE MARK. Every bit cell begins with a change of level, and a one
      has a second change in its middle; a zero has none. So the line is read
      by the intervals between its changes of level: a whole cell is a zero,
      two half cells in a row are a one. Which way the level goes means
      nothing, so the reader never asks.
    - THE CHANGES are found as zero crossings with hysteresis: the line must
      have gone beyond a quarter of its peak on one side before a crossing to
      the other side counts, so a wobble near nought is not an edge. A
      crossing is placed on the first sample of the new sign. A line that
      falls quiet - below the quarter for longer than a bit - has ended, and
      that end is an edge too, placed where it fell through half its peak: it
      is what closes the last half cell of the last frame, as the next frame's
      first edge would have.
    - AN INTERVAL SHORTER THAN THREE QUARTERS OF A BIT is a half cell, a
      longer one a whole cell; one below a quarter of a bit or above a bit and
      a half is no part of a bit at all - a glitch, or the line gone - and the
      reader starts again.
    - THE SYNC WORD, bits 64 to 79 in the order they arrive, is 0011 1111
      1111 1101. The reader keeps the last eighty bits and calls them a frame
      when their last sixteen are the sync word: twelve ones in a row appear
      nowhere else in a frame.
    - THE FIELDS, each binary-coded decimal, least significant bit first:
      frame units 0-3, frame tens 8-9, the drop-frame flag 10, second units
      16-19, second tens 24-26, minute units 32-35, minute tens 40-42, hour
      units 48-51, hour tens 56-57.

    Each frame is reported with the sample of the edge that began its bit 0,
    whether its ones are even - the polarity bit's whole purpose - and its
    drop-frame flag. Test code: plain, allocating, and in no hurry.
*/

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ltcreader
{
    struct Frame
    {
        int hours = 0;
        int minutes = 0;
        int seconds = 0;
        int frames = 0;

        bool dropFrame = false;

        //  Whether the eighty bits hold an even number of ones.
        bool evenParity = false;

        //  The eighty bits as they arrived, bit 0 first.
        std::array<bool, 80> bits {};

        //  The sample - counted from the origin the reader was given - of the
        //  edge that began bit 0.
        std::int64_t sample = 0;
    };

    //  A change of level the reader saw, or the place the line fell quiet.
    struct Edge
    {
        std::int64_t sample = 0;
        bool endsSignal = false;
    };

    /*  THE CHANGES OF LEVEL in a stretch of line, in order. `quietSamples` is
        how long the line has to stay below a quarter of its peak to count as
        ended: longer than any real interval, so a bit. */
    inline std::vector<Edge> edgesOf (const float* samples, std::size_t count, double quietSamples)
    {
        std::vector<Edge> edges;
        float peak = 0.0f;

        for (std::size_t n = 0; n < count; ++n)
            peak = std::fmax (peak, std::fabs (samples[n]));

        if (! (peak > 0.0f))
            return edges;

        const auto threshold = peak / 4.0f;
        const auto half = peak / 2.0f;

        //  +1 or -1: the side of nought the line was last seen beyond the
        //  threshold on; 0 while it is quiet.
        int side = 0;

        int sign = 0;
        std::int64_t signSince = 0;
        std::int64_t quietSince = -1;
        std::int64_t belowHalfSince = -1;

        for (std::size_t index = 0; index < count; ++index)
        {
            const auto n = static_cast<std::int64_t> (index);
            const auto x = samples[index];
            const auto magnitude = std::fabs (x);
            const int newSign = x > 0.0f ? 1 : (x < 0.0f ? -1 : 0);

            if (newSign != sign)
            {
                sign = newSign;
                signSince = n;
            }

            if (magnitude < half)
            {
                if (belowHalfSince < 0)
                    belowHalfSince = n;
            }
            else
            {
                belowHalfSince = -1;
            }

            if (magnitude < threshold)
            {
                if (quietSince < 0)
                    quietSince = n;
            }
            else
            {
                quietSince = -1;
            }

            if (x > threshold && side != 1)
            {
                edges.push_back ({ signSince, false });
                side = 1;
            }
            else if (x < -threshold && side != -1)
            {
                edges.push_back ({ signSince, false });
                side = -1;
            }
            else if (side != 0 && quietSince >= 0 && static_cast<double> (n - quietSince) >= quietSamples)
            {
                auto at = belowHalfSince >= 0 ? belowHalfSince : quietSince;

                if (! edges.empty() && at <= edges.back().sample)
                    at = edges.back().sample + 1;

                edges.push_back ({ at, true });
                side = 0;
            }
        }

        return edges;
    }

    namespace detail
    {
        inline int field (const std::array<bool, 80>& bits, std::size_t first, std::size_t width)
        {
            int value = 0;

            for (std::size_t b = 0; b < width; ++b)
                if (bits[first + b])
                    value |= 1 << b;

            return value;
        }

        //  Bits 64 to 79 in the order they arrive.
        constexpr std::array<bool, 16> syncWord { false, false, true, true, true, true, true, true,
                                                  true, true, true, true, true, true, false, true };

        inline Frame frameOf (const std::vector<bool>& window, const std::vector<std::int64_t>& starts,
                              std::int64_t origin)
        {
            Frame frame;
            const auto first = window.size() - 80;

            for (std::size_t n = 0; n < 80; ++n)
                frame.bits[n] = window[first + n];

            frame.frames = field (frame.bits, 0, 4) + 10 * field (frame.bits, 8, 2);
            frame.seconds = field (frame.bits, 16, 4) + 10 * field (frame.bits, 24, 3);
            frame.minutes = field (frame.bits, 32, 4) + 10 * field (frame.bits, 40, 3);
            frame.hours = field (frame.bits, 48, 4) + 10 * field (frame.bits, 56, 2);
            frame.dropFrame = frame.bits[10];

            int ones = 0;

            for (const auto bit : frame.bits)
                ones += bit ? 1 : 0;

            frame.evenParity = ones % 2 == 0;
            frame.sample = origin + starts[first];
            return frame;
        }

        inline bool endsInSync (const std::vector<bool>& window)
        {
            if (window.size() < 80)
                return false;

            const auto first = window.size() - 16;

            for (std::size_t n = 0; n < 16; ++n)
                if (window[first + n] != syncWord[n])
                    return false;

            return true;
        }
    }

    /*  EVERY WHOLE FRAME in a stretch of line, in the order they arrived.
        `sampleRate` and `framesPerSecond` give the length of a bit, which is
        all the reader needs to tell a half cell from a whole one; `origin` is
        added to every sample it reports. */
    inline std::vector<Frame> decode (const float* samples, std::size_t count, double sampleRate,
                                      double framesPerSecond, std::int64_t origin = 0)
    {
        std::vector<Frame> frames;

        if (samples == nullptr || count == 0 || ! (sampleRate > 0.0) || ! (framesPerSecond > 0.0))
            return frames;

        const auto bitLength = sampleRate / (framesPerSecond * 80.0);
        const auto edges = edgesOf (samples, count, bitLength);

        std::vector<bool> window;
        std::vector<std::int64_t> starts;
        bool halfWaiting = false;
        std::int64_t halfStart = 0;

        const auto restart = [&]
        {
            window.clear();
            starts.clear();
            halfWaiting = false;
        };

        const auto push = [&] (bool bit, std::int64_t start)
        {
            window.push_back (bit);
            starts.push_back (start);

            if (window.size() > 80)
            {
                window.erase (window.begin());
                starts.erase (starts.begin());
            }

            if (detail::endsInSync (window))
            {
                frames.push_back (detail::frameOf (window, starts, origin));
                window.clear();
                starts.clear();
            }
        };

        for (std::size_t i = 0; i + 1 < edges.size(); ++i)
        {
            if (edges[i].endsSignal)
            {
                restart();
                continue;
            }

            const auto length = static_cast<double> (edges[i + 1].sample - edges[i].sample);

            if (length < 0.25 * bitLength || length > 1.5 * bitLength)
            {
                restart();
                continue;
            }

            if (length < 0.75 * bitLength)
            {
                if (halfWaiting)
                {
                    halfWaiting = false;
                    push (true, halfStart);
                }
                else
                {
                    halfWaiting = true;
                    halfStart = edges[i].sample;
                }
            }
            else
            {
                /*  A half cell left waiting before a whole one: the reader was
                    out of step by half a bit, and a whole cell puts it back. */
                halfWaiting = false;
                push (false, edges[i].sample);
            }
        }

        return frames;
    }
}
