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
    A CUE'S GRADE (Phase 8a, namespace draft 36, VP and VU): the author's
    controls - contrast, saturation, gamma, hue, and four curves - worked out
    once here, in arithmetic the GPU's shader repeats line for line.

    In order, on the colour's display-space numbers 0..1 (VD):
      - GAMMA: out = in ^ (1 / gamma) - above one brightens the middle;
      - CONTRAST about mid-grey: out = (in - 0.5) x contrast % + 0.5;
      - HUE, the colour turned about the grey axis by `hue` degrees, and
        SATURATION, its distance from grey scaled by `saturation` %;
      - THE CURVES: the luminosity curve on all three channels, then each
        colour's own - baked by the engine into one table a channel, the
        luminosity curve folded in, 256 steps.

    100 % contrast and saturation, gamma 1, hue 0 and no curves change
    nothing, and are what a cue starts with. Pure, no allocation.
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wfg::video
{
    struct Grade
    {
        double contrast = 100.0;
        double saturation = 100.0;
        double gamma = 1.0;
        double hue = 0.0;

        /*  The curves baked: red, green and blue, each the luminosity curve
            then the channel's own, 0..255 to 0..255. */
        std::array<std::array<std::uint8_t, 256>, 3> tables {};

        bool hasCurves = false;

        bool isIdentity() const noexcept
        {
            return std::abs (contrast - 100.0) < 1e-9 && std::abs (saturation - 100.0) < 1e-9
                && std::abs (gamma - 1.0) < 1e-9 && std::abs (hue) < 1e-9 && ! hasCurves;
        }
    };

    /*  A CURVE AS ITS POINTS SAY: (in, out) pairs 0..1, straight between, flat
        before the first and after the last; no points is the line in = out. */
    inline double curveAt (const std::vector<std::pair<double, double>>& points, double in) noexcept
    {
        if (points.empty())
            return in;

        if (in <= points.front().first)
            return points.front().second;

        for (std::size_t n = 1; n < points.size(); ++n)
            if (in <= points[n].first)
            {
                const auto& a = points[n - 1];
                const auto& b = points[n];
                const auto span = b.first - a.first;
                return span > 0.0 ? a.second + (b.second - a.second) * (in - a.first) / span : b.second;
            }

        return points.back().second;
    }

    /*  `d*` text as a curve: in, out, in, out... sorted by in, clamped to 0..1. */
    inline std::vector<std::pair<double, double>> curveFrom (const std::vector<double>& numbers)
    {
        std::vector<std::pair<double, double>> points;

        for (std::size_t n = 0; n + 1 < numbers.size(); n += 2)
            points.push_back ({ std::clamp (numbers[n], 0.0, 1.0), std::clamp (numbers[n + 1], 0.0, 1.0) });

        std::stable_sort (points.begin(), points.end(),
                          [] (const auto& a, const auto& b) { return a.first < b.first; });
        return points;
    }

    /*  THE FOUR CURVES BAKED into the three tables. */
    inline void bakeCurves (Grade& grade, const std::vector<std::pair<double, double>>& luma,
                            const std::array<std::vector<std::pair<double, double>>, 3>& channels)
    {
        grade.hasCurves = ! luma.empty() || ! channels[0].empty() || ! channels[1].empty() || ! channels[2].empty();

        for (std::size_t channel = 0; channel < 3; ++channel)
            for (int step = 0; step < 256; ++step)
            {
                const auto through = curveAt (channels[channel], curveAt (luma, static_cast<double> (step) / 255.0));
                grade.tables[channel][static_cast<std::size_t> (step)]
                    = static_cast<std::uint8_t> (std::lround (std::clamp (through, 0.0, 1.0) * 255.0));
            }
    }

    /*  ONE COLOUR GRADED, 0..1 in and out, in the order above. */
    inline void applyGrade (const Grade& grade, double& red, double& green, double& blue) noexcept
    {
        double c[3] { red, green, blue };
        const auto inverseGamma = 1.0 / std::max (0.01, grade.gamma);

        for (auto& v : c)
        {
            v = std::pow (std::clamp (v, 0.0, 1.0), inverseGamma);
            v = (v - 0.5) * grade.contrast / 100.0 + 0.5;
        }

        /*  THE GREY AXIS: luma by Rec. 709's weights, the rest is colour. The
            hue turns the colour about the axis (1, 1, 1) - Rodrigues' rotation
            - and the saturation scales it. */
        const auto luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        double colour[3] { c[0] - luma, c[1] - luma, c[2] - luma };

        if (std::abs (grade.hue) > 1e-9)
        {
            const auto turn = grade.hue * 3.14159265358979323846 / 180.0;
            const auto cosine = std::cos (turn), sine = std::sin (turn);
            const auto k = 1.0 / std::sqrt (3.0);
            const double axis[3] { k, k, k };

            const auto along = axis[0] * colour[0] + axis[1] * colour[1] + axis[2] * colour[2];
            const double cross[3] { axis[1] * colour[2] - axis[2] * colour[1],
                                    axis[2] * colour[0] - axis[0] * colour[2],
                                    axis[0] * colour[1] - axis[1] * colour[0] };

            for (int n = 0; n < 3; ++n)
                colour[n] = colour[n] * cosine + cross[n] * sine + axis[n] * along * (1.0 - cosine);
        }

        const auto scale = grade.saturation / 100.0;

        for (int n = 0; n < 3; ++n)
            c[n] = std::clamp (luma + colour[n] * scale, 0.0, 1.0);

        if (grade.hasCurves)
            for (std::size_t n = 0; n < 3; ++n)
                c[n] = static_cast<double> (grade.tables[n][static_cast<std::size_t> (std::lround (c[n] * 255.0))]) / 255.0;

        red = c[0];
        green = c[1];
        blue = c[2];
    }
}
