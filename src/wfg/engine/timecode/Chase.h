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
    HOW A CHASER CATCHES UP WITH ITS CODE (namespace draft §60, TG.9; AIF,
    AIN, AIS, AJC).

    A time group's member that chases by `varispeed` or `timestretch` is never
    moved while it is near its code: it is run a little faster or a little
    slower until it is back in step - varispeed bending the pitch as a tape
    does, timestretch keeping it (AIF). This is the law that says how much
    faster or slower, once a tick, from how far out of step the hook measured
    it. Past `sync/chaseJump` it says "jump" instead, and the hook moves the
    sound with TG.8's crossfaded jump (AIN).

    THE LAW (AIS). The speed is the code's speed times one plus a correction,
    and the correction is the error taken back over two seconds, held to
    `sync/chaseRate`:

        factor = codeSpeed * (1 - clamp (error / 2 s, -chaseRate, +chaseRate))

    the error ahead when positive, so a member ahead of its code runs slower
    and one behind runs faster. THE CODE'S SPEED FIRST, because a code that
    drifts - a machine fifty parts per million fast - is a speed the member
    must simply run at, not an error it should keep catching: holding the
    code's speed, a member in step stays in step however long the code runs,
    and the correction is left with only what the code's line could not say
    in advance - a re-anchor, a step in its speed, a jump. PROPORTIONAL, so
    the error falls away as an exponential with a two-second time constant and
    never changes sign on the way: a member fifty milliseconds behind runs one
    percent fast (the clamp) until it is twenty behind, then eases in, and is
    within a millisecond some nine seconds after it started. Nothing in it
    integrates, so nothing winds up and nothing rings; the hook's own lag - the
    speed is placed a launch horizon ahead and ramped over a tick, so a
    correction is first measured two ticks after it is decided - is a fiftieth
    of the time constant, far too short to make it overshoot.

    WITH HYSTERESIS. It starts correcting only once the member is more than
    `sync/chaseTolerance` out (10 ms, under the twenty at which two sounds
    flam), and once started it carries on until the member is back within a
    millisecond; in between it holds the code's speed. Without the band, a
    member drifting at the tolerance would switch its correction on and off
    tick after tick, and a speed that moves every tick is a pitch that wavers
    under varispeed. The band also makes "in step" mean something: within the
    tolerance and not being steered.

    A STRETCHED VOICE STOPS SOONER (AJC, the implementer's). The steering
    measures the run's own clock - the arithmetic the voice is told to play by
    - and not the sound, so nothing a stretcher does can make it hunt. But a
    stretched voice's place in its file is only right to within one to three of
    the stretcher's 256-sample chunks (M14, namespace draft §22.10: the state
    the stretcher primed from, one machine to the next), five to sixteen
    milliseconds at 48 kHz, so steering its clock closer than one chunk buys
    nothing anybody can hear, and keeps the stretcher's ratio moving for
    longer than it needs to. A timestretch chaser's catch-up therefore ends
    within one chunk at the interface's rate (5.3 ms at 48 kHz) rather than a
    millisecond: its steady error, as heard, is that chunk at most on its
    clock plus the stretcher's own one to three, held - never hunted.

    PURE AND INLINE: no clock, no JUCE, nothing allocated - asked once a tick
    per chaser by the Runner's hook, and of made-up errors by the tests.
*/

#include <algorithm>
#include <cmath>

namespace wfg::timecode
{
    /*  HOW CLOSE A CATCH-UP BRINGS A MEMBER BEFORE IT HOLDS THE CODE'S SPEED
        AGAIN: a millisecond (AIS) - and for a stretched voice one of the
        stretcher's 256-sample chunks at the interface's rate, when that is
        longer (AJC, above). */
    inline constexpr double chaseSettleSeconds = 0.001;
    inline constexpr double stretchChunkFrames = 256.0;

    inline double chaseSettleFor (bool stretched, double sampleRate) noexcept
    {
        if (! stretched || ! std::isfinite (sampleRate) || ! (sampleRate > 0.0))
            return chaseSettleSeconds;

        return std::max (chaseSettleSeconds, stretchChunkFrames / sampleRate);
    }

    //  The numbers the law reads, in seconds and fractions: the show's `sync` rows, and its two fixed ones.
    struct ChaseLaw
    {
        double start = 0.010;                   // out of step before a catch-up begins: `sync/chaseTolerance`
        double settle = chaseSettleSeconds;     // back within this, it ends and holds the code's speed
        double jump = 0.25;                     // out of step past this, it jumps instead: `sync/chaseJump`
        double most = 0.01;                     // the most it adds to or takes from the code's speed: `sync/chaseRate`
        double over = 2.0;                      // the seconds an error is taken back over: the gain is one over this
    };

    //  What the law tells the hook to do with the member this tick.
    enum class ChaseMove
    {
        hold,           // in step: run at the code's speed
        catchUp,        // out of step: run at the code's speed times a correction
        jump            // too far out to catch up: jump to where the code is (TG.8)
    };

    struct ChaseStep
    {
        ChaseMove move = ChaseMove::hold;
        double factor = 1.0;            // what the member's own speed is multiplied by
        bool correcting = false;        // the hysteresis, carried to the next tick
    };

    /*  ONE TICK OF THE LAW: `error` the member's distance from its code in the
        code's seconds, ahead when positive; `codeSpeed` code seconds per Go.dot
        second (a code that is not running forwards is not steered, and reads
        as one here); `correcting` what the last tick said. A jump leaves the
        speed at the code's and the hysteresis off, since a jump lands in step.
        An error that is not a number holds. */
    inline ChaseStep chaseStep (const ChaseLaw& law, double error, double codeSpeed, bool correcting) noexcept
    {
        ChaseStep out;
        out.factor = std::isfinite (codeSpeed) && codeSpeed > 0.0 ? codeSpeed : 1.0;

        if (! std::isfinite (error))
            return out;

        const auto size = std::abs (error);

        if (size > std::max (0.0, law.jump))
        {
            out.move = ChaseMove::jump;
            return out;
        }

        /*  THE BAND: begun past `start`, ended within `settle`. A settle wider
            than the start - a tolerance set below a stretcher's chunk - is held
            to it, so the band is never inside out: a correction that ended the
            moment it began would switch on and off every other tick. */
        const auto start = std::max (0.0, law.start);
        const auto settle = std::clamp (law.settle, 0.0, start);

        out.correcting = correcting ? size > settle : size > start;

        if (! out.correcting)
            return out;

        const auto most = std::clamp (law.most, 0.0, 1.0);
        const auto over = law.over > 0.0 ? law.over : 2.0;

        out.move = ChaseMove::catchUp;
        out.factor *= 1.0 - std::clamp (error / over, -most, most);
        return out;
    }
}
