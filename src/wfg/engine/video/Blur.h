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
    A BLUR ON THE PROCESSOR (namespace draft 61): one plane of numbers made soft,
    as a text's outline is blurred into a glow (AKL).

    A RADIUS, the author's measure, is how far the blur reaches, in the plane's
    own points: the Gaussian's spread is a third of it, so by the radius almost
    nothing is left. It is made of three passes of a running average each way -
    which come within a few percent of a Gaussian, and cost the same at a
    radius of two hundred as at two - the plane's edges held, so a blur near
    the edge does not fade into nothing that is not there.

    Pure, std only; allocates a row's worth of scratch.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wfg::video
{
    namespace blurring
    {
        /*  THE THREE BOXES whose passes make a Gaussian of spread `sigma`:
            their half-widths, each a whole number of points (Kovesi's rule). */
        inline void boxesFor (double sigma, int (&halves)[3]) noexcept
        {
            const auto ideal = std::sqrt (12.0 * sigma * sigma / 3.0 + 1.0);
            auto lower = static_cast<int> (std::floor (ideal));

            if (lower % 2 == 0)
                --lower;

            const auto upper = lower + 2;
            const auto lowerCount = static_cast<int> (std::lround ((12.0 * sigma * sigma - 3.0 * lower * lower
                                                                    - 12.0 * lower - 9.0) / (-4.0 * lower - 4.0)));

            for (int n = 0; n < 3; ++n)
                halves[n] = ((n < lowerCount ? lower : upper) - 1) / 2;
        }

        /*  ONE RUNNING AVERAGE along a line of `count` points `stride` apart,
            its ends held. */
        inline void boxPass (float* line, int count, std::ptrdiff_t stride, int half, std::vector<float>& scratch)
        {
            if (half <= 0 || count <= 1)
                return;

            scratch.resize (static_cast<std::size_t> (count));

            for (int n = 0; n < count; ++n)
                scratch[static_cast<std::size_t> (n)] = line[n * stride];

            const auto at = [&scratch, count] (int n)
            {
                return scratch[static_cast<std::size_t> (std::clamp (n, 0, count - 1))];
            };

            const auto width = static_cast<double> (2 * half + 1);
            double sum = 0.0;

            for (int n = -half; n <= half; ++n)
                sum += at (n);

            for (int n = 0; n < count; ++n)
            {
                line[n * stride] = static_cast<float> (sum / width);
                sum += at (n + half + 1) - at (n - half);
            }
        }
    }

    /*  `plane`, `width` by `height` points row by row, blurred to `radius`. */
    inline void blurPlane (std::vector<float>& plane, int width, int height, double radius)
    {
        if (radius <= 0.0 || width <= 0 || height <= 0
              || plane.size() < static_cast<std::size_t> (width) * static_cast<std::size_t> (height))
            return;

        int halves[3] {};
        blurring::boxesFor (radius / 3.0, halves);
        std::vector<float> scratch;

        for (const auto half : halves)
        {
            for (int row = 0; row < height; ++row)
                blurring::boxPass (plane.data() + static_cast<std::ptrdiff_t> (row) * width, width, 1, half, scratch);

            for (int column = 0; column < width; ++column)
                blurring::boxPass (plane.data() + column, height, width, half, scratch);
        }
    }

    //==============================================================================
    /*  A CANVAS BLURRED BY A BLUR CUE (namespace draft 61, AKN, AKO), on the
        processor for the monitor - in arithmetic the renderer's shaders repeat
        step for step, so the monitor and the projector show one blur.

        A LARGE BLUR IS MADE SMALL: the picture halved k times - each point the
        average of the four beneath it, the edges held - until the spread left
        is eight points or less; blurred there by a Gaussian of the spread that
        is left, its taps out to three spreads; and laid back on the full
        picture by straight-line interpolation. What the halving and the laying
        back blur by themselves is taken off the spread asked for - the
        halvings add a variance of (4^k - 1) / 12 points, the laying back
        4^k / 6 - so a radius fading through the place where one more halving
        is taken does not jump.

        SIGMA is the Gaussian's spread in the picture's own points: a third of
        the cue's radius, scaled to the picture drawn. */
    namespace pyramid
    {
        constexpr double mostSpread = 8.0;

        struct Plan
        {
            int halvings = 0;
            double spread = 0.0;        ///< at the level halved to, in its points
        };

        inline Plan planFor (double sigma) noexcept
        {
            Plan plan;

            if (! (sigma > 0.0))
                return plan;

            while (sigma / std::ldexp (1.0, plan.halvings) > mostSpread && plan.halvings < 16)
                ++plan.halvings;

            const auto scale = std::ldexp (1.0, 2 * plan.halvings);    // 4^k
            const auto taken = (scale - 1.0) / 12.0 + (plan.halvings > 0 ? scale / 6.0 : 0.0);
            plan.spread = std::sqrt (std::max (0.0, (sigma * sigma - taken) / scale));
            return plan;
        }

        /*  The Gaussian's taps from the middle out, to three spreads, the
            whole summing to one; a single tap of one for no spread. */
        inline std::vector<double> tapsFor (double spread)
        {
            if (! (spread > 1.0e-6))
                return { 1.0 };

            const auto reach = static_cast<int> (std::ceil (3.0 * spread));
            std::vector<double> taps (static_cast<std::size_t> (reach) + 1);
            double sum = 0.0;

            for (int n = 0; n <= reach; ++n)
            {
                const auto weight = std::exp (-0.5 * n * n / (spread * spread));
                taps[static_cast<std::size_t> (n)] = weight;
                sum += n == 0 ? weight : 2.0 * weight;
            }

            for (auto& tap : taps)
                tap /= sum;

            return taps;
        }

        inline std::size_t indexOf (int x, int y, int width, int channel) noexcept
        {
            return (static_cast<std::size_t> (y) * static_cast<std::size_t> (width) + static_cast<std::size_t> (x)) * 3
                     + static_cast<std::size_t> (channel);
        }

        /*  Half the size, rounded up: each point the average of the four
            beneath it, a missing one the edge's. Three channels a point. */
        inline void halve (const std::vector<double>& in, int width, int height,
                           std::vector<double>& out, int& halfWidth, int& halfHeight)
        {
            halfWidth = (width + 1) / 2;
            halfHeight = (height + 1) / 2;
            out.assign (static_cast<std::size_t> (halfWidth) * static_cast<std::size_t> (halfHeight) * 3, 0.0);

            const auto at = [&in, width, height] (int x, int y, int c)
            {
                return in[indexOf (std::min (x, width - 1), std::min (y, height - 1), width, c)];
            };

            for (int y = 0; y < halfHeight; ++y)
                for (int x = 0; x < halfWidth; ++x)
                    for (int c = 0; c < 3; ++c)
                        out[indexOf (x, y, halfWidth, c)] = 0.25 * (at (2 * x, 2 * y, c) + at (2 * x + 1, 2 * y, c)
                                                                     + at (2 * x, 2 * y + 1, c) + at (2 * x + 1, 2 * y + 1, c));
        }

        /*  One pass of the taps, across or down, the edges held. */
        inline void pass (std::vector<double>& rgb, int width, int height, const std::vector<double>& taps, bool across)
        {
            if (taps.size() <= 1)
                return;

            const auto reach = static_cast<int> (taps.size()) - 1;
            const auto source = rgb;

            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        double sum = 0.0;

                        for (int n = -reach; n <= reach; ++n)
                        {
                            const auto weight = taps[static_cast<std::size_t> (n < 0 ? -n : n)];
                            sum += weight * (across ? source[indexOf (std::clamp (x + n, 0, width - 1), y, width, c)]
                                                    : source[indexOf (x, std::clamp (y + n, 0, height - 1), width, c)]);
                        }

                        rgb[indexOf (x, y, width, c)] = sum;
                    }
        }
    }

    /*  `rgb`, `width` by `height` points of three channels row by row, blurred
        to the spread `sigma` in its own points. */
    inline void blurPicture (std::vector<double>& rgb, int width, int height, double sigma)
    {
        if (! (sigma > 0.0) || width <= 0 || height <= 0)
            return;

        const auto plan = pyramid::planFor (sigma);

        std::vector<double> level = rgb, half;
        int levelWidth = width, levelHeight = height;

        for (int k = 0; k < plan.halvings; ++k)
        {
            int halfWidth = 0, halfHeight = 0;
            pyramid::halve (level, levelWidth, levelHeight, half, halfWidth, halfHeight);
            level.swap (half);
            levelWidth = halfWidth;
            levelHeight = halfHeight;
        }

        const auto taps = pyramid::tapsFor (plan.spread);
        pyramid::pass (level, levelWidth, levelHeight, taps, true);
        pyramid::pass (level, levelWidth, levelHeight, taps, false);

        if (plan.halvings == 0)
        {
            rgb.swap (level);
            return;
        }

        /*  LAID BACK by straight lines between the halved points' middles. */
        const auto step = std::ldexp (1.0, plan.halvings);

        const auto at = [&level, levelWidth, levelHeight] (int x, int y, int c)
        {
            return level[pyramid::indexOf (std::clamp (x, 0, levelWidth - 1), std::clamp (y, 0, levelHeight - 1),
                                           levelWidth, c)];
        };

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto fx = (x + 0.5) / step - 0.5;
                const auto fy = (y + 0.5) / step - 0.5;
                const auto x0 = static_cast<int> (std::floor (fx));
                const auto y0 = static_cast<int> (std::floor (fy));
                const auto tx = fx - x0, ty = fy - y0;

                for (int c = 0; c < 3; ++c)
                {
                    const auto top = at (x0, y0, c) * (1.0 - tx) + at (x0 + 1, y0, c) * tx;
                    const auto bottom = at (x0, y0 + 1, c) * (1.0 - tx) + at (x0 + 1, y0 + 1, c) * tx;
                    rgb[pyramid::indexOf (x, y, width, c)] = top * (1.0 - ty) + bottom * ty;
                }
            }
    }
}
