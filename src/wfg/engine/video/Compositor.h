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
    renderer has read; a mask is its outline's cover. A blur cue needs the
    points round each one, so a canvas holding one is drawn whole
    (`compositeInto`, namespace draft 61) - and a point read alone passes it
    over.
*/

#include <wfg/engine/video/Blur.h>
#include <wfg/engine/video/Geometry.h>
#include <wfg/engine/video/Key.h>
#include <wfg/engine/video/TextLook.h>
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

        /*  AND A TEXT CUE'S WORDS (namespace draft 61, AKJ): the colour of the
            layer's drawing at (u, v) of the canvas's frame, 0..1 from its
            bottom-left, straight 0..255 and its alpha - false where no word is,
            or none is drawn yet. None by default. */
        virtual bool textAt (const region::LayerReading&, double, double,
                             double&, double&, double&, double&) const  { return false; }
    };

    /*  A CANVAS'S COLOUR TAKEN DOWN BY ITS LEVEL (namespace draft §38, WT):
        each channel times `factor`, towards black - what the renderer's one
        black pass over the whole composite does on the GPU. */
    inline std::uint32_t scaledColour (std::uint32_t rgb, double factor) noexcept
    {
        const auto f = std::clamp (factor, 0.0, 1.0);
        const auto channel = [rgb, f] (int shift)
        {
            const auto value = static_cast<double> ((rgb >> shift) & 0xffu) * f;
            return static_cast<std::uint32_t> (std::lround (value)) << shift;
        };

        return channel (16) | channel (8) | channel (0);
    }

    /*  ONE LAYER AT ONE POINT, before it is laid: its colour, straight 0..255,
        and how much of it covers the point - its alpha times its opacity, the
        DCAs' share in it. False where it shows nothing there: no opacity left,
        off its frame, a picture not read, a blur cue (which shows nothing of
        its own, namespace draft 61) or a source the CPU does not draw. */
    inline bool layerAt (const region::LayerReading& layer, std::int64_t sample, double canvasWidth, double canvasHeight,
                         double x, double y, const PictureSampler* pictures,
                         double& r, double& g, double& b, double& a) noexcept
    {
        const auto opacity = opacityOf (layer, sample);

        if (! (opacity > 0.0))
            return false;

        double alpha = 1.0;
        double u = 0.0, v = 0.0;
        r = g = b = 0.0;

        if (layer.source == region::Source::fill || layer.source == region::Source::mask)
        {
            const auto place = placementOf (layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight);

            /*  A MASK is a fill of the canvas's size, covering only where
                its shape does - the shape on the canvas the geometry moved. */
            const auto onCanvas = place.toTexture (x, y, u, v);

            if (layer.source == region::Source::mask)
            {
                if (! onCanvas)
                    u = v = -1.0;

                alpha = onCanvas ? mask::coverAt (layer.shape, u, v, canvasWidth, canvasHeight)
                                 : (layer.shape.invert ? 1.0 : 0.0);
            }
            else if (! onCanvas)
            {
                return false;
            }

            r = static_cast<double> ((layer.paint >> 16) & 0xffu);
            g = static_cast<double> ((layer.paint >> 8) & 0xffu);
            b = static_cast<double> (layer.paint & 0xffu);
        }
        else if (layer.source == region::Source::picture)
        {
            int width = 0, height = 0;

            if (pictures == nullptr || ! pictures->sizeOf (layer.file, width, height))
                return false;

            const auto place = placementOf (layer, sample, canvasWidth, canvasHeight,
                                            static_cast<double> (width), static_cast<double> (height));

            if (! place.toTexture (x, y, u, v) || ! pictures->colourAt (layer.file, u, v, r, g, b, alpha))
                return false;
        }
        else if (layer.source == region::Source::movie)
        {
            /*  A MOVIE: the frame its playhead says, at this sample (VZ). */
            int width = 0, height = 0;

            if (pictures == nullptr || ! pictures->movieSizeOf (layer.file, width, height))
                return false;

            const auto place = placementOf (layer, sample, canvasWidth, canvasHeight,
                                            static_cast<double> (width), static_cast<double> (height));
            const auto seconds = valueOf (layer, Property::time, sample, 0.0);

            if (! place.toTexture (x, y, u, v)
                  || ! pictures->movieColourAt (layer.file, seconds, u, v, r, g, b, alpha))
                return false;
        }
        else if (layer.source == region::Source::text)
        {
            /*  A TEXT CUE (namespace draft 61, AKJ): the canvas's frame,
                moved as a fill's, and on it the words where they were drawn. */
            const auto place = placementOf (layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight);

            if (pictures == nullptr || ! place.toTexture (x, y, u, v)
                  || ! pictures->textAt (layer, u, v, r, g, b, alpha))
                return false;
        }
        else
        {
            return false;
        }

        /*  THE ALPHA SWITCH, THE KEY AND THE GRADE, on a picture's and a
            movie's colour, in that order (namespace draft 61, AKG; VW, VU).
            A picture whose alpha is not read is drawn as the GPU reads it,
            premultiplied: black where it was clear (AKE). A matte shows its
            paint, ungraded. */
        if (layer.source == region::Source::picture || layer.source == region::Source::movie)
        {
            double gr = r / 255.0, gg = g / 255.0, gb = b / 255.0;

            if (! layer.useAlpha)
            {
                if (layer.source == region::Source::picture)
                {
                    gr *= alpha;
                    gg *= alpha;
                    gb *= alpha;
                }

                alpha = 1.0;
            }

            applyKey (layer.key, layer.paint, gr, gg, gb, alpha);

            if (layer.key.mode != KeyMode::matte && ! layer.grade.isIdentity())
                applyGrade (layer.grade, gr, gg, gb);

            r = gr * 255.0;
            g = gg * 255.0;
            b = gb * 255.0;
        }

        a = opacity * std::clamp (alpha, 0.0, 1.0);
        return true;
    }

    /*  THE BLEND (VD), on display-space numbers 0..255, premultiplied - the
        GPU's blend equations exactly: normal covers by `a`; add adds the
        light; screen is 1 - (1 - under)(1 - colour); multiply darkens as a gel
        does. */
    inline double laidOver (region::Blend blend, double a, double under, double over) noexcept
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
    }

    /*  A channel 0..255 as a byte, rounded. */
    inline std::uint32_t byteOf (double value) noexcept
    {
        return static_cast<std::uint32_t> (std::clamp (std::lround (value), 0L, 255L));
    }

    /*  THE CANVAS'S COLOUR AT ONE POINT, as 0xRRGGBB: (x, y) on its pixels
        from its middle, y up. Every layer of the stack laid over the last,
        normal blending in display space (VD), from black. A blur cue needs
        the points around this one, which a point does not have: it is passed
        over here, and drawn by `compositeInto`. */
    inline std::uint32_t colourAt (const std::vector<const region::LayerReading*>& stack, std::int64_t sample,
                                   double canvasWidth, double canvasHeight, double x, double y,
                                   const PictureSampler* pictures) noexcept
    {
        double red = 0.0, green = 0.0, blue = 0.0;

        for (const auto* layer : stack)
        {
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;

            if (! layerAt (*layer, sample, canvasWidth, canvasHeight, x, y, pictures, r, g, b, a))
                continue;

            red = laidOver (layer->blend, a, red, r);
            green = laidOver (layer->blend, a, green, g);
            blue = laidOver (layer->blend, a, blue, b);
        }

        return (byteOf (red) << 16) | (byteOf (green) << 8) | byteOf (blue);
    }

    /*  WHERE A BLUR CUE BLURS, 0..1 at the point (x, y) (namespace draft 61,
        AKN): its outline as a mask's is, moved by its geometry, turned inside
        out by `invert`, and covering outside its moved frame when it is
        (AKP); with no outline, the whole canvas, `invert` meaning nothing. */
    inline double blurCoverAt (const region::LayerReading& layer, std::int64_t sample, double canvasWidth,
                               double canvasHeight, double x, double y) noexcept
    {
        if (layer.shape.count < 3)
            return 1.0;

        const auto place = placementOf (layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight);
        double u = 0.0, v = 0.0;

        if (! place.toTexture (x, y, u, v))
            return layer.shape.invert ? 1.0 : 0.0;

        return mask::coverAt (layer.shape, u, v, canvasWidth, canvasHeight);
    }

    /*  THE CANVAS DRAWN WHOLE, `width` by `height` points of three channels
        0..255 row by row from the top - each point's middle at the place
        `colourAt` reads it, so a stack with no blur in it is what `colourAt`
        says, point for point. A BLUR CUE (namespace draft 61, AKN, AKO)
        blurs what lies beneath it (Blur.h, its radius scaled to the points
        drawn, the spread a third of it) and lays the blurred canvas back
        through its cover times its opacity; the layers above it are laid
        on that. */
    inline void compositeInto (const std::vector<const region::LayerReading*>& stack, std::int64_t sample,
                               double canvasWidth, double canvasHeight, int width, int height,
                               const PictureSampler* pictures, std::vector<double>& rgb)
    {
        rgb.assign (static_cast<std::size_t> (std::max (0, width)) * static_cast<std::size_t> (std::max (0, height)) * 3, 0.0);

        const auto xOf = [width, canvasWidth] (int column) { return ((column + 0.5) / width - 0.5) * canvasWidth; };
        const auto yOf = [height, canvasHeight] (int row) { return (0.5 - (row + 0.5) / height) * canvasHeight; };

        for (const auto* layer : stack)
        {
            if (layer->source == region::Source::blur)
            {
                const auto a = opacityOf (*layer, sample);
                const auto radius = valueOf (*layer, Property::blur, sample, layer->blur);

                if (! (a > 0.0) || ! (radius > 0.0))
                    continue;

                auto blurred = rgb;
                blurPicture (blurred, width, height, radius / 3.0 * width / canvasWidth);

                for (int row = 0; row < height; ++row)
                    for (int column = 0; column < width; ++column)
                    {
                        const auto m = a * blurCoverAt (*layer, sample, canvasWidth, canvasHeight, xOf (column), yOf (row));

                        for (int c = 0; c < 3; ++c)
                        {
                            const auto at = pyramid::indexOf (column, row, width, c);
                            rgb[at] = rgb[at] * (1.0 - m) + blurred[at] * m;
                        }
                    }

                continue;
            }

            for (int row = 0; row < height; ++row)
                for (int column = 0; column < width; ++column)
                {
                    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;

                    if (! layerAt (*layer, sample, canvasWidth, canvasHeight, xOf (column), yOf (row), pictures, r, g, b, a))
                        continue;

                    const auto at = pyramid::indexOf (column, row, width, 0);
                    rgb[at] = laidOver (layer->blend, a, rgb[at], r);
                    rgb[at + 1] = laidOver (layer->blend, a, rgb[at + 1], g);
                    rgb[at + 2] = laidOver (layer->blend, a, rgb[at + 2], b);
                }
        }
    }

    /*  Whether a stack holds a blur cue, which only `compositeInto` draws. */
    inline bool blurs (const std::vector<const region::LayerReading*>& stack) noexcept
    {
        return std::any_of (stack.begin(), stack.end(),
                            [] (const region::LayerReading* layer) { return layer->source == region::Source::blur; });
    }

    /*  One drawn point as 0xRRGGBB, taken down by the canvas's level. */
    inline std::uint32_t colourIn (const std::vector<double>& rgb, int column, int row, int width, double level) noexcept
    {
        const auto at = pyramid::indexOf (column, row, width, 0);
        const auto f = std::clamp (level, 0.0, 1.0);
        return (byteOf (rgb[at] * f) << 16) | (byteOf (rgb[at + 1] * f) << 8) | byteOf (rgb[at + 2] * f);
    }

    /*  The middle of a 1920 by 1080 canvas with no picture read: what a fill
        alone shows there. */
    inline std::uint32_t fillsAt (const std::vector<const region::LayerReading*>& stack, std::int64_t sample) noexcept
    {
        return colourAt (stack, sample, 1920.0, 1080.0, 0.0, 0.0, nullptr);
    }

    /*  WHAT A STACK SHOWS, AS ONE COLOUR (namespace draft §38, WR): the average
        of `colourAt` over an eight by eight grid of the canvas, each point the
        middle of its cell, as 0xRRGGBB. Sixty-four reads of pictures the
        renderer already holds - no read back from the graphics card. A stack of
        one layer is that layer over black, its opacity and DCA in it. */
    inline std::uint32_t tintOf (const std::vector<const region::LayerReading*>& stack, std::int64_t sample,
                                 double canvasWidth, double canvasHeight, const PictureSampler* pictures) noexcept
    {
        constexpr int grid = 8;
        double red = 0.0, green = 0.0, blue = 0.0;

        for (int column = 0; column < grid; ++column)
        {
            for (int row = 0; row < grid; ++row)
            {
                const auto x = ((column + 0.5) / grid - 0.5) * canvasWidth;
                const auto y = ((row + 0.5) / grid - 0.5) * canvasHeight;
                const auto rgb = colourAt (stack, sample, canvasWidth, canvasHeight, x, y, pictures);

                red += static_cast<double> ((rgb >> 16) & 0xffu);
                green += static_cast<double> ((rgb >> 8) & 0xffu);
                blue += static_cast<double> (rgb & 0xffu);
            }
        }

        constexpr auto cells = static_cast<double> (grid * grid);
        const auto channel = [] (double total, int shift)
        {
            return static_cast<std::uint32_t> (std::clamp (std::lround (total / cells), 0L, 255L)) << shift;
        };

        return channel (red, 16) | channel (green, 8) | channel (blue, 0);
    }
}
