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

/*
    A PROJECTOR ON LINUX (namespace draft §44.4): an X11 child window filling
    the JUCE window, made on a connection to the X server of Go.dot's own -
    the one EGL draws on too, so the one context draws into every projector -
    and an EGL window surface on it, made current while it is drawn.

    PACED BY A CLOCK, for now: the render thread wakes at the fastest
    display's rate and draws every projector, each swapped without waiting for
    its refresh. A compositor shows the newest; without one a frame may tear.
    Waiting on each display's own refresh, as Windows and macOS do (YJ), is
    left for when a Linux booth asks for it.
*/

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <X11/Xlib.h>

#include <wfg/engine/video/render/GpuNative.h>
#include <wfg/engine/video/render/Projector.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::video::render
{
    namespace
    {
        ::Display* connection()
        {
            static ::Display* const opened = []
            {
                XInitThreads();
                return XOpenDisplay (nullptr);
            }();

            return opened;
        }

        ::Window windowOf (void* handle) noexcept
        {
            return static_cast<::Window> (reinterpret_cast<std::uintptr_t> (handle));
        }

        class GlSurface final : public Surface
        {
        public:
            explicit GlSurface (::Window windowToDrawIn) : window (windowToDrawIn) {}

            ~GlSurface() override
            {
                auto* display = static_cast<EGLDisplay> (gpu::native::eglDisplay());

                if (surface != EGL_NO_SURFACE && display != EGL_NO_DISPLAY)
                {
                    eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext> (gpu::native::eglContext()));
                    eglDestroySurface (display, surface);
                }
            }

            bool make (std::string& why)
            {
                auto* display = static_cast<EGLDisplay> (gpu::native::eglDisplay());
                auto* config = static_cast<EGLConfig> (gpu::native::eglConfig());

                if (display == EGL_NO_DISPLAY || config == nullptr)
                {
                    why = "OpenGL has no display to draw on";
                    return false;
                }

                surface = eglCreateWindowSurface (display, config, static_cast<EGLNativeWindowType> (window), nullptr);

                if (surface == EGL_NO_SURFACE)
                {
                    why = "EGL would not draw into the projector's window";
                    return false;
                }

                return true;
            }

            bool acquire (sg_swapchain& swapchain) override
            {
                auto* display = static_cast<EGLDisplay> (gpu::native::eglDisplay());

                if (eglMakeCurrent (display, surface, surface, static_cast<EGLContext> (gpu::native::eglContext())) != EGL_TRUE)
                    return false;

                //  Swapped as soon as it is drawn: the clock paces it (above).
                eglSwapInterval (display, 0);

                EGLint width = 0, height = 0;
                eglQuerySurface (display, surface, EGL_WIDTH, &width);
                eglQuerySurface (display, surface, EGL_HEIGHT, &height);

                if (width < 1 || height < 1)
                    return false;

                swapchain = {};
                swapchain.width = width;
                swapchain.height = height;
                swapchain.color_format = SG_PIXELFORMAT_RGBA8;
                swapchain.depth_format = SG_PIXELFORMAT_NONE;
                swapchain.sample_count = 1;
                swapchain.gl.framebuffer = 0;
                drawn = true;
                return true;
            }

            /*  Shown as soon as it is drawn - before the next projector is made
                current - and the context put back on no surface. */
            void present() override
            {
                if (! drawn)
                    return;

                auto* display = static_cast<EGLDisplay> (gpu::native::eglDisplay());
                eglMakeCurrent (display, surface, surface, static_cast<EGLContext> (gpu::native::eglContext()));
                eglSwapBuffers (display, surface);
                eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext> (gpu::native::eglContext()));
                drawn = false;
            }

        private:
            ::Window window = 0;
            EGLSurface surface = EGL_NO_SURFACE;
            bool drawn = false;
        };
    }

    //==============================================================================
    void* nativeDisplay()
    {
        return connection();
    }

    NativeView makeNativeView (void* parentWindow, int width, int height)
    {
        NativeView made;
        auto* x = connection();

        if (x == nullptr || parentWindow == nullptr)
            return made;

        XSetWindowAttributes attributes {};
        attributes.background_pixel = BlackPixel (x, DefaultScreen (x));

        const auto child = XCreateWindow (x, windowOf (parentWindow), 0, 0,
                                          static_cast<unsigned int> (std::max (1, width)),
                                          static_cast<unsigned int> (std::max (1, height)), 0, CopyFromParent,
                                          InputOutput, nullptr, CWBackPixel, &attributes);
        XMapWindow (x, child);
        XFlush (x);

        made.handle = reinterpret_cast<void*> (static_cast<std::uintptr_t> (child));
        return made;
    }

    void fitNativeView (const NativeView& view, void*, int width, int height)
    {
        if (auto* x = connection(); x != nullptr && view.handle != nullptr)
        {
            XResizeWindow (x, windowOf (view.handle), static_cast<unsigned int> (std::max (1, width)),
                           static_cast<unsigned int> (std::max (1, height)));
            XFlush (x);
        }
    }

    void destroyNativeView (NativeView& view)
    {
        if (auto* x = connection(); x != nullptr && view.handle != nullptr)
        {
            XDestroyWindow (x, windowOf (view.handle));
            XFlush (x);
        }

        view = {};
    }

    std::unique_ptr<Surface> makeSurface (const NativeView& view, std::string& why)
    {
        if (view.handle == nullptr)
        {
            why = "the projector's window has no view to draw into";
            return nullptr;
        }

        auto surface = std::make_unique<GlSurface> (windowOf (view.handle));

        if (! surface->make (why))
            return nullptr;

        return surface;
    }

    bool waitForRefresh (std::vector<Surface*>& surfaces, double fastestHz, int timeoutMs)
    {
        using steady = std::chrono::steady_clock;
        static thread_local steady::time_point next = steady::now();

        const auto period = std::chrono::duration_cast<steady::duration> (
            std::chrono::duration<double> (1.0 / std::clamp (fastestHz, 24.0, 240.0)));
        const auto now = steady::now();

        if (next < now - period)
            next = now;

        next += period;

        if (next - now > std::chrono::milliseconds (std::max (1, timeoutMs)))
            next = now + std::chrono::milliseconds (std::max (1, timeoutMs));

        std::this_thread::sleep_until (next);

        for (auto* surface : surfaces)
            surface->due = true;

        return ! surfaces.empty();
    }

    FramePool::FramePool() = default;
    FramePool::~FramePool() = default;
}
