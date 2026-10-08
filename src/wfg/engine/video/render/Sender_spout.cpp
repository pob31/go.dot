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
    A SPOUT SENDER (namespace draft §44, YA): SpoutDX (ThirdParty/spout, BSD),
    opened on the renderer's own Direct3D 11 device, so the output's picture is
    copied on the graphics card into the texture Spout shares - no trip
    through memory. Other programs find it by its name in Spout's list.

    No JUCE here: Spout's headers bring windowsx.h's macros.
*/

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <spout/SpoutDX.h>

#include <wfg/engine/video/render/Sender.h>

#include <string>

namespace wfg::video::render
{
    namespace
    {
        class SpoutSender final : public Sender
        {
        public:
            bool open (const std::string& name, std::string& why)
            {
                auto* device = static_cast<ID3D11Device*> (const_cast<void*> (sg_d3d11_device()));

                if (device == nullptr || ! spout.OpenDirectX11 (device))
                {
                    why = "Spout would not open on the renderer's Direct3D device";
                    return false;
                }

                if (! spout.SetSenderName (name.c_str()))
                {
                    why = "Spout would not take the name " + name;
                    return false;
                }

                return true;
            }

            ~SpoutSender() override
            {
                spout.ReleaseSender();
                spout.CloseDirectX11();
            }

            bool send (sg_image picture, int, int) override
            {
                auto* texture = static_cast<ID3D11Texture2D*> (const_cast<void*> (sg_d3d11_query_image_info (picture).tex2d));

                if (texture == nullptr)
                    return false;

                const auto sent = spout.SendTexture (texture);

                //  Spout used the device's context behind sokol's back.
                sg_reset_state_cache();
                return sent;
            }

        private:
            spoutDX spout;
        };
    }

    std::unique_ptr<Sender> makeSpoutSender (const std::string& name, std::string& why)
    {
        auto sender = std::make_unique<SpoutSender>();

        if (! sender->open (name, why))
            return nullptr;

        return sender;
    }
}
