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
    WHERE A PICTURE LIES ON ITS CANVAS (Phase 8a, namespace draft 36, VO, VQ,
    VT). One answer, read by the GPU - the four corners it draws - and by the
    CPU reference - what lies under one point of the canvas - so the two cannot
    disagree about where a picture is.

    On the canvas's own pixels, with the origin at its middle and y UP (VT:
    up is up). In order:

      - FITTED (VO): fit, the whole picture with its shape kept; fill, the
        canvas covered with its shape kept; stretch, the canvas covered;
      - SCALED by `scale` % of that fitted size (VQ);
      - FLIPPED about its own centre - the texture, not the place;
      - TURNED clockwise by `rotation` degrees about its own centre (VT);
      - MOVED by `offsetX` % of the canvas's width and `offsetY` % of its
        height (VQ).

    Pure, no allocation: a frame computes it for every layer.
*/

#include <cmath>

namespace wfg::video
{
    struct Placement
    {
        double canvasWidth = 1920.0;
        double canvasHeight = 1080.0;

        /** The picture's own size in pixels; a fill's is the canvas's. */
        double pictureWidth = 1920.0;
        double pictureHeight = 1080.0;

        int fit = 0;            ///< region::Fit: 0 fit, 1 fill, 2 stretch
        double scale = 100.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double rotation = 0.0;
        bool flipH = false;
        bool flipV = false;

        /** The picture's half-size on the canvas, fitted and scaled. */
        void halfSize (double& halfWidth, double& halfHeight) const noexcept
        {
            double width = canvasWidth, height = canvasHeight;

            if (fit != 2 && pictureWidth > 0.0 && pictureHeight > 0.0)
            {
                const auto across = canvasWidth / pictureWidth;
                const auto down = canvasHeight / pictureHeight;
                const auto factor = fit == 1 ? std::fmax (across, down) : std::fmin (across, down);

                width = pictureWidth * factor;
                height = pictureHeight * factor;
            }

            halfWidth = 0.5 * width * scale / 100.0;
            halfHeight = 0.5 * height * scale / 100.0;
        }

        /*  A point of the picture - (-1, -1) its bottom-left corner, (1, 1) its
            top-right, as drawn before any flip - to the canvas. */
        void toCanvas (double u, double v, double& x, double& y) const noexcept
        {
            double halfWidth = 0.0, halfHeight = 0.0;
            halfSize (halfWidth, halfHeight);

            const auto localX = u * halfWidth;
            const auto localY = v * halfHeight;
            const auto turn = rotation * 3.14159265358979323846 / 180.0;
            const auto c = std::cos (turn), s = std::sin (turn);

            /*  CLOCKWISE with y up: (x, y) to (x cos + y sin, -x sin + y cos). */
            x = localX * c + localY * s + offsetX / 100.0 * canvasWidth;
            y = -localX * s + localY * c + offsetY / 100.0 * canvasHeight;
        }

        /*  A point of the canvas back to the picture's texture: (0, 0) its
            bottom-left texel, (1, 1) its top-right, the flips applied. False
            when the point is not on the picture. */
        bool toTexture (double x, double y, double& textureU, double& textureV) const noexcept
        {
            double halfWidth = 0.0, halfHeight = 0.0;
            halfSize (halfWidth, halfHeight);

            if (! (halfWidth > 0.0) || ! (halfHeight > 0.0))
                return false;

            const auto dx = x - offsetX / 100.0 * canvasWidth;
            const auto dy = y - offsetY / 100.0 * canvasHeight;
            const auto turn = rotation * 3.14159265358979323846 / 180.0;
            const auto c = std::cos (turn), s = std::sin (turn);

            /*  The turn undone: the inverse of a rotation is its transpose. */
            const auto localX = dx * c - dy * s;
            const auto localY = dx * s + dy * c;

            const auto u = localX / halfWidth;
            const auto v = localY / halfHeight;

            if (u < -1.0 || u > 1.0 || v < -1.0 || v > 1.0)
                return false;

            textureU = (flipH ? -u : u) * 0.5 + 0.5;
            textureV = (flipV ? -v : v) * 0.5 + 0.5;
            return true;
        }
    };
}
