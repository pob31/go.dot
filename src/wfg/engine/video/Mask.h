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
    HOW MUCH OF A MASK COVERS ONE POINT (Phase 8a, namespace draft 35, UY and
    VF): a mask cue's shape - a polygon of up to `maxPoints` corners, in 0..1
    of the canvas from its top-left corner - filled even-odd, its edge feathered
    over `feather` pixels straddling it, and turned inside out when inverted.

    Worked out once here, and repeated line for line by the mask's shader.
    Pure, no allocation.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace wfg::video::mask
{
    constexpr int maxPoints = 64;

    struct Shape
    {
        int count = 0;
        float x[maxPoints] {};      ///< 0..1 of the canvas's width, from the left
        float y[maxPoints] {};      ///< 0..1 of its height, from the top
        float feather = 0.0f;       ///< pixels, across the edge
        bool invert = false;
    };

    /*  THE COVER AT (u, v) of the canvas - u from the left, v from the BOTTOM,
        as a texture is addressed - on a canvas of this size: 1 inside, 0
        outside, the edge a ramp `feather` pixels wide. */
    inline double coverAt (const Shape& shape, double u, double v, double canvasWidth, double canvasHeight) noexcept
    {
        if (shape.count < 3)
            return shape.invert ? 1.0 : 0.0;

        const auto px = u * canvasWidth;
        const auto py = (1.0 - v) * canvasHeight;

        bool inside = false;
        auto nearest = 1.0e30;

        for (int n = 0, previous = shape.count - 1; n < shape.count; previous = n++)
        {
            const auto ax = static_cast<double> (shape.x[previous]) * canvasWidth;
            const auto ay = static_cast<double> (shape.y[previous]) * canvasHeight;
            const auto bx = static_cast<double> (shape.x[n]) * canvasWidth;
            const auto by = static_cast<double> (shape.y[n]) * canvasHeight;

            //  EVEN-ODD: a ray to the right crossing this edge.
            if ((by > py) != (ay > py) && px < (ax - bx) * (py - by) / (ay - by) + bx)
                inside = ! inside;

            //  AND HOW FAR THE EDGE IS.
            const auto ex = ax - bx, ey = ay - by;
            const auto length = ex * ex + ey * ey;
            const auto t = length > 0.0 ? std::clamp (((px - bx) * ex + (py - by) * ey) / length, 0.0, 1.0) : 0.0;
            const auto dx = px - (bx + t * ex), dy = py - (by + t * ey);
            nearest = std::min (nearest, std::sqrt (dx * dx + dy * dy));
        }

        double cover = 0.0;

        if (shape.feather > 0.0f)
        {
            const auto signedDistance = inside ? nearest : -nearest;
            cover = std::clamp (0.5 + signedDistance / static_cast<double> (shape.feather), 0.0, 1.0);
        }
        else
        {
            cover = inside ? 1.0 : 0.0;
        }

        return shape.invert ? 1.0 - cover : cover;
    }
}
