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
    WHERE A LAYER'S OPACITY IS AT ONE SAMPLE, from the points it was given
    (Phase 8a, namespace draft 35.4). One function, read by three: the Runner,
    to know where a picture is when Esc takes it down; the renderer, at every
    frame; and the tests, which check the two against each other.

    Straight between two points; before the first, nought - a layer is not
    there until it is placed; after the last, the last, held rather than
    guessed past (§35.4). Two points at one sample are a step, and the later
    one wins. Pure, no allocation, no clock: what makes it fit the renderer's
    frame loop and a replay alike.
*/

#include <wfg/engine/video/VideoSink.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace wfg::video
{
    /*  `points` in the order they were placed, which is the order of their
        samples: the Runner never places one behind another. A point whose
        sample is below nought is "now", and counts as the first instant. */
    template <typename Points>
    double opacityAt (const Points& points, std::int64_t sample) noexcept
    {
        double value = 0.0;
        std::int64_t from = -1;
        bool started = false;

        for (const auto& point : points)
        {
            const auto at = std::max<std::int64_t> (point.sample, 0);

            if (at > sample)
            {
                /*  THE SEGMENT WE ARE IN: from the last point reached to this
                    one, straight. Before any point, nothing is up. */
                if (! started)
                    return 0.0;

                const auto span = at - from;

                if (span <= 0)
                    return point.opacity;

                const auto t = static_cast<double> (sample - from) / static_cast<double> (span);
                return value + (point.opacity - value) * std::clamp (t, 0.0, 1.0);
            }

            value = point.opacity;
            from = at;
            started = true;
        }

        return started ? value : 0.0;
    }

    /*  The sample of the last point, or -1 with none: where a layer's opacity
        stops moving. */
    template <typename Points>
    std::int64_t lastSampleOf (const Points& points) noexcept
    {
        std::int64_t last = -1;

        for (const auto& point : points)
            last = std::max (last, point.sample);

        return last;
    }

    /*  0xRRGGBB from "#RRGGBB", or nullopt-free: black for anything else,
        since a colour that will not read is a fill of black and never a
        refusal mid-show. */
    inline std::uint32_t paintFromText (const char* text, std::size_t size) noexcept
    {
        if (size != 7 || text[0] != '#')
            return 0x000000;

        std::uint32_t rgb = 0;

        for (std::size_t i = 1; i < 7; ++i)
        {
            const auto c = text[i];
            std::uint32_t digit = 0;

            if (c >= '0' && c <= '9')       digit = static_cast<std::uint32_t> (c - '0');
            else if (c >= 'a' && c <= 'f')  digit = static_cast<std::uint32_t> (10 + (c - 'a'));
            else if (c >= 'A' && c <= 'F')  digit = static_cast<std::uint32_t> (10 + (c - 'A'));
            else                            return 0x000000;

            rgb = (rgb << 4) | digit;
        }

        return rgb;
    }
}
