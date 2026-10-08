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

/*  AN OUTPUT SENT OVER SPOUT (namespace draft §44, YA; N.1), Windows only: the
    renderer draws an output into its sender's picture and Spout shares it; a
    Spout receiver - as another program would be - finds it by its name, and
    the colour it reads is the canvas's, calibrated as an output is. On WARP,
    so CI's runners, which have no graphics card, send and receive too.

    Spout's headers come last: they bring windowsx.h's macros. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/Painter.h>
#include <wfg/engine/video/render/Receiver.h>
#include <wfg/engine/video/render/Sender.h>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <spout/SpoutDX.h>

#include <chrono>
#include <cstdint>
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

    /*  The texture's middle pixel as 0xRRGGBB, through a staging copy. */
    std::uint32_t middleOf (ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc (&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ID3D11Texture2D* staging = nullptr;

        if (FAILED (device->CreateTexture2D (&desc, nullptr, &staging)))
            return 0xFFFFFFFFu;

        context->CopyResource (staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        std::uint32_t rgb = 0xFFFFFFFFu;

        if (SUCCEEDED (context->Map (staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const auto* pixel = static_cast<const std::uint8_t*> (mapped.pData) + (desc.Height / 2) * mapped.RowPitch + 4 * (desc.Width / 2);
            const auto bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
            rgb = (static_cast<std::uint32_t> (pixel[bgra ? 2 : 0]) << 16) | (static_cast<std::uint32_t> (pixel[1]) << 8)
                | static_cast<std::uint32_t> (pixel[bgra ? 0 : 2]);
            context->Unmap (staging, 0);
        }

        staging->Release();
        return rgb;
    }
}

TEST_CASE ("video gpu: an output sent over Spout is found by its name and reads as its canvas (N.1)")
{
    video::gpu::OpenOptions options;
    options.software = true;
    std::string why;
    REQUIRE_MESSAGE (video::gpu::open (options, why), why);

    const std::string name = "Go.dot test sender " + std::to_string (GetCurrentProcessId());
    std::uint32_t received = 0xFFFFFFFFu;
    std::string receivedName;

    {
        NoPictures pictures;
        video::render::Painter painter (pictures);
        REQUIRE_MESSAGE (painter.make (why), why);

        auto sender = video::render::makeSender (video::region::OutputKind::spout, name, 60.0, why);
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
        output.kind = video::region::OutputKind::spout;
        output.cdl.slope[2] = 0.5;      // blue halved: the calibration reaches what is sent

        auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));
        auto* context = static_cast<ID3D11DeviceContext*> (const_cast<void*> (sg_d3d11_device_context()));

        spoutDX receiver;
        REQUIRE (receiver.OpenDirectX11 (device));
        receiver.SetReceiverName (name.c_str());

        /*  A FEW FRAMES, as the render thread sends them, the receiver looking
            after each - it connects on one and copies on the next. */
        for (int frame = 0; frame < 20 && received == 0xFFFFFFFFu; ++frame)
        {
            painter.beginFrame (config, { fill }, {});
            painter.drawOutput (output, 100, painter.offscreenTarget ("send:O1", 64, 36, video::render::senderFormat()));
            sg_commit();
            CHECK (sender->send (painter.offscreenImage ("send:O1"), 64, 36));
            painter.endFrame();

            if (receiver.ReceiveTexture() && ! receiver.IsUpdated())
                if (auto* texture = receiver.GetSenderTexture())
                {
                    received = middleOf (device, context, texture);
                    receivedName = receiver.GetSenderName();
                }

            sg_reset_state_cache();
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        receiver.ReleaseReceiver();
        receiver.CloseDirectX11();
    }

    video::gpu::close();

    INFO ("received " << received << " from " << receivedName);
    CHECK (receivedName == name);

    //  0x3080C0 with blue at half slope: (48, 128, 96), dithered by one at most.
    CHECK (std::abs (static_cast<int> ((received >> 16) & 0xffu) - 0x30) <= 1);
    CHECK (std::abs (static_cast<int> ((received >> 8) & 0xffu) - 0x80) <= 1);
    CHECK (std::abs (static_cast<int> (received & 0xffu) - 0x60) <= 1);
}

TEST_CASE ("video gpu: a picture taken in over Spout is what a capture cue shows (N.3)")
{
    video::gpu::OpenOptions options;
    options.software = true;
    std::string why;
    REQUIRE_MESSAGE (video::gpu::open (options, why), why);

    const std::string name = "Go.dot test input " + std::to_string (GetCurrentProcessId());
    std::vector<float> rgba;
    int w = 0, h = 0;
    bool connected = false;

    {
        NoPictures pictures;
        video::render::Painter painter (pictures);
        REQUIRE_MESSAGE (painter.make (why), why);

        //  ANOTHER PROGRAM'S PICTURE: a sender of a flat 0x40A060, as Go.dot's own outputs send.
        auto sender = video::render::makeSender (video::region::OutputKind::spout, name, 60.0, why);
        REQUIRE_MESSAGE (sender != nullptr, why);

        auto receiver = video::render::makeReceiver (video::region::OutputKind::spout, name, why);
        REQUIRE_MESSAGE (receiver != nullptr, why);

        video::region::LayerReading fill;
        fill.id = "L1";
        fill.canvas = "C1";
        fill.source = video::region::Source::fill;
        fill.paint = 0x40A060u;
        fill.rings[static_cast<int> (video::Property::opacity)].count = 1;
        fill.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, 1.0 };

        //  THE CAPTURE: a cue on canvas C2 showing input IN1, stretched over it.
        video::region::LayerReading capture;
        capture.id = "L2";
        capture.canvas = "C2";
        capture.source = video::region::Source::capture;
        capture.input = "IN1";
        capture.fit = video::region::Fit::stretch;
        capture.rings[static_cast<int> (video::Property::opacity)].count = 1;
        capture.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, 1.0 };

        video::region::ConfigReading config;
        config.canvases.push_back ({ "C1", 64, 36 });
        config.canvases.push_back ({ "C2", 32, 32 });

        video::region::OutputReading output;
        output.id = "O1";
        output.canvas = "C1";
        output.kind = video::region::OutputKind::spout;

        for (int frame = 0; frame < 30 && ! connected; ++frame)
        {
            receiver->update();
            sg_reset_state_cache();
            painter.setInputPicture ("IN1", receiver->picture(), receiver->width(), receiver->height());

            painter.beginFrame (config, { fill, capture }, {});
            painter.drawOutput (output, 100, painter.offscreenTarget ("send:O1", 64, 36, video::render::senderFormat()));
            painter.canvas ("C2", 100);
            sg_commit();
            sender->send (painter.offscreenImage ("send:O1"), 64, 36);

            connected = receiver->connected();

            if (connected)
                REQUIRE (video::gpu::readBack (painter.canvasImage ("C2"), rgba, w, h));

            painter.endFrame();
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        INFO ("the receiver said: " << receiver->problem());
        CHECK (connected);
        CHECK (receiver->width() == 64);
        CHECK (receiver->height() == 36);
    }

    video::gpu::close();

    REQUIRE (connected);
    REQUIRE (w == 32);

    const auto* middle = rgba.data() + 4 * (static_cast<std::size_t> (h / 2) * static_cast<std::size_t> (w) + static_cast<std::size_t> (w / 2));
    INFO ("the capture reads " << middle[0] * 255.0f << " " << middle[1] * 255.0f << " " << middle[2] * 255.0f);
    CHECK (std::abs (middle[0] * 255.0f - 64.0f) <= 2.0f);
    CHECK (std::abs (middle[1] * 255.0f - 160.0f) <= 2.0f);
    CHECK (std::abs (middle[2] * 255.0f - 96.0f) <= 2.0f);
}

TEST_CASE ("video gpu: a cue through an insert shows what the other program sends back, and black before it does (N.4)")
{
    video::gpu::OpenOptions options;
    options.software = true;
    std::string why;
    REQUIRE_MESSAGE (video::gpu::open (options, why), why);

    const auto pid = std::to_string (GetCurrentProcessId());
    const std::string out = "Go.dot test insert out " + pid;
    const std::string back = "Go.dot test insert back " + pid;

    std::uint32_t first = 0xFFFFFFFFu;
    std::uint32_t last = 0xFFFFFFFFu;

    {
        NoPictures pictures;
        video::render::Painter painter (pictures);
        REQUIRE_MESSAGE (painter.make (why), why);

        auto sender = video::render::makeSender (video::region::OutputKind::spout, out, 60.0, why);
        REQUIRE_MESSAGE (sender != nullptr, why);
        auto returns = video::render::makeReceiver (video::region::OutputKind::spout, back, why);
        REQUIRE_MESSAGE (returns != nullptr, why);

        /*  THE OTHER PROGRAM: takes Go.dot's picture in, and sends back its
            middle colour turned inside out - proof the picture went round. */
        auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));
        auto* context = static_cast<ID3D11DeviceContext*> (const_cast<void*> (sg_d3d11_device_context()));
        spoutDX echoIn, echoOut;
        REQUIRE (echoIn.OpenDirectX11 (device));
        REQUIRE (echoOut.OpenDirectX11 (device));
        echoIn.SetReceiverName (out.c_str());
        echoOut.SetSenderName (back.c_str());

        video::region::LayerReading fill;
        fill.id = "L1";
        fill.canvas = "C1";
        fill.source = video::region::Source::fill;
        fill.paint = 0x204080u;
        fill.insert = "IS1";
        fill.fit = video::region::Fit::stretch;
        fill.rings[static_cast<int> (video::Property::opacity)].count = 1;
        fill.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, 1.0 };

        video::region::ConfigReading config;
        config.canvases.push_back ({ "C1", 32, 32 });

        for (int frame = 0; frame < 40; ++frame)
        {
            returns->update();
            sg_reset_state_cache();
            painter.setInsertReturn ("IS1", returns->picture(), returns->width(), returns->height());

            painter.beginFrame (config, { fill }, {});
            const auto drew = painter.drawInsertPicture ("IS1", 100, video::render::senderFormat());
            painter.canvas ("C1", 100);
            sg_commit();

            if (drew)
                sender->send (painter.insertImage ("IS1"), 32, 32);

            std::vector<float> rgba;
            int w = 0, h = 0;
            REQUIRE (video::gpu::readBack (painter.canvasImage ("C1"), rgba, w, h));
            const auto* middle = rgba.data() + 4 * (static_cast<std::size_t> (h / 2) * static_cast<std::size_t> (w) + static_cast<std::size_t> (w / 2));
            last = (static_cast<std::uint32_t> (std::lround (middle[0] * 255.0f)) << 16)
                 | (static_cast<std::uint32_t> (std::lround (middle[1] * 255.0f)) << 8)
                 | static_cast<std::uint32_t> (std::lround (middle[2] * 255.0f));

            if (frame == 0)
                first = last;

            painter.endFrame();

            //  The echo: what came in, its middle inverted, sent back as a 16 by 16 picture.
            if (echoIn.ReceiveTexture() && ! echoIn.IsUpdated())
                if (auto* texture = echoIn.GetSenderTexture())
                {
                    D3D11_TEXTURE2D_DESC desc {};
                    texture->GetDesc (&desc);
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    desc.MiscFlags = 0;
                    ID3D11Texture2D* staging = nullptr;

                    if (SUCCEEDED (device->CreateTexture2D (&desc, nullptr, &staging)))
                    {
                        context->CopyResource (staging, texture);
                        D3D11_MAPPED_SUBRESOURCE mapped {};

                        if (SUCCEEDED (context->Map (staging, 0, D3D11_MAP_READ, 0, &mapped)))
                        {
                            const auto* p = static_cast<const std::uint8_t*> (mapped.pData) + (desc.Height / 2) * mapped.RowPitch + 4 * (desc.Width / 2);
                            std::vector<std::uint8_t> inverted (16 * 16 * 4);

                            for (std::size_t at = 0; at < inverted.size(); at += 4)
                            {
                                inverted[at] = static_cast<std::uint8_t> (255 - p[0]);
                                inverted[at + 1] = static_cast<std::uint8_t> (255 - p[1]);
                                inverted[at + 2] = static_cast<std::uint8_t> (255 - p[2]);
                                inverted[at + 3] = 255;
                            }

                            context->Unmap (staging, 0);

                            D3D11_TEXTURE2D_DESC made {};
                            made.Width = made.Height = 16;
                            made.MipLevels = made.ArraySize = 1;
                            made.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                            made.SampleDesc.Count = 1;
                            made.Usage = D3D11_USAGE_DEFAULT;
                            made.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                            D3D11_SUBRESOURCE_DATA data {};
                            data.pSysMem = inverted.data();
                            data.SysMemPitch = 16 * 4;
                            ID3D11Texture2D* reply = nullptr;

                            if (SUCCEEDED (device->CreateTexture2D (&made, &data, &reply)))
                            {
                                echoOut.SendTexture (reply);
                                reply->Release();
                            }
                        }

                        staging->Release();
                    }
                }

            sg_reset_state_cache();
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        echoOut.ReleaseSender();
        echoIn.ReleaseReceiver();
        echoIn.CloseDirectX11();
        echoOut.CloseDirectX11();
    }

    video::gpu::close();

    //  Black until anything came back; then the fill inside out: 0x204080 -> 0xDFBF7F.
    INFO ("first " << first << ", last " << last);
    CHECK (first == 0u);
    CHECK (std::abs (static_cast<int> ((last >> 16) & 0xffu) - 0xDF) <= 2);
    CHECK (std::abs (static_cast<int> ((last >> 8) & 0xffu) - 0xBF) <= 2);
    CHECK (std::abs (static_cast<int> (last & 0xffu) - 0x7F) <= 2);
}
