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
    A TEXT CUE'S WORDS DRAWN (namespace draft 61, AKJ, AKL): by the renderer,
    on a thread of its own, into a picture its layer then shows as a picture is
    shown.

    LAID OUT AS ON A SLIDE whose frame is the canvas: the words wrapped in a box
    `wrap` % of the canvas's width, centred, each line aligned in it; the block
    set at the top, middle or bottom, as far from those edges as the box is from
    the sides - 5 % of the width when nothing wraps. ONLY THE WORDS' OWN BOUNDS
    are drawn, with room for the outline and its blur, at the canvas's pixels
    times `scale` (one to four, so a title scaled up stays sharp), and where
    they lie on the frame is said as fractions of it.

    THE OUTLINE lies beneath the fill, `outline` canvas pixels wide outside the
    letters, blurred by `outlineBlur` (Blur.h) into a glow; the fill, the cue's
    paint, drawn over it.
*/

#include <wfg/engine/video/TextLook.h>

#include <juce_graphics/juce_graphics.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::video
{
    struct TextRaster
    {
        /*  The words, a software ARGB image (premultiplied, rows from the
            top), invalid when there is nothing to show. */
        juce::Image image;

        /*  Where it lies on the canvas's frame, 0..1 from its top-left. */
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    };

    /*  At most this many pixels across or down: a slide drawn four times over
        on a canvas of 8K is cut to it. */
    constexpr int textRasterMost = 8192;

    TextRaster rasteriseText (const TextLook& look, std::uint32_t fill, int canvasWidth, int canvasHeight,
                              double scale);

    /*  OF `families`, THOSE THIS MACHINE HAS NOT GOT (namespace draft 61, AKL),
        each once, in the order given: asked of the machine, never of the show,
        since a main and a backup can differ. */
    std::vector<std::string> fontsNotInstalled (const std::vector<std::string>& families);
}
