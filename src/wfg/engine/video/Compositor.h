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

    A fill is the canvas's own size, placed by the same geometry as a picture
    (§36, VW): scaled down it is a panel. A picture is sampled from what the
    renderer has read. Masks join this file when they draw.
*/

#include <wfg/engine/video/Geometry.h>
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

    /*  One moving value of a layer at `sample`: its points of that property,
        and before the first the cue's own number (`before`). */
    inline double valueOf (const region::LayerReading& layer, Property property, std::int64_t sample,
                           double before) noexcept
    {
        return valueAt (layer.ring (property), sample, before, [] (const Point&) { return true; });
    }

    /*  How solid one layer is at `sample`: nothing once its removal has come,
        else what its opacity's points say, times what the DCAs above its cue
        leave of it (namespace draft 37.5, WE) - all of it until they say. */
    inline double opacityOf (const region::LayerReading& layer, std::int64_t sample) noexcept
    {
        if (sample >= layer.removeAt)
            return 0.0;

        return std::clamp (valueOf (layer, Property::opacity, sample, 0.0), 0.0, 1.0)
             * std::clamp (valueOf (layer, Property::dca, sample, 1.0), 0.0, 1.0);
    }

    /*  Where the layer lies at `sample`, on a canvas of this size, for a
        picture of that size - a fill's is the canvas's (Geometry.h). */
    inline Placement placementOf (const region::LayerReading& layer, std::int64_t sample,
                                  double canvasWidth, double canvasHeight,
                                  double pictureWidth, double pictureHeight) noexcept
    {
        Placement place;
        place.canvasWidth = canvasWidth;
        place.canvasHeight = canvasHeight;
        place.pictureWidth = pictureWidth;
        place.pictureHeight = pictureHeight;
        place.fit = static_cast<int> (layer.fit);
        place.scale = valueOf (layer, Property::scale, sample, layer.scale);
        place.offsetX = valueOf (layer, Property::offsetX, sample, layer.offsetX);
        place.offsetY = valueOf (layer, Property::offsetY, sample, layer.offsetY);
        place.rotation = valueOf (layer, Property::rotation, sample, layer.rotation);
        place.flipH = layer.flipH;
        place.flipV = layer.flipV;
        return place;
    }

    /*  WHAT A PICTURE IS, for the CPU: its size, and its colour at one point of
        it - (0, 0) the bottom-left, (1, 1) the top-right - as 0..255 straight
        colour and 0..1 alpha. The renderer answers from the pictures it has
        read; a test from one it made up. A picture it has not read is not
        there yet, and shows nothing. */
    struct PictureSampler
    {
        virtual ~PictureSampler() = default;
        virtual bool sizeOf (const std::string& path, int& width, int& height) const = 0;
        virtual bool colourAt (const std::string& path, double u, double v,
                               double& red, double& green, double& blue, double& alpha) const = 0;

        /*  AND A MOVIE'S (§37): its size, and the colour of the frame showing
            at `seconds` of the file - false while that frame has not been
            read. None by default. */
        virtual bool movieSizeOf (const std::string&, int&, int&) const  { return false; }

        virtual bool movieColourAt (const std::string&, double, double, double,
                                    double&, double&, double&, double&) const  { return false; }
    };

    /*  THE CANVAS'S COLOUR AT ONE POINT, as 0xRRGGBB: (x, y) on its pixels
        from its middle, y up. Every layer of the stack laid over the last,
        normal blending in display space (VD), from black. */
    inline std::uint32_t colourAt (const std::vector<const region::LayerReading*>& stack, std::int64_t sample,
                                   double canvasWidth, double canvasHeight, double x, double y,
                                   const PictureSampler* pictures) noexcept
    {
        double red = 0.0, green = 0.0, blue = 0.0;

        for (const auto* layer : stack)
        {
            const auto opacity = opacityOf (*layer, sample);

            if (! (opacity > 0.0))
                continue;

            double r = 0.0, g = 0.0, b = 0.0, alpha = 1.0;
            double u = 0.0, v = 0.0;

            if (layer->source == region::Source::fill || layer->source == region::Source::mask)
            {
                const auto place = placementOf (*layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight);

                /*  A MASK is a fill of the canvas's size, covering only where
                    its shape does - the shape on the canvas the geometry moved. */
                const auto onCanvas = place.toTexture (x, y, u, v);

                if (layer->source == region::Source::mask)
                {
                    if (! onCanvas)
                        u = v = -1.0;

                    alpha = onCanvas ? mask::coverAt (layer->shape, u, v, canvasWidth, canvasHeight)
                                     : (layer->shape.invert ? 1.0 : 0.0);
                }
                else if (! onCanvas)
                {
                    continue;
                }

                r = static_cast<double> ((layer->paint >> 16) & 0xffu);
                g = static_cast<double> ((layer->paint >> 8) & 0xffu);
                b = static_cast<double> (layer->paint & 0xffu);
            }
            else if (layer->source == region::Source::picture)
            {
                int width = 0, height = 0;

                if (pictures == nullptr || ! pictures->sizeOf (layer->file, width, height))
                    continue;

                const auto place = placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                static_cast<double> (width), static_cast<double> (height));

                if (! place.toTexture (x, y, u, v) || ! pictures->colourAt (layer->file, u, v, r, g, b, alpha))
                    continue;
            }
            else if (layer->source == region::Source::movie)
            {
                /*  A MOVIE: the frame its playhead says, at this sample (VZ). */
                int width = 0, height = 0;

                if (pictures == nullptr || ! pictures->movieSizeOf (layer->file, width, height))
                    continue;

                const auto place = placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                static_cast<double> (width), static_cast<double> (height));
                const auto seconds = valueOf (*layer, Property::time, sample, 0.0);

                if (! place.toTexture (x, y, u, v)
                      || ! pictures->movieColourAt (layer->file, seconds, u, v, r, g, b, alpha))
                    continue;
            }
            else
            {
                continue;
            }

            /*  THE GRADE, on a picture's and a movie's colour (VW, VU). */
            if ((layer->source == region::Source::picture || layer->source == region::Source::movie)
                  && ! layer->grade.isIdentity())
            {
                double gr = r / 255.0, gg = g / 255.0, gb = b / 255.0;
                applyGrade (layer->grade, gr, gg, gb);
                r = gr * 255.0;
                g = gg * 255.0;
                b = gb * 255.0;
            }

            const auto a = opacity * std::clamp (alpha, 0.0, 1.0);

            /*  THE BLEND (VD), on display-space numbers, premultiplied - the
                GPU's blend equations exactly: normal covers by `a`; add adds
                the light; screen is 1 - (1 - under)(1 - colour); multiply
                darkens as a gel does. */
            const auto lay = [a, blend = layer->blend] (double under, double over)
            {
                const auto premultiplied = over * a;

                switch (blend)
                {
                    case region::Blend::add:      return std::min (255.0, under + premultiplied);
                    case region::Blend::screen:   return under * (1.0 - premultiplied / 255.0) + premultiplied;
                    case region::Blend::multiply: return under * (1.0 - a) + under * premultiplied / 255.0;
                    case region::Blend::normal:   break;
                }

                return under * (1.0 - a) + premultiplied;
            };

            red = lay (red, r);
            green = lay (green, g);
            blue = lay (blue, b);
        }

        const auto channel = [] (double value)
        {
            return static_cast<std::uint32_t> (std::clamp (std::lround (value), 0L, 255L));
        };

        return (channel (red) << 16) | (channel (green) << 8) | channel (blue);
    }

    /*  The middle of a 1920 by 1080 canvas with no picture read: what a fill
        alone shows there. */
    inline std::uint32_t fillsAt (const std::vector<const region::LayerReading*>& stack, std::int64_t sample) noexcept
    {
        return colourAt (stack, sample, 1920.0, 1080.0, 0.0, 0.0, nullptr);
    }
}
