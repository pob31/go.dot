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
    A PROJECTOR ON macOS (namespace draft §44.4): a view backed by a Metal
    layer, filling the JUCE window, passing the pointer through to it; the
    render thread takes the layer's next drawable each frame and sokol shows
    it at the end of the pass. A DISPLAY LINK on each projector's display says
    when that display refreshes (YJ), so each is drawn for its own.

    Written to build with or without ARC.
*/

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <wfg/engine/video/render/Projector.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#if __has_feature(objc_arc)
 #define WFG_RETAIN(object) (object)
 #define WFG_RELEASE(object) (void) 0
#else
 #define WFG_RETAIN(object) [(object) retain]
 #define WFG_RELEASE(object) [(object) release]
#endif

extern "C" void* objc_autoreleasePoolPush (void);
extern "C" void objc_autoreleasePoolPop (void*);

@interface GoDotProjectorView : NSView
@end

@implementation GoDotProjectorView
- (CALayer*) makeBackingLayer       { return [CAMetalLayer layer]; }
- (BOOL) wantsUpdateLayer           { return YES; }
- (BOOL) isOpaque                   { return YES; }
- (NSView*) hitTest: (NSPoint) point { (void) point; return nil; }
@end

namespace wfg::video::render
{
    namespace
    {
        /*  ONE SEMAPHORE for every display link: signalled at any projector's
            refresh, waited on by the render thread. */
        dispatch_semaphore_t refreshed()
        {
            static dispatch_semaphore_t semaphore = dispatch_semaphore_create (0);
            return semaphore;
        }

        CAMetalLayer* layerOf (const NativeView& view)
        {
            auto* nsView = (__bridge NSView*) view.handle;
            return (CAMetalLayer*) [nsView layer];
        }

       #pragma clang diagnostic push
       #pragma clang diagnostic ignored "-Wdeprecated-declarations"

        class MetalSurface final : public Surface
        {
        public:
            MetalSurface (CAMetalLayer* layerToDrawIn, std::uint32_t displayToFollow)
                : layer (WFG_RETAIN (layerToDrawIn)), display (displayToFollow)
            {
                layer.device = (__bridge id<MTLDevice>) sg_mtl_device();

                /*  ITS DISPLAY'S REFRESH: a display link on the display the
                    window is on, signalling the render thread. */
                if (CVDisplayLinkCreateWithCGDisplay (display, &link) == kCVReturnSuccess)
                {
                    CVDisplayLinkSetOutputCallback (link, &MetalSurface::onRefresh, this);
                    CVDisplayLinkStart (link);
                }
            }

            ~MetalSurface() override
            {
                if (link != nullptr)
                {
                    CVDisplayLinkStop (link);
                    CVDisplayLinkRelease (link);
                }

                if (drawable != nil)
                    WFG_RELEASE (drawable);

                WFG_RELEASE (layer);
            }

            bool acquire (sg_swapchain& swapchain) override
            {
                const auto size = layer.drawableSize;

                if (size.width < 1.0 || size.height < 1.0)
                    return false;

                id<CAMetalDrawable> next = [layer nextDrawable];

                if (next == nil)
                    return false;

                drawable = WFG_RETAIN (next);

                swapchain = {};
                swapchain.width = static_cast<int> (size.width);
                swapchain.height = static_cast<int> (size.height);
                swapchain.color_format = SG_PIXELFORMAT_BGRA8;
                swapchain.depth_format = SG_PIXELFORMAT_NONE;
                swapchain.sample_count = 1;
                swapchain.metal.current_drawable = (__bridge const void*) drawable;
                return true;
            }

            //  sokol showed it at the end of the pass; here it is only let go.
            void present() override
            {
                if (drawable != nil)
                    WFG_RELEASE (drawable);

                drawable = nil;
            }

            std::atomic<bool> refreshedSince { false };

        private:
            static CVReturn onRefresh (CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags,
                                       CVOptionFlags*, void* context)
            {
                static_cast<MetalSurface*> (context)->refreshedSince.store (true, std::memory_order_release);
                dispatch_semaphore_signal (refreshed());
                return kCVReturnSuccess;
            }

            CAMetalLayer* layer = nil;
            std::uint32_t display = 0;
            CVDisplayLinkRef link = nullptr;
            id<CAMetalDrawable> drawable = nil;
        };

       #pragma clang diagnostic pop
    }

    //==============================================================================
    void* nativeDisplay()
    {
        return nullptr;
    }

    NativeView makeNativeView (void* parentWindow, int, int)
    {
        NativeView made;
        auto* parent = (__bridge NSView*) parentWindow;

        if (parent == nil)
            return made;

        auto* view = [[GoDotProjectorView alloc] initWithFrame: [parent bounds]];
        [view setWantsLayer: YES];
        [view setAutoresizingMask: NSViewWidthSizable | NSViewHeightSizable];
        [parent addSubview: view];

        auto* layer = (CAMetalLayer*) [view layer];
        const auto scale = [[parent window] backingScaleFactor];
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake ([parent bounds].size.width * scale, [parent bounds].size.height * scale);
        layer.displaySyncEnabled = YES;
        layer.maximumDrawableCount = 3;

        //  Black until the device's first frame (the flash Windows showed, 2026-10-08).
        layer.backgroundColor = CGColorGetConstantColor (kCGColorBlack);

        if (auto* screen = [[parent window] screen])
            made.display = [[[screen deviceDescription] objectForKey: @"NSScreenNumber"] unsignedIntValue];

        made.handle = (__bridge void*) view;    // held by its parent, and by this until destroyed
        return made;
    }

    void fitNativeView (const NativeView& view, void* parentWindow, int, int)
    {
        auto* parent = (__bridge NSView*) parentWindow;
        auto* nsView = (__bridge NSView*) view.handle;

        if (parent == nil || nsView == nil)
            return;

        [nsView setFrame: [parent bounds]];
        const auto scale = [[parent window] backingScaleFactor];
        layerOf (view).contentsScale = scale;
        layerOf (view).drawableSize = CGSizeMake ([parent bounds].size.width * scale, [parent bounds].size.height * scale);
    }

    void destroyNativeView (NativeView& view)
    {
        if (auto* nsView = (__bridge NSView*) view.handle)
        {
            [nsView removeFromSuperview];
            WFG_RELEASE (nsView);
        }

        view = {};
    }

    std::unique_ptr<Surface> makeSurface (const NativeView& view, std::string& why)
    {
        if (view.handle == nullptr)
        {
            why = "the projector's window has no view to draw into";
            return nullptr;
        }

        return std::make_unique<MetalSurface> (layerOf (view), view.display);
    }

    bool waitForRefresh (std::vector<Surface*>& surfaces, double, int timeoutMs)
    {
        const auto timeout = dispatch_time (DISPATCH_TIME_NOW, static_cast<std::int64_t> (std::max (1, timeoutMs)) * 1000000);

        if (dispatch_semaphore_wait (refreshed(), timeout) != 0)
            return false;

        //  Every surface whose display refreshed since it was last drawn.
        auto any = false;

        for (auto* surface : surfaces)
            if (static_cast<MetalSurface*> (surface)->refreshedSince.exchange (false, std::memory_order_acq_rel))
            {
                surface->due = true;
                any = true;
            }

        return any;
    }

    FramePool::FramePool() : pool (objc_autoreleasePoolPush()) {}
    FramePool::~FramePool()  { objc_autoreleasePoolPop (pool); }
}
