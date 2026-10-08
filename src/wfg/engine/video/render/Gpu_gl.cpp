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
    THE DEVICE ON LINUX: OpenGL 4.1 core, through EGL (namespace draft §44.4) -
    one context, made current on no surface at all, which a picture drawn in
    memory needs, and on each projector's window in turn as it is drawn. On
    the X display the projectors' windows are on when there are windows;
    without, Mesa's surfaceless platform first, the default display after.
    `software` asks Mesa for its software rasteriser (llvmpipe), which every
    test draws on.
*/

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <wfg/engine/video/render/GpuNative.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wfg::video::gpu::native
{
    namespace
    {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
        EGLConfig chosenConfig = nullptr;

        EGLDisplay displayToUse (void* x11)
        {
            if (x11 != nullptr)
            {
               #ifdef EGL_PLATFORM_X11_KHR
                const auto onX = eglGetPlatformDisplay (EGL_PLATFORM_X11_KHR, x11, nullptr);

                if (onX != EGL_NO_DISPLAY)
                    return onX;
               #endif

                return eglGetDisplay (reinterpret_cast<EGLNativeDisplayType> (x11));
            }

           #ifdef EGL_PLATFORM_SURFACELESS_MESA
            const auto surfaceless = eglGetPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);

            if (surfaceless != EGL_NO_DISPLAY)
                return surfaceless;
           #endif

            return eglGetDisplay (EGL_DEFAULT_DISPLAY);
        }
    }

    const char* backendName() noexcept
    {
        return "OpenGL";
    }

    bool open (const OpenOptions& options, sg_environment& environment, std::string& adapter, std::string& why)
    {
        if (options.software)
            setenv ("LIBGL_ALWAYS_SOFTWARE", "1", 1);

        display = displayToUse (options.nativeDisplay);

        if (display == EGL_NO_DISPLAY || eglInitialize (display, nullptr, nullptr) != EGL_TRUE)
        {
            display = EGL_NO_DISPLAY;
            why = "EGL found no display to draw with";
            return false;
        }

        if (eglBindAPI (EGL_OPENGL_API) != EGL_TRUE)
        {
            close();
            why = "EGL does not offer OpenGL here";
            return false;
        }

        /*  With windows, one that draws into them - an X window's default
            visual has no alpha; without, any: the context is made current on
            no surface. */
        const auto windowed = options.nativeDisplay != nullptr;
        const EGLint configWanted[] { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, windowed ? EGL_WINDOW_BIT : 0,
                                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, windowed ? 0 : 8,
                                      EGL_NONE };
        EGLConfig config = nullptr;
        EGLint configs = 0;

        if (eglChooseConfig (display, configWanted, &config, 1, &configs) != EGL_TRUE || configs < 1)
        {
            close();
            why = "EGL has no configuration for OpenGL";
            return false;
        }

        const EGLint contextWanted[] { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                                       EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                       EGL_NONE };
        context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextWanted);

        if (context == EGL_NO_CONTEXT
              || eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE)
        {
            close();
            why = "OpenGL 4.1 would not start through EGL";
            return false;
        }

        chosenConfig = config;

        if (const auto* renderer = reinterpret_cast<const char*> (glGetString (GL_RENDERER)))
            adapter = renderer;

        environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
        return true;
    }

    void close()
    {
        if (display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

            if (context != EGL_NO_CONTEXT)
                eglDestroyContext (display, context);

            eglTerminate (display);
        }

        context = EGL_NO_CONTEXT;
        display = EGL_NO_DISPLAY;
        chosenConfig = nullptr;
    }

    void* eglDisplay() noexcept  { return display; }
    void* eglContext() noexcept  { return context; }
    void* eglConfig() noexcept   { return chosenConfig; }

    bool readBack (sg_image image, int width, int height, sg_pixel_format, std::vector<float>& rgba)
    {
        const auto info = sg_gl_query_image_info (image);
        const auto texture = info.tex[info.active_slot];

        if (texture == 0)
            return false;

        GLuint frame = 0;
        glGenFramebuffers (1, &frame);
        glBindFramebuffer (GL_FRAMEBUFFER, frame);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        const auto complete = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        std::vector<float> rows;

        if (complete)
        {
            rows.resize (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4);
            glPixelStorei (GL_PACK_ALIGNMENT, 1);
            glReadPixels (0, 0, width, height, GL_RGBA, GL_FLOAT, rows.data());
        }

        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers (1, &frame);
        sg_reset_state_cache();

        if (! complete)
            return false;

        //  OpenGL's first row is the picture's bottom: turned the right way up.
        const auto rowFloats = static_cast<std::size_t> (width) * 4;
        rgba.resize (rows.size());

        for (int y = 0; y < height; ++y)
            std::copy_n (rows.data() + static_cast<std::size_t> (height - 1 - y) * rowFloats, rowFloats,
                         rgba.data() + static_cast<std::size_t> (y) * rowFloats);

        return true;
    }
}
