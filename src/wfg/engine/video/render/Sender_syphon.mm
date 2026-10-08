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
    A SYPHON SENDER (namespace draft §44, YA): a Syphon server (ThirdParty/
    Syphon, BSD; GoDotSyphonServer.h says why it is Go.dot's own on Syphon's
    base class) on the renderer's own Metal device. The output's picture -
    BGRA, one sample, the right way up - is copied into the IOSurface Syphon
    shares, on a command buffer of its own on sokol's queue, after the frame
    sokol committed. Other programs find it by its name in Syphon's directory.

    Written to build with or without ARC.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <wfg/engine/video/render/GoDotSyphonServer.h>

#include <wfg/engine/video/render/Sender.h>

#include <string>

#if __has_feature(objc_arc)
 #define WFG_RELEASE(object) (void) 0
#else
 #define WFG_RELEASE(object) [(object) release]
#endif

namespace wfg::video::render
{
    namespace
    {
        class SyphonSender final : public Sender
        {
        public:
            bool open (const std::string& name, std::string& why)
            {
                id<MTLDevice> device = (__bridge id<MTLDevice>) sg_mtl_device();

                if (device == nil)
                {
                    why = "Syphon has no Metal device to share from";
                    return false;
                }

                server = [[GoDotSyphonServer alloc] initWithName: [NSString stringWithUTF8String: name.c_str()]
                                                          device: device];

                if (server == nil)
                {
                    why = "Syphon would not make a server named " + name;
                    return false;
                }

                return true;
            }

            ~SyphonSender() override
            {
                if (server != nil)
                {
                    [server stop];
                    WFG_RELEASE (server);
                }
            }

            bool send (sg_image picture, int width, int height) override
            {
                const auto info = sg_mtl_query_image_info (picture);
                id<MTLTexture> texture = (__bridge id<MTLTexture>) info.tex[info.active_slot];
                id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>) sg_mtl_command_queue();

                if (texture == nil || queue == nil)
                    return false;

                auto published = false;

                @autoreleasepool
                {
                    id<MTLCommandBuffer> commands = [queue commandBuffer];
                    published = [server publishTexture: texture
                                       onCommandBuffer: commands
                                                 width: static_cast<NSUInteger> (width)
                                                height: static_cast<NSUInteger> (height)];
                    [commands commit];
                }

                return published;
            }

        private:
            GoDotSyphonServer* server = nil;
        };
    }

    std::unique_ptr<Sender> makeSyphonSender (const std::string& name, std::string& why)
    {
        auto sender = std::make_unique<SyphonSender>();

        if (! sender->open (name, why))
            return nullptr;

        return sender;
    }
}
