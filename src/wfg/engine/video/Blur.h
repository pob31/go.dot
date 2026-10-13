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
}
