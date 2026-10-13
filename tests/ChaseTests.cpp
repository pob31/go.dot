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
    The catch-up's steering law (namespace draft §60, TG.9; AIS, AJC), alone,
    on made-up errors: timecode/Chase.h.

    What is pinned here: the speed is the code's, exactly, while a member is in
    step, so a code fifty parts per million fast is followed and not chased;
    out of step it is the code's times a correction of the error over two
    seconds, held to `sync/chaseRate`; the band - begun past the tolerance,
    ended within a millisecond, the code's speed held in between, and a band
    set inside out held the right way round; past `sync/chaseJump` a jump; a
    stretched voice ending within one of the stretcher's chunks. And the law
    driven round a loop the way the Runner's hook drives it - the error
    measured a launch horizon ahead, the speed decided there placed from there
    and reached over a tick, so two ticks pass before a decision is first
    measured - from a step of fifty milliseconds: closed in about nine
    seconds, never past the code (no overshoot, no ringing), and steady
    afterwards with nothing left moving.

    Pure arithmetic; it runs under fr_FR as well as C with the rest.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/timecode/Chase.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    bool sameBits (double a, double b)
    {
        return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b);
    }

    /*  A CHASER ROUND THE HOOK'S LOOP, a tick at a time (50 a second). Its
        error is measured each tick; the factor the law gives is first placed
        a tick later (the next tick's speed pass), from a launch horizon on,
        ramped over a tick - so the error moves, each tick, by what the speed
        two decisions back and the one before that made of the tick, against
        a code running at `codeSpeed`. What it records: the error each tick,
        the factor each tick, and the hysteresis. */
    struct Loop
    {
        static constexpr double tick = 0.02;

        timecode::ChaseLaw law;
        double codeSpeed = 1.0;
        double error = 0.0;
        bool correcting = false;

        std::deque<double> placed;                       // factors on their way to the voice, oldest first
        std::vector<double> errors, factors;
        std::vector<timecode::ChaseMove> moves;

        void step()
        {
            const auto said = timecode::chaseStep (law, error, codeSpeed, correcting);
            correcting = said.correcting;
            errors.push_back (error);
            factors.push_back (said.factor);
            moves.push_back (said.move);

            //  What the voice plays over the next tick: the ramp between the two oldest still on their way.
            const auto heard = 0.5 * (placed[0] + placed[1]);
            placed.pop_front();
            placed.push_back (said.factor);

            error += tick * (heard - codeSpeed);
        }

        void run (double seconds)
        {
            //  Until the step, in step: what was on its way to the voice was the code's own speed.
            if (errors.empty())
                placed.assign (3, codeSpeed);

            const auto count = static_cast<int> (std::lround (seconds / tick));

            for (int n = 0; n < count; ++n)
                step();
        }

        //  The first tick from which the hysteresis stayed off, or -1.
        int settledAt() const
        {
            auto from = -1;

            for (std::size_t n = 0; n < moves.size(); ++n)
            {
                if (moves[n] == timecode::ChaseMove::hold)
                {
                    if (from < 0)
                        from = static_cast<int> (n);
                }
                else
                {
                    from = -1;
                }
            }

            return from;
        }
    };
}

//==============================================================================
TEST_CASE ("chase law: in step it runs at exactly the code's speed - a code fifty parts per million fast is followed, not chased")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const timecode::ChaseLaw law;

    for (const auto speed : { 1.0, 1.00005, 0.99995, 1.001, 0.5 })
    {
        INFO ("code speed " << speed);

        for (const auto error : { 0.0, 0.0005, -0.0009, 0.009, -0.0099 })
        {
            const auto step = timecode::chaseStep (law, error, speed, false);
            CHECK (step.move == timecode::ChaseMove::hold);
            CHECK_FALSE (step.correcting);
            CHECK (sameBits (step.factor, speed));
        }
    }

    //  A code that is not running forwards is not the law's to follow: one.
    for (const auto speed : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() })
        CHECK (sameBits (timecode::chaseStep (law, 0.0, speed, false).factor, 1.0));

    //  And an error that is not a number holds.
    const auto unknown = timecode::chaseStep (law, std::numeric_limits<double>::quiet_NaN(), 1.00005, true);
    CHECK (unknown.move == timecode::ChaseMove::hold);
    CHECK (sameBits (unknown.factor, 1.00005));
}

TEST_CASE ("chase law: out of step, the code's speed times the error taken back over two seconds - held to sync/chaseRate")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    timecode::ChaseLaw law;

    //  Behind runs faster, ahead slower: four milliseconds is a fifth of a percent.
    CHECK (timecode::chaseStep (law, -0.004, 1.0, true).factor == doctest::Approx (1.002).epsilon (1e-12));
    CHECK (timecode::chaseStep (law, 0.004, 1.0, true).factor == doctest::Approx (0.998).epsilon (1e-12));
    CHECK (timecode::chaseStep (law, -0.015, 1.0, false).factor == doctest::Approx (1.0075).epsilon (1e-12));

    //  THE CLAMP: past twenty milliseconds the correction is one percent, however far, up to the jump.
    for (const auto error : { 0.0201, 0.05, 0.2, 0.249 })
    {
        INFO ("error " << error);
        CHECK (timecode::chaseStep (law, -error, 1.0, false).factor == doctest::Approx (1.01).epsilon (1e-12));
        CHECK (timecode::chaseStep (law, error, 1.0, false).factor == doctest::Approx (0.99).epsilon (1e-12));
        CHECK (timecode::chaseStep (law, -error, 1.0, false).move == timecode::ChaseMove::catchUp);
    }

    //  On the code's own speed, multiplied.
    CHECK (timecode::chaseStep (law, -0.1, 1.00005, false).factor == doctest::Approx (1.00005 * 1.01).epsilon (1e-12));
    CHECK (timecode::chaseStep (law, 0.1, 1.001, false).factor == doctest::Approx (1.001 * 0.99).epsilon (1e-12));

    //  `sync/chaseRate` at half a percent, and at nought: a catch-up that adds nothing.
    law.most = 0.005;
    CHECK (timecode::chaseStep (law, -0.1, 1.0, false).factor == doctest::Approx (1.005).epsilon (1e-12));
    law.most = 0.0;
    CHECK (sameBits (timecode::chaseStep (law, -0.1, 1.0, false).factor, 1.0));
}

TEST_CASE ("chase law: the band - begun past the tolerance, ended back within a millisecond, the code's speed held in between")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const timecode::ChaseLaw law;

    //  Not correcting: nine milliseconds out is in step; eleven begins.
    CHECK (timecode::chaseStep (law, -0.009, 1.0, false).move == timecode::ChaseMove::hold);
    CHECK_FALSE (timecode::chaseStep (law, -0.009, 1.0, false).correcting);
    CHECK (timecode::chaseStep (law, -0.011, 1.0, false).move == timecode::ChaseMove::catchUp);
    CHECK (timecode::chaseStep (law, -0.011, 1.0, false).correcting);
    CHECK (timecode::chaseStep (law, 0.011, 1.0, false).correcting);

    //  Correcting: it carries on inside the tolerance, down to a millisecond...
    const auto five = timecode::chaseStep (law, -0.005, 1.0, true);
    CHECK (five.move == timecode::ChaseMove::catchUp);
    CHECK (five.correcting);
    CHECK (five.factor == doctest::Approx (1.0025).epsilon (1e-12));
    CHECK (timecode::chaseStep (law, -0.0011, 1.0, true).correcting);

    //  ...and stops there, at the code's speed exactly.
    const auto done = timecode::chaseStep (law, -0.0009, 1.00005, true);
    CHECK (done.move == timecode::ChaseMove::hold);
    CHECK_FALSE (done.correcting);
    CHECK (sameBits (done.factor, 1.00005));

    //  Stopped, five milliseconds out does not start it again: the band is between.
    CHECK_FALSE (timecode::chaseStep (law, -0.005, 1.0, false).correcting);

    /*  A BAND SET INSIDE OUT - a tolerance of two milliseconds below a
        stretched voice's chunk - is held the right way round: begun past two,
        it ends within two, and never switches off the tick it switched on. */
    timecode::ChaseLaw tight;
    tight.start = 0.002;
    tight.settle = 0.005;
    CHECK (timecode::chaseStep (tight, -0.003, 1.0, false).correcting);
    CHECK (timecode::chaseStep (tight, -0.003, 1.0, true).correcting);
    CHECK_FALSE (timecode::chaseStep (tight, -0.0019, 1.0, true).correcting);
}

TEST_CASE ("chase law: past sync/chaseJump it jumps instead of catching up, at the code's speed and with nothing to carry on")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    timecode::ChaseLaw law;

    for (const auto correcting : { false, true })
    {
        const auto past = timecode::chaseStep (law, -0.26, 1.00005, correcting);
        CHECK (past.move == timecode::ChaseMove::jump);
        CHECK_FALSE (past.correcting);
        CHECK (sameBits (past.factor, 1.00005));
        CHECK (timecode::chaseStep (law, 0.3, 1.0, correcting).move == timecode::ChaseMove::jump);
        CHECK (timecode::chaseStep (law, -0.24, 1.0, correcting).move == timecode::ChaseMove::catchUp);
    }

    //  The row moved: a second.
    law.jump = 1.0;
    CHECK (timecode::chaseStep (law, -0.3, 1.0, false).move == timecode::ChaseMove::catchUp);
    CHECK (timecode::chaseStep (law, -1.01, 1.0, false).move == timecode::ChaseMove::jump);
}

TEST_CASE ("chase law: a stretched voice's catch-up ends within one of the stretcher's chunks, a plain one within a millisecond")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    //  256 samples at the interface's rate (AJC): 5.3 ms at 48 kHz, 5.8 at 44.1; never under a millisecond.
    CHECK (timecode::chaseSettleFor (true, 48000.0) == doctest::Approx (256.0 / 48000.0).epsilon (1e-12));
    CHECK (timecode::chaseSettleFor (true, 44100.0) == doctest::Approx (256.0 / 44100.0).epsilon (1e-12));
    CHECK (timecode::chaseSettleFor (true, 384000.0) == doctest::Approx (0.001).epsilon (1e-12));
    CHECK (timecode::chaseSettleFor (false, 48000.0) == doctest::Approx (0.001).epsilon (1e-12));
    CHECK (timecode::chaseSettleFor (true, 0.0) == doctest::Approx (0.001).epsilon (1e-12));

    //  The same fifty milliseconds, round the loop: the stretched one lets go at its chunk, sooner.
    Loop plain;
    plain.error = -0.050;
    plain.run (20.0);

    Loop stretched;
    stretched.law.settle = timecode::chaseSettleFor (true, 48000.0);
    stretched.error = -0.050;
    stretched.run (20.0);

    REQUIRE (plain.settledAt() > 0);
    REQUIRE (stretched.settledAt() > 0);
    CHECK (stretched.settledAt() < plain.settledAt());

    CHECK (std::abs (plain.error) < 0.001);
    CHECK (std::abs (stretched.error) <= 256.0 / 48000.0);
    CHECK (std::abs (stretched.error) > 0.004);

    //  And held there, at the code's speed: nothing moves once it has let go.
    CHECK (sameBits (stretched.factors.back(), 1.0));
    CHECK (stretched.errors.back() == doctest::Approx (stretched.errors[static_cast<std::size_t> (stretched.settledAt()) + 5]).epsilon (1e-12));
}

TEST_CASE ("chase law: round the hook's loop, a step of fifty milliseconds closes in about nine seconds without ringing, either way, and stays closed")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    for (const auto start : { -0.050, 0.050 })
    {
        INFO ("from " << start * 1000.0 << " ms");

        Loop loop;
        loop.codeSpeed = 1.00005;
        loop.error = start;
        loop.run (60.0);

        const auto sign = start < 0.0 ? -1.0 : 1.0;

        /*  NO OVERSHOOT: the error never crosses to the code's other side, and
            its size never grows - a step taken back as an exponential, not a
            swing. */
        for (std::size_t n = 0; n < loop.errors.size(); ++n)
        {
            INFO ("tick " << n);
            CHECK (loop.errors[n] * sign >= 0.0);

            if (n > 0)
                CHECK (std::abs (loop.errors[n]) <= std::abs (loop.errors[n - 1]) + 1.0e-12);
        }

        //  AT THE CLAMP while it is far: one percent on the code's speed.
        CHECK (loop.factors[2] == doctest::Approx (1.00005 * (1.0 - sign * 0.01)).epsilon (1e-12));

        /*  CLOSED IN ABOUT NINE SECONDS: three at one percent from fifty to
            twenty, then the exponential's two-second time constant from twenty
            to one, two times the log of twenty - six. */
        const auto settled = loop.settledAt();
        REQUIRE (settled > 0);
        const auto seconds = static_cast<double> (settled) * Loop::tick;
        CHECK (seconds > 8.0);
        CHECK (seconds < 10.0);
        CHECK (std::abs (loop.errors[static_cast<std::size_t> (settled)]) <= 0.001);

        //  STAYS CLOSED: a code followed at its own speed, for the fifty seconds after.
        CHECK (std::abs (loop.error) <= 0.001);
        CHECK (loop.error == doctest::Approx (loop.errors[static_cast<std::size_t> (settled) + 5]).epsilon (1e-9));
        CHECK (sameBits (loop.factors.back(), 1.00005));
        CHECK_FALSE (loop.correcting);
    }

    /*  FROM JUST PAST THE TOLERANCE, never at the clamp: the exponential
        alone, two times the log of ten and a half - 4.7 seconds. */
    Loop near;
    near.error = -0.0105;
    near.run (20.0);
    REQUIRE (near.settledAt() > 0);
    CHECK (static_cast<double> (near.settledAt()) * Loop::tick == doctest::Approx (2.0 * std::log (10.5)).epsilon (0.05));

    //  And nine milliseconds out never starts: in step as it is.
    Loop inside;
    inside.error = -0.009;
    inside.run (10.0);
    CHECK (inside.settledAt() == 0);
    CHECK (inside.error == doctest::Approx (-0.009).epsilon (1e-12));
}
