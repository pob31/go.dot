/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  Spectral colour: the analysis, the pyramid, the cache and the thread
    (PR 5.7, PRD §3.30, namespace draft §14.12).

    THE FIRST THREE CASES ARE §3.30'S CHECK, and they come first on purpose.
    A colour ramp is exactly the kind of thing that looks right and is wrong:
    a log axis computed from bin index rather than bin frequency, a flatness
    on power where it should be on magnitude, a window that leaks and drags
    every centroid upward - each shifts the picture instead of breaking it,
    and over unfamiliar material a shifted ramp looks like a correct one. So
    every signal here is generated a line before it is analysed, and its
    answer is known before the code runs: a sine's centroid is its frequency
    because that is what a sine is, white noise is flat by definition, and a
    sweep's frequency at any instant is a formula. The sine is analysed at
    three rates, because a bin's frequency is `b * rate / N` and a bug that
    used its index would be right at one rate at most.

    `tests/blackbox/timbre_cache.py` asserts the same facts through a decoder
    of its own; this suite proves the engine agrees with itself, and that one
    is the one that counts.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/audio/MediaAnalyser.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/document/ShowDocument.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    namespace timbre = audio::timbre;

    constexpr double pi = 3.14159265358979323846;

    /*  Degrees between two hues, the short way round. */
    double hueApart (double one, double other)
    {
        const auto apart = std::fmod (std::fabs (one - other), 360.0);
        return std::min (apart, 360.0 - apart);
    }

    std::vector<float> sineAt (double hertz, double seconds, double rate, float amplitude)
    {
        std::vector<float> samples (static_cast<std::size_t> (seconds * rate));

        for (std::size_t n = 0; n < samples.size(); ++n)
            samples[n] = amplitude * static_cast<float> (std::sin (2.0 * pi * hertz * static_cast<double> (n) / rate));

        return samples;
    }

    /*  Uniform, from a seeded generator, so every run analyses the same noise. */
    std::vector<float> whiteNoise (double seconds, double rate)
    {
        juce::Random draw { 20260914 };
        std::vector<float> samples (static_cast<std::size_t> (seconds * rate));

        for (auto& sample : samples)
            sample = draw.nextFloat() - 0.5f;

        return samples;
    }

    constexpr double sweepFrom = 100.0;
    constexpr double sweepTo = 8000.0;
    constexpr double sweepSeconds = 8.0;

    /*  Exponential: equal times climb equal fractions of an octave, which is
        the axis the centroid is taken on. */
    double sweepHertzAt (double seconds)
    {
        return sweepFrom * std::pow (sweepTo / sweepFrom, seconds / sweepSeconds);
    }

    std::vector<float> sweep (double rate)
    {
        const auto growth = std::log (sweepTo / sweepFrom);
        const auto scale = 2.0 * pi * sweepFrom * sweepSeconds / growth;

        std::vector<float> samples (static_cast<std::size_t> (sweepSeconds * rate));

        for (std::size_t n = 0; n < samples.size(); ++n)
        {
            const auto seconds = static_cast<double> (n) / rate;
            samples[n] = 0.5f * static_cast<float> (std::sin (scale * (std::exp (growth * seconds / sweepSeconds) - 1.0)));
        }

        return samples;
    }

    audio::TimbrePyramid analyseMono (const std::vector<float>& samples, double rate)
    {
        const float* channels[] = { samples.data() };
        return timbre::analyse (channels, 1, static_cast<std::int64_t> (samples.size()), rate);
    }

    /*  The centre of the stretch a finest-level frame describes. */
    double frameSeconds (std::size_t index, double rate)
    {
        return (static_cast<double> (index) * timbre::hopSize + timbre::hopSize / 2.0) / rate;
    }

    /*  The frames whose windows lie wholly inside the signal: the first two
        and the last two reach past an edge. */
    template <typename Visit>
    void forEachSteadyFrame (const std::vector<timbre::Frame>& frames, Visit&& visit)
    {
        for (std::size_t index = 2; index + 2 < frames.size(); ++index)
            visit (index, frames[index]);
    }

    /*  THE RULE FOR A COARSER FRAME, restated from its wording in Timbre.h
        rather than called - a test that asked `pairOf` what `pairOf` does
        would pass whatever it did. The floor here is `std::floor` on a double,
        a different road to the same integer. */
    timbre::Frame expectedPair (const timbre::Frame& first, const timbre::Frame& second)
    {
        timbre::Frame paired;
        paired.peak = std::max (first.peak, second.peak);

        if (first.lightness == 0 && second.lightness == 0)
            return paired;

        if (first.lightness == 0 || second.lightness == 0)
        {
            const auto& sounding = first.lightness == 0 ? second : first;
            paired.hue = sounding.hue;
            paired.saturation = sounding.saturation;
            paired.lightness = sounding.lightness;
            return paired;
        }

        paired.saturation = static_cast<std::uint8_t> ((first.saturation + second.saturation + 1) / 2);
        paired.lightness = static_cast<std::uint8_t> ((first.lightness + second.lightness + 1) / 2);

        auto arc = static_cast<int> (second.hue) - static_cast<int> (first.hue);

        while (arc >= 128)
            arc -= 256;

        while (arc < -128)
            arc += 256;

        auto firstWeight = static_cast<double> (first.saturation);
        auto secondWeight = static_cast<double> (second.saturation);

        if (first.saturation == 0 && second.saturation == 0)
            firstWeight = secondWeight = 1.0;

        const auto step = static_cast<int> (std::floor ((2.0 * arc * secondWeight + firstWeight + secondWeight)
                                                        / (2.0 * (firstWeight + secondWeight))));

        paired.hue = static_cast<std::uint8_t> (((static_cast<int> (first.hue) + step) % 256 + 256) % 256);
        return paired;
    }

    //==============================================================================
    /*  A folder of its own per case, removed afterwards. */
    struct ScratchFolder
    {
        ScratchFolder()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-timbre-test-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScratchFolder() { folder.deleteRecursively(); }

        ScratchFolder (const ScratchFolder&) = delete;
        ScratchFolder& operator= (const ScratchFolder&) = delete;

        juce::File folder;
    };

    bool writeWav (const juce::File& file, const std::vector<float>& samples, int rate)
    {
        if (file.getParentDirectory().createDirectory().failed())
            return false;

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return false;

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (24));

        if (writer == nullptr)
            return false;

        const float* channels[] = { samples.data() };
        return writer->writeFromFloatArrays (channels, 1, static_cast<int> (samples.size()));
    }

    /*  What a WAV holds, read back the way any reader would - so the file's
        analysis can be compared with the same samples analysed in memory. */
    std::vector<float> readMono (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };

        if (reader == nullptr)
            return {};

        std::vector<float> samples (static_cast<std::size_t> (reader->lengthInSamples));
        float* channels[] = { samples.data() };
        reader->read (channels, 1, 0, static_cast<int> (samples.size()));
        return samples;
    }

    juce::File cacheFileFor (const juce::File& media, const std::string& hash)
    {
        return media.getChildFile (".timbre").getChildFile (juce::String (hash) + ".tpy");
    }

    bool isHexDigest (const std::string& text)
    {
        return text.size() == 64
            && std::all_of (text.begin(), text.end(),
                            [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    bool samePyramid (const audio::TimbrePyramid& one, const audio::TimbrePyramid& other)
    {
        return one.sampleRate == other.sampleRate && one.samples == other.samples
            && one.levels == other.levels;
    }
}

//==============================================================================
TEST_CASE ("timbre: the ramp's stops are the author's, and its lightness climbs the whole band")
{
    /*  Blue held to 250 Hz and red not before 800 (author, 2026-09-25: "I
        would bias a bit towards the blues"); plan decision 8 had them at 150
        and 500. */
    CHECK (timbre::rampHue (40.0) == doctest::Approx (280.0));
    CHECK (timbre::rampHue (250.0) == doctest::Approx (240.0));
    CHECK (timbre::rampHue (800.0) == doctest::Approx (0.0));
    CHECK (timbre::rampHue (2500.0) == doctest::Approx (30.0));
    CHECK (timbre::rampHue (6000.0) == doctest::Approx (60.0));
    CHECK (timbre::rampHue (12000.0) == doctest::Approx (120.0));

    //  And the body of most material is violet and blue now, not red: 500 Hz is past magenta's 300.
    CHECK (timbre::rampHue (500.0) > 300.0);
    CHECK (timbre::rampHue (500.0) < 330.0);

    /*  Held at the ends, including a centroid nobody could have. */
    CHECK (timbre::rampHue (20.0) == doctest::Approx (280.0));
    CHECK (timbre::rampHue (20000.0) == doctest::Approx (120.0));
    CHECK (timbre::rampHue (std::nan ("")) == doctest::Approx (280.0));

    /*  Between red and orange, the log of the frequency and not the frequency:
        1 kHz is a fifth of the way from 800 Hz to 2.5 kHz in octaves. */
    CHECK (timbre::rampHue (1000.0) == doctest::Approx (30.0 * std::log (1.25) / std::log (3.125)));

    /*  THE HUE TURNS BACK, and this pins it so a palette change is seen: from
        purple at 40 Hz it falls to deep blue at 250 Hz, then climbs the other
        way round the wheel. What the sweep check below asserts is therefore
        the lightness, which never turns back. */
    CHECK (timbre::rampHue (100.0) > timbre::rampHue (250.0));
    CHECK (timbre::rampHue (300.0) > timbre::rampHue (250.0));

    CHECK (timbre::rampLightness (40.0) == doctest::Approx (0.15));
    CHECK (timbre::rampLightness (16000.0) == doctest::Approx (0.85));
    CHECK (timbre::rampLightness (10.0) == doctest::Approx (0.15));
    CHECK (timbre::rampLightness (30000.0) == doctest::Approx (0.85));

    auto previous = 0.0;

    for (auto hertz = 40.0; hertz <= 16000.0; hertz *= 1.05)
    {
        const auto lightness = timbre::rampLightness (hertz);
        CHECK (lightness >= previous);
        previous = lightness;
    }
}

TEST_CASE ("timbre: a 1 kHz sine is saturated, at the ramp's 1 kHz hue, whatever the rate")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        INFO ("at " << rate << " Hz");

        const auto pyramid = analyseMono (sineAt (1000.0, 3.0, rate, 0.5f), rate);
        const auto& frames = pyramid.levels.front();

        REQUIRE (frames.size() == static_cast<std::size_t> (std::ceil (3.0 * rate / timbre::hopSize)));
        CHECK (pyramid.sampleRate == static_cast<std::uint32_t> (rate));
        CHECK (pyramid.samples == static_cast<std::uint64_t> (3.0 * rate));

        int grey = 0;
        int offHue = 0;
        int offLightness = 0;
        int offPeak = 0;

        forEachSteadyFrame (frames, [&] (std::size_t, const timbre::Frame& frame)
        {
            grey += timbre::saturationOf (frame) <= 0.8 ? 1 : 0;
            offHue += hueApart (timbre::hueOf (frame), timbre::rampHue (1000.0)) > 3.0 ? 1 : 0;
            offLightness += std::fabs (timbre::lightnessOf (frame) - timbre::rampLightness (1000.0)) > 0.01 ? 1 : 0;
            offPeak += std::fabs (timbre::peakOf (frame) - 0.5) > 0.01 ? 1 : 0;
        });

        CHECK (grey == 0);
        CHECK (offHue == 0);
        CHECK (offLightness == 0);
        CHECK (offPeak == 0);
    }
}

TEST_CASE ("timbre: white noise is grey, and still a reading")
{
    constexpr double rate = 48000.0;
    const auto pyramid = analyseMono (whiteNoise (3.0, rate), rate);

    double saturation = 0.0;
    double lightness = 0.0;
    int frames = 0;
    int silent = 0;

    forEachSteadyFrame (pyramid.levels.front(), [&] (std::size_t, const timbre::Frame& frame)
    {
        saturation += timbre::saturationOf (frame);
        lightness += timbre::lightnessOf (frame);
        silent += timbre::isSilent (frame) ? 1 : 0;
        ++frames;
    });

    REQUIRE (frames > 100);

    /*  Noise set against its own neighbourhood scatters around it - a
        geometric mean over an arithmetic near 0.6, past `noisyAt` - so it
        reads grey, and not only when it is white (the next case). */
    CHECK (saturation / frames < 0.2);

    /*  A reading, not the silence a missing pyramid would be - and a bright
        one, since white noise's power is where the bins are, at the top. */
    CHECK (silent == 0);
    CHECK (lightness / frames > 0.6);
}

TEST_CASE ("timbre: noise is grey wherever it sits in the spectrum, and a harmonic tone is vivid")
{
    /*  The author, 2026-09-25: the colours "don't desaturate on a broader,
        noisier signal". White noise always read grey; noise with a slope or in
        part of the band - pink, a hi-hat's top octaves - read as vivid as a
        sine, because the flatness was over the whole band. Now each bin is
        set against its own neighbourhood, where the energy is. */
    constexpr double rate = 48000.0;

    const auto meanSaturation = [] (const std::vector<float>& samples)
    {
        const auto pyramid = analyseMono (samples, rate);
        double sum = 0.0;
        int frames = 0;

        forEachSteadyFrame (pyramid.levels.front(), [&] (std::size_t, const timbre::Frame& frame)
        {
            sum += timbre::saturationOf (frame);
            ++frames;
        });

        REQUIRE (frames > 50);
        return sum / frames;
    };

    //  Pink: white noise through Paul Kellet's filter, a slope of 3 dB an octave.
    auto pink = whiteNoise (2.0, rate);
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;

        for (auto& sample : pink)
        {
            const auto white = static_cast<double> (sample);
            b0 = 0.99886 * b0 + white * 0.0555179;
            b1 = 0.99332 * b1 + white * 0.0750759;
            b2 = 0.96900 * b2 + white * 0.1538520;
            b3 = 0.86650 * b3 + white * 0.3104856;
            b4 = 0.55000 * b4 + white * 0.5329522;
            b5 = -0.7616 * b5 - white * 0.0168980;
            sample = static_cast<float> ((b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11);
            b6 = white * 0.115926;
        }
    }

    //  A hi-hat's band: white noise high-passed at 6 kHz, four poles.
    auto hat = whiteNoise (2.0, rate);
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter filter;
            filter.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 6000.0));
            filter.processSamples (hat.data(), static_cast<int> (hat.size()));
        }
    }

    //  A sawtooth at 220 Hz: every harmonic, each a peak over its neighbourhood.
    std::vector<float> saw (static_cast<std::size_t> (2.0 * rate));
    {
        for (std::size_t n = 0; n < saw.size(); ++n)
        {
            double value = 0.0;

            for (int harmonic = 1; 220.0 * harmonic < 16000.0; ++harmonic)
                value += std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * harmonic
                                   * static_cast<double> (n) / rate) / harmonic;

            saw[n] = static_cast<float> (0.3 * value);
        }
    }

    CHECK (meanSaturation (pink) < 0.2);
    CHECK (meanSaturation (hat) < 0.25);
    CHECK (meanSaturation (saw) > 0.8);
}

TEST_CASE ("timbre: a sweep walks the ramp, and its lightness never falls")
{
    constexpr double rate = 48000.0;
    const auto pyramid = analyseMono (sweep (rate), rate);
    const auto& frames = pyramid.levels.front();

    int offHue = 0;
    int offLightness = 0;
    int falls = 0;
    std::uint8_t previous = 0;
    std::uint8_t first = 0;
    std::uint8_t last = 0;

    forEachSteadyFrame (frames, [&] (std::size_t index, const timbre::Frame& frame)
    {
        const auto hertz = sweepHertzAt (frameSeconds (index, rate));

        offHue += hueApart (timbre::hueOf (frame), timbre::rampHue (hertz)) > 6.0 ? 1 : 0;
        offLightness += std::fabs (timbre::lightnessOf (frame) - timbre::rampLightness (hertz)) > 0.02 ? 1 : 0;

        if (index == 2)
            first = frame.lightness;
        else if (frame.lightness + 1 < previous)
            ++falls;

        previous = frame.lightness;
        last = frame.lightness;
    });

    CHECK (offHue == 0);
    CHECK (offLightness == 0);
    CHECK (falls == 0);
    CHECK (last - first > static_cast<int> (0.4 * 255));
}

TEST_CASE ("timbre: silence has no colour, and lends none to a coarser level")
{
    constexpr double rate = 48000.0;

    const std::vector<float> nothing (static_cast<std::size_t> (rate), 0.0f);
    const auto quiet = analyseMono (nothing, rate);

    CHECK (std::all_of (quiet.levels.front().begin(), quiet.levels.front().end(),
                        [] (const timbre::Frame& frame) { return frame == timbre::Frame {}; }));

    /*  Five seconds of silence then five of a 1 kHz sine: at the coarsest
        level, every frame that sounds at all has the sine's hue and
        lightness - the frame straddling the join included, give or take the
        few finest frames whose windows catch the edge of the tone. Averaged
        in, silence would have HALVED that frame's lightness, which is a lie
        in the one dimension that is the frequency axis. */
    auto half = std::vector<float> (static_cast<std::size_t> (5.0 * rate), 0.0f);
    const auto tone = sineAt (1000.0, 5.0, rate, 0.5f);
    half.insert (half.end(), tone.begin(), tone.end());

    const auto pyramid = analyseMono (half, rate);
    REQUIRE (pyramid.levels.size() >= 2);

    const auto& coarsest = pyramid.levels.back();
    int dragged = 0;

    for (const auto& frame : coarsest)
    {
        if (timbre::isSilent (frame))
            continue;

        dragged += hueApart (timbre::hueOf (frame), timbre::rampHue (1000.0)) > 10.0 ? 1 : 0;
        dragged += std::fabs (timbre::lightnessOf (frame) - timbre::rampLightness (1000.0)) > 0.03 ? 1 : 0;
    }

    CHECK (dragged == 0);
    CHECK (timbre::isSilent (coarsest.front()));
    CHECK (! timbre::isSilent (coarsest.back()));

    /*  And the rule on its own, where it is easiest to read. */
    const timbre::Frame silentFrame { 0, 0, 0, 3 };
    const timbre::Frame sounding { 200, 180, 120, 90 };

    CHECK (timbre::pairOf (silentFrame, sounding) == timbre::Frame { 200, 180, 120, 90 });
    CHECK (timbre::pairOf (sounding, silentFrame) == timbre::Frame { 200, 180, 120, 90 });
    CHECK (timbre::pairOf (silentFrame, silentFrame) == timbre::Frame { 0, 0, 0, 3 });
}

TEST_CASE ("timbre: a coarser frame's hue goes the short way round, and a grey frame lends it none")
{
    /*  Across the seam: 250 to 10 is sixteen steps up, not two hundred and
        forty down - so equal weights land at 2, past the seam. */
    CHECK (timbre::pairOf ({ 250, 100, 100, 0 }, { 10, 100, 100, 0 }).hue == 2);
    CHECK (timbre::pairOf ({ 10, 100, 100, 0 }, { 250, 100, 100, 0 }).hue == 2);

    /*  A grey frame beside a vivid one: the vivid one's hue, untouched. */
    CHECK (timbre::pairOf ({ 40, 0, 100, 0 }, { 170, 200, 100, 0 }).hue == 170);
    CHECK (timbre::pairOf ({ 170, 200, 100, 0 }, { 40, 0, 100, 0 }).hue == 170);

    /*  Two grey frames: equal shares, since neither has more hue than the
        other. */
    CHECK (timbre::pairOf ({ 20, 0, 100, 0 }, { 40, 0, 100, 0 }).hue == 30);

    /*  A share by saturation: three quarters of the way from 0 to 40. */
    CHECK (timbre::pairOf ({ 0, 50, 100, 0 }, { 40, 150, 100, 0 }).hue == 30);

    /*  Means with a half rounded up, and the larger peak. */
    const auto paired = timbre::pairOf ({ 0, 10, 101, 7 }, { 0, 11, 100, 9 });
    CHECK (paired.saturation == 11);
    CHECK (paired.lightness == 101);
    CHECK (paired.peak == 9);
}

TEST_CASE ("timbre: every level is the one below it, paired, down to a Gogo bar")
{
    /*  Material with some variety in it - a chord of sines under noise, then
        silence, then a sweep - so the pairs are not all alike. */
    constexpr double rate = 48000.0;

    auto samples = whiteNoise (20.0, rate);
    const auto chord = sineAt (440.0, 20.0, rate, 0.3f);

    for (std::size_t n = 0; n < samples.size(); ++n)
        samples[n] = samples[n] * 0.2f + chord[n];

    samples.insert (samples.end(), static_cast<std::size_t> (3.0 * rate), 0.0f);

    const auto rising = sweep (rate);
    samples.insert (samples.end(), rising.begin(), rising.end());

    const auto pyramid = analyseMono (samples, rate);

    REQUIRE (pyramid.levels.front().size()
             == static_cast<std::size_t> ((samples.size() + timbre::hopSize - 1) / timbre::hopSize));
    REQUIRE (pyramid.levels.size() >= 3);

    for (std::size_t level = 1; level < pyramid.levels.size(); ++level)
    {
        INFO ("level " << level);

        const auto& finer = pyramid.levels[level - 1];
        const auto& coarser = pyramid.levels[level];

        REQUIRE (coarser.size() == (finer.size() + 1) / 2);

        int wrong = 0;

        for (std::size_t i = 0; i < coarser.size(); ++i)
        {
            const auto& second = 2 * i + 1 < finer.size() ? finer[2 * i + 1] : finer[2 * i];
            wrong += coarser[i] != expectedPair (finer[2 * i], second) ? 1 : 0;
        }

        CHECK (wrong == 0);
    }

    /*  Halvings stop at the first level a forty-pixel bar can read, and not
        one level later or earlier. */
    CHECK (pyramid.levels.back().size() <= timbre::coarsestFrames);
    CHECK (pyramid.levels[pyramid.levels.size() - 2].size() > timbre::coarsestFrames);

    /*  A clip shorter than a bar is its finest level and nothing else; a file
        of no samples is one empty level. */
    CHECK (analyseMono (sineAt (1000.0, 1.0, rate, 0.5f), rate).levels.size() == 1);

    const auto empty = analyseMono ({}, rate);
    REQUIRE (empty.levels.size() == 1);
    CHECK (empty.levels.front().empty());
}

TEST_CASE ("timbre: a pyramid written and read back is the same, byte for byte, and a damaged one is refused")
{
    constexpr double rate = 44100.0;
    const auto pyramid = analyseMono (sweep (rate), rate);
    const auto bytes = timbre::write (pyramid);

    audio::TimbrePyramid back;
    REQUIRE (timbre::read (bytes.data(), bytes.size(), back));
    CHECK (samePyramid (back, pyramid));
    CHECK (timbre::write (back) == bytes);

    /*  The layout's fixed fields, where Timbre.h says they are. */
    REQUIRE (bytes.size() > 32);
    CHECK (std::string (bytes.begin(), bytes.begin() + 4) == "WFGT");
    CHECK (bytes[4] == timbre::formatVersion);
    CHECK (static_cast<std::size_t> (bytes[6]) == pyramid.levels.size());
    CHECK ((bytes[8] | (bytes[9] << 8) | (bytes[10] << 16)) == 44100);

    /*  Every damage a disk or a person could do, each refused, and the
        pyramid it was read into left as it was. */
    const auto refused = [&pyramid] (std::vector<std::uint8_t> damaged)
    {
        auto into = pyramid;
        const auto ok = timbre::read (damaged.data(), damaged.size(), into);
        return ! ok && samePyramid (into, pyramid);
    };

    auto shorter = bytes;
    shorter.pop_back();
    CHECK (refused (shorter));

    auto longer = bytes;
    longer.push_back (0);
    CHECK (refused (longer));

    auto magic = bytes;
    magic[0] = 'X';
    CHECK (refused (magic));

    auto version = bytes;
    version[4] = static_cast<std::uint8_t> (timbre::formatVersion + 1);
    CHECK (refused (version));

    auto hop = bytes;
    hop[17] = static_cast<std::uint8_t> (hop[17] ^ 0x01);
    CHECK (refused (hop));

    auto levels = bytes;
    levels[6] = static_cast<std::uint8_t> (levels[6] + 1);
    CHECK (refused (levels));

    auto flipped = bytes;
    flipped[bytes.size() / 2] = static_cast<std::uint8_t> (flipped[bytes.size() / 2] ^ 0x40);
    CHECK (refused (flipped));

    CHECK (refused ({}));
    CHECK (! timbre::read (nullptr, 0, back));

    /*  An empty pyramid round-trips too: a file with no samples. */
    const auto emptyBytes = timbre::write (analyseMono ({}, rate));
    audio::TimbrePyramid empty;
    REQUIRE (timbre::read (emptyBytes.data(), emptyBytes.size(), empty));
    CHECK (empty.frames() == 0);
}

TEST_CASE ("timbre: the frame at a moment is the finest one whose stretch holds it, held at both ends")
{
    /*  What `/godot/run/<id>/timbre` reads on every tick (PR 5.8). Answered
        as an INDEX into the finest level, found by walking that level for
        the pointer - so a wrong frame fails with the number it was rather
        than with two addresses, and a frame from a coarser level, which
        this must never return, fails as -2. */
    const auto indexAt = [] (const audio::TimbrePyramid& pyramid, double seconds) -> long long
    {
        const auto* frame = timbre::frameAt (pyramid, seconds);

        if (frame == nullptr)
            return -1;

        const auto& finest = pyramid.levels.front();

        for (std::size_t k = 0; k < finest.size(); ++k)
            if (&finest[k] == frame)
                return static_cast<long long> (k);

        return -2;
    };

    /*  Frames alike on purpose: which one came back is told by where it is,
        not by what it holds. */
    const auto steady = [] (std::size_t count, std::uint32_t rate)
    {
        return timbre::pyramidOf (std::vector<timbre::Frame> (count, timbre::Frame { 30, 200, 120, 90 }),
                                  rate,
                                  static_cast<std::uint64_t> (count) * static_cast<std::uint64_t> (timbre::hopSize));
    };

    /*  NO ANSWER TO GIVE: nothing analysed, a file of no samples, a pyramid
        that does not know its rate, and a position that is not a number. */
    CHECK (timbre::frameAt (audio::TimbrePyramid {}, 0.0) == nullptr);
    CHECK (timbre::frameAt (steady (0u, 48000u), 0.0) == nullptr);
    CHECK (timbre::frameAt (steady (10u, 0u), 0.0) == nullptr);

    const auto at48 = steady (400u, 48000u);
    const auto at44 = steady (11100u, 44100u);

    REQUIRE (at48.levels.size() > 1);       // coarser levels exist, and are never what is read

    CHECK (timbre::frameAt (at48, std::numeric_limits<double>::quiet_NaN()) == nullptr);
    CHECK (timbre::frameAt (at48, std::numeric_limits<double>::infinity()) == nullptr);
    CHECK (timbre::frameAt (at48, -std::numeric_limits<double>::infinity()) == nullptr);

    /*  THE START, AND BEFORE IT: a negative position is the first frame,
        however far before the start it is. */
    CHECK (indexAt (at48, 0.0) == 0);
    CHECK (indexAt (at48, -1.0e-9) == 0);
    CHECK (indexAt (at48, -3.0) == 0);
    CHECK (indexAt (at48, -1.0e300) == 0);

    /*  THE RULE, where the answer is not a whole number of hops - and at two
        rates, because a lookup that forgot the rate would be right at one of
        them at most. */
    CHECK (indexAt (at48, 0.5) == 23);      // 24 000 samples: 23.44 hops
    CHECK (indexAt (at48, 1.0) == 46);      // 48 000: 46.88
    CHECK (indexAt (at44, 0.5) == 21);      // 22 050: 21.53
    CHECK (indexAt (at44, 1.0) == 43);      // 44 100: 43.07

    /*  ON A BOUNDARY, where rounding the wrong way is a frame early or late.
        Eight seconds at 48 kHz is 384 000 samples, exactly 375 hops, and 256
        seconds at 44.1 kHz is 11 289 600, exactly 11 025: at each rate the
        first positions that are a whole number of hops AND exact in binary,
        so the product carries no rounding and the boundary is really one.
        A sample earlier is still the frame before it. */
    CHECK (indexAt (at48, 8.0) == 375);
    CHECK (indexAt (at48, 8.0 - 1.0 / 48000.0) == 374);
    CHECK (indexAt (at44, 256.0) == 11025);
    CHECK (indexAt (at44, 256.0 - 1.0 / 44100.0) == 11024);

    /*  THE END. Four hundred frames at 48 kHz run to 8.533 seconds: the last
        two stretches by the rule, then everything after them held on the
        last - a finished run keeps its last playhead. */
    CHECK (indexAt (at48, 8.5) == 398);     // 398.44 hops
    CHECK (indexAt (at48, 8.52) == 399);    // 399.38: the last frame's own stretch
    CHECK (indexAt (at48, 400.0 * static_cast<double> (timbre::hopSize) / 48000.0) == 399);
    CHECK (indexAt (at48, 9.0) == 399);
    CHECK (indexAt (at48, 1.0e300) == 399);
    CHECK (indexAt (at44, 300.0) == 11099);

    /*  And a clip one frame long is that frame, wherever it is asked. */
    const auto single = steady (1u, 48000u);
    CHECK (indexAt (single, -1.0) == 0);
    CHECK (indexAt (single, 0.0) == 0);
    CHECK (indexAt (single, 60.0) == 0);
}

//==============================================================================
TEST_CASE ("timbre cache: a file is analysed once, keyed by its bytes, and a second look does no work")
{
    ScratchFolder scratch;
    const auto media = scratch.folder.getChildFile ("media");
    const auto mediaFolder = media.getFullPathName().toStdString();

    const auto tone = sineAt (1000.0, 2.0, 48000.0, 0.5f);
    REQUIRE (writeWav (media.getChildFile ("tone.wav"), tone, 48000));

    const auto built = audio::analyseMediaFile (mediaFolder, "tone.wav", false);

    REQUIRE (built.outcome == audio::MediaAnalysis::Outcome::built);
    CHECK (isHexDigest (built.contentHash));
    CHECK (built.seconds == doctest::Approx (2.0));
    CHECK (built.framesAnalysed == built.frames);
    CHECK (built.frames == 94u);
    REQUIRE (built.pyramid != nullptr);

    const auto cache = cacheFileFor (media, built.contentHash);
    REQUIRE (cache.existsAsFile());
    CHECK (built.bytesOnDisk == cache.getSize());
    CHECK (juce::String (audio::describe (built.outcome)) == "built");

    /*  THE FILE'S ANALYSIS IS THE SAMPLES' ANALYSIS: the reader hands the
        analyser the same stretches `timbre::analyse` would, the last one
        short. */
    CHECK (samePyramid (*built.pyramid, analyseMono (readMono (media.getChildFile ("tone.wav")), 48000.0)));

    juce::MemoryBlock firstBytes;
    REQUIRE (cache.loadFileAsData (firstBytes));

    /*  A second look reads the cache and colours nothing. */
    const auto second = audio::analyseMediaFile (mediaFolder, "tone.wav", false);

    CHECK (second.outcome == audio::MediaAnalysis::Outcome::cached);
    CHECK (second.framesAnalysed == 0u);
    CHECK (second.contentHash == built.contentHash);
    REQUIRE (second.pyramid != nullptr);
    CHECK (samePyramid (*second.pyramid, *built.pyramid));

    /*  The same bytes under another name: the same key, and no work. */
    REQUIRE (media.getChildFile ("elsewhere").createDirectory().wasOk());
    REQUIRE (media.getChildFile ("tone.wav").copyFileTo (media.getChildFile ("elsewhere").getChildFile ("tone copy.wav")));
    const auto renamed = audio::analyseMediaFile (mediaFolder, "elsewhere/tone copy.wav", false);

    CHECK (renamed.outcome == audio::MediaAnalysis::Outcome::cached);
    CHECK (renamed.contentHash == built.contentHash);

    /*  --force builds it again, and writes what it wrote before. */
    const auto forced = audio::analyseMediaFile (mediaFolder, "tone.wav", true);
    CHECK (forced.outcome == audio::MediaAnalysis::Outcome::built);

    juce::MemoryBlock forcedBytes;
    REQUIRE (cache.loadFileAsData (forcedBytes));
    CHECK (forcedBytes == firstBytes);

    /*  A cache cut short is a miss, built again - never drawn - and whole
        again afterwards. */
    REQUIRE (cache.replaceWithData (firstBytes.getData(), firstBytes.getSize() - 4));
    const auto mended = audio::analyseMediaFile (mediaFolder, "tone.wav", false);
    CHECK (mended.outcome == audio::MediaAnalysis::Outcome::built);

    juce::MemoryBlock mendedBytes;
    REQUIRE (cache.loadFileAsData (mendedBytes));
    CHECK (mendedBytes == firstBytes);

    /*  And no temp is left beside it. */
    CHECK (media.getChildFile (".timbre").getNumberOfChildFiles (juce::File::findFiles) == 1);
}

TEST_CASE ("timbre cache: a folder that cannot be written costs the cache and not the colours")
{
    ScratchFolder scratch;
    const auto media = scratch.folder.getChildFile ("media");
    REQUIRE (writeWav (media.getChildFile ("tone.wav"), sineAt (1000.0, 1.0, 48000.0, 0.5f), 48000));

    /*  A FILE where the folder should be: refused on all three platforms and
        under any privilege, which a read-only folder is not on an elevated
        Windows process (BundleTests' rig, for the same reason). */
    REQUIRE (media.getChildFile (".timbre").replaceWithText ("in the way"));

    const auto analysis = audio::analyseMediaFile (media.getFullPathName().toStdString(), "tone.wav", false);

    CHECK (analysis.outcome == audio::MediaAnalysis::Outcome::inMemory);
    CHECK (analysis.pyramid != nullptr);
    CHECK (analysis.bytesOnDisk == 0);
    CHECK (isHexDigest (analysis.contentHash));
    CHECK (media.getChildFile (".timbre").existsAsFile());

    /*  No bundle, no media folder: in memory, and nowhere written. */
    const auto loose = audio::analyseMediaFile (std::string(),
                                                media.getChildFile ("tone.wav").getFullPathName().toStdString(),
                                                false);
    CHECK (loose.outcome == audio::MediaAnalysis::Outcome::inMemory);
    CHECK (loose.pyramid != nullptr);
}

TEST_CASE ("timbre cache: a file that is not there, one that is not audio, and a stop, keep nothing")
{
    ScratchFolder scratch;
    const auto media = scratch.folder.getChildFile ("media");
    const auto mediaFolder = media.getFullPathName().toStdString();

    REQUIRE (writeWav (media.getChildFile ("tone.wav"), sineAt (1000.0, 1.0, 48000.0, 0.5f), 48000));
    REQUIRE (media.getChildFile ("notes.wav").replaceWithText ("this is not a sound"));

    const auto missing = audio::analyseMediaFile (mediaFolder, "absent.wav", false);
    CHECK (missing.outcome == audio::MediaAnalysis::Outcome::missing);
    CHECK (missing.contentHash.empty());
    CHECK (missing.pyramid == nullptr);

    const auto unnamed = audio::analyseMediaFile (mediaFolder, "", false);
    CHECK (unnamed.outcome == audio::MediaAnalysis::Outcome::missing);

    /*  The bytes are hashed - they are there - and nothing is built from them. */
    const auto notAudio = audio::analyseMediaFile (mediaFolder, "notes.wav", false);
    CHECK (notAudio.outcome == audio::MediaAnalysis::Outcome::unreadable);
    CHECK (isHexDigest (notAudio.contentHash));
    CHECK (notAudio.pyramid == nullptr);

    /*  A stop already raised: nothing hashed that could be trusted, nothing
        built and nothing written. */
    const std::atomic<bool> stop { true };
    const auto stopped = audio::analyseMediaFile (mediaFolder, "tone.wav", false, &stop);

    CHECK (stopped.outcome == audio::MediaAnalysis::Outcome::stopped);
    CHECK (stopped.contentHash.empty());
    CHECK (stopped.pyramid == nullptr);
    CHECK (! media.getChildFile (".timbre").exists());
}

//==============================================================================
namespace
{
    /*  A show naming two sounds, a bundle folder holding them. */
    struct TwoSounds
    {
        TwoSounds()
        {
            const auto list = document.createList ("Sound");
            built = list.ok;

            built = built && writeWav (media.getChildFile ("tone.wav"), sineAt (1000.0, 2.0, 48000.0, 0.5f), 48000);
            built = built && writeWav (media.getChildFile ("hiss.wav"), whiteNoise (2.0, 48000.0), 48000);

            add (list.id, 0, "Tone", "tone.wav");
            add (list.id, 1, "Hiss", "hiss.wav");
        }

        void add (const std::string& parent, int index, const char* cueName, const char* file)
        {
            const auto made = document.createCue (parent, index, "media", cueName);
            built = built && made.ok;
            built = built && document.setAttribute ("/godot/cue/" + made.id + "/file", file).ok;
        }

        std::string mediaFolder() const { return media.getFullPathName().toStdString(); }

        ScratchFolder scratch;
        juce::File media { scratch.folder.getChildFile ("media") };
        doc::ShowDocument document;
        bool built = false;
    };

    /*  TRAP 3: a wait must see what its check reads. `outstanding` falls
        only after a record is published, so waiting on it and then reading
        the snapshot reads what was waited for. Bounded by real time, because
        the work is real time - a hash and an FFT on a loaded runner. */
    bool waitUntilIdle (const audio::MediaAnalyser& analyser)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

        while (analyser.outstanding() > 0)
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return true;
    }
}

TEST_CASE ("media analyser: every file the show names gets its record, off the calling thread, once")
{
    TwoSounds show;
    REQUIRE (show.built);

    audio::MediaInfo info { show.document, show.mediaFolder() };
    audio::MediaAnalyser analyser { info, show.mediaFolder() };

    /*  Before anything runs, the records have their seconds and nothing
        else: the tree publishes those as nothing at all. */
    for (const auto& entry : *info.snapshot())
    {
        CHECK (entry.second.contentHash.empty());
        CHECK (entry.second.pyramid == nullptr);
    }

    /*  Queued in the show's order, and once: a show edit re-offers every
        file, and all but the new ones are dropped. */
    const auto named = audio::mediaFilesNamedBy (show.document);
    REQUIRE (named == std::vector<std::string> { "tone.wav", "hiss.wav" });

    for (const auto& path : named)
        CHECK (analyser.queue (path));

    for (const auto& path : named)
        CHECK (! analyser.queue (path));

    CHECK (! analyser.queue (""));
    CHECK (analyser.outstanding() == 2u);

    REQUIRE (analyser.start());
    CHECK (! analyser.start());
    REQUIRE (waitUntilIdle (analyser));

    const auto records = info.snapshot();

    for (const auto& path : named)
    {
        INFO (path);
        REQUIRE (records->count (path) == 1u);

        const auto& record = records->at (path);
        CHECK (isHexDigest (record.contentHash));
        REQUIRE (record.pyramid != nullptr);
        CHECK (record.pyramid->frames() == 94u);

        /*  The frozen seconds, not the analyser's - the two halves agree
            about a file they both know (MediaInfo.h). */
        CHECK (record.seconds == doctest::Approx (info.durations()->at (path)));
        CHECK (cacheFileFor (show.media, record.contentHash).existsAsFile());
    }

    /*  The sine is saturated and the hiss is not: the right pyramid under the
        right name. */
    const auto meanSaturation = [] (const audio::TimbrePyramid& pyramid)
    {
        double sum = 0.0;

        for (const auto& frame : pyramid.levels.front())
            sum += timbre::saturationOf (frame);

        return sum / static_cast<double> (pyramid.frames());
    };

    CHECK (meanSaturation (*records->at ("tone.wav").pyramid) > 0.8);
    CHECK (meanSaturation (*records->at ("hiss.wav").pyramid) < 0.2);

    /*  A FILE IMPORTED AFTER THE OPEN is analysed and published too, with its
        own seconds - `durations()` never hears of it (MediaInfo.h). */
    REQUIRE (writeWav (show.media.getChildFile ("late.wav"), sineAt (500.0, 1.5, 48000.0, 0.5f), 48000));
    CHECK (analyser.queue ("late.wav"));
    REQUIRE (waitUntilIdle (analyser));

    const auto later = info.snapshot();
    REQUIRE (later->count ("late.wav") == 1u);
    CHECK (later->at ("late.wav").seconds == doctest::Approx (1.5));
    CHECK (later->at ("late.wav").pyramid != nullptr);
    /*  LEARNED, since 2026-09-22: a file imported after the show opened had no
        length at all until it was reopened, and now the analyser's reading is
        the answer. */
    REQUIRE (info.durations()->count ("late.wav") == 1u);

    /*  A file that cannot be coloured is published as nothing - a record
        with a hash and no pyramid would send a client to a route that
        refuses it. */
    CHECK (analyser.queue ("absent.wav"));
    REQUIRE (waitUntilIdle (analyser));
    CHECK (info.snapshot()->count ("absent.wav") == 0u);

    analyser.stop();
    CHECK (! analyser.isRunning());
}

TEST_CASE ("media analyser: a stop with work still queued returns, and forgets it")
{
    TwoSounds show;
    REQUIRE (show.built);

    audio::MediaInfo info { show.document, show.mediaFolder() };

    {
        audio::MediaAnalyser analyser { info, show.mediaFolder() };

        for (const auto& path : audio::mediaFilesNamedBy (show.document))
            analyser.queue (path);

        REQUIRE (analyser.start());
        analyser.stop();

        CHECK (! analyser.isRunning());
        CHECK (analyser.outstanding() == 0u);
    }

    /*  And one never started is destroyed without a thread to join. */
    audio::MediaAnalyser idle { info, show.mediaFolder() };
    CHECK (idle.queue ("tone.wav"));
    CHECK (! idle.isRunning());
}
