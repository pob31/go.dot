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
    AN OUTPUT SENT TO OTHER PROGRAMS (namespace draft §44, YA). Its canvas is
    drawn as a projector's is - its zones, its warp, its calibration - into a
    picture of its own on the device, and that picture handed on: SPOUT on
    Windows, the Direct3D 11 texture shared with any program on the machine;
    SYPHON on macOS, the Metal texture; NDI over the network, its pixels read
    back and handed to the runtime the user installed (Ndi.h).

    Render thread, the device open, after sg_commit: what was drawn this frame
    is on the GPU's queue, and a sender copies it from there. Never JUCE: Spout's
    headers bring windowsx.h's macros, and are kept to Sender_spout.cpp.
*/

#include <wfg/engine/video/VideoRegion.h>

#include <sokol/sokol_gfx.h>

#include <memory>
#include <string>

namespace wfg::video::render
{
    class Sender
    {
    public:
        virtual ~Sender() = default;

        /*  `picture`, drawn this frame in senderFormat(), sent: false when it
            could not be. */
        virtual bool send (sg_image picture, int width, int height) = 0;
    };

    /*  A sender of `kind` under `name`, at `frameRate`; null and why where
        this system has not got that kind - Spout is Windows's, Syphon the
        Mac's - or the NDI runtime is not installed. */
    std::unique_ptr<Sender> makeSender (region::OutputKind kind, const std::string& name, double frameRate, std::string& why);

    /*  The format a sender's picture is drawn in: what Spout and Syphon share
        as they come - BGRA - and RGBA where the device has no BGRA (OpenGL). */
    sg_pixel_format senderFormat();

    /*  Each system's own, behind makeSender. */
    std::unique_ptr<Sender> makeSpoutSender (const std::string& name, std::string& why);
    std::unique_ptr<Sender> makeSyphonSender (const std::string& name, std::string& why);
    std::unique_ptr<Sender> makeNdiSender (const std::string& name, double frameRate, std::string& why);
}
