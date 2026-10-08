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
    A PROJECTOR'S WINDOW, AS THE DEVICE DRAWS INTO IT (namespace draft §44.4).

    Two halves on two threads. The MESSAGE THREAD makes the window - a JUCE
    window covering its display, placed as §39.13 places it - and in it a
    NATIVE VIEW of the system's own, filling it, which JUCE never paints: a
    child window on Windows, a Metal-backed view on macOS, an X11 child on
    Linux. The RENDER THREAD makes a SURFACE on that view - a flip-model swap
    chain, a Metal layer's drawables, an EGL window surface - draws a frame
    into it and shows it.

    EACH DISPLAY AT ITS OWN REFRESH (YJ): `waitForRefresh` sleeps until one or
    more of the surfaces' displays is ready for a frame - a waitable swap chain
    each on Windows, a display link each on macOS - and says which. Linux, for
    now, wakes at the fastest display's rate and draws them all.

    One file a system: Projector_d3d11.cpp, Projector_metal.mm, Projector_gl.cpp.
*/

#include <sokol/sokol_gfx.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video::render
{
    /*  THE NATIVE VIEW: a child window or view filling the JUCE window, by its
        system's handle. `display` is the display it is on, where the system
        has a number for it (macOS) - nought elsewhere. */
    struct NativeView
    {
        void* handle = nullptr;
        std::uint32_t display = 0;
    };

    /*  Linux: the X display every projector's view is made on and EGL draws
        on - a connection of Go.dot's own, opened once. Null elsewhere. */
    void* nativeDisplay();

    /*  Message thread. A view filling `parentWindow` (the JUCE window's
        handle), `width` by `height` of its pixels; an empty one when the
        system will not make one. */
    NativeView makeNativeView (void* parentWindow, int width, int height);

    /*  Message thread: the view fitted to its window again, after the window
        was moved or resized. */
    void fitNativeView (const NativeView& view, void* parentWindow, int width, int height);

    /*  Message thread, once no surface is made on it. */
    void destroyNativeView (NativeView& view);

    //==============================================================================
    class Surface
    {
    public:
        virtual ~Surface() = default;

        /*  WHAT THIS FRAME DRAWS INTO: the swap chain, at the view's present
            size in pixels - false when there is nothing to draw into (a
            minimised window, a drawable not given). Made again at a new size
            here. */
        virtual bool acquire (sg_swapchain& swapchain) = 0;

        /*  After sg_commit: the frame shown, at the display's next refresh. */
        virtual void present() = 0;

        /*  True once its display has refreshed since it last drew - set by
            `waitForRefresh`, cleared by the caller. */
        bool due = false;
    };

    /*  Render thread, the device open: a surface on `view`; null and why when
        the system will not make one. */
    std::unique_ptr<Surface> makeSurface (const NativeView& view, std::string& why);

    /*  Render thread: sleeps until a surface's display has refreshed, or
        `timeoutMs` goes by, and marks each due one. `fastestHz` paces a system
        that cannot wait on a display (Linux, for now). False on a timeout. */
    bool waitForRefresh (std::vector<Surface*>& surfaces, double fastestHz, int timeoutMs);

    /*  A FRAME'S OBJECTS LET GO AT ITS END: an autorelease pool on macOS,
        where the render thread is no thread of Cocoa's; nothing elsewhere. */
    class FramePool
    {
    public:
        FramePool();
        ~FramePool();

        FramePool (const FramePool&) = delete;
        FramePool& operator= (const FramePool&) = delete;

    private:
        [[maybe_unused]] void* pool = nullptr;    ///< macOS's; nothing elsewhere
    };
}
