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
    A SYPHON SERVER THAT ONLY BLITS (namespace draft §44, YA): Syphon's own
    base class, through its subclassing interface, publishing a Metal texture
    the renderer drew - BGRA, one sample, the right way up - by a plain copy
    into the IOSurface it shares. Syphon's SyphonMetalServer does the same and
    also makes, whether it is needed or not, a renderer for textures it cannot
    copy, whose shaders it loads from Syphon.framework's bundle: linked into
    Go.dot there is no such bundle. So this, and never that.

    Built in wfg_syphon, with ARC, as Syphon's own sources are.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonServerBase.h>

NS_ASSUME_NONNULL_BEGIN

@interface GoDotSyphonServer : SyphonServerBase

- (nullable instancetype) initWithName: (NSString*) name device: (id<MTLDevice>) device;

/*  `texture`'s top-left `width` by `height`, copied on `commands` and
    published once they are done; NO when it could not be. */
- (BOOL) publishTexture: (id<MTLTexture>) texture
        onCommandBuffer: (id<MTLCommandBuffer>) commands
                  width: (NSUInteger) width
                 height: (NSUInteger) height;

@end

NS_ASSUME_NONNULL_END
