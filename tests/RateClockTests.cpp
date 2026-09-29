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
    A cue's speed as arithmetic, on its own (namespace draft §22.4): where a
    speed that moves in straight lines takes the file, and when it gets there -
    the one class the Runner and the audio thread both ask, so that neither is
    ever estimating the other.

    And the audio side's queue in front of it (RateVoice): what a late
    breakpoint does, and that a voice nobody moves is the identity.

    Nothing here plays audio. What the speed sounds like is `AudioTests`'.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/RateVoice.h>
#include <wfg/engine/clock/RateClock.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace wfg;

namespace
{
    bool sameBits (double a, double b)
    {
        return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b);
    }
}

TEST_CASE ("rate clock: a clock nobody moves reads the file exactly where the time is")
{
    /*  EXACTLY, bit for bit, because this is the case of every cue at one: a
        render at one has to be the render of the day before (§22.4), and a
        clock that answered 3.0000000000000004 for 3 would move a read by a
        rounding and nobody could say where the difference came from. */
    RateClock clock;
    clock.start (10.0, 1.0);

    for (const auto t : { 10.0, 10.5, 13.25, 1.0e6 + 0.1 })
    {
        CHECK (sameBits (clock.sourceAt (t), t));
        CHECK (sameBits (*clock.whenSourceReaches (t), t));
    }

    CHECK (clock.isIdentityFrom (10.0));
    CHECK (clock.isIdentityFrom (5.0));
    CHECK (sameBits (clock.rateAt (12.0), 1.0));
}

TEST_CASE ("rate clock: a held speed moves the file at that speed")
{
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (2.0, 1.0));   // one until two
    REQUIRE (clock.place (2.0, 0.5));   // a step to a half at two

    CHECK (clock.sourceAt (1.0) == doctest::Approx (1.0));
    CHECK (clock.sourceAt (2.0) == doctest::Approx (2.0));
    CHECK (clock.sourceAt (4.0) == doctest::Approx (3.0));   // two seconds at a half: one
    CHECK (clock.rateAt (3.0) == doctest::Approx (0.5));

    CHECK (*clock.whenSourceReaches (3.0) == doctest::Approx (4.0));
    CHECK_FALSE (clock.isIdentityFrom (2.0));
    CHECK (clock.isIdentityFrom (1.0) == false);
}

TEST_CASE ("rate clock: a straight ramp integrates by the trapezoid, and back")
{
    /*  One to two over a second from t = 0: the speed at t is 1 + t, so the
        file at t is t + t²/2 - a second and a half at the end, 0.625 halfway. */
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (1.0, 2.0));

    CHECK (clock.rateAt (0.5) == doctest::Approx (1.5));
    CHECK (clock.sourceAt (0.5) == doctest::Approx (0.625));
    CHECK (clock.sourceAt (1.0) == doctest::Approx (1.5));
    CHECK (clock.sourceAt (2.0) == doctest::Approx (3.5));   // then held at two

    //  And the inverse, across the ramp and after it.
    for (const auto t : { 0.1, 0.37, 0.5, 0.999, 1.0, 1.7 })
    {
        const auto source = clock.sourceAt (t);
        INFO ("t " << t << ", source " << source);
        CHECK (*clock.whenSourceReaches (source) == doctest::Approx (t).epsilon (1.0e-12));
    }
}

TEST_CASE ("rate clock: down to nought and back up - a tape stopped and started")
{
    /*  One to nought over two seconds, held for one, and back to one over two.
        The file moves one second down, not at all while stopped, one up. */
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (2.0, 0.0));
    REQUIRE (clock.place (3.0, 0.0));
    REQUIRE (clock.place (5.0, 1.0));

    CHECK (clock.sourceAt (2.0) == doctest::Approx (1.0));
    CHECK (clock.sourceAt (3.0) == doctest::Approx (1.0));
    CHECK (clock.sourceAt (5.0) == doctest::Approx (2.0));

    //  Reaching the second where it stopped is the moment it stopped, not later.
    //  Half a second further is on the way back up, where the speed is d/2 and
    //  the file has moved d²/4 since three: d = √2.
    CHECK (*clock.whenSourceReaches (1.0) == doctest::Approx (2.0));
    CHECK (*clock.whenSourceReaches (1.5) == doctest::Approx (3.0 + std::sqrt (2.0)));

    //  Never moving backwards, at any step through it.
    auto previous = clock.sourceAt (0.0);

    for (auto t = 0.0; t < 6.0; t += 0.01)
    {
        const auto source = clock.sourceAt (t);
        CHECK (source >= previous - 1.0e-12);
        previous = source;
    }
}

TEST_CASE ("rate clock: held at nought, the file never gets further")
{
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (1.0, 0.0));

    CHECK (clock.sourceAt (100.0) == doctest::Approx (0.5));
    CHECK (*clock.whenSourceReaches (0.25) == doctest::Approx (1.0 - std::sqrt (0.5)));
    CHECK_FALSE (clock.whenSourceReaches (0.6).has_value());
}

TEST_CASE ("rate clock: what it will not take")
{
    RateClock clock;
    clock.start (5.0, 1.0);

    CHECK_FALSE (clock.place (4.0, 1.0));                                        // before the last point
    CHECK_FALSE (clock.place (6.0, -0.5));                                       // backwards
    CHECK_FALSE (clock.place (6.0, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE (clock.place (std::numeric_limits<double>::infinity(), 1.0));
    CHECK (clock.size() == 1);

    //  Full is full: the caller forgets the past first.
    for (int i = 1; i < RateClock::capacity; ++i)
        REQUIRE (clock.place (5.0 + i, 1.0));

    CHECK_FALSE (clock.place (100.0, 1.0));
    CHECK (clock.size() == RateClock::capacity);
}

TEST_CASE ("rate clock: letting the past go changes nothing about the present")
{
    RateClock clock;
    clock.start (0.0, 1.0);

    for (int i = 1; i <= 40; ++i)
        REQUIRE (clock.place (0.02 * i, 1.0 + 0.02 * i));

    const auto before = clock.sourceAt (0.7);
    const auto when = *clock.whenSourceReaches (before + 0.1);

    clock.forgetBefore (0.5);

    CHECK (clock.size() < 40);
    CHECK (clock.front().at <= 0.5);
    CHECK (sameBits (clock.sourceAt (0.7), before));
    CHECK (sameBits (*clock.whenSourceReaches (before + 0.1), when));
}

TEST_CASE ("rate clock: identity is judged from a moment on, bit for bit")
{
    /*  A voice that played a cue at a half and then a cue at one: from the
        second cue's launch on, the clock is the identity again, and its reads
        must be exactly one-for-one - not one-for-one plus what the first cue's
        arithmetic left over. */
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (1.0, 1.0));
    REQUIRE (clock.place (1.0, 0.5));
    REQUIRE (clock.place (3.0, 0.5));
    REQUIRE (clock.place (3.0, 1.0));

    CHECK (clock.isIdentityFrom (3.0));
    CHECK (clock.isIdentityFrom (4.5));
    CHECK_FALSE (clock.isIdentityFrom (2.0));
    CHECK_FALSE (clock.isIdentityFrom (0.5));

    //  Nearly one is not one.
    REQUIRE (clock.place (5.0, std::nextafter (1.0, 2.0)));
    CHECK_FALSE (clock.isIdentityFrom (4.5));
}

TEST_CASE ("rate clock: a speed that went away and came back is not the identity, whatever has been let go")
{
    /*  Found while the Runner's half was written (S.3): identity was read off
        the points the clock still held, and the clock lets go of its past. A
        cue slowed to a half and brought back to one, once the half had been
        forgotten, read as one for ever - and its reads jumped forward by the
        time it had spent slow. */
    RateClock clock;
    clock.start (0.0, 1.0);
    REQUIRE (clock.place (1.0, 1.0));
    REQUIRE (clock.place (1.0, 0.5));
    REQUIRE (clock.place (2.0, 0.5));
    REQUIRE (clock.place (3.0, 1.0));
    REQUIRE (clock.place (4.0, 1.0));

    const auto before = clock.sourceAt (10.0);

    clock.forgetBefore (5.0);

    CHECK_FALSE (clock.isIdentityFrom (0.5));
    CHECK_FALSE (clock.isIdentityFrom (2.5));
    CHECK (clock.isIdentityFrom (3.0));
    CHECK (clock.isIdentityFrom (6.0));
    CHECK (sameBits (clock.sourceAt (10.0), before));
}

TEST_CASE ("rate voice: a voice nobody moves is the identity, and a late breakpoint is placed now and counted")
{
    audio::RateVoice voice;

    voice.drain (0.0);
    CHECK (voice.clock().isIdentityFrom (0.0));
    CHECK (voice.lateCount() == 0);

    //  Two, ahead: a hold at ten and a ramp to a half by eleven.
    REQUIRE (voice.post ({ 10.0, 1.0 }));
    REQUIRE (voice.post ({ 11.0, 0.5 }));
    voice.drain (1.0);

    CHECK (voice.clock().rateAt (10.5) == doctest::Approx (0.75));
    CHECK (voice.lateCount() == 0);

    //  One for a moment already played goes in at the block's start.
    REQUIRE (voice.post ({ 11.5, 0.25 }));
    voice.drain (12.0);

    CHECK (voice.lateCount() == 1);
    CHECK (voice.clock().rateAt (12.0) == doctest::Approx (0.25));

    //  And what came before the block is let go, keeping the point in force.
    CHECK (voice.clock().front().at <= 12.0);
}

TEST_CASE ("rate voice: the queue holds sixty-four and says when it is full")
{
    audio::RateVoice voice;

    for (int i = 0; i < audio::RateVoice::inboxSize; ++i)
        REQUIRE (voice.post ({ 1.0 + i, 1.0 }));

    CHECK_FALSE (voice.post ({ 100.0, 1.0 }));

    voice.drain (0.0);
    CHECK (voice.post ({ 100.0, 1.0 }));
}
