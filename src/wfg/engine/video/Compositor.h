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
    WHAT A CANVAS IS AT ONE SAMPLE, worked out on the CPU (Phase 8a, namespace
    draft 35.7). The reference the GPU's drawing is held to, and what a
    renderer with no window publishes for a test to read - so the rules of the
    picture are written once, here, in arithmetic anybody can check:

      - THE STACK: the layers on this canvas, lowest `layer` first, and of two
        on one layer the one brought up first (VI) - so the later is on top;
      - A LAYER IS THERE from its first point to its removal, at the opacity
        its points give for that sample (VideoRamp.h);
      - NORMAL BLENDING IN DISPLAY SPACE (VD): each layer laid over what is
        under it as `under * (1 - a) + colour * a`, on the 0..255 numbers the
        colour is written in, from a canvas that starts black.

    Fills only in V.1: a fill covers the whole canvas, so the middle of it is
    every pixel of it. Masks and pictures join this file as they draw.
*/

#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/video/VideoRegion.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::video
{
    /*  The layers of one canvas in the order they are laid down, bottom
        first, as pointers into `layers`. */
    inline std::vector<const region::LayerReading*> stackOf (const std::vector<region::LayerReading>& layers,
                                                             const std::string& canvas)
    {
        std::vector<const region::LayerReading*> stack;

        for (const auto& layer : layers)
            if (layer.canvas == canvas)
                stack.push_back (&layer);

        std::stable_sort (stack.begin(), stack.end(),
                          [] (const region::LayerReading* a, const region::LayerReading* b)
                          {
                              if (a->layer != b->layer)
                                  return a->layer < b->layer;

                              return a->order < b->order;
                          });

        return stack;
    }

    /*  How solid one layer is at `sample`: nothing once its removal has come,
        else what its points say. */
    inline double opacityOf (const region::LayerReading& layer, std::int64_t sample) noexcept
    {
        if (sample >= layer.removeAt)
            return 0.0;

        struct View
        {
            const Point* first;
            const Point* last;
            const Point* begin() const noexcept { return first; }
            const Point* end() const noexcept   { return last; }
        };

        return std::clamp (opacityAt (View { layer.points, layer.points + layer.pointCount }, sample), 0.0, 1.0);
    }

    /*  THE CANVAS'S COLOUR where every fill covers it, as 0xRRGGBB: what the
        middle of it shows. */
    inline std::uint32_t fillsAt (const std::vector<const region::LayerReading*>& stack, std::int64_t sample) noexcept
    {
        double red = 0.0, green = 0.0, blue = 0.0;

        for (const auto* layer : stack)
        {
            if (layer->source != region::Source::fill)
                continue;

            const auto a = opacityOf (*layer, sample);

            if (! (a > 0.0))
                continue;

            const auto r = static_cast<double> ((layer->paint >> 16) & 0xffu);
            const auto g = static_cast<double> ((layer->paint >> 8) & 0xffu);
            const auto b = static_cast<double> (layer->paint & 0xffu);

            red = red * (1.0 - a) + r * a;
            green = green * (1.0 - a) + g * a;
            blue = blue * (1.0 - a) + b * a;
        }

        const auto channel = [] (double value)
        {
            return static_cast<std::uint32_t> (std::clamp (std::lround (value), 0L, 255L));
        };

        return (channel (red) << 16) | (channel (green) << 8) | channel (blue);
    }
}
