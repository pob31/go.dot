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
    THE DEVICE ON macOS: Metal (namespace draft §44.4), the system's default
    device - the one Syphon shares its pictures on. No software rasteriser to
    ask for: a machine with no Metal device has none here, and says so.

    Written to build with or without ARC: every object made here is let go by
    hand where ARC is off, and nothing is relied on to be retained by a cast.
*/

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <wfg/engine/video/render/GpuNative.h>

#include <string>
#include <vector>

#if __has_feature(objc_arc)
 #define WFG_RELEASE(object) (void) 0
#else
 #define WFG_RELEASE(object) [object release]
#endif

namespace wfg::video::gpu::native
{
    namespace
    {
        id<MTLDevice> device = nil;
    }

    const char* backendName() noexcept
    {
        return "Metal";
    }

    bool open (const OpenOptions&, sg_environment& environment, std::string& adapter, std::string& why)
    {
        device = MTLCreateSystemDefaultDevice();

        if (device == nil)
        {
            why = "this Mac has no Metal device";
            return false;
        }

        adapter = [[device name] UTF8String];
        environment.metal.device = (__bridge const void*) device;
        environment.defaults.color_format = SG_PIXELFORMAT_BGRA8;
        return true;
    }

    void close()
    {
        if (device != nil)
        {
            WFG_RELEASE (device);
            device = nil;
        }
    }

    bool readBack (sg_image image, int width, int height, sg_pixel_format format, std::vector<float>& rgba)
    {
        const auto info = sg_mtl_query_image_info (image);
        id<MTLTexture> texture = (__bridge id<MTLTexture>) info.tex[info.active_slot];
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>) sg_mtl_command_queue();

        if (texture == nil || queue == nil || device == nil)
            return false;

        const auto bytesPerRow = static_cast<NSUInteger> (width * bytesPerPixel (format));
        const auto bytes = bytesPerRow * static_cast<NSUInteger> (height);

        @autoreleasepool
        {
            /*  A COPY THE CPU CAN READ: a render target lives where only the
                GPU reaches, so it is blitted into a shared buffer on the queue
                sokol draws on - after the frame sokol committed - and waited
                for. */
            id<MTLBuffer> buffer = [device newBufferWithLength: bytes options: MTLResourceStorageModeShared];

            if (buffer == nil)
                return false;

            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromTexture: texture
                      sourceSlice: 0
                      sourceLevel: 0
                     sourceOrigin: MTLOriginMake (0, 0, 0)
                       sourceSize: MTLSizeMake (static_cast<NSUInteger> (width), static_cast<NSUInteger> (height), 1)
                         toBuffer: buffer
                destinationOffset: 0
           destinationBytesPerRow: bytesPerRow
         destinationBytesPerImage: bytes];
            [blit endEncoding];
            [commands commit];
            [commands waitUntilCompleted];

            const auto* data = static_cast<const std::uint8_t*> ([buffer contents]);
            rgba.assign (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4, 0.0f);

            for (int y = 0; y < height; ++y)
                rowToFloats (data + static_cast<std::size_t> (y) * bytesPerRow, width, format,
                             rgba.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (width) * 4);

            WFG_RELEASE (buffer);
        }

        return true;
    }
}
