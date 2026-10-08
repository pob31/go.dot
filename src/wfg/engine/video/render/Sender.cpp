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

#include <wfg/engine/video/render/Sender.h>

namespace wfg::video::render
{
    std::unique_ptr<Sender> makeSender (region::OutputKind kind, const std::string& name, double frameRate, std::string& why)
    {
        switch (kind)
        {
            case region::OutputKind::spout:   return makeSpoutSender (name, why);
            case region::OutputKind::syphon:  return makeSyphonSender (name, why);
            case region::OutputKind::ndi:     return makeNdiSender (name, frameRate, why);
            case region::OutputKind::display: break;
        }

        why = "a display is not sent";
        return nullptr;
    }

    sg_pixel_format senderFormat()
    {
        return sg_query_pixelformat (SG_PIXELFORMAT_BGRA8).render ? SG_PIXELFORMAT_BGRA8 : SG_PIXELFORMAT_RGBA8;
    }

   #if ! defined (_WIN32)
    std::unique_ptr<Sender> makeSpoutSender (const std::string&, std::string& why)
    {
        why = "Spout is Windows's: this system has none";
        return nullptr;
    }
   #endif

   #if ! defined (__APPLE__)
    std::unique_ptr<Sender> makeSyphonSender (const std::string&, std::string& why)
    {
        why = "Syphon is the Mac's: this system has none";
        return nullptr;
    }
   #endif
}
