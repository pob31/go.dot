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
    THE RECORDER OF A SAMPLING CHANNEL, pure (Phase 9c, stage 9c.1, namespace
    draft §19.9): a take recorded and closed to the sample; Undo bit for bit;
    the largest step at a wrap on a sine; the points moved while it plays; a
    full take closing itself; a pass refused when every layer is in use; hold,
    clear, overdub and through; the take's picture; the memory set aside at
    prepare; and every block under the real-time check. With WFG_SNAPSHOT_DIR
    set, a render to listen to as well.

    Every rig runs blocks of 128 from sample nought and keeps what came out, so
    `heard[s]` is the channel at Go.dot's sample `s`, and a press placed at a
    sample lands inside a block as a real one does.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/Looper.h>
#include <wfg/engine/rt/RtCheck.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using namespace wfg;
using audio::Looper;
using audio::TakeState;
using Verb = audio::Looper::Verb;
using Kind = audio::Looper::Event::Kind;

namespace
{
    constexpr double rate = 48000.0;
    constexpr double twoPi = 6.28318530717958647692;

    /*  A value that says which sample it was, within twenty thousand: what a
        take plays back can be checked against what went in, sample for sample. */
    float identify (std::int64_t sample, int channel)
    {
        const auto value = static_cast<float> (sample % 20000 + 1) / 32768.0f;
        return channel == 0 ? value : -value;
    }

    struct Rig
    {
        explicit Rig (double takeSeconds, int layers = 4)
        {
            looper.prepare ({ rate, 2, takeSeconds, layers });
            rt::resetCounts();
        }

        void post (Verb verb, std::int64_t at, std::int64_t in = 0, std::int64_t out = 0)
        {
            REQUIRE (looper.post ({ verb, at, in, out }));
        }

        /*  Blocks of 128 until the clock reaches `until`, the input made by
            `source`; each block under the real-time check. */
        void runTo (std::int64_t until, const std::function<float (std::int64_t, int)>& source)
        {
            std::array<float, 128> left {}, right {};
            float* pointers[] { left.data(), right.data() };

            while (clock < until)
            {
                const auto block = static_cast<int> (std::min<std::int64_t> (128, until - clock));

                for (int n = 0; n < block; ++n)
                {
                    left[static_cast<std::size_t> (n)] = source (clock + n, 0);
                    right[static_cast<std::size_t> (n)] = source (clock + n, 1);
                }

                {
                    const rt::ScopedRealtimeCheck ours { rt::Region::ours };
                    looper.process (pointers, 2, block, clock);
                }

                heard.insert (heard.end(), left.begin(), left.begin() + block);
                heardRight.insert (heardRight.end(), right.begin(), right.begin() + block);
                clock += block;
            }
        }

        std::vector<Looper::Event> events()
        {
            std::vector<Looper::Event> out;
            Looper::Event event;

            while (looper.nextEvent (event))
                out.push_back (event);

            return out;
        }

        float at (std::int64_t sample) const { return heard[static_cast<std::size_t> (sample)]; }

        /** How many samples of [from, to) differ from what `expected` says. */
        int differences (std::int64_t from, std::int64_t to, const std::function<float (std::int64_t)>& expected) const
        {
            auto count = 0;

            for (auto s = from; s < to; ++s)
                if (std::abs (at (s) - expected (s)) > 1.0e-6f)
                    ++count;

            return count;
        }

        /** The largest step between one sample and the next over [from, to). */
        float largestStep (std::int64_t from, std::int64_t to) const
        {
            auto largest = 0.0f;

            for (auto s = std::max<std::int64_t> (1, from); s < to; ++s)
                largest = std::max (largest, std::abs (at (s) - at (s - 1)));

            return largest;
        }

        Looper looper;
        std::int64_t clock = 0;
        std::vector<float> heard, heardRight;
    };

    float silence (std::int64_t, int) { return 0.0f; }
}

//==============================================================================
TEST_CASE ("looper: a take is recorded and closed to the sample, and loops from its first sample")
{
    Rig rig (1.0);
    const auto fade = rig.looper.crossfadeSamples();
    CHECK (fade == 480);

    rig.post (Verb::record, 100);
    rig.post (Verb::loop, 5100);
    rig.runTo (12000, identify);

    //  What the audio thread says it did: closed at the press, five thousand long.
    const auto events = rig.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0].kind == Kind::closed);
    CHECK (events[0].at == 5100);
    CHECK (events[0].length == 5000);

    CHECK (rig.looper.state() == TakeState::looping);
    CHECK (rig.looper.length() == 5000);
    CHECK (rig.looper.loopIn() == 0);
    CHECK (rig.looper.loopOut() == 5000);

    //  Silent while it recorded - `through` is off - and rising over the first crossfade after.
    CHECK (rig.differences (0, 5100, [] (std::int64_t) { return 0.0f; }) == 0);
    CHECK (std::abs (rig.at (5100)) < std::abs (identify (100, 0)));

    /*  Then the take itself, sample for sample: the input at 100 is the loop's
        first sample, and the one at 5099 its last, until the first wrap. */
    CHECK (rig.differences (5100 + fade, 10100, [] (std::int64_t s) { return identify (s - 5000, 0); }) == 0);
    CHECK (rig.heardRight[5100 + 1000] == doctest::Approx (identify (1100, 1)));

    //  The wrap at 10100: the post-roll, recorded past the take's end, fades out as its start fades in.
    const auto into = 10100 + fade / 2;
    const auto t = static_cast<double> (fade / 2) + 0.5;
    const auto expected = std::sin (1.57079632679 * t / fade) * identify (100 + fade / 2, 0)
                            + std::cos (1.57079632679 * t / fade) * identify (5100 + fade / 2, 0);
    CHECK (rig.at (into) == doctest::Approx (expected).epsilon (1.0e-4));
}

TEST_CASE ("looper: at a wrap on a sine it steps no further than the sine does, where a hard join would jump")
{
    /*  M43's question, asked as a bound (namespace draft §19.9): a take whose
        length is no whole number of the sine's periods, so the join is where a
        naive loop clicks. The equal-power crossfade may lift two sines in step
        by three decibels, never more; the step between samples stays near the
        sine's own. */
    Rig rig (2.0);
    const auto sine = [] (std::int64_t s, int) { return 0.5f * static_cast<float> (std::sin (twoPi * 441.0 * static_cast<double> (s) / rate)); };
    const auto sineStep = static_cast<float> (0.5 * twoPi * 441.0 / rate);

    const std::int64_t length = 30001;
    rig.post (Verb::record, 0);
    rig.post (Verb::loop, length);
    rig.runTo (length * 6, sine);

    const auto fade = rig.looper.crossfadeSamples();
    const auto largest = rig.largestStep (length + fade, length * 6);
    const auto hardJoin = std::abs (sine (length - 1, 0) - sine (0, 0)) + sineStep;

    MESSAGE ("largest step " << largest << " against the sine's own " << sineStep << "; a hard join here would step " << hardJoin);
    CHECK (largest <= 1.6f * sineStep);
    CHECK (largest < hardJoin);
}

TEST_CASE ("looper: Undo takes the top layer off, and what is left is what was heard before it, bit for bit")
{
    Rig rig (1.0, 2);
    const std::int64_t period = 6000;

    //  The take from one source, a layer from another, and nothing else heard (through is off).
    const auto source = [period] (std::int64_t s, int channel) -> float
    {
        if (s < period + 480)
            return 0.3f * static_cast<float> (std::sin (0.013 * static_cast<double> (s))) * (channel == 0 ? 1.0f : 0.5f);

        if (s >= 3 * period && s < 4 * period)
            return 0.2f * static_cast<float> (std::sin (0.021 * static_cast<double> (s)));

        return 0.0f;
    };

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, period);                 // the loop starts at 6000, wraps every 6000 from 12000
    rig.post (Verb::record, 3 * period);           // a layer laid for one period
    rig.post (Verb::record, 4 * period);           // closed
    rig.post (Verb::undo, 5 * period);             // and taken off
    rig.runTo (7 * period, source);

    CHECK (rig.looper.layerCount() == 0);
    CHECK (rig.looper.state() == TakeState::looping);

    //  The layer was heard while it lay on the take...
    auto layerHeard = 0;

    for (std::int64_t k = 0; k < period; ++k)
        if (rig.at (4 * period + k) != rig.at (2 * period + k))
            ++layerHeard;

    CHECK (layerHeard > period / 2);

    //  ...and after Undo the loop is the loop before the layer, sample for sample, bit for bit.
    auto differ = 0;

    for (std::int64_t k = 0; k < period; ++k)
    {
        if (rig.at (6 * period + k) != rig.at (2 * period + k))
            ++differ;

        if (rig.heardRight[static_cast<std::size_t> (6 * period + k)] != rig.heardRight[static_cast<std::size_t> (2 * period + k)])
            ++differ;
    }

    CHECK (differ == 0);
}

TEST_CASE ("looper: a pass laid and closed mid-loop, and points ridden across its edges, never step")
{
    /*  FOUND BY EAR, in the render below before this case existed: a pass laid
        on a loop had nothing recorded past the out point, so at every wrap the
        layer fell to silence in one sample - a tenth of full scale on a chord.
        A pass now rises and falls over a crossfade at its ends and is laid on
        both sides of every wrap (Looper.h), and nothing here may step further
        than the two sines' own slopes allow, crossfades and all. */
    Rig rig (1.0);
    const auto take = [] (std::int64_t s) { return 0.3 * std::sin (twoPi * 330.0 * static_cast<double> (s) / rate); };
    const auto pass = [] (std::int64_t s) { return 0.2 * std::sin (twoPi * 523.0 * static_cast<double> (s) / rate); };
    const auto source = [&take, &pass] (std::int64_t s, int) -> float
    {
        if (s < 30001 + 480)
            return static_cast<float> (take (s));

        return s >= 40000 && s < 67000 ? static_cast<float> (pass (s)) : 0.0f;
    };
    const auto slopes = static_cast<float> (0.3 * twoPi * 330.0 / rate + 0.2 * twoPi * 523.0 / rate);

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 30001);
    rig.post (Verb::points, 33000, 5000, 20000);   // the playhead behind the new in point: a jump
    rig.post (Verb::overdub, 40000);               // a pass begun mid-loop...
    rig.post (Verb::overdub, 67000);               // ...and closed mid-loop, most of two loops on
    rig.post (Verb::points, 80000, 0, 30001);      // the whole take again: the pass's edges inside the loop
    rig.post (Verb::points, 110000, 12000, 26000);
    rig.runTo (140000, source);

    CHECK (rig.looper.layerCount() == 1);

    const auto largest = rig.largestStep (30001 + 480, 140000);
    MESSAGE ("largest step " << largest << " against the two sines' slopes together, " << slopes);
    CHECK (largest <= 1.6f * slopes);
}

TEST_CASE ("looper: the points move while it plays - inside, it plays on; outside, it goes to the in point, crossfaded")
{
    Rig rig (1.0);
    const auto fade = rig.looper.crossfadeSamples();

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 12000);                  // the take is 0..11999 and loops from 12000
    rig.post (Verb::points, 15000, 2000, 8000);    // the playhead is at 3000: inside, it plays on
    rig.runTo (15200, identify);

    CHECK (rig.looper.loopIn() == 2000);
    CHECK (rig.looper.loopOut() == 8000);
    CHECK (rig.differences (15000, 15200, [] (std::int64_t s) { return identify (s - 12000, 0); }) == 0);

    //  It wraps at the new out: 8000 is reached at 20000, and 2000 plays from there.
    rig.runTo (21000, identify);
    CHECK (rig.differences (20000 + fade, 21000, [] (std::int64_t s) { return identify (s - 18000, 0); }) == 0);

    //  A move that leaves the playhead behind the new in point sends it there, crossfaded.
    rig.post (Verb::points, 21000, 9000, 11000);
    rig.runTo (22000, identify);
    CHECK (rig.differences (21000 + fade, 22000, [] (std::int64_t s) { return identify (s - 21000 + 9000, 0); }) == 0);

    //  Kept in the take and two crossfades apart, whatever is asked.
    rig.post (Verb::points, 22000, 5000, 5100);
    rig.runTo (22128, identify);
    CHECK (rig.looper.loopIn() == 5000);
    CHECK (rig.looper.loopOut() == 5000 + 2 * fade);

    rig.post (Verb::points, 22128, -40, 999999);
    rig.runTo (22256, identify);
    CHECK (rig.looper.loopIn() == 0);
    CHECK (rig.looper.loopOut() == 12000);
}

TEST_CASE ("looper: a take that fills its memory closes itself and loops, and says so")
{
    Rig rig (0.25);                                 // twelve thousand samples at the most
    CHECK (rig.looper.capacity() == 12000);

    rig.post (Verb::record, 0);
    rig.runTo (20000, identify);

    const auto events = rig.events();
    REQUIRE (events.size() == 1);
    CHECK (events[0].kind == Kind::full);
    CHECK (events[0].at == 12000);
    CHECK (events[0].length == 12000);
    CHECK (rig.looper.state() == TakeState::looping);
    CHECK (rig.differences (12000 + rig.looper.crossfadeSamples(), 20000,
                            [] (std::int64_t s) { return identify (s - 12000, 0); }) == 0);
}

TEST_CASE ("looper: a pass is refused when every layer is in use, and Undo never takes the take")
{
    Rig rig (1.0, 1);

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 6000);
    rig.post (Verb::record, 12000);                // a layer
    rig.post (Verb::record, 18000);                // closed: the one layer this channel keeps
    rig.post (Verb::record, 24000);                // refused
    rig.runTo (25000, identify);

    auto events = rig.events();
    REQUIRE (events.size() == 2);
    CHECK (events[1].kind == Kind::layersFull);
    CHECK (rig.looper.layerCount() == 1);
    CHECK (rig.looper.state() == TakeState::looping);

    rig.post (Verb::undo, 25000);
    rig.post (Verb::undo, 30000);                  // nothing left to take but the take: kept
    rig.runTo (31000, identify);
    CHECK (rig.looper.layerCount() == 0);
    CHECK (rig.looper.length() == 6000);
    CHECK (rig.looper.state() == TakeState::looping);

    //  Undo while it records abandons the recording whole.
    Rig second (1.0);
    second.post (Verb::record, 0);
    second.post (Verb::undo, 3000);
    second.runTo (4000, identify);
    CHECK (second.looper.state() == TakeState::empty);
    CHECK (second.looper.length() == 0);
    CHECK (second.events().empty());
}

TEST_CASE ("looper: hold keeps the take and silences it, and Loop plays it again from the in point")
{
    Rig rig (1.0);
    const auto fade = rig.looper.crossfadeSamples();

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 6000);
    rig.post (Verb::points, 7000, 1000, 5000);
    rig.post (Verb::hold, 9000);
    rig.post (Verb::loop, 20000);
    rig.runTo (22000, identify);

    CHECK (rig.differences (9000 + fade, 20000, [] (std::int64_t) { return 0.0f; }) == 0);
    CHECK (rig.differences (20000 + fade, 22000, [] (std::int64_t s) { return identify (s - 20000 + 1000, 0); }) == 0);
    CHECK (rig.looper.state() == TakeState::looping);

    //  Its cue ending while it records keeps the take, held silent.
    Rig second (1.0);
    second.post (Verb::record, 0);
    second.post (Verb::hold, 4000);
    second.runTo (8000, identify);
    CHECK (second.looper.state() == TakeState::held);
    CHECK (second.looper.length() == 4000);
    CHECK (second.differences (4000, 8000, [] (std::int64_t) { return 0.0f; }) == 0);
}

TEST_CASE ("looper: Clear empties the channel, and the next take is its own")
{
    Rig rig (1.0);
    const auto fade = rig.looper.crossfadeSamples();
    const auto source = [] (std::int64_t s, int) { return s < 13000 ? 0.5f : 0.25f; };

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 12000);
    rig.post (Verb::clear, 20000);
    rig.runTo (21000, source);

    CHECK (rig.looper.state() == TakeState::empty);
    CHECK (rig.looper.length() == 0);
    CHECK (rig.differences (20000 + fade, 21000, [] (std::int64_t) { return 0.0f; }) == 0);

    for (int chunk = 0; chunk < 12000 / Looper::peakSamples; ++chunk)
        CHECK (rig.looper.peak (0, chunk) == doctest::Approx (0.0f));

    //  A shorter take at a quarter: nothing of the first comes back.
    rig.post (Verb::record, 30000);
    rig.post (Verb::loop, 36000);
    rig.runTo (60000, source);

    CHECK (rig.looper.length() == 6000);
    CHECK (rig.differences (36000 + fade, 42000, [] (std::int64_t) { return 0.25f; }) == 0);

    /*  And never near the first take's half: at the wraps the equal-power
        crossfade may lift two equal sides towards a quarter times the root of
        two, which is all it is. */
    auto loudest = 0.0f;

    for (std::int64_t s = 36000; s < 60000; ++s)
        loudest = std::max (loudest, rig.at (s));

    CHECK (loudest < 0.36f);
}

TEST_CASE ("looper: an overdub longer than the loop adds into its own beginning, and is heard from its second time round")
{
    Rig rig (1.0);
    const auto source = [] (std::int64_t s, int) -> float
    {
        if (s < 6480)
            return 0.1f;

        return s >= 12000 && s < 27000 ? 0.01f : 0.0f;
    };

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 6000);                   // wraps at 12000, 18000, 24000, ...
    rig.post (Verb::record, 12000);                // a pass laid for two and a half loops
    rig.post (Verb::record, 27000);
    rig.runTo (36000, source);

    //  The first time round only the take; the second, the take and the first pass.
    CHECK (rig.at (12000 + 1500) == doctest::Approx (0.1f));
    CHECK (rig.at (18000 + 1500) == doctest::Approx (0.11f));

    //  Laid: three passes over the first half of the loop, two over the second.
    CHECK (rig.at (30000 + 1500) == doctest::Approx (0.13f));
    CHECK (rig.at (30000 + 4500) == doctest::Approx (0.12f));
    CHECK (rig.looper.layerCount() == 1);
}

TEST_CASE ("looper: through adds the input to what the channel sounds; off, the loop alone")
{
    Rig rig (1.0);
    const auto fade = rig.looper.crossfadeSamples();
    const auto steady = [] (std::int64_t, int) { return 0.2f; };

    rig.looper.setThrough (true);
    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 6000);
    rig.runTo (9000, steady);

    CHECK (rig.differences (0, 6000, [] (std::int64_t) { return 0.2f; }) == 0);
    CHECK (rig.differences (6000 + fade, 9000, [] (std::int64_t) { return 0.4f; }) == 0);

    rig.looper.setThrough (false);
    rig.runTo (10000, steady);
    CHECK (rig.differences (9128, 10000, [] (std::int64_t) { return 0.2f; }) == 0);
}

TEST_CASE ("looper: the take's picture follows it as it records, a chunk at a time")
{
    Rig rig (1.0);
    const auto half = [] (std::int64_t, int) { return 0.5f; };

    rig.post (Verb::record, 0);
    rig.runTo (3000, half);

    CHECK (rig.looper.state() == TakeState::recording);
    CHECK (rig.looper.length() == 3000);
    CHECK (rig.looper.peakCount() == (3000 + Looper::peakSamples - 1) / Looper::peakSamples);

    for (int chunk = 0; chunk < rig.looper.peakCount(); ++chunk)
        CHECK (rig.looper.peak (0, chunk) == doctest::Approx (0.5f));

    CHECK (rig.looper.peak (1, 0) == doctest::Approx (0.0f));
    CHECK (rig.looper.peak (0, 9999) == doctest::Approx (0.0f));
}

TEST_CASE ("looper: the memory is set aside at prepare, and a channel with no take seconds holds none and touches nothing")
{
    Looper looper;
    looper.prepare ({ rate, 2, 10.0, 2 });

    const auto room = static_cast<std::size_t> ((480000 + 480 + Looper::peakSamples - 1) / Looper::peakSamples) * Looper::peakSamples;
    const auto chunks = room / Looper::peakSamples;
    CHECK (looper.capacity() == 480000);
    CHECK (looper.bytes() == 3 * 2 * room * sizeof (float) + 3 * chunks * (sizeof (std::uint32_t) + sizeof (float)));

    Looper none;
    none.prepare ({ rate, 2, 0.0, 4 });
    CHECK (none.capacity() == 0);

    std::array<float, 64> left {}, right {};
    left.fill (0.3f);
    right.fill (-0.3f);
    float* pointers[] { left.data(), right.data() };
    REQUIRE (none.post ({ Verb::record, 0, 0, 0 }));
    none.process (pointers, 2, 64, 0);
    CHECK (left[10] == doctest::Approx (0.3f));
    CHECK (right[10] == doctest::Approx (-0.3f));
    CHECK (none.state() == TakeState::empty);
}

TEST_CASE ("looper: a whole night of it allocates nothing, locks nothing and calls nothing on the audio thread")
{
    Rig rig (1.0, 3);
    const auto sine = [] (std::int64_t s, int) { return 0.4f * static_cast<float> (std::sin (0.02 * static_cast<double> (s))); };

    rig.post (Verb::record, 100);
    rig.post (Verb::loop, 9000);
    rig.post (Verb::overdub, 12000);
    rig.post (Verb::overdub, 20000);
    rig.post (Verb::points, 21000, 1000, 7000);
    rig.post (Verb::record, 23000);
    rig.post (Verb::undo, 26000);
    rig.post (Verb::hold, 30000);
    rig.post (Verb::record, 34000);
    rig.post (Verb::loop, 39000);
    rig.post (Verb::clear, 44000);
    rig.post (Verb::record, 45000);
    rig.post (Verb::loop, 50000);
    rig.runTo (60000, sine);

    CHECK (rig.looper.state() == TakeState::looping);

    if (rt::isCounting())
        CHECK (rt::violations() == 0);
}

TEST_CASE ("looper: a render to listen to - a take of a chord, its points ridden while it loops")
{
    /*  NOT AN ASSERTION BUT AN EAR. With WFG_SNAPSHOT_DIR set, four seconds of
        a chord taken, then looped while the in and out points are moved every
        quarter of a second, and a layer laid on top: `looper-points.wav`,
        where a click at a wrap or a move would be heard. Skipped otherwise. */
    const auto folder = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {});

    if (folder.isEmpty())
        return;

    Rig rig (5.0);
    const auto chord = [] (std::int64_t s, int channel) -> float
    {
        if (s >= 4 * 48000 + 480 && (s < 12 * 48000 || s >= 13 * 48000))
            return 0.0f;

        const auto time = static_cast<double> (s) / rate;
        const auto root = channel == 0 ? 220.0 : 220.5;
        return static_cast<float> (0.15 * (std::sin (twoPi * root * time) + std::sin (twoPi * root * 1.25 * time)
                                            + std::sin (twoPi * root * 1.5 * time)));
    };

    rig.post (Verb::record, 0);
    rig.post (Verb::loop, 4 * 48000);

    for (int step = 0; step < 24; ++step)
    {
        const auto at = static_cast<std::int64_t> (5 * 48000 + step * 12000);
        const auto in = static_cast<std::int64_t> ((step % 5) * 17000);
        rig.post (Verb::points, at, in, in + 30000 + (step % 3) * 9000);
    }

    rig.post (Verb::record, 12 * 48000);
    rig.post (Verb::record, 13 * 48000);
    rig.runTo (16 * 48000, chord);

    juce::AudioBuffer<float> buffer (2, static_cast<int> (rig.heard.size()));
    std::copy (rig.heard.begin(), rig.heard.end(), buffer.getWritePointer (0));
    std::copy (rig.heardRight.begin(), rig.heardRight.end(), buffer.getWritePointer (1));

    const auto file = juce::File (folder).getChildFile ("looper-points.wav");
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
    REQUIRE (stream != nullptr);

    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate (rate)
                                                      .withNumChannels (2)
                                                      .withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    MESSAGE ("wrote " << file.getFullPathName());
}
