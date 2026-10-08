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

#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/GpuNative.h>

#include <mutex>
#include <string>

namespace wfg::video::gpu
{
    namespace
    {
        bool opened = false;
        std::string adapterName;

        /*  WHAT SOKOL SAID LAST, kept rather than printed: the renderer's
            standard error is nobody's to read in a show (§39), and its problem
            readout is where a fault is said. */
        std::mutex messageLock;
        std::string message;

        void logged (const char* /*tag*/, std::uint32_t level, std::uint32_t item, const char* text,
                     std::uint32_t line, const char* /*file*/, void* /*user*/)
        {
            //  Errors and warnings; sokol's information is not a fault.
            if (level > 2)
                return;

            const std::lock_guard<std::mutex> hold (messageLock);
            message = text != nullptr ? std::string (text)
                                      : "sokol_gfx item " + std::to_string (item) + " at line " + std::to_string (line);
        }
    }

    bool open (const OpenOptions& options, std::string& why)
    {
        if (opened)
            return true;

        sg_environment environment {};

        if (! native::open (options, environment, adapterName, why))
            return false;

        environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
        environment.defaults.sample_count = 1;

        sg_desc desc {};
        desc.environment = environment;
        desc.logger.func = logged;

        //  Room for many pictures, movies' frames and warps at once.
        desc.buffer_pool_size = 256;
        desc.image_pool_size = 1024;
        desc.view_pool_size = 2048;
        desc.sampler_pool_size = 16;
        desc.shader_pool_size = 32;
        desc.pipeline_pool_size = 256;
        desc.uniform_buffer_size = 8 * 1024 * 1024;

        sg_setup (&desc);

        if (! sg_isvalid())
        {
            native::close();
            why = "the graphics library would not start on " + adapterName;
            return false;
        }

        opened = true;
        return true;
    }

    void close()
    {
        if (! opened)
            return;

        sg_shutdown();
        native::close();
        opened = false;
    }

    bool isOpen() noexcept
    {
        return opened;
    }

    std::string describe()
    {
        return std::string (native::backendName()) + (adapterName.empty() ? std::string {} : " on " + adapterName);
    }

    std::string lastMessage()
    {
        const std::lock_guard<std::mutex> hold (messageLock);
        return message;
    }

    bool originTopLeft()
    {
        return sg_query_features().origin_top_left;
    }

    bool readBack (sg_image image, std::vector<float>& rgba, int& width, int& height)
    {
        if (! opened || sg_query_image_state (image) != SG_RESOURCESTATE_VALID)
            return false;

        const auto desc = sg_query_image_desc (image);
        width = desc.width;
        height = desc.height;

        if (width <= 0 || height <= 0 || native::bytesPerPixel (desc.pixel_format) == 0)
            return false;

        return native::readBack (image, width, height, desc.pixel_format, rgba);
    }
}
