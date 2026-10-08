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
    AN NDI RECEIVER (namespace draft §44, YB): the source found by its name
    among what NDI sees on the network and on this machine, received as BGRA on
    a thread of its own - NDI's decoding is its own threads' work, never the
    render thread's - and the newest frame kept; the render thread uploads it
    once a frame, however many cues show it.

    The finder is one for the process, made the first time it is wanted and
    kept, as the runtime is (Ndi.h).
*/

#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/Ndi.h>
#include <wfg/engine/video/render/Receiver.h>

#include <ndi/Processing.NDI.Lib.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::video::render
{
    namespace
    {
        double secondsNow() noexcept
        {
            return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        struct Source
        {
            std::string name;
            std::string url;
        };

        /*  WHAT NDI SEES NOW, local sources included, through the process's
            one finder - made the first time and kept. Copied out under a lock:
            the finder's list is good only until the next time anybody asks. */
        std::vector<Source> currentSources()
        {
            static std::mutex lock;
            static NDIlib_find_instance_t finder = nullptr;
            const std::lock_guard<std::mutex> hold (lock);
            const auto* lib = ndi::runtime().lib;
            std::vector<Source> out;

            if (lib == nullptr)
                return out;

            if (finder == nullptr)
            {
                NDIlib_find_create_t settings;
                settings.show_local_sources = true;
                finder = lib->find_create_v2 (&settings);
            }

            if (finder == nullptr)
                return out;

            std::uint32_t count = 0;
            const auto* sources = lib->find_get_current_sources (finder, &count);

            for (std::uint32_t n = 0; sources != nullptr && n < count; ++n)
                if (sources[n].p_ndi_name != nullptr)
                    out.push_back ({ sources[n].p_ndi_name, sources[n].p_url_address != nullptr ? sources[n].p_url_address : "" });

            return out;
        }

        class NdiReceiver final : public Receiver
        {
        public:
            explicit NdiReceiver (std::string senderToTake) : sender (std::move (senderToTake))
            {
                receiving = std::thread ([this] { receive(); });
            }

            ~NdiReceiver() override
            {
                stopping.store (true);

                if (receiving.joinable())
                    receiving.join();

                release();
            }

            void update() override
            {
                std::vector<std::uint8_t> pixels;
                int w = 0, h = 0;

                {
                    const std::lock_guard<std::mutex> hold (lock);

                    if (frameNumber == uploaded)
                        return;

                    uploaded = frameNumber;
                    pixels.swap (newest);
                    w = newestWidth;
                    h = newestHeight;
                }

                if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4)
                    return;

                /*  A NEW IMAGE A FRAME, as a movie's frames are: BGRA where the
                    device reads it (Direct3D, Metal), turned to RGBA where it
                    does not (OpenGL). */
                const auto bgra = sg_query_pixelformat (SG_PIXELFORMAT_BGRA8).sample;

                if (! bgra)
                    for (std::size_t at = 0; at + 3 < pixels.size(); at += 4)
                        std::swap (pixels[at], pixels[at + 2]);

                release();

                sg_image_desc desc {};
                desc.width = w;
                desc.height = h;
                desc.pixel_format = bgra ? SG_PIXELFORMAT_BGRA8 : SG_PIXELFORMAT_RGBA8;
                desc.data.mip_levels[0].ptr = pixels.data();
                desc.data.mip_levels[0].size = static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4;
                image = sg_make_image (desc);

                sg_view_desc view {};
                view.texture.image = image;
                textureView = sg_make_view (view);
                pictureWidth = w;
                pictureHeight = h;

                //  Handed back, so the next frame is copied into memory already made.
                const std::lock_guard<std::mutex> hold (lock);

                if (spare.capacity() < pixels.capacity())
                    spare.swap (pixels);
            }

            sg_view picture() const override     { return textureView; }
            int width() const override           { return pictureWidth; }
            int height() const override          { return pictureHeight; }
            bool connected() const override      { return arriving.load() && textureView.id != SG_INVALID_ID; }

            double frameRate() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return arrivals.rate (secondsNow());
            }

            std::string problem() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return trouble;
            }

        private:
            void say (const std::string& text)
            {
                const std::lock_guard<std::mutex> hold (lock);
                trouble = text;
            }

            /*  THE RECEIVING THREAD: the source looked for until it is found,
                then its frames taken as they come, the newest kept. */
            void receive()
            {
                const auto* lib = ndi::runtime().lib;

                if (lib == nullptr)
                {
                    say (ndi::runtime().problem);
                    return;
                }

                NDIlib_recv_instance_t instance = nullptr;
                auto lastSeen = secondsNow();

                while (! stopping.load())
                {
                    if (instance == nullptr)
                    {
                        for (const auto& source : currentSources())
                            if (instance == nullptr && source.name == sender)
                            {
                                NDIlib_source_t found;
                                found.p_ndi_name = source.name.c_str();
                                found.p_url_address = source.url.empty() ? nullptr : source.url.c_str();

                                NDIlib_recv_create_v3_t settings;
                                settings.source_to_connect_to = found;
                                settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
                                settings.bandwidth = NDIlib_recv_bandwidth_highest;
                                settings.allow_video_fields = false;
                                instance = lib->recv_create_v3 (&settings);
                            }

                        if (instance == nullptr)
                        {
                            say ("nothing is sending over NDI as " + sender);
                            std::this_thread::sleep_for (std::chrono::milliseconds (250));
                            continue;
                        }
                    }

                    NDIlib_video_frame_v2_t frame;

                    if (lib->recv_capture_v2 (instance, &frame, nullptr, nullptr, 100) == NDIlib_frame_type_video)
                    {
                        if (frame.p_data != nullptr && frame.xres > 0 && frame.yres > 0)
                            keep (frame);

                        lib->recv_free_video_v2 (instance, &frame);
                        lastSeen = secondsNow();
                        arriving.store (true);
                        say ({});
                    }
                    else if (secondsNow() - lastSeen > 2.0)
                    {
                        arriving.store (false);
                        say ("nothing has arrived from " + sender + " for two seconds");
                    }
                }

                if (instance != nullptr)
                    lib->recv_destroy (instance);
            }

            void keep (const NDIlib_video_frame_v2_t& frame)
            {
                const auto row = static_cast<std::size_t> (frame.xres) * 4;
                std::vector<std::uint8_t> pixels;

                {
                    const std::lock_guard<std::mutex> hold (lock);
                    pixels.swap (spare);
                }

                pixels.resize (row * static_cast<std::size_t> (frame.yres));

                for (int y = 0; y < frame.yres; ++y)
                    std::memcpy (pixels.data() + static_cast<std::size_t> (y) * row,
                                 frame.p_data + static_cast<std::ptrdiff_t> (y) * frame.line_stride_in_bytes, row);

                const std::lock_guard<std::mutex> hold (lock);
                newest.swap (pixels);
                newestWidth = frame.xres;
                newestHeight = frame.yres;
                ++frameNumber;
                arrivals.arrived (secondsNow());
            }

            void release()
            {
                if (textureView.id != SG_INVALID_ID)
                    sg_destroy_view (textureView);

                if (image.id != SG_INVALID_ID)
                    sg_destroy_image (image);

                textureView = {};
                image = {};
            }

            const std::string sender;
            std::thread receiving;
            std::atomic<bool> stopping { false };
            std::atomic<bool> arriving { false };

            mutable std::mutex lock;
            std::vector<std::uint8_t> newest, spare;
            int newestWidth = 0;
            int newestHeight = 0;
            std::uint64_t frameNumber = 0;
            std::uint64_t uploaded = 0;
            ArrivalRate arrivals;
            std::string trouble;

            sg_image image {};
            sg_view textureView {};
            int pictureWidth = 0;
            int pictureHeight = 0;
        };
    }

    std::unique_ptr<Receiver> makeNdiReceiver (const std::string& sender, std::string& why)
    {
        if (ndi::runtime().lib == nullptr)
        {
            why = ndi::runtime().problem;
            return nullptr;
        }

        return std::make_unique<NdiReceiver> (sender);
    }

    std::string discoverNdi()
    {
        std::string out;

        for (const auto& source : currentSources())
            out += "ndi\t" + source.name + "\n";

        return out;
    }
}
