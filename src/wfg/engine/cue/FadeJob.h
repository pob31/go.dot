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
    A fade: a run's level moved from where it is to where a cue says, over time.

    IN THE dB DOMAIN, and that is the only choice that sounds like a fader. A
    linear ramp in gain spends most of its time in the top few decibels and then
    drops off a cliff; a linear ramp in dB is what a hand on a fader does and
    what every desk in every theatre has trained everyone to expect.

    IT RUNS ON THE TICK THREAD AT CONTROL RATE (PRD §3.4), fifty values a
    second, and each one is a single atomic store into the output stage. The
    audio side interpolates between them - which is what makes fifty values a
    second sound like a continuous fade rather than fifty steps.

    ITS VALUES ARE NOT LOGGED, and that is the rule rather than an optimisation.
    §3.15: state transitions are events and continuous readouts are not. The GO
    that started the fade is in the log; the fifty numbers a second are derived
    from it and from the document, so a replay recomputes them rather than
    reading them back. A log that carried them would be a log of the clock.

    A FADE TAKES OVER FROM A FADE. Starting one on a run that is already fading
    begins from where the level HAS GOT TO, not from where the first fade began
    - otherwise the second fade would jump, which on a PA is a click and on a
    show is a mistake nobody can explain afterwards.
*/

#include <cstdint>
#include <string>

namespace wfg::cue
{
    /*  The shape a fade takes between its two levels.

        Two of them until Phase 5's curve editor, which is when a designer gets
        to draw one. Both are monotonic: a fade that overshot would put a level
        somewhere nobody asked for, briefly, and briefly is enough.
    */
    enum class FadeCurve
    {
        /** Straight in dB - what a hand on a fader does. */
        linear,

        /** Eased at both ends, for a fade that should not announce itself. */
        sCurve
    };

    /** From the document's spelling. Anything unknown reads as linear. */
    FadeCurve fadeCurveFrom (const std::string& text) noexcept;

    /*  The level at a point through a fade, in dB.

        `progress` runs 0 to 1 and is clamped, so a caller that overshot by a
        tick gets the destination rather than a level past it.
    */
    double fadeLevelDb (double fromDb, double toDb, double progress, FadeCurve) noexcept;

    //==============================================================================
    /*  One fade in flight. A value the Runner holds and advances; it owns
        nothing and touches nothing.
    */
    struct FadeJob
    {
        /** The run whose level moves. */
        std::string target;

        /*  The run of the FADE CUE itself, which reports done when the fade
            reaches its end. A fade is a cue, so pressing GO on it creates a run
            like any other - and that run finishing is how a group will know the
            fade is over (§3.6). */
        std::string self;

        double fromDb = 0.0;
        double toDb = 0.0;

        int ticksTotal = 0;
        int ticksDone = 0;

        FadeCurve curve = FadeCurve::linear;

        /*  Whether the target is stopped when the fade arrives. What a stop cue
            with the `fade` verb is: a fade to silence, and then a stop - so the
            sound is already gone before the clip stops, and Tracktion's own
            click suppression has nothing left to suppress. */
        bool stopWhenDone = false;

        /*  WHEN THE STOP LANDS, as an absolute tick rather than as a countdown.

            The two differ only when a fade takes over from a stop, and that is
            exactly the case this exists for. A STOP IS NOT A FADE AND IT STILL
            HAPPENS (author, 2026-09-06): riding the level back up over a stop
            that is already running does not withdraw the stop, it just decides
            what the cue sounds like on the way out. An operator who fired a
            three-second stop and then touched a fader has not changed their
            mind about the cue going away - and a cue that could be kept alive
            by accident is a cue nobody can get rid of.

            Absolute, so that no arithmetic between the takeover and the arrival
            can move it. The inheriting job copies this number and nothing else
            about the stop. */
        std::int64_t stopsAtTick = 0;

        /*  Finished and waiting to be forgotten. A separate flag from
            isFinished() because a job whose LEVEL has arrived may still be
            holding a stop that has not - a short fade over the top of a long
            fade-and-stop is exactly that shape. */
        bool retired = false;

        /*  From `runError`, when the fade was never going to do anything: its
            target names an identifier this show does not contain.

            EMPTY IS THE ORDINARY CASE, including "the target is not running",
            which §3.8 makes a silent no-op rather than a failure. A pointer at
            nothing is not that, and the difference is what the `refers` column
            made checkable. */
        std::string failure;

        /** Whether the level has reached its destination. */
        bool isFinished() const noexcept { return ticksDone >= ticksTotal; }

        /** The level now. */
        double currentDb() const noexcept
        {
            if (ticksTotal <= 0)
                return toDb;

            return fadeLevelDb (fromDb, toDb,
                                static_cast<double> (ticksDone) / ticksTotal, curve);
        }
    };
}
