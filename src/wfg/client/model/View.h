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
    WHICH STRETCH OF TIME A PANEL IS LOOKING AT, and the arithmetic for moving
    it about.

    Every horizontal view in the foot panel asks the same question — a waveform
    with its in and out points, a group's members laid out against the clock,
    a fade's curve — so the answer lives once, here, and none of them carries a
    private copy of "seconds to pixels" that could drift from its neighbour's.

    ZOOMING IS ABOUT A POINT AND NOT ABOUT AN EDGE. The second under the
    pointer stays under the pointer: that is what makes a wheel feel like it is
    magnifying the thing rather than scrolling it, and it is the one property
    worth asserting about this file. Anchoring to the left edge instead would
    send whatever somebody was looking at off the side of the panel every time
    they leaned on the wheel.

    IT NEVER LEAVES THE FILE. The window is clamped into [0, length] on every
    move, so a panel cannot be scrolled into empty time and there is no state
    in which the bar is blank because the view wandered off. A window wider
    than the file is the whole file.

    AND IT HAS A FLOOR, because a pyramid has one: below a few milliseconds
    there are no finer frames to show and further zoom would draw the same
    handful of columns wider and wider, which looks like a fault. The floor is
    a number of seconds rather than of samples, because this library knows
    nothing about sample rates and should not start now.

    std only, and a value: a test can zoom and pan it a hundred times and assert
    where it ended up, with no window anywhere.
*/

#include <algorithm>

namespace wfg::client::model
{
    struct View
    {
        double from = 0.0;    ///< the second at the left edge
        double to = 0.0;      ///< the second at the right edge
        double length = 0.0;  ///< how long the thing being looked at is

        /** The shortest window this will zoom to. Ten milliseconds. */
        static constexpr double floorSeconds = 0.01;

        double span() const noexcept { return to - from; }

        bool isWholeThing() const noexcept
        {
            return from <= 0.0 && to >= length;
        }

        /** The whole of it, which is where every view starts. */
        void reset (double lengthToUse) noexcept
        {
            length = std::max (0.0, lengthToUse);
            from = 0.0;
            to = length;
        }

        /*  Pulled back inside the thing, keeping the span where it can. A
            window wider than the file becomes the file; one that has been
            pushed off an end slides back rather than being squashed against
            it, because a squashed window silently changes the zoom somebody
            chose. */
        void clamp() noexcept
        {
            if (! (length > 0.0))
            {
                from = to = 0.0;
                return;
            }

            auto width = std::min (std::max (span(), floorSeconds), length);

            if (from < 0.0)            from = 0.0;
            if (from + width > length) from = length - width;
            if (from < 0.0)            from = 0.0;

            to = from + width;
        }

        /*  THE SECOND AT A PIXEL, and the pixel of a second. `width` is the
            panel's, in pixels; a width of nought answers the left edge rather
            than dividing by it. */
        double secondsForX (double x, int width) const noexcept
        {
            if (width <= 0)
                return from;

            return from + span() * x / static_cast<double> (width);
        }

        double xForSeconds (double seconds, int width) const noexcept
        {
            if (! (span() > 0.0) || width <= 0)
                return 0.0;

            return (seconds - from) / span() * static_cast<double> (width);
        }

        /*  ZOOMED ABOUT A PIXEL: `factor` below one narrows the window, above
            one widens it, and the second under `x` is where it was when this
            returns. */
        void zoomAbout (double x, int width, double factor) noexcept
        {
            if (! (length > 0.0) || ! (factor > 0.0) || width <= 0)
                return;

            const auto anchor = secondsForX (x, width);
            const auto wanted = std::min (std::max (span() * factor, floorSeconds), length);
            const auto share = span() > 0.0 ? (anchor - from) / span() : 0.0;

            from = anchor - wanted * share;
            to = from + wanted;
            clamp();
        }

        /** Slid by a number of pixels, the span kept. */
        void panBy (double pixels, int width) noexcept
        {
            if (! (span() > 0.0) || width <= 0)
                return;

            const auto seconds = span() * pixels / static_cast<double> (width);

            from += seconds;
            to += seconds;
            clamp();
        }
    };
}
