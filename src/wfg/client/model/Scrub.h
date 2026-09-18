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
    A scrub in progress: the arithmetic of a hand dragging a playhead.

    The author's own design (2026-09-18): "Scrubbing within the bounds of the
    strip is 1:1 but dragging with the cursor going above or below increases
    the precision of the increments for fine tuning. This is especially
    important on long media files. Pushing against the window edge in
    precision mode will keep sliding the cursor in the given direction."

    THREE RULES, AND THEY ARE ALL HERE RATHER THAN IN THE PANE, so a test can
    drag a pointer with numbers and read where the head would land:

      1:1 INSIDE THE STRIP - a pixel of travel is a pixel of the bar, which
      over a ninety-minute track drawn four hundred pixels wide is thirteen
      seconds a pixel. Fine for reaching the right minute, useless for the
      right beat.

      FINER ABOVE AND BELOW - every `unit` pixels the pointer is above or
      below the strip HALVES the seconds a pixel of travel moves the head, down
      to a floor of one part in two hundred and fifty-six. It is the distance
      from the strip that sets the rate, not the distance travelled, so a hand
      can come back down for coarse movement and go up again for fine, and
      the head does not jump when the rate changes: the travel is integrated
      step by step at whatever rate each step was made at.

      PUSHED AGAINST AN EDGE, IT KEEPS GOING - a pointer that has run out of
      window while in precision mode is one whose owner wants MORE travel in
      that direction, so the head slides on at a steady pace, at the rate the
      height sets, until the pointer comes back.

    THE TARGET IS A SECOND, never a pixel, and it is clamped to the material:
    nought at the left, the length at the right, and unbounded when the length
    is not known - a group has no file to run out of.

    std only, like the rest of model/.
*/

#include <string>

namespace wfg::client::model
{
    class Scrub
    {
    public:
        struct Setup
        {
            /** Where the head is when the hand takes it, in seconds. */
            double position = 0.0;

            /** How long the material is, in seconds; nought means unbounded. */
            double length = 0.0;

            /** What a pixel of travel is worth inside the strip. */
            double secondsPerPixel = 0.0;

            /** How many pixels above or below the strip halve the rate. */
            double unit = 40.0;

            /** The pointer's x when the hand took the head. */
            double x = 0.0;
        };

        void begin (const Setup& setup);
        void end();

        bool active() const noexcept { return live; }

        /*  The pointer moved to `x`, `distance` pixels outside the strip
            (nought inside). Answers where the head now is. */
        double moveTo (double x, double distance);

        /*  The pointer is pushed against an edge: slides the head `direction`
            (-1 left, +1 right) for `seconds` of wall time at the rate that
            `distance` sets, as if the pointer had travelled `pushPixelsPerSecond`
            pixels a second. Answers where the head now is. */
        double push (int direction, double seconds, double distance);

        double target() const noexcept { return at; }
        double rate() const noexcept { return lastRate; }

        /*  ONE RECORD PER POSITION THE HAND SETTLES ON, and not one per pixel:
            a drag makes a hundred events a second, and each `run.seek` stops
            and re-asks a voice. Answers whether a send is due - the head has
            moved since the last one and `intervalMs` has passed - and marks it
            sent when it is. The release sends regardless, through `settle`. */
        bool due (double nowMs, double intervalMs = 200.0);
        bool settle();

        /** The rate `distance` pixels from the strip sets: 1, 1/2, 1/4 ... 1/256. */
        static double rateFor (double distance, double unit) noexcept;

        static constexpr double pushPixelsPerSecond = 90.0;

    private:
        double clamp (double seconds) const noexcept;
        bool unsent() const noexcept;

        bool live = false;
        double at = 0.0;
        double length = 0.0;
        double secondsPerPixel = 0.0;
        double unit = 40.0;
        double lastX = 0.0;
        double lastRate = 1.0;
        double sentAt = -1.0;
        double sentMs = -1.0e9;
    };

    /** Seconds as a clock: `m:ss.t`, and hours in front when there are any. */
    std::string clockText (double seconds);

    /** The rate as a word beside the clock: empty at 1:1, else `1/8`. */
    std::string rateText (double rate);
}
