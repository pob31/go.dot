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

//  GoDotSyphonServer.h says why. The surface and the blit are SyphonMetalServer's (BSD).

#import "GoDotSyphonServer.h"

#import <IOSurface/IOSurface.h>
#import <Syphon/SyphonSubclassing.h>

@implementation GoDotSyphonServer
{
    id<MTLDevice> _device;
    id<MTLTexture> _surfaceTexture;
}

- (nullable instancetype) initWithName: (NSString*) name device: (id<MTLDevice>) device
{
    self = [super initWithName: name options: nil];

    if (self)
        _device = device;

    return self;
}

- (void) dealloc
{
    @synchronized (self) {
        _surfaceTexture = nil;
    }
}

- (void) stop
{
    @synchronized (self) {
        _surfaceTexture = nil;
    }

    [super stop];
}

//  The IOSurface Syphon shares, as a Metal texture - made again at a new size.
- (nullable id<MTLTexture>) surfaceOfWidth: (NSUInteger) width height: (NSUInteger) height
{
    @synchronized (self) {
        if (_surfaceTexture != nil && (_surfaceTexture.width != width || _surfaceTexture.height != height))
            _surfaceTexture = nil;

        if (_surfaceTexture == nil)
        {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat: MTLPixelFormatBGRA8Unorm
                                                                                                  width: width
                                                                                                 height: height
                                                                                              mipmapped: NO];
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            IOSurfaceRef surface = [self newSurfaceForWidth: width height: height options: nil];

            if (surface)
            {
                _surfaceTexture = [_device newTextureWithDescriptor: descriptor iosurface: surface plane: 0];
                CFRelease (surface);
            }
        }

        return _surfaceTexture;
    }
}

- (BOOL) publishTexture: (id<MTLTexture>) texture
        onCommandBuffer: (id<MTLCommandBuffer>) commands
                  width: (NSUInteger) width
                 height: (NSUInteger) height
{
    width = MIN (width, texture.width);
    height = MIN (height, texture.height);

    if (width == 0 || height == 0 || texture.pixelFormat != MTLPixelFormatBGRA8Unorm || texture.sampleCount != 1)
        return NO;

    id<MTLTexture> destination = [self surfaceOfWidth: width height: height];

    if (destination == nil)
        return NO;

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture: texture
              sourceSlice: 0
              sourceLevel: 0
             sourceOrigin: MTLOriginMake (0, 0, 0)
               sourceSize: MTLSizeMake (width, height, 1)
                toTexture: destination
         destinationSlice: 0
         destinationLevel: 0
        destinationOrigin: MTLOriginMake (0, 0, 0)];
    [blit endEncoding];

    __weak GoDotSyphonServer* weakSelf = self;
    [commands addCompletedHandler: ^(id<MTLCommandBuffer> done) {
        (void) done;
        [weakSelf publish];
    }];

    return YES;
}

@end
