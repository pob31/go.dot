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
    CueDyn, the arithmetic (namespace draft §141, SC.6): Go.dot's own dynamics
    stage, without a Tracktion engine. Off is the same to the bit - every render
    driver in the tree runs through this stage once it is on every track (SC.7).
    On, it is spatcore's law (proved there, PR #37) with Go.dot's numbers: one
    gain on every channel, the louder side of the key heard, the track itself
    when no key is picked and silence when a picked key is not there.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/CueDyn.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace wfg;

namespace
{
    constexpr double rate = 48000.0;
    constexpr int block = 256;

    std::vector<float> sine (int n, float hz, float amplitude, double sampleRate = rate)
    {
        std::vector<float> out (static_cast<std::size_t> (n));

        for (int i = 0; i < n; ++i)
            out[static_cast<std::size_t> (i)]
                = amplitude * static_cast<float> (std::sin (2.0 * 3.141592653589793 * hz * i / sampleRate));

        return out;
    }

    /*  Runs `channels` through the stage in blocks; the key, when there is
        one, read from `key` at the same span. */
    void run (audio::CueDyn& dyn, std::vector<std::vector<float>>& channels,
              bool keyed = false, const std::vector<const std::vector<float>*>& key = {})
    {
        const auto n = static_cast<int> (channels[0].size());

        for (int at = 0; at < n; at += block)
        {
            const auto len = std::min (block, n - at);
            std::vector<float*> out;
            std::vector<const float*> in;

            for (auto& c : channels)
                out.push_back (c.data() + at);

            for (const auto* k : key)
                in.push_back (k->data() + at);

            dyn.process (out.data(), static_cast<int> (out.size()), len,
                         keyed, in.empty() ? nullptr : in.data(), static_cast<int> (in.size()));
        }
    }

    bool sameBits (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::bit_cast<std::uint32_t> (a[i]) != std::bit_cast<std::uint32_t> (b[i]))
                return false;

        return true;
    }

    /** The mean of out/in over the last quarter, where `in` is above a fifth of its peak. */
    float steadyGain (const std::vector<float>& out, const std::vector<float>& in)
    {
        auto peak = 0.0f;

        for (const auto s : in)
            peak = std::max (peak, std::abs (s));

        auto sum = 0.0;
        auto count = 0;

        for (auto i = in.size() * 3 / 4; i < in.size(); ++i)
            if (std::abs (in[i]) > 0.2f * peak)
            {
                sum += out[i] / in[i];
                ++count;
            }

        return count > 0 ? static_cast<float> (sum / count) : 0.0f;
    }

    audio::DynSettings compressor()
    {
        audio::DynSettings s;
        s.on = true;
        s.compOn = true;
        s.thresholdDb = -20.0f;
        s.ratio = 4.0f;
        s.attackMs = 1.0f;
        s.releaseMs = 500.0f;
        s.kneeDb = 0.0f;
        return s;
    }
}

TEST_CASE ("dyn: off is not called - the track the same to the bit, whatever the numbers say")
{
    audio::CueDyn dyn;
    dyn.prepare (2, rate, block);

    auto s = compressor();
    s.on = false;
    s.makeupDb = 12.0f;
    s.gateOn = true;
    dyn.set (s);

    const auto left = sine (4800, 1000.0f, 0.9f);
    const auto right = sine (4800, 300.0f, 0.001f);
    std::vector<std::vector<float>> channels { left, right };
    run (dyn, channels);

    CHECK (sameBits (channels[0], left));
    CHECK (sameBits (channels[1], right));
    CHECK (dyn.reductionDb() == 0.0f);
    CHECK_FALSE (dyn.isOn());

    //  And a default-constructed stage is off, the resting state (PRD §4.6).
    CHECK_FALSE (audio::DynSettings {}.on);
    CHECK (audio::DynSettings {}.isIdentity());
}

TEST_CASE ("dyn: a compressor hearing itself - four to one above minus twenty, one gain on both sides")
{
    /*  A 1 kHz sine at a half peaks at -6 dBFS: fourteen decibels over, a gain
        of three quarters of that off - -10.5 dB, 0.30 - read by a peak
        follower with a fast attack and a slow release, so the gain sits near
        it. The quiet side takes the loud side's gain: linked (BHT). */
    audio::CueDyn dyn;
    dyn.prepare (2, rate, block);
    dyn.set (compressor());

    const auto loud = sine (48000, 1000.0f, 0.5f);
    const auto soft = sine (48000, 1000.0f, 0.05f);
    std::vector<std::vector<float>> channels { loud, soft };
    run (dyn, channels);

    const auto left = steadyGain (channels[0], loud);
    const auto right = steadyGain (channels[1], soft);
    CHECK (left == doctest::Approx (0.30f).epsilon (0.1));
    CHECK (right == doctest::Approx (left).epsilon (0.0001));
    CHECK (dyn.reductionDb() == doctest::Approx (-10.5f).epsilon (0.1));
}

TEST_CASE ("dyn: a key - a soft cue under a loud key ducks, and a picked key that is not there is silence")
{
    const auto loud = sine (48000, 1000.0f, 0.5f);
    const auto soft = sine (48000, 1000.0f, 0.05f);

    SUBCASE ("a loud key: the soft cue takes the key's reduction")
    {
        audio::CueDyn dyn;
        dyn.prepare (2, rate, block);
        dyn.set (compressor());

        std::vector<std::vector<float>> channels { soft, soft };
        run (dyn, channels, true, { &loud });
        CHECK (steadyGain (channels[0], soft) == doctest::Approx (0.30f).epsilon (0.1));
        CHECK (sameBits (channels[0], channels[1]));
    }

    SUBCASE ("a picked key found silent: the compressor rests, to the bit")
    {
        audio::CueDyn dyn;
        dyn.prepare (2, rate, block);
        dyn.set (compressor());

        std::vector<std::vector<float>> channels { loud, loud };
        run (dyn, channels, true, {});
        CHECK (sameBits (channels[0], loud));
        CHECK (dyn.reductionDb() == 0.0f);
    }

    SUBCASE ("the louder side of a two-sided key is heard")
    {
        const std::vector<float> nothing (loud.size(), 0.0f);
        audio::CueDyn dyn;
        dyn.prepare (1, rate, block);
        dyn.set (compressor());

        std::vector<std::vector<float>> channels { soft };
        run (dyn, channels, true, { &nothing, &loud });
        CHECK (steadyGain (channels[0], soft) == doctest::Approx (0.30f).epsilon (0.1));
    }
}

TEST_CASE ("dyn: a gate - shut on a silent key down to its range, open on a loud one")
{
    auto s = audio::DynSettings {};
    s.on = true;
    s.compOn = false;
    s.gateOn = true;
    s.gateThresholdDb = -40.0f;
    s.gateRangeDb = -40.0f;
    s.gateAttackMs = 1.0f;
    s.gateHoldMs = 10.0f;
    s.gateReleaseMs = 20.0f;

    const auto loud = sine (48000, 1000.0f, 0.5f);
    const auto soft = sine (48000, 1000.0f, 0.05f);
    const std::vector<float> nothing (loud.size(), 0.0f);

    audio::CueDyn shut;
    shut.prepare (1, rate, block);
    shut.set (s);
    std::vector<std::vector<float>> closed { loud };
    run (shut, closed, true, { &nothing });
    CHECK (steadyGain (closed[0], loud) == doctest::Approx (0.01f).epsilon (0.05));
    CHECK (shut.reductionDb() == doctest::Approx (-40.0f).epsilon (0.01));

    audio::CueDyn open;
    open.prepare (1, rate, block);
    open.set (s);
    std::vector<std::vector<float>> heard { soft };
    run (open, heard, true, { &loud });
    CHECK (steadyGain (heard[0], soft) == doctest::Approx (1.0f).epsilon (0.001));
}

TEST_CASE ("dyn: the key filter - rumble under a high-pass barely reaches the detector")
{
    auto s = compressor();
    s.keyLowCutHz = 500.0f;

    audio::CueDyn dyn;
    dyn.prepare (1, rate, block);
    dyn.set (s);

    const auto rumble = sine (48000, 50.0f, 0.5f);
    const auto soft = sine (48000, 1000.0f, 0.05f);
    std::vector<std::vector<float>> channels { soft };
    run (dyn, channels, true, { &rumble });
    CHECK (steadyGain (channels[0], soft) > 0.9f);
}

TEST_CASE ("dyn: a track wider than two hears itself folded, and every channel takes one gain")
{
    audio::CueDyn dyn;
    dyn.prepare (4, rate, block);
    dyn.set (compressor());

    const auto loud = sine (48000, 1000.0f, 0.5f);
    const auto soft = sine (48000, 1000.0f, 0.05f);
    std::vector<std::vector<float>> channels { soft, soft, loud, soft };
    run (dyn, channels);

    //  Folded: the loud third channel on the left at a half (BHT).
    const auto gain = steadyGain (channels[0], soft);
    CHECK (gain < 0.9f);
    CHECK (steadyGain (channels[1], soft) == doctest::Approx (gain).epsilon (0.0001));
    CHECK (steadyGain (channels[2], loud) == doctest::Approx (gain).epsilon (0.0001));
}

TEST_CASE ("dyn: switched on again it starts from unity, and a reset clears the detector")
{
    const auto loud = sine (24000, 1000.0f, 0.5f);
    const auto soft = sine (24000, 1000.0f, 0.05f);

    audio::CueDyn dyn;
    dyn.prepare (1, rate, block);
    dyn.set (compressor());

    std::vector<std::vector<float>> first { loud };
    run (dyn, first);
    CHECK (dyn.reductionDb() < -5.0f);

    SUBCASE ("off, then on over a soft cue: no reduction carried over")
    {
        auto off = compressor();
        off.on = false;
        dyn.set (off);
        std::vector<std::vector<float>> idle { soft };
        run (dyn, idle);

        dyn.set (compressor());
        std::vector<std::vector<float>> again { soft };
        run (dyn, again);
        CHECK (sameBits (again[0], soft));
    }

    SUBCASE ("a reset: the next block starts at unity")
    {
        dyn.reset();
        std::vector<std::vector<float>> again { soft };
        run (dyn, again);
        CHECK (sameBits (again[0], soft));
    }

    SUBCASE ("without either, the release is still on its way back")
    {
        std::vector<std::vector<float>> again { soft };
        run (dyn, again);
        CHECK_FALSE (sameBits (again[0], soft));
    }
}

TEST_CASE ("dyn: set keeps what it was told, and the numbers move a sounding stage")
{
    audio::CueDyn dyn;
    dyn.prepare (1, rate, block);

    auto s = compressor();
    s.makeupDb = 6.0f;
    s.keyHighCutHz = 9000.0f;
    dyn.set (s);
    CHECK (dyn.settings().sameAs (s));
    CHECK (dyn.isOn());

    //  Make-up alone, the compressor at one to one: a steady six decibels up.
    s.ratio = 1.0f;
    dyn.set (s);

    const auto soft = sine (24000, 1000.0f, 0.05f);
    std::vector<std::vector<float>> channels { soft };
    run (dyn, channels);
    CHECK (steadyGain (channels[0], soft) == doctest::Approx (1.9953f).epsilon (0.001));
}

TEST_CASE ("dyn: at a low rate the key filter holds, and nothing comes out that is not a number")
{
    //  Spatcore guards its sidechain high cut under Nyquist (its 2026 fix); the
    //  defaults here ask for 20 kHz at 22.05 kHz.
    audio::CueDyn dyn;
    dyn.prepare (2, 22050.0, block);
    auto s = compressor();
    s.gateOn = true;
    dyn.set (s);

    const auto loud = sine (22050, 1000.0f, 0.5f, 22050.0);
    std::vector<std::vector<float>> channels { loud, loud };
    run (dyn, channels);

    auto finite = true;

    for (const auto& c : channels)
        for (const auto v : c)
            finite = finite && std::isfinite (v);

    CHECK (finite);
    CHECK (std::isfinite (dyn.reductionDb()));
    CHECK (steadyGain (channels[0], loud) == doctest::Approx (0.30f).epsilon (0.1));
}
