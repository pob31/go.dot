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
    What a sound is made of, frame by frame - PRD §3.30's arithmetic, and
    nothing else (namespace draft §14.12).

    PURE. No thread, no file, no cache and no clock: samples in, a pyramid
    out, and the pyramid to bytes and back. `MediaAnalyser` is what opens a
    file, hashes it and keeps the cache; this is what it asks. So a test can
    hand it a sine it generated a line earlier and know the answer before the
    code runs - which is the whole of what §3.30 demands of it before anything
    is drawn, because a colour ramp is exactly the kind of thing that looks
    right and is wrong.

    ONE FRAME PER HOP. Frame k describes samples [k hop, (k + 1) hop) - the
    stretch of the file one pixel of a bar covers at the finest zoom - and its
    2048-sample window is CENTRED on that stretch, half a hop either side, so
    the colour drawn over a stretch is the colour of that stretch and not of
    the one before it. Outside the file is silence. A file of n samples has
    ceil(n / hop) frames, and none at all when it has no samples.

    FOUR BYTES A FRAME, and what carries what is §3.30's, not a choice made
    here:
      - HUE is the ramp's hue at the frame's spectral centroid, taken on a
        log-frequency axis from 40 Hz to 16 kHz;
      - SATURATION is one minus the spectral flatness of the MAGNITUDE
        spectrum over that band - a sine near 1, broadband noise near 0.15.
        On power, noise would read 0.44 and could never be grey: the
        geometric-over-arithmetic mean of exponentially distributed powers is
        e to the minus Euler's constant, whatever the level;
      - LIGHTNESS climbs from 0.15 to 0.85 with the log of that same
        centroid, so it IS the frequency axis - dark low, bright high - and a
        colourblind operator separates the bass bed from the high effect by
        brightness alone (§4.8);
      - PEAK is the largest absolute sample in the stretch, over every
        channel, clipped at full scale: the waveform's height, so the editor
        draws its outline and its colour from one file.

    SILENCE HAS NO COLOUR. A frame whose in-band power is below what a
    -100 dBFS sine would put there has hue, saturation and lightness all
    nought. Lightness nought is below the ramp's darkest, so it cannot be
    mistaken for the bottom of the ramp; and it is how a coarser level knows
    to borrow nothing from it (`pairOf`).

    THE RAMP IS NOT MONOTONIC IN HUE, and the check that proves the code says
    so rather than hiding it. Plan decision 8's stops run near-black purple
    (280 degrees) at 40 Hz, deep blue (240) at 150 Hz, then red, orange,
    yellow and green the other way round the wheel - so from 40 Hz to 150 Hz
    the hue turns back before it climbs. What IS monotonic is the lightness,
    by construction; the sweep check asserts that, and asserts that every
    frame's hue is the ramp's hue at the sweep's frequency then. The stops
    are the author's to move once the three signals are on screen (§14.12).
*/

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace wfg::audio
{
    struct TimbrePyramid;

    namespace timbre
    {
        /*  The analysis, as fixed numbers. A cache written with any other
            window or hop is refused on reading and built again. */
        constexpr int windowSize = 2048;
        constexpr int hopSize = 1024;

        /*  The pyramid stops at the first level with this many frames or
            fewer: a Gogo bar is about forty pixels wide, and a level coarser
            than it would be read by nothing. */
        constexpr std::size_t coarsestFrames = 64;

        /*  BUMPED WHENEVER THE ANALYSIS CHANGES - a stop moved, a rule for a
            coarser level changed - and not only when the layout does: a cache
            is keyed by the content of the audio alone, so this number is the
            only thing that tells a pyramid computed by an older rule from one
            computed by this one. A file carrying another version is refused
            and rebuilt, never read. */
        constexpr std::uint16_t formatVersion = 1;

        /*  The band the centroid and the flatness are taken over. */
        constexpr double lowestHertz = 40.0;
        constexpr double highestHertz = 16000.0;

        /*  Lightness at the bottom and the top of that band. */
        constexpr double darkest = 0.15;
        constexpr double brightest = 0.85;

        /*  One frame: hue in 256 steps of a full turn, the other three in 255
            steps of the unit range. */
        struct Frame
        {
            std::uint8_t hue = 0;
            std::uint8_t saturation = 0;
            std::uint8_t lightness = 0;
            std::uint8_t peak = 0;
        };

        bool operator== (const Frame&, const Frame&) noexcept;
        bool operator!= (const Frame&, const Frame&) noexcept;

        /*  THE RAMP: the hue, in degrees in [0, 360), of a centroid at
            `hertz`. Interpolated in the log of the frequency between plan
            decision 8's six stops, and held at the end stops outside them. */
        double rampHue (double hertz);

        /*  The lightness, in [0.15, 0.85], of a centroid at `hertz`: linear
            in its log across the band, and held at the band's ends. */
        double rampLightness (double hertz);

        /*  A frame's bytes, as the numbers a client prints. */
        double hueOf (const Frame&) noexcept;          // degrees, [0, 360)
        double saturationOf (const Frame&) noexcept;   // [0, 1]
        double lightnessOf (const Frame&) noexcept;    // [0, 1]; nought is silence
        double peakOf (const Frame&) noexcept;         // [0, 1]

        /*  A frame is silent when it carries no colour at all. */
        bool isSilent (const Frame&) noexcept;

        //==========================================================================
        /*  ONE FRAME OF A COARSER LEVEL, from two of the finer one - and the
            rule is written out because a test and a black-box driver each
            restate it and must land on the same byte.

              - A silent frame lends nothing: paired with a sounding one, the
                pair has the sounding one's colour; two silent frames make a
                silent one. Averaging silence in would pull the lightness down
                - a lie in the one dimension that is the frequency axis - and
                its hue, nought by convention, would tint every quiet stretch
                red.
              - Otherwise saturation and lightness are the mean, a half rounded
                up; and the hue moves from the first frame's towards the
                second's the SHORTER way round the wheel, by a share of the arc
                equal to the second's share of the two saturations (a half
                step rounded up; equal shares when both are grey). A grey frame
                has no hue worth averaging, and a vivid one dragged halfway
                towards it would be a colour neither frame had.
              - Peak is the larger of the two.

            A level with an odd number of frames pairs its last one with
            itself, which leaves it unchanged. */
        Frame pairOf (const Frame& first, const Frame& second) noexcept;

        /*  The level above `finer`: ceil(size / 2) frames by `pairOf`. */
        std::vector<Frame> halve (const std::vector<Frame>& finer);

        /*  A whole pyramid over a finest level: halvings until a level has
            `coarsestFrames` or fewer. */
        TimbrePyramid pyramidOf (std::vector<Frame> finest, std::uint32_t sampleRate,
                                 std::uint64_t samples);

        //==========================================================================
        /*  THE ANALYSIS, one stretch at a time, so a file an hour long is read
            a hop at a time rather than held whole. Hand it every stretch in
            order - `hopSize` samples of each channel, and fewer only for the
            last - then `finish` once.

            One per thread: it owns an FFT and its scratch. */
        class Analyser
        {
        public:
            explicit Analyser (double sampleRate);
            ~Analyser();

            Analyser (const Analyser&) = delete;
            Analyser& operator= (const Analyser&) = delete;
            Analyser (Analyser&&) = delete;
            Analyser& operator= (Analyser&&) = delete;

            /*  The next stretch: `count` samples, at most `hopSize`, of each
                of `numChannels` channels. The channels are summed and
                averaged for the spectrum; the peak is taken over all of them,
                so a stereo pair in opposite phase still has a waveform. */
            void add (const float* const* channels, int numChannels, int count);

            /*  Frames computed so far: the unit of the work the verb reports. */
            std::size_t framesAnalysed() const noexcept;

            /*  The last frame, then the levels above the finest. Once. */
            TimbrePyramid finish();

        private:
            struct Impl;
            std::unique_ptr<Impl> impl;
        };

        /*  The same, over buffers already in memory - what the tests call. */
        TimbrePyramid analyse (const float* const* channels, int numChannels,
                               std::int64_t numSamples, double sampleRate);

        //==========================================================================
        /*  THE FILE, `<bundle>/media/.timbre/<sha256>.tpy`, little-endian:

                 0   4   "WFGT"
                 4   2   formatVersion
                 6   2   level count, L
                 8   4   sample rate, Hz
                12   4   window (2048)
                16   4   hop (1024)
                20   4   FNV-1a, 32 bits, of every byte from offset 32 to the end
                24   8   samples analysed
                32  8L   per level: frame count, then the offset of its first
                         frame from the start of the file (four bytes each)
                 .       the frames, finest level first, four bytes each: hue,
                         saturation, lightness, peak

            The checksum is there because the cache is written without being
            made durable - it is regenerable, and M23 found a durable replace
            costs the Windows box several milliseconds a file, which an import
            of hundreds of files has no show's sake to pay - so a power cut may
            leave a file the right length and the wrong bytes. `read` refuses
            it, and the analyser builds it again. */
        std::vector<std::uint8_t> write (const TimbrePyramid&);

        /*  True when `bytes` is a whole pyramid this build would have written:
            every field above checked, every level the size the one below it
            implies, the level count the one `pyramidOf` stops at, the length
            exact and the checksum right. `into` is untouched when it is
            false. */
        bool read (const std::uint8_t* bytes, std::size_t size, TimbrePyramid& into);
    }

    /*  WHAT A FILE SOUNDS LIKE, AT EVERY ZOOM. MediaInfo.h declares it and
        holds it by `shared_ptr<const ...>`; this is the definition, a STRUCT as
        that declaration says (MSVC C4099 and Clang -Wmismatched-tags would warn
        at a mismatch; GCC would not).

        Level 0 is one frame per hop; each level after it is half the one below,
        by `timbre::pairOf`, down to `coarsestFrames` or fewer. A client drawing
        a bar picks the level whose frame count is nearest its pixel count and
        reads it once, so no redraw recomputes anything (§3.30). */
    struct TimbrePyramid
    {
        std::uint32_t sampleRate = 0;
        std::uint64_t samples = 0;
        std::vector<std::vector<timbre::Frame>> levels;

        /*  The finest level's frame count, or nought for an empty pyramid. */
        std::size_t frames() const noexcept { return levels.empty() ? 0 : levels.front().size(); }
    };
}
