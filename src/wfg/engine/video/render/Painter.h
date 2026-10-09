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
    WHAT THE RENDERER DRAWS, ON THE ONE DEVICE (namespace draft §44.4).

    A frame: the configuration and the layers read once (`beginFrame`); each
    canvas an output asks for composited ONCE at that frame's sample into a
    16-bit float picture of its own (`canvas`) - however many projectors,
    senders and inserts read it; then each output's zones, warps and
    calibration from those pictures into its window or its picture
    (`drawOutput`); `sg_commit`; and what no frame used let go (`endFrame`).

    The arithmetic is the OpenGL renderer's, line for line, and Compositor.h's,
    which a test holds it to: the stack bottom first, each layer placed by
    Geometry.h and blended in display space, premultiplied (VD); the canvas's
    level as one black pass over the whole (§38, WT); the mesh and the CDL of
    Mapping.h, dithered down to eight bits.

    Used on the render thread only, with gpu::open done.
*/

#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/Mapping.h>
#include <wfg/engine/video/VideoRegion.h>

#include <juce_graphics/juce_graphics.h>
#include <sokol/sokol_gfx.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video::render
{
    /*  A MOVIE'S FRAME AS THE STORE READ IT (namespace draft 37): HAP's DXT
        blocks, laid top row first, or a preview's straight RGBA (§37.5, WF).
        Handed out whole and never changed, so it is read without a lock. */
    struct MovieFrame
    {
        hap::Texture texture = hap::Texture::none;
        std::vector<std::uint8_t> blocks;
        int width = 0;
        int height = 0;
        int index = -1;
        std::vector<std::uint8_t> rgba;

        bool drawable() const noexcept  { return texture != hap::Texture::none || ! rgba.empty(); }
    };

    /*  WHERE THE PICTURES COME FROM: the renderer's stores, or a test's own. */
    struct Sources
    {
        virtual ~Sources() = default;

        /*  A picture, read: a software ARGB image (premultiplied, rows from the
            top), invalid while it is being read; `version` moves when it is read
            again. */
        virtual juce::Image picture (const std::string& path, std::uint64_t& version) const = 0;

        /*  The frame of the movie at `path` showing at `seconds` of its file,
            or the nearest read before it; null while none is. */
        virtual std::shared_ptr<const MovieFrame> movieFrame (const std::string& path, double seconds) const = 0;
    };

    /*  WHERE AN OUTPUT IS DRAWN: an offscreen picture's colour attachment, or a
        window's swap chain - and, inside it, the display's own pixels, from its
        top-left (the window reaches a pixel past one edge, §39.13). */
    struct Target
    {
        sg_view colour {};
        sg_swapchain swapchain {};
        sg_pixel_format format = SG_PIXELFORMAT_NONE;
        int width = 0;
        int height = 0;
        int viewX = 0;
        int viewY = 0;
        int viewWidth = 0;
        int viewHeight = 0;
    };

    class Painter
    {
    public:
        explicit Painter (const Sources& sourcesToRead);
        ~Painter();

        Painter (const Painter&) = delete;
        Painter& operator= (const Painter&) = delete;

        /*  The shaders, samplers and the mesh's indices; false, and why, when
            the device will not compile them. */
        bool make (std::string& why);

        /*  WHAT THE VIDEO INPUTS BROUGHT IN (namespace draft §44, YC): each
            input's newest picture and its size, by identifier - what a
            capture draws. Set before the frame's draws, kept until set again;
            an invalid view takes one away. */
        void setInputPicture (const std::string& inputId, sg_view picture, int width, int height);

        /*  WHAT CAME BACK THROUGH EACH INSERT (§44, YE): the other program's
            picture, by insert, drawn in the place of the cue that holds it. An
            invalid view takes it away, and that cue shows black. */
        void setInsertReturn (const std::string& insertId, sg_view picture, int width, int height);

        /*  THE PICTURE AN INSERT SENDS this frame (YF): its cue's alone, after
            its source and grade, at its own size, before its geometry, opacity
            and blend - of two cues through one insert, the later's (YG). False
            when no cue goes through it now. After beginFrame, outside a pass. */
        bool drawInsertPicture (const std::string& insertId, std::int64_t sample, sg_pixel_format format);
        sg_image insertImage (const std::string& insertId) const;

        /*  AN INSERT NO CUE GOES THROUGH, sent black at the size it last sent
            (or a canvas's) so the other program finds it (§47, AAK). */
        bool drawInsertBlack (const std::string& insertId, sg_pixel_format format);

        /** How many cues go through `insertId` this frame: more than one is the YG warning. */
        int insertUsers (const std::string& insertId) const;

        /*  The frame's reading: the configuration, the layers, and each
            canvas's level (1 for one nobody moved). */
        void beginFrame (const region::ConfigReading& config, std::vector<region::LayerReading> layers,
                         std::function<double (const std::string&)> levelOf);

        /*  Canvas `id` composited at `sample` - once, however often asked for
            the same sample - and its picture to read; invalid for a canvas the
            configuration does not hold. Outside any pass. */
        sg_view canvas (const std::string& id, std::int64_t sample);

        /*  Output `output`, whole, at `sample`: its canvas and its zones through
            their warps, calibrated, dithered, into `into` - and its test
            pattern over it when on. Outside any pass. */
        void drawOutput (const region::OutputReading& output, std::int64_t sample, const Target& into);

        /*  After sg_commit: the pictures, frames and tables no draw used this
            frame let go. */
        void endFrame();

        /** Canvas `id`'s picture, as last composited - for a test to read back. */
        sg_image canvasImage (const std::string& id) const;

        /*  A PICTURE OF ONE'S OWN to draw an output into: made, or made again at
            a new size, under `key`. For a test, and for a sender. */
        Target offscreenTarget (const std::string& key, int width, int height, sg_pixel_format format);
        sg_image offscreenImage (const std::string& key) const;

        /** That picture let go, when its sender goes. */
        void releaseOffscreen (const std::string& key);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
