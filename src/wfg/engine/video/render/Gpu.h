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
    THE RENDERER'S ONE GRAPHICS DEVICE (namespace draft §44.4, XZ): each
    system's own - Direct3D 11 on Windows, Metal on macOS, OpenGL through EGL
    on Linux - made here and handed to sokol_gfx, which every drawing call then
    goes through. One per process: sokol_gfx is one, and the renderer draws
    every canvas once on it for every projector, sender and insert.

    Opened and used on ONE thread - the render thread, or a test's - and never
    another: sokol_gfx and the Direct3D immediate context are not shared.

    `software` asks for the system's software rasteriser - WARP on Windows,
    llvmpipe on Linux - so a test draws the same pixels on a machine with no
    graphics card, as CI's are. macOS has none to ask for; there a test runs
    where the machine has a Metal device and says so where it has not.
*/

#include <sokol/sokol_gfx.h>

#include <string>
#include <vector>

namespace wfg::video::gpu
{
    struct OpenOptions
    {
        bool software = false;

        /*  A POINT ON THE FIRST PROJECTOR'S DISPLAY, in the system's pixels:
            Windows draws on the graphics card driving it - on a laptop with
            two, the one the projector is wired to - and with none, on the
            fastest card it has. */
        bool hasPoint = false;
        int pointX = 0;
        int pointY = 0;

        /*  Linux: the X display the projectors' windows are on (Projector.h's
            nativeDisplay), so the one context draws into them too; null draws
            in memory only. */
        void* nativeDisplay = nullptr;
    };

    /** The device made and sokol_gfx set up on it; false and why not. */
    bool open (const OpenOptions& options, std::string& why);

    /** sokol_gfx shut down and the device let go. */
    void close();

    bool isOpen() noexcept;

    /** "Direct3D 11 on <adapter>", for the renderer's readout. */
    std::string describe();

    /** The last error or warning sokol_gfx gave, empty when none. */
    std::string lastMessage();

    /*  WHERE A PICTURE THE GPU DREW HAS ITS FIRST ROW: at the top (Direct3D,
        Metal) or the bottom (OpenGL). A shader reading such a picture back
        flips it where it is the bottom. */
    bool originTopLeft();

    /*  A PICTURE THE GPU DREW, READ BACK: `image` (RGBA8, BGRA8, RGBA16 or RGBA16F,
        a colour attachment) as four floats a pixel, 0..1, rows from the top
        whatever the backend - for a test, and later for a sender that needs
        the pixels in memory. Waits for the GPU. Call it outside a pass, after
        sg_commit. */
    bool readBack (sg_image image, std::vector<float>& rgba, int& width, int& height);
}
