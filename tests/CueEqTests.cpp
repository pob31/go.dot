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

/*
    CueEq, the arithmetic - PR 9a.1.

    These run without a Tracktion engine, which is the point of the class being
    separate from the plugin that will hold it. The one test that matters most
    is the passthrough: a flat EQ that moved one sample by one bit would break
    every render driver in the tree, because their arithmetic is that each
    output sample equals the gain. The second is the one that pins the picture
    to the sound: a sine measured through the filters reads what EqMath says it
    should, so the curve the desktop draws is the response the voice plays.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/CueEq.h>

#include <juce_core/juce_core.h>
#include <wfg/engine/audio/EqMath.h>
#include <wfg/engine/rt/RtCheck.h>

#include "TestSupport.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    constexpr double rate = 48000.0;
    constexpr int block = 128;

    /*  A stereo block of frames the EQ filters in place, and the pointers the
        audio side hands over. */
    struct Buffers
    {
        Buffers (int numChannels, int numSamples)
            : data (static_cast<std::size_t> (numChannels),
                    std::vector<float> (static_cast<std::size_t> (numSamples), 0.0f)),
              channels (numChannels), samples (numSamples)
        {
            for (auto& channel : data)
                pointers.push_back (channel.data());
        }

        float* const* out() noexcept { return pointers.data(); }

        std::vector<std::vector<float>> data;
        std::vector<float*> pointers;
        int channels, samples;
    };

    /*  Runs a steady sine through the EQ for `seconds` and answers the RMS of
        the last half second, in dB relative to the input's RMS - which is the
        magnitude response at that frequency once the filters have settled. */
    double measureDb (audio::CueEq& eq, double frequency, double seconds = 1.5, double sampleRate = rate)
    {
        Buffers buffers { 2, block };
        const auto totalFrames = static_cast<long> (seconds * sampleRate);
        const auto settleFrames = totalFrames / 3;

        auto phase = 0.0;
        const auto step = 2.0 * audio::eqmath::pi * frequency / sampleRate;

        auto sumOut = 0.0;
        auto sumIn = 0.0;
        long counted = 0;

        for (long frame = 0; frame < totalFrames; frame += block)
        {
            for (int n = 0; n < block; ++n)
            {
                const auto x = static_cast<float> (0.25 * std::sin (phase));
                phase += step;
                buffers.data[0][static_cast<std::size_t> (n)] = x;
                buffers.data[1][static_cast<std::size_t> (n)] = x;
            }

            std::vector<float> input (buffers.data[0]);

            eq.process (buffers.out(), buffers.channels, block);

            if (frame >= totalFrames - settleFrames)
            {
                for (int n = 0; n < block; ++n)
                {
                    const auto y = static_cast<double> (buffers.data[0][static_cast<std::size_t> (n)]);
                    const auto x = static_cast<double> (input[static_cast<std::size_t> (n)]);
                    sumOut += y * y;
                    sumIn += x * x;
                    ++counted;
                }
            }
        }

        if (counted == 0 || sumIn <= 0.0 || sumOut <= 0.0)
            return -200.0;

        return 10.0 * std::log10 (sumOut / sumIn);
    }

    audio::EqSettings withBand (int index, audio::EqSettings::Shape shape,
                                float freq, float gain, float q)
    {
        auto settings = audio::EqSettings::flat();
        settings.band[index] = { shape, freq, gain, q };
        return settings;
    }

    bool sameBits (float a, float b) noexcept
    {
        return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b);
    }
}

//==============================================================================
TEST_CASE ("cue eq: a default-constructed EqSettings is flat, and eq.reset's target")
{
    const audio::EqSettings flat;

    CHECK (flat.isIdentity());
    CHECK (flat.on);
    CHECK_FALSE (flat.hpf);
    CHECK_FALSE (flat.lpf);

    for (const auto& band : flat.band)
        CHECK_FALSE (audio::EqSettings::bandIsActive (band));

    CHECK (flat.sameAs (audio::EqSettings::flat()));

    auto shaped = flat;
    shaped.band[1].gain = 6.0f;
    CHECK_FALSE (shaped.isIdentity());
    CHECK_FALSE (shaped.sameAs (flat));

    /*  Off is identity whatever the bands say: the A/B a designer reaches for. */
    shaped.on = false;
    CHECK (shaped.isIdentity());
}

TEST_CASE ("cue eq: flat is bit-exact passthrough - the render drivers depend on it")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);
    eq.set (audio::EqSettings::flat());

    std::mt19937 random { 9 };
    std::uniform_real_distribution<float> noise { -1.0f, 1.0f };

    Buffers buffers { 2, block };

    for (int pass = 0; pass < 80; ++pass)
    {
        std::vector<std::vector<float>> before;

        for (auto& channel : buffers.data)
        {
            for (auto& sample : channel)
                sample = noise (random);

            before.push_back (channel);
        }

        eq.process (buffers.out(), buffers.channels, block);

        for (std::size_t ch = 0; ch < buffers.data.size(); ++ch)
            for (std::size_t n = 0; n < buffers.data[ch].size(); ++n)
                REQUIRE (sameBits (buffers.data[ch][n], before[ch][n]));
    }

    /*  And with the EQ off but a band shaped - still untouched. */
    auto off = withBand (1, audio::EqSettings::Shape::peak, 1000.0f, 12.0f, 1.0f);
    off.on = false;
    eq.set (off);

    for (auto& channel : buffers.data)
        for (auto& sample : channel)
            sample = noise (random);

    const auto before = buffers.data;
    eq.process (buffers.out(), buffers.channels, block);

    for (std::size_t ch = 0; ch < buffers.data.size(); ++ch)
        for (std::size_t n = 0; n < buffers.data[ch].size(); ++n)
            REQUIRE (sameBits (buffers.data[ch][n], before[ch][n]));
}

TEST_CASE ("cue eq: a +6 dB peak at 1 kHz reads +6 there and nought a decade away")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);
    eq.set (withBand (1, audio::EqSettings::Shape::peak, 1000.0f, 6.0f, 1.0f));

    CHECK (measureDb (eq, 1000.0) == doctest::Approx (6.0).epsilon (0.02));
    CHECK (measureDb (eq, 100.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));
    CHECK (measureDb (eq, 10000.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));
}

TEST_CASE ("cue eq: a -12 dB peak cuts twelve decibels at its centre")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);
    eq.set (withBand (2, audio::EqSettings::Shape::peak, 2000.0f, -12.0f, 2.0f));

    CHECK (measureDb (eq, 2000.0) == doctest::Approx (-12.0).epsilon (0.02));
}

TEST_CASE ("cue eq: a band switched off is out of the signal and keeps its numbers")
{
    /*  The press of a gain rotary on a surface page (author, 2026-09-25): off
        takes the band out, on brings back the same band. A band that is off
        is not a gain of nought - the gain is still there to come back. */
    auto settings = withBand (1, audio::EqSettings::Shape::peak, 1000.0f, 6.0f, 1.0f);
    settings.band[1].on = false;

    CHECK_FALSE (audio::EqSettings::bandIsActive (settings.band[1]));
    CHECK (settings.isIdentity());
    CHECK (audio::eqmath::responseDb (settings, 1000.0, rate) == doctest::Approx (0.0));

    audio::CueEq eq;
    eq.prepare (2, rate, block);
    eq.set (settings);

    CHECK (eq.isIdentity());
    CHECK (measureDb (eq, 1000.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.01));

    const auto held = eq.settings();
    CHECK_FALSE (held.band[1].on);
    CHECK (sameBits (held.band[1].gain, 6.0f));

    /*  Back on: the same band, heard at once. */
    settings.band[1].on = true;
    CHECK_FALSE (settings.sameAs (held));
    eq.set (settings);

    CHECK (eq.settings().band[1].on);
    CHECK (measureDb (eq, 1000.0) == doctest::Approx (6.0).epsilon (0.02));
}

TEST_CASE ("cue eq: the high-pass is -3 dB at its frequency and twelve an octave below")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);

    auto settings = audio::EqSettings::flat();
    settings.hpf = true;
    settings.hpfFreq = 100.0f;
    eq.set (settings);

    CHECK (measureDb (eq, 100.0) == doctest::Approx (-3.0).epsilon (0.1));
    CHECK (measureDb (eq, 50.0) < -11.0);
    CHECK (measureDb (eq, 2000.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));
}

TEST_CASE ("cue eq: the low-pass mirrors it")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);

    auto settings = audio::EqSettings::flat();
    settings.lpf = true;
    settings.lpfFreq = 8000.0f;
    eq.set (settings);

    CHECK (measureDb (eq, 8000.0) == doctest::Approx (-3.0).epsilon (0.1));
    CHECK (measureDb (eq, 16000.0) < -11.0);
    CHECK (measureDb (eq, 500.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));
}

TEST_CASE ("cue eq: a low shelf lifts everything below its corner, a high shelf everything above")
{
    audio::CueEq low;
    low.prepare (2, rate, block);
    low.set (withBand (0, audio::EqSettings::Shape::lowShelf, 100.0f, 6.0f, 0.7f));

    CHECK (measureDb (low, 20.0) == doctest::Approx (6.0).epsilon (0.05));
    CHECK (measureDb (low, 5000.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));

    audio::CueEq high;
    high.prepare (2, rate, block);
    high.set (withBand (3, audio::EqSettings::Shape::highShelf, 8000.0f, -6.0f, 0.7f));

    CHECK (measureDb (high, 18000.0) == doctest::Approx (-6.0).epsilon (0.05));
    CHECK (measureDb (high, 200.0) == doctest::Approx (0.0).scale (1.0).epsilon (0.1));
}

TEST_CASE ("cue eq: what EqMath draws is what the filters play, at 44.1, 48 and 96 kHz")
{
    /*  The one that pins the desktop's curve to the sound: the response
        predicted from the coefficients and the response measured through the
        filters agree within a fifth of a decibel, at every point above and at
        every rate a show runs at. */
    auto settings = audio::EqSettings::flat();
    settings.hpf = true;
    settings.hpfFreq = 60.0f;
    settings.band[0] = { audio::EqSettings::Shape::lowShelf, 150.0f, 3.0f, 0.7f };
    settings.band[1] = { audio::EqSettings::Shape::peak, 700.0f, -8.0f, 1.5f };
    settings.band[2] = { audio::EqSettings::Shape::peak, 3000.0f, 4.0f, 0.5f };
    settings.band[3] = { audio::EqSettings::Shape::highShelf, 9000.0f, -2.0f, 0.7f };

    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        audio::CueEq eq;
        eq.prepare (2, sampleRate, block);
        eq.set (settings);

        for (const auto frequency : { 40.0, 100.0, 300.0, 700.0, 1500.0, 3000.0, 6000.0, 12000.0 })
        {
            INFO ("rate " << sampleRate << " frequency " << frequency);
            const auto predicted = audio::eqmath::responseDb (settings, frequency, sampleRate);
            const auto measured = measureDb (eq, frequency, 1.5, sampleRate);
            CHECK (std::abs (measured - predicted) < 0.2);
        }
    }
}

TEST_CASE ("cue eq: EqMath's response is nought for a flat EQ and additive across sections")
{
    const auto flat = audio::EqSettings::flat();

    for (const auto frequency : { 20.0, 200.0, 2000.0, 20000.0 })
        CHECK (audio::eqmath::responseDb (flat, frequency, rate) == doctest::Approx (0.0));

    auto two = flat;
    two.band[1] = { audio::EqSettings::Shape::peak, 1000.0f, 6.0f, 1.0f };
    two.band[2] = { audio::EqSettings::Shape::peak, 1000.0f, -2.0f, 1.0f };

    CHECK (audio::eqmath::responseDb (two, 1000.0, rate) == doctest::Approx (4.0).epsilon (0.01));

    /*  A section's own maths: a peak reads its gain at the centre, exactly. */
    const auto peak = audio::eqmath::peak (rate, 1000.0, 1.0, 6.0);
    CHECK (audio::eqmath::magnitudeDb (peak, 1000.0, rate) == doctest::Approx (6.0).epsilon (0.001));
    CHECK (audio::eqmath::magnitudeDb (audio::eqmath::identity(), 1000.0, rate) == doctest::Approx (0.0));
}

TEST_CASE ("cue eq: set() moves the sound while it plays, without a NaN or a jump above the gain")
{
    audio::CueEq eq;
    eq.prepare (2, rate, block);
    eq.set (audio::EqSettings::flat());

    Buffers buffers { 2, block };
    auto phase = 0.0;
    const auto step = 2.0 * audio::eqmath::pi * 1000.0 / rate;
    auto peak = 0.0f;

    for (int pass = 0; pass < 400; ++pass)
    {
        /*  A different setting every few blocks - a rotary turning. */
        if (pass % 5 == 0)
            eq.set (withBand (1, audio::EqSettings::Shape::peak, 1000.0f,
                              static_cast<float> ((pass / 5) % 13) - 6.0f, 1.0f));

        for (int n = 0; n < block; ++n)
        {
            const auto x = static_cast<float> (0.25 * std::sin (phase));
            phase += step;
            buffers.data[0][static_cast<std::size_t> (n)] = x;
            buffers.data[1][static_cast<std::size_t> (n)] = x;
        }

        eq.process (buffers.out(), buffers.channels, block);

        for (const auto& channel : buffers.data)
            for (const auto sample : channel)
            {
                REQUIRE (std::isfinite (sample));
                peak = std::max (peak, std::abs (sample));
            }
    }

    /*  +6 dB on 0.25 is 0.5; a little over for the step transient is fine,
        a runaway is not. */
    CHECK (peak < 0.6f);

    /*  And what was last set reads back. */
    const auto last = eq.settings();
    CHECK (sameBits (last.band[1].freq, 1000.0f));
}

TEST_CASE ("cue eq: reset clears the delay lines at the next block")
{
    audio::CueEq eq;
    eq.prepare (1, rate, block);
    eq.set (withBand (0, audio::EqSettings::Shape::lowShelf, 200.0f, 12.0f, 0.7f));

    Buffers buffers { 1, block };

    for (auto& sample : buffers.data[0])
        sample = 0.5f;

    eq.process (buffers.out(), 1, block);

    /*  With state in the delay lines, a silent block would ring; after a
        reset it is exactly silent. */
    eq.reset();

    for (auto& sample : buffers.data[0])
        sample = 0.0f;

    eq.process (buffers.out(), 1, block);

    for (const auto sample : buffers.data[0])
        REQUIRE (sameBits (sample, 0.0f));
}

TEST_CASE ("cue eq: process allocates nothing")
{
    if (! rt::isCounting())
    {
        MESSAGE ("WFG_RT_CHECKS is off; the counting allocator is not compiled in");
        return;
    }

    audio::CueEq eq;
    eq.prepare (8, rate, block);

    auto settings = audio::EqSettings::flat();
    settings.hpf = true;
    settings.lpf = true;

    for (auto& band : settings.band)
        band.gain = 3.0f;

    eq.set (settings);

    Buffers buffers { 8, block };
    rt::resetCounts();

    {
        rt::ScopedRealtimeCheck region { rt::Region::ours };

        for (int pass = 0; pass < 200; ++pass)
        {
            /*  A change mid-run rebuilds coefficients on the audio thread,
                which must allocate nothing either. */
            if (pass == 100)
            {
                settings.band[2].gain = -3.0f;
                eq.set (settings);
                eq.reset();
            }

            eq.process (buffers.out(), buffers.channels, block);
        }
    }

    CHECK (rt::violations() == 0);
}

TEST_CASE ("cue eq: channels beyond what was prepared are left alone")
{
    audio::CueEq eq;
    eq.prepare (1, rate, block);
    eq.set (withBand (1, audio::EqSettings::Shape::peak, 1000.0f, 12.0f, 1.0f));

    Buffers buffers { 3, block };

    for (auto& channel : buffers.data)
        for (auto& sample : channel)
            sample = 0.5f;

    eq.process (buffers.out(), 3, block);

    for (const auto sample : buffers.data[1])
        REQUIRE (sameBits (sample, 0.5f));

    for (const auto sample : buffers.data[2])
        REQUIRE (sameBits (sample, 0.5f));

    CHECK (eq.numChannels() == 1);
    CHECK (eq.sampleRate() == doctest::Approx (rate));
}

//==============================================================================
/*  M30 - the EQ's block cost (§17.9, PRD §6.11): thirty-two voices of two
    channels at 96 kHz in blocks of sixty-four, ten thousand blocks each way -
    every section in (the high-pass, the low-pass, four peaks), a flat EQ (the
    identity fast path), and the EQ switched off - as microseconds per block
    for all thirty-two, against a block period of 667 us. Run with --no-skip
    on a quiet machine; the figures go to §17.9. */
TEST_CASE ("M30: the EQ's block cost for thirty-two voices - every section in, flat, and off" * doctest::skip())
{
    constexpr int voices = 32, channels = 2, blockSize = 64, blocks = 10000;
    constexpr double sampleRate = 96000.0;

    std::vector<audio::CueEq> eqs (voices);
    std::vector<std::vector<float>> audio (voices, std::vector<float> (channels * blockSize, 0.1f));

    for (auto& eq : eqs)
        eq.prepare (channels, sampleRate, blockSize);

    const auto run = [&] (const audio::EqSettings& settings, const char* what)
    {
        for (auto& eq : eqs)
            eq.set (settings);

        std::vector<float*> pointers (channels);

        //  A warm-up so the first block's coefficient build is not in the figure.
        for (int v = 0; v < voices; ++v)
        {
            for (int c = 0; c < channels; ++c) pointers[static_cast<std::size_t> (c)] = audio[static_cast<std::size_t> (v)].data() + c * blockSize;
            eqs[static_cast<std::size_t> (v)].process (pointers.data(), channels, blockSize);
        }

        const auto t0 = std::chrono::steady_clock::now();

        for (int n = 0; n < blocks; ++n)
            for (int v = 0; v < voices; ++v)
            {
                for (int c = 0; c < channels; ++c) pointers[static_cast<std::size_t> (c)] = audio[static_cast<std::size_t> (v)].data() + c * blockSize;
                eqs[static_cast<std::size_t> (v)].process (pointers.data(), channels, blockSize);
            }

        const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count() / blocks;
        MESSAGE (std::string (what) << ": " << juce::String (us, 2) << " us per block for " << voices << " voices ("
                 << juce::String (us / voices, 3) << " us a voice), of a " << juce::String (blockSize * 1.0e6 / sampleRate, 1) << " us block");
    };

    audio::EqSettings busy = audio::EqSettings::flat();
    busy.hpf = true;
    busy.lpf = true;
    for (auto& band : busy.band) band.gain = 3.0f;
    run (busy, "every section in");

    run (audio::EqSettings::flat(), "flat (identity)");

    auto off = audio::EqSettings::flat();
    off.on = false;
    run (off, "switched off");
}
