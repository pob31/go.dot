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

#include <wfg/engine/audio/Timbre.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <juce_dsp/juce_dsp.h>

namespace wfg::audio::timbre
{
    namespace
    {
        constexpr int fftOrder = 11;

        static_assert ((1 << fftOrder) == windowSize, "the FFT is the window");
        static_assert (windowSize == 2 * hopSize && hopSize % 2 == 0,
                       "a frame's window is its own stretch and half of each neighbour's");

        /*  PLAN DECISION 8'S SIX STOPS, a starting point and not a palette
            (§14.12). The hue is UNWRAPPED - 360 is red, 480 is green - so the
            interpolation from deep blue to red goes through magenta and not
            back through cyan; it is wrapped into [0, 360) only at the end.

            The first segment runs DOWN, purple to blue, and every other one
            up. That is the stops as written, not an accident, and it is why
            nothing in the tests asserts a monotonic hue (Timbre.h). */
        struct RampStop
        {
            double hertz;
            double hue;
        };

        constexpr std::array<RampStop, 6> ramp { {
            {    40.0, 280.0 },     // near-black purple
            {   150.0, 240.0 },     // deep blue
            {   500.0, 360.0 },     // red
            {  1500.0, 390.0 },     // orange
            {  4000.0, 420.0 },     // yellow
            { 12000.0, 480.0 },     // green
        } };

        /*  -100 dBFS: below this a frame has no colour. */
        constexpr double silentAmplitude = 1.0e-5;

        /*  What a magnitude of nought is taken as inside a logarithm. A sine
            generated in floating point leaves no bin at exactly nought, but a
            window that is half past the end of the file can. */
        constexpr double smallestMagnitude = 1.0e-20;

        constexpr std::size_t headerSize = 32;
        constexpr std::size_t levelEntrySize = 8;
        constexpr std::size_t frameSize = 4;
        constexpr std::array<std::uint8_t, 4> magic { { 'W', 'F', 'G', 'T' } };

        std::uint8_t unitToByte (double value) noexcept
        {
            if (! (value > 0.0))
                return 0;

            if (value >= 1.0)
                return 255;

            return static_cast<std::uint8_t> (std::lround (value * 255.0));
        }

        std::uint8_t hueToByte (double degrees) noexcept
        {
            const auto step = std::lround (degrees * 256.0 / 360.0);
            return static_cast<std::uint8_t> (((step % 256) + 256) % 256);
        }

        /*  Rounded towards minus infinity, for a positive denominator - which
            is what Python's `//` does, and a driver restates `pairOf` in
            Python. C++'s `/` rounds towards nought instead. */
        int floorDivide (int numerator, int denominator) noexcept
        {
            return numerator >= 0 ? numerator / denominator
                                  : -((-numerator + denominator - 1) / denominator);
        }

        std::uint32_t fnv1a (const std::uint8_t* bytes, std::size_t size) noexcept
        {
            std::uint32_t hash = 0x811C9DC5u;

            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 0x01000193u;
            }

            return hash;
        }

        void putLittleEndian (std::uint8_t* at, std::uint64_t value, int bytes) noexcept
        {
            for (int i = 0; i < bytes; ++i)
                at[i] = static_cast<std::uint8_t> ((value >> (8 * i)) & 0xFFu);
        }

        std::uint64_t getLittleEndian (const std::uint8_t* at, int bytes) noexcept
        {
            std::uint64_t value = 0;

            for (int i = 0; i < bytes; ++i)
                value |= static_cast<std::uint64_t> (at[i]) << (8 * i);

            return value;
        }

        std::size_t levelCountFor (std::size_t finestFrames) noexcept
        {
            std::size_t levels = 1;

            for (auto frames = finestFrames; frames > coarsestFrames; frames = (frames + 1) / 2)
                ++levels;

            return levels;
        }

        std::uint64_t framesFor (std::uint64_t samples) noexcept
        {
            const auto hop = static_cast<std::uint64_t> (hopSize);
            return (samples + hop - 1) / hop;
        }
    }

    //==============================================================================
    bool operator== (const Frame& left, const Frame& right) noexcept
    {
        return left.hue == right.hue && left.saturation == right.saturation
            && left.lightness == right.lightness && left.peak == right.peak;
    }

    bool operator!= (const Frame& left, const Frame& right) noexcept
    {
        return ! (left == right);
    }

    double rampHue (double hertz)
    {
        /*  Written so that a NaN lands on the first stop rather than falling
            through every comparison below. */
        if (! (hertz > ramp.front().hertz))
            return std::fmod (ramp.front().hue, 360.0);

        for (std::size_t i = 1; i < ramp.size(); ++i)
        {
            if (hertz < ramp[i].hertz)
            {
                const auto& low = ramp[i - 1];
                const auto& high = ramp[i];
                const auto along = std::log (hertz / low.hertz) / std::log (high.hertz / low.hertz);

                return std::fmod (low.hue + along * (high.hue - low.hue), 360.0);
            }
        }

        return std::fmod (ramp.back().hue, 360.0);
    }

    double rampLightness (double hertz)
    {
        if (! (hertz > lowestHertz))
            return darkest;

        if (hertz >= highestHertz)
            return brightest;

        const auto along = std::log (hertz / lowestHertz) / std::log (highestHertz / lowestHertz);
        return darkest + (brightest - darkest) * along;
    }

    double hueOf (const Frame& frame) noexcept         { return frame.hue * 360.0 / 256.0; }
    double saturationOf (const Frame& frame) noexcept  { return frame.saturation / 255.0; }
    double lightnessOf (const Frame& frame) noexcept   { return frame.lightness / 255.0; }
    double peakOf (const Frame& frame) noexcept        { return frame.peak / 255.0; }

    /*  Lightness nought is the mark: a sounding frame's is never below the
        ramp's darkest, 0.15, which is 38 in a byte. */
    bool isSilent (const Frame& frame) noexcept        { return frame.lightness == 0; }

    //==============================================================================
    Frame pairOf (const Frame& first, const Frame& second) noexcept
    {
        Frame paired;
        paired.peak = std::max (first.peak, second.peak);

        const auto firstSilent = isSilent (first);
        const auto secondSilent = isSilent (second);

        if (firstSilent && secondSilent)
            return paired;

        if (firstSilent || secondSilent)
        {
            const auto& sounding = firstSilent ? second : first;

            paired.hue = sounding.hue;
            paired.saturation = sounding.saturation;
            paired.lightness = sounding.lightness;
            return paired;
        }

        paired.saturation = static_cast<std::uint8_t> ((first.saturation + second.saturation + 1) / 2);
        paired.lightness = static_cast<std::uint8_t> ((first.lightness + second.lightness + 1) / 2);

        /*  The signed arc from the first hue to the second the shorter way,
            in [-128, 127] steps; a half-turn exactly is taken downwards. */
        const auto arc = ((static_cast<int> (second.hue) - static_cast<int> (first.hue) + 384) % 256) - 128;

        auto firstWeight = static_cast<int> (first.saturation);
        auto secondWeight = static_cast<int> (second.saturation);

        if (firstWeight + secondWeight == 0)
        {
            firstWeight = 1;
            secondWeight = 1;
        }

        const auto total = firstWeight + secondWeight;

        /*  arc * secondWeight / total, a half rounded up. */
        const auto step = floorDivide (2 * arc * secondWeight + total, 2 * total);

        paired.hue = static_cast<std::uint8_t> ((static_cast<int> (first.hue) + step + 256) % 256);
        return paired;
    }

    std::vector<Frame> halve (const std::vector<Frame>& finer)
    {
        std::vector<Frame> coarser;
        coarser.reserve ((finer.size() + 1) / 2);

        for (std::size_t i = 0; i < finer.size(); i += 2)
            coarser.push_back (pairOf (finer[i], i + 1 < finer.size() ? finer[i + 1] : finer[i]));

        return coarser;
    }

    TimbrePyramid pyramidOf (std::vector<Frame> finest, std::uint32_t sampleRate,
                             std::uint64_t samples)
    {
        TimbrePyramid pyramid;
        pyramid.sampleRate = sampleRate;
        pyramid.samples = samples;
        pyramid.levels.push_back (std::move (finest));

        /*  `halve` has returned before `push_back` runs, so the reference it
            was handed cannot be one the growth has just invalidated. */
        while (pyramid.levels.back().size() > coarsestFrames)
            pyramid.levels.push_back (halve (pyramid.levels.back()));

        return pyramid;
    }

    const Frame* frameAt (const TimbrePyramid& pyramid, double seconds) noexcept
    {
        if (pyramid.frames() == 0 || pyramid.sampleRate == 0 || ! std::isfinite (seconds))
            return nullptr;

        const auto& finest = pyramid.levels.front();

        /*  KEPT A DOUBLE UNTIL IT IS KNOWN TO BE AN INDEX. Converting first
            would be undefined for a position of 1e300 seconds, and a negative
            one is no size_t at all; compared as a double, both simply land on
            an end. */
        const auto index = std::floor (seconds * static_cast<double> (pyramid.sampleRate)
                                       / static_cast<double> (hopSize));

        if (! (index > 0.0))
            return &finest.front();

        if (index >= static_cast<double> (finest.size() - 1))
            return &finest.back();

        return &finest[static_cast<std::size_t> (index)];
    }

    //==============================================================================
    struct Analyser::Impl
    {
        explicit Impl (double rate)
            : sampleRate (rate > 0.0 && std::isfinite (rate) ? rate : 0.0),
              fft (fftOrder),
              window (static_cast<std::size_t> (windowSize)),
              scratch (static_cast<std::size_t> (2 * windowSize)),
              before (static_cast<std::size_t> (hopSize)),
              current (static_cast<std::size_t> (hopSize)),
              after (static_cast<std::size_t> (hopSize))
        {
            /*  PERIODIC Hann, the one whose transform of a bin-centred sine is
                exactly three bins - which is what the silence floor below is
                worked out from. */
            for (std::size_t n = 0; n < window.size(); ++n)
                window[n] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi
                                                                      * static_cast<double> (n)
                                                                      / windowSize));

            /*  THE BAND, BY EACH BIN'S CENTRE FREQUENCY, b * rate / N - never
                by its index, which is the first of §14.12's ways to shift the
                picture without breaking it. */
            if (sampleRate > 0.0)
            {
                firstBin = std::max (1, static_cast<int> (std::ceil (lowestHertz * windowSize / sampleRate)));
                lastBin = std::min (windowSize / 2,
                                    static_cast<int> (std::floor (highestHertz * windowSize / sampleRate)));

                for (auto bin = firstBin; bin <= lastBin; ++bin)
                    logHertz.push_back (std::log (bin * sampleRate / windowSize));
            }
        }

        /*  The frame for the stretch in `current`, whose neighbours are
            `before` and `after`. */
        Frame computeFrame()
        {
            constexpr auto half = static_cast<std::size_t> (hopSize / 2);
            constexpr auto hop = static_cast<std::size_t> (hopSize);

            for (std::size_t i = 0; i < half; ++i)
                scratch[i] = before[half + i] * window[i];

            for (std::size_t i = 0; i < hop; ++i)
                scratch[half + i] = current[i] * window[half + i];

            for (std::size_t i = 0; i < half; ++i)
                scratch[half + hop + i] = after[i] * window[half + hop + i];

            std::fill (scratch.begin() + windowSize, scratch.end(), 0.0f);

            fft.performFrequencyOnlyForwardTransform (scratch.data(), true);

            double power = 0.0;
            double weightedLogHertz = 0.0;
            double magnitudeSum = 0.0;
            double logMagnitudeSum = 0.0;

            for (auto bin = firstBin; bin <= lastBin; ++bin)
            {
                const auto magnitude = static_cast<double> (scratch[static_cast<std::size_t> (bin)]);
                const auto binPower = magnitude * magnitude;

                power += binPower;
                weightedLogHertz += binPower * logHertz[static_cast<std::size_t> (bin - firstBin)];
                magnitudeSum += magnitude;
                logMagnitudeSum += std::log (std::max (magnitude, smallestMagnitude));
            }

            Frame frame;
            frame.peak = unitToByte (static_cast<double> (currentPeak));

            if (lastBin < firstBin || ! (power > silentPower))
                return frame;

            const auto binCount = static_cast<double> (lastBin - firstBin + 1);
            const auto centroid = std::exp (weightedLogHertz / power);
            const auto flatness = std::exp (logMagnitudeSum / binCount) / (magnitudeSum / binCount);

            frame.hue = hueToByte (rampHue (centroid));
            frame.saturation = unitToByte (1.0 - flatness);
            frame.lightness = unitToByte (rampLightness (centroid));
            return frame;
        }

        /*  Moves the three stretches along by one, leaving `after` to be
            written: its storage is the old `before`'s. */
        void rotate()
        {
            std::swap (before, current);
            std::swap (current, after);
            std::fill (after.begin(), after.end(), 0.0f);
        }

        const double sampleRate;

        /*  THE ONE JUCE TYPE HERE, and the reason this is a pimpl: a header
            the tree reads through `MediaInfo.h` names no audio library. */
        juce::dsp::FFT fft;

        std::vector<float> window;
        std::vector<float> scratch;

        /*  The stretch being coloured, and one either side of it. */
        std::vector<float> before;
        std::vector<float> current;
        std::vector<float> after;
        float currentPeak = 0.0f;
        float afterPeak = 0.0f;

        int firstBin = 1;
        int lastBin = 0;
        std::vector<double> logHertz;

        /*  The in-band power a -100 dBFS sine puts in the spectrum. A
            bin-centred sine of amplitude A through a periodic Hann window of N
            is three bins, A N / 4 and A N / 8 either side of it, so its power
            is 3 A^2 N^2 / 32 - and an off-centre one spreads the same power a
            little wider. Anything quieter has no centroid worth naming. */
        const double silentPower = 3.0 * windowSize * windowSize * silentAmplitude * silentAmplitude / 32.0;

        std::uint64_t samples = 0;
        std::size_t stretches = 0;
        std::vector<Frame> frames;
        bool finished = false;
    };

    Analyser::Analyser (double sampleRate)
        : impl (std::make_unique<Impl> (sampleRate))
    {
    }

    Analyser::~Analyser() = default;

    void Analyser::add (const float* const* channels, int numChannels, int count)
    {
        auto& state = *impl;

        if (state.finished)
            return;

        count = std::clamp (count, 0, hopSize);

        state.rotate();

        auto peak = 0.0f;

        if (channels != nullptr && numChannels > 0)
        {
            const auto share = 1.0f / static_cast<float> (numChannels);

            for (auto channel = 0; channel < numChannels; ++channel)
            {
                const auto* samples = channels[channel];

                if (samples == nullptr)
                    continue;

                for (auto i = 0; i < count; ++i)
                {
                    state.after[static_cast<std::size_t> (i)] += samples[i] * share;
                    peak = std::max (peak, std::abs (samples[i]));
                }
            }
        }

        state.currentPeak = state.afterPeak;
        state.afterPeak = peak;
        state.samples += static_cast<std::uint64_t> (count);
        ++state.stretches;

        /*  A stretch is coloured once the one after it has arrived: its window
            reaches half a hop into that neighbour. */
        if (state.stretches >= 2)
            state.frames.push_back (state.computeFrame());
    }

    std::size_t Analyser::framesAnalysed() const noexcept
    {
        return impl->frames.size();
    }

    TimbrePyramid Analyser::finish()
    {
        auto& state = *impl;

        /*  The last stretch's neighbour is a stretch of silence past the end. */
        if (! state.finished && state.stretches >= 1)
        {
            state.rotate();
            state.currentPeak = state.afterPeak;
            state.afterPeak = 0.0f;
            state.frames.push_back (state.computeFrame());
        }

        state.finished = true;

        return pyramidOf (std::move (state.frames),
                          static_cast<std::uint32_t> (std::lround (state.sampleRate)),
                          state.samples);
    }

    TimbrePyramid analyse (const float* const* channels, int numChannels,
                           std::int64_t numSamples, double sampleRate)
    {
        Analyser analyser { sampleRate };

        const auto channelCount = static_cast<std::size_t> (std::max (numChannels, 0));
        std::vector<const float*> stretch (channelCount, nullptr);

        for (std::int64_t start = 0; start < numSamples; start += hopSize)
        {
            const auto count = static_cast<int> (std::min<std::int64_t> (hopSize, numSamples - start));

            for (std::size_t channel = 0; channel < channelCount; ++channel)
                stretch[channel] = channels[channel] == nullptr ? nullptr : channels[channel] + start;

            analyser.add (stretch.data(), numChannels, count);
        }

        return analyser.finish();
    }

    //==============================================================================
    std::vector<std::uint8_t> write (const TimbrePyramid& pyramid)
    {
        const auto levelCount = pyramid.levels.size();

        std::size_t size = headerSize + levelEntrySize * levelCount;

        for (const auto& level : pyramid.levels)
            size += frameSize * level.size();

        std::vector<std::uint8_t> bytes (size, 0);
        auto* const out = bytes.data();

        std::copy (magic.begin(), magic.end(), out);
        putLittleEndian (out + 4, formatVersion, 2);
        putLittleEndian (out + 6, levelCount, 2);
        putLittleEndian (out + 8, pyramid.sampleRate, 4);
        putLittleEndian (out + 12, static_cast<std::uint64_t> (windowSize), 4);
        putLittleEndian (out + 16, static_cast<std::uint64_t> (hopSize), 4);
        putLittleEndian (out + 24, pyramid.samples, 8);

        auto offset = headerSize + levelEntrySize * levelCount;

        for (std::size_t level = 0; level < levelCount; ++level)
        {
            const auto& frames = pyramid.levels[level];

            putLittleEndian (out + headerSize + levelEntrySize * level, frames.size(), 4);
            putLittleEndian (out + headerSize + levelEntrySize * level + 4, offset, 4);

            for (const auto& frame : frames)
            {
                out[offset] = frame.hue;
                out[offset + 1] = frame.saturation;
                out[offset + 2] = frame.lightness;
                out[offset + 3] = frame.peak;
                offset += frameSize;
            }
        }

        putLittleEndian (out + 20, fnv1a (out + headerSize, size - headerSize), 4);
        return bytes;
    }

    bool read (const std::uint8_t* bytes, std::size_t size, TimbrePyramid& into)
    {
        if (bytes == nullptr || size < headerSize)
            return false;

        if (! std::equal (magic.begin(), magic.end(), bytes))
            return false;

        if (getLittleEndian (bytes + 4, 2) != formatVersion
            || getLittleEndian (bytes + 12, 4) != static_cast<std::uint64_t> (windowSize)
            || getLittleEndian (bytes + 16, 4) != static_cast<std::uint64_t> (hopSize))
            return false;

        const auto levelCount = static_cast<std::size_t> (getLittleEndian (bytes + 6, 2));
        const auto sampleRate = static_cast<std::uint32_t> (getLittleEndian (bytes + 8, 4));
        const auto samples = getLittleEndian (bytes + 24, 8);

        if (levelCount == 0 || sampleRate == 0)
            return false;

        const auto tableEnd = headerSize + levelEntrySize * levelCount;

        if (tableEnd > size)
            return false;

        /*  EVERY LEVEL THE SIZE THE ONE BELOW IT IMPLIES, and the finest the
            size the samples imply, so a file whose table was written by some
            other rule is refused rather than drawn. */
        std::uint64_t expectedFrames = framesFor (samples);

        if (levelCount != levelCountFor (static_cast<std::size_t> (expectedFrames)))
            return false;

        std::size_t expectedOffset = tableEnd;
        TimbrePyramid decoded;
        decoded.sampleRate = sampleRate;
        decoded.samples = samples;

        for (std::size_t level = 0; level < levelCount; ++level)
        {
            const auto* const entry = bytes + headerSize + levelEntrySize * level;
            const auto frameCount = getLittleEndian (entry, 4);
            const auto offset = getLittleEndian (entry + 4, 4);

            if (frameCount != expectedFrames || offset != expectedOffset)
                return false;

            if (frameCount > (size - expectedOffset) / frameSize)
                return false;

            std::vector<Frame> frames (static_cast<std::size_t> (frameCount));

            for (auto& frame : frames)
            {
                frame.hue = bytes[expectedOffset];
                frame.saturation = bytes[expectedOffset + 1];
                frame.lightness = bytes[expectedOffset + 2];
                frame.peak = bytes[expectedOffset + 3];
                expectedOffset += frameSize;
            }

            decoded.levels.push_back (std::move (frames));
            expectedFrames = (expectedFrames + 1) / 2;
        }

        if (expectedOffset != size)
            return false;

        if (getLittleEndian (bytes + 20, 4) != fnv1a (bytes + headerSize, size - headerSize))
            return false;

        into = std::move (decoded);
        return true;
    }
}
