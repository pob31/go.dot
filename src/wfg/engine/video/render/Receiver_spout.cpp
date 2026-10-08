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
    A SPOUT RECEIVER (namespace draft §44, YB): SpoutDX on the renderer's own
    Direct3D 11 device, which copies the sender's shared texture into one of its
    own each time a new frame arrives - and that texture is given to sokol as
    it is, made again when the sender changes size. On the same graphics card
    as the sender, as Spout requires (YM).

    No JUCE here: Spout's headers bring windowsx.h's macros.
*/

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <spout/SpoutDX.h>

#include <wfg/engine/video/render/Receiver.h>

#include <chrono>
#include <string>

namespace wfg::video::render
{
    namespace
    {
        double secondsNow() noexcept
        {
            return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        sg_pixel_format formatOf (DXGI_FORMAT format) noexcept
        {
            switch (format)
            {
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8X8_UNORM:     return SG_PIXELFORMAT_BGRA8;
                case DXGI_FORMAT_R8G8B8A8_UNORM:     return SG_PIXELFORMAT_RGBA8;
                case DXGI_FORMAT_R16G16B16A16_UNORM: return SG_PIXELFORMAT_RGBA16;
                case DXGI_FORMAT_R16G16B16A16_FLOAT: return SG_PIXELFORMAT_RGBA16F;
                case DXGI_FORMAT_R32G32B32A32_FLOAT: return SG_PIXELFORMAT_RGBA32F;
                default:                             return SG_PIXELFORMAT_NONE;
            }
        }

        class SpoutReceiver final : public Receiver
        {
        public:
            bool open (const std::string& senderToTake, std::string& why)
            {
                auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));

                if (device == nullptr || ! spout.OpenDirectX11 (device))
                {
                    why = "Spout would not open on the renderer's Direct3D device";
                    return false;
                }

                sender = senderToTake;
                spout.SetReceiverName (sender.c_str());
                return true;
            }

            ~SpoutReceiver() override
            {
                release();
                spout.ReleaseReceiver();
                spout.CloseDirectX11();
            }

            void update() override
            {
                const auto frameBefore = spout.GetSenderFrame();
                spout.ReceiveTexture();

                //  A new sender, or a new size: its texture is made again.
                if (spout.IsUpdated())
                    spout.ReceiveTexture();

                sg_reset_state_cache();
                live = spout.IsConnected();

                if (! live)
                {
                    release();
                    return;
                }

                auto* texture = spout.GetSenderTexture();

                if (texture == nullptr)
                    return;

                if (texture != injected)
                {
                    release();

                    D3D11_TEXTURE2D_DESC desc {};
                    texture->GetDesc (&desc);
                    const auto format = formatOf (desc.Format);

                    if (format == SG_PIXELFORMAT_NONE)
                    {
                        trouble = "the sender's pictures are in a format Go.dot does not draw";
                        return;
                    }

                    sg_image_desc made {};
                    made.width = static_cast<int> (desc.Width);
                    made.height = static_cast<int> (desc.Height);
                    made.pixel_format = format;
                    made.d3d11_texture = texture;
                    image = sg_make_image (made);

                    sg_view_desc view {};
                    view.texture.image = image;
                    textureView = sg_make_view (view);

                    injected = texture;
                    pictureWidth = made.width;
                    pictureHeight = made.height;
                    trouble.clear();
                }

                if (spout.GetSenderFrame() != frameBefore)
                    arrivals.arrived (secondsNow());
            }

            sg_view picture() const override     { return textureView; }
            int width() const override           { return pictureWidth; }
            int height() const override          { return pictureHeight; }
            bool connected() const override      { return live && textureView.id != SG_INVALID_ID; }
            double frameRate() const override    { return arrivals.rate (secondsNow()); }
            double age() const override          { return arrivals.age (secondsNow()); }

            std::string problem() const override
            {
                if (! trouble.empty())
                    return trouble;

                return live ? std::string {} : "nothing is sending over Spout as " + sender;
            }

        private:
            void release()
            {
                if (textureView.id != SG_INVALID_ID)
                    sg_destroy_view (textureView);

                if (image.id != SG_INVALID_ID)
                    sg_destroy_image (image);

                textureView = {};
                image = {};
                injected = nullptr;
                pictureWidth = pictureHeight = 0;
            }

            spoutDX spout;
            std::string sender;
            std::string trouble;
            ID3D11Texture2D* injected = nullptr;
            sg_image image {};
            sg_view textureView {};
            int pictureWidth = 0;
            int pictureHeight = 0;
            bool live = false;
            ArrivalRate arrivals;
        };
    }

    std::unique_ptr<Receiver> makeSpoutReceiver (const std::string& sender, std::string& why)
    {
        auto receiver = std::make_unique<SpoutReceiver>();

        if (! receiver->open (sender, why))
            return nullptr;

        return receiver;
    }

    std::string discoverSpout()
    {
        //  Spout's list of senders is shared memory any instance reads.
        static spoutDX lister;
        std::string out;

        for (const auto& name : lister.GetSenderList())
            if (! name.empty())
                out += "spout\t" + name + "\n";

        return out;
    }
}
