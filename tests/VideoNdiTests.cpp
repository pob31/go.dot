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

/*  AN OUTPUT SENT OVER NDI (namespace draft §44, YA; N.2): the renderer draws
    an output into its sender's picture, reads it back and hands it to the NDI
    runtime; an NDI receiver - as another machine's program would be - finds
    the source by its name and the colour it reads is the canvas's, calibrated.

    NDI's runtime is installed by the user, never shipped (the author's word,
    2026-10-08): on a machine without it - CI's - the case says so and checks
    only that the sender says why it cannot be. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/Ndi.h>
#include <wfg/engine/video/render/Painter.h>
#include <wfg/engine/video/render/Sender.h>

#include <ndi/Processing.NDI.Lib.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    struct NoPictures final : video::render::Sources
    {
        juce::Image picture (const std::string&, std::uint64_t&) const override  { return {}; }

        std::shared_ptr<const video::render::MovieFrame> movieFrame (const std::string&, double) const override
        {
            return nullptr;
        }
    };
}

TEST_CASE ("video gpu: an output sent over NDI is found by its name and reads as its canvas (N.2)")
{
    video::gpu::OpenOptions options;
    options.software = true;
    std::string why;

    if (! video::gpu::open (options, why))
    {
        MESSAGE ("no device to draw on here (" << why << ")");
        return;
    }

    const auto& runtime = video::ndi::runtime();

    if (runtime.lib == nullptr)
    {
        //  No runtime: the sender is refused, and says why in a sentence.
        auto sender = video::render::makeSender (video::region::OutputKind::ndi, "Go.dot test", 30.0, why);
        CHECK (sender == nullptr);
        CHECK (why.find ("NDI") != std::string::npos);
        MESSAGE ("no NDI runtime here: " << why);
        video::gpu::close();
        return;
    }

    MESSAGE ("NDI " << runtime.version << " from " << runtime.path);

    const auto name = "Go.dot test " + std::to_string (juce::Random::getSystemRandom().nextInt (1 << 30));
    std::uint32_t received = 0xFFFFFFFFu;

    {
        NoPictures pictures;
        video::render::Painter painter (pictures);
        REQUIRE_MESSAGE (painter.make (why), why);

        auto sender = video::render::makeSender (video::region::OutputKind::ndi, name, 30.0, why);
        REQUIRE_MESSAGE (sender != nullptr, why);

        video::region::LayerReading fill;
        fill.id = "L1";
        fill.canvas = "C1";
        fill.source = video::region::Source::fill;
        fill.paint = 0x3080C0u;
        fill.rings[static_cast<int> (video::Property::opacity)].count = 1;
        fill.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, 1.0 };

        video::region::ConfigReading config;
        config.canvases.push_back ({ "C1", 64, 36 });

        video::region::OutputReading output;
        output.id = "O1";
        output.canvas = "C1";
        output.kind = video::region::OutputKind::ndi;
        output.cdl.slope[2] = 0.5;

        /*  THE RECEIVER, looking for the source among this machine's. */
        const auto* lib = runtime.lib;
        NDIlib_find_create_t findSettings;
        findSettings.show_local_sources = true;
        auto* finder = lib->find_create_v2 (&findSettings);
        REQUIRE (finder != nullptr);

        NDIlib_recv_instance_t receiver = nullptr;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (20);

        while (std::chrono::steady_clock::now() < until && received == 0xFFFFFFFFu)
        {
            painter.beginFrame (config, { fill }, {});
            painter.drawOutput (output, 100, painter.offscreenTarget ("send:O1", 64, 36, video::render::senderFormat()));
            sg_commit();
            CHECK (sender->send (painter.offscreenImage ("send:O1"), 64, 36));
            painter.endFrame();

            if (receiver == nullptr)
            {
                std::uint32_t count = 0;
                const auto* sources = lib->find_get_current_sources (finder, &count);

                for (std::uint32_t n = 0; n < count; ++n)
                    if (sources[n].p_ndi_name != nullptr && std::string (sources[n].p_ndi_name).find (name) != std::string::npos)
                    {
                        NDIlib_recv_create_v3_t recvSettings;
                        recvSettings.source_to_connect_to = sources[n];
                        recvSettings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
                        recvSettings.bandwidth = NDIlib_recv_bandwidth_highest;
                        receiver = lib->recv_create_v3 (&recvSettings);
                    }
            }
            else
            {
                NDIlib_video_frame_v2_t frame;

                if (lib->recv_capture_v2 (receiver, &frame, nullptr, nullptr, 50) == NDIlib_frame_type_video)
                {
                    if (frame.p_data != nullptr && frame.xres > 0 && frame.yres > 0)
                    {
                        const auto* pixel = frame.p_data + (frame.yres / 2) * frame.line_stride_in_bytes + 4 * (frame.xres / 2);
                        received = (static_cast<std::uint32_t> (pixel[2]) << 16) | (static_cast<std::uint32_t> (pixel[1]) << 8)
                                 | static_cast<std::uint32_t> (pixel[0]);
                    }

                    lib->recv_free_video_v2 (receiver, &frame);
                }
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (33));
        }

        if (receiver != nullptr)
            lib->recv_destroy (receiver);

        lib->find_destroy (finder);
    }

    video::gpu::close();

    INFO ("received " << received << " from " << name);

    //  0x3080C0, blue at half slope: (48, 128, 96) - NDI's codec moves a flat colour by a step or two.
    CHECK (std::abs (static_cast<int> ((received >> 16) & 0xffu) - 0x30) <= 4);
    CHECK (std::abs (static_cast<int> ((received >> 8) & 0xffu) - 0x80) <= 4);
    CHECK (std::abs (static_cast<int> (received & 0xffu) - 0x60) <= 4);
}
