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
    AN NDI SENDER (namespace draft §44, YA; N.2): the output's picture read back
    from the graphics card - NDI sends pixels from memory, where Spout and
    Syphon share a texture - and handed to the runtime the user installed
    (Ndi.h), which encodes and sends it on its own threads. Sent asynchronously:
    the runtime holds a frame's pixels until the next is sent, so two buffers
    take turns. Its timecode is NDI's own, made from the clock at sending.

    The read back waits for the frame to be drawn. One picture of a canvas's
    size, at the sender's rate; a two-deep ring that reads a frame late, if a
    show ever needs the render thread back, is left for then.
*/

#include <wfg/engine/video/render/Ndi.h>
#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/Sender.h>

#include <ndi/Processing.NDI.Lib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wfg::video::render
{
    namespace
    {
        class NdiSender final : public Sender
        {
        public:
            explicit NdiSender (double rateToSend) : rate (std::clamp (rateToSend, 1.0, 240.0)) {}

            bool open (const std::string& name, std::string& why)
            {
                const auto& runtime = ndi::runtime();

                if (runtime.lib == nullptr)
                {
                    why = runtime.problem;
                    return false;
                }

                lib = runtime.lib;

                NDIlib_send_create_t create;
                create.p_ndi_name = name.c_str();
                create.p_groups = nullptr;

                //  Paced by the render thread, never by NDI.
                create.clock_video = false;
                create.clock_audio = false;

                instance = lib->send_create (&create);

                if (instance == nullptr)
                {
                    why = "NDI would not make a sender named " + name;
                    return false;
                }

                return true;
            }

            ~NdiSender() override
            {
                if (instance != nullptr)
                {
                    //  The runtime lets go of the last frame's pixels before they go.
                    lib->send_send_video_async_v2 (instance, nullptr);
                    lib->send_destroy (instance);
                }
            }

            bool send (sg_image picture, int, int) override
            {
                auto& pixels = buffers[next];
                int width = 0, height = 0;
                auto format = SG_PIXELFORMAT_NONE;

                if (! gpu::readBackBytes (picture, pixels, width, height, format))
                    return false;

                NDIlib_video_frame_v2_t frame;
                frame.xres = width;
                frame.yres = height;
                frame.FourCC = format == SG_PIXELFORMAT_BGRA8 ? NDIlib_FourCC_video_type_BGRA : NDIlib_FourCC_video_type_RGBA;
                frame.frame_rate_N = static_cast<int> (std::lround (rate * 1000.0));
                frame.frame_rate_D = 1000;
                frame.picture_aspect_ratio = static_cast<float> (width) / static_cast<float> (std::max (1, height));
                frame.frame_format_type = NDIlib_frame_format_type_progressive;
                frame.timecode = NDIlib_send_timecode_synthesize;
                frame.p_data = pixels.data();
                frame.line_stride_in_bytes = width * 4;

                lib->send_send_video_async_v2 (instance, &frame);
                next = 1 - next;
                return true;
            }

        private:
            double rate = 60.0;
            const NDIlib_v6* lib = nullptr;
            NDIlib_send_instance_t instance = nullptr;
            std::vector<std::uint8_t> buffers[2];
            int next = 0;
        };
    }

    std::unique_ptr<Sender> makeNdiSender (const std::string& name, double frameRate, std::string& why)
    {
        auto sender = std::make_unique<NdiSender> (frameRate);

        if (! sender->open (name, why))
            return nullptr;

        return sender;
    }
}
