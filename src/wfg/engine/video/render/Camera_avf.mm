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
    CAMERAS ON MACOS, THROUGH AVFOUNDATION (namespace draft §72, APS).

    Listed by a discovery session - built-in, external and Continuity cameras -
    by name and unique ID, which is the camera's identifier after `cam:`;
    listing lights nothing. Opened, its formats are read - each size at its
    fastest rate - the one asked for made the device's active format, and a
    capture session hands each frame, BGRA, to a delegate on a queue of its own.

    PERMISSION. macOS asks the user once whether Go.dot may use cameras
    (NSCameraUsageDescription in Info.plist; the hardened runtime's camera
    entitlement): until it is given the input says so, and is tried again
    every two seconds as any input that cannot open is; refused, it says where
    to allow it.

    Built by CI, owed to the author's Mac mini. Written to build with or
    without ARC.
*/

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <wfg/engine/video/render/Camera.h>

#include <wfg/engine/video/VideoModes.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#if __has_feature(objc_arc)
 #define WFG_RELEASE(object) (void) 0
#else
 #define WFG_RELEASE(object) [(object) release]
#endif

namespace wfg::video::camera { class AvfSource; }

/*  The capture session's delegate: each frame handed to its source, under the
    source's lock, until the source lets go of it. */
@interface GoDotCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
@public
    std::mutex* lock;
    wfg::video::camera::AvfSource* owner;
}
@end

namespace wfg::video::camera
{
    class AvfSource final : public Source
    {
    public:
        AvfSource (std::string wordToUse, std::vector<std::string> formatsToUse)
            : word (std::move (wordToUse)), offered (std::move (formatsToUse)) {}

        ~AvfSource() override
        {
            if (session != nil)
                [session stopRunning];

            if (output != nil)
                [output setSampleBufferDelegate: nil queue: nil];

            //  A frame already on the queue runs before the delegate lets go:
            //  nothing reaches this source, or its lock, once it has gone.
            if (queue != nullptr)
                dispatch_sync (queue, ^{});

            if (delegate != nil)
            {
                const std::scoped_lock hold (lock);
                delegate->owner = nullptr;
                delegate->lock = nullptr;
            }

            WFG_RELEASE (output);
            WFG_RELEASE (input);
            WFG_RELEASE (session);
            WFG_RELEASE (delegate);

            if (queue != nil)
            {
               #if ! __has_feature(objc_arc)
                dispatch_release (queue);
               #endif
            }
        }

        bool begin (AVCaptureDevice* device, AVCaptureDeviceFormat* format, CMTime frameDuration, std::string& why)
        {
            NSError* error = nil;
            input = [[AVCaptureDeviceInput alloc] initWithDevice: device error: &error];

            if (input == nil)
            {
                why = "the camera would not open: another program may have it";
                return false;
            }

            session = [[AVCaptureSession alloc] init];
            output = [[AVCaptureVideoDataOutput alloc] init];
            output.videoSettings = @{ (NSString*) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
            output.alwaysDiscardsLateVideoFrames = YES;

            delegate = [[GoDotCameraDelegate alloc] init];
            delegate->lock = &lock;
            delegate->owner = this;
            queue = dispatch_queue_create ("go.dot camera", DISPATCH_QUEUE_SERIAL);
            [output setSampleBufferDelegate: delegate queue: queue];

            [session beginConfiguration];

            if (! [session canAddInput: input] || ! [session canAddOutput: output])
            {
                [session commitConfiguration];
                why = "the camera would not be read";
                return false;
            }

            [session addInput: input];
            [session addOutput: output];

            if ([device lockForConfiguration: &error])
            {
                device.activeFormat = format;
                device.activeVideoMinFrameDuration = frameDuration;
                device.activeVideoMaxFrameDuration = frameDuration;
                [device unlockForConfiguration];
            }

            [session commitConfiguration];
            [session startRunning];
            return true;
        }

        /*  On the delegate's queue, under the lock. */
        void arrived (CMSampleBufferRef sample)
        {
            CVImageBufferRef image = CMSampleBufferGetImageBuffer (sample);

            if (image == nullptr || CVPixelBufferLockBaseAddress (image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
                return;

            const auto w = static_cast<int> (CVPixelBufferGetWidth (image));
            const auto h = static_cast<int> (CVPixelBufferGetHeight (image));
            const auto rowBytes = CVPixelBufferGetBytesPerRow (image);
            const auto* base = static_cast<const std::uint8_t*> (CVPixelBufferGetBaseAddress (image));

            latest.resize (static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4);

            for (int y = 0; y < h && base != nullptr; ++y)
            {
                auto* to = latest.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (w) * 4;
                std::memcpy (to, base + static_cast<std::size_t> (y) * rowBytes, static_cast<std::size_t> (w) * 4);

                for (std::size_t x = 3; x < static_cast<std::size_t> (w) * 4; x += 4)
                    to[x] = 255;
            }

            CVPixelBufferUnlockBaseAddress (image, kCVPixelBufferLock_ReadOnly);
            latestWidth = w;
            latestHeight = h;
            ++frameSerial;
            arrivedAt.store (nowNanos());
        }

        bool newest (std::vector<std::uint8_t>& bgra, int& width, int& height, std::uint64_t& serial) override
        {
            const std::scoped_lock hold (lock);

            if (frameSerial == serial || latest.empty())
                return false;

            bgra = latest;
            width = latestWidth;
            height = latestHeight;
            serial = frameSerial;
            return true;
        }

        bool signal() override
        {
            return running() && arrivedLately (arrivedAt.load());
        }

        std::string formatWord() override             { return word; }

        std::string problem() override
        {
            return running() ? std::string() : std::string ("the camera stopped: unplugged, or taken by another program");
        }

        std::vector<std::string> formats() override   { return offered; }

    private:
        bool running() const
        {
            //  isInterrupted is iOS's: a Mac's session that loses its camera stops running.
            return session != nil && session.isRunning;
        }

        const std::string word;
        const std::vector<std::string> offered;
        std::atomic<std::int64_t> arrivedAt { 0 };
        AVCaptureSession* session = nil;
        AVCaptureDeviceInput* input = nil;
        AVCaptureVideoDataOutput* output = nil;
        GoDotCameraDelegate* delegate = nil;
        dispatch_queue_t queue = nullptr;

        std::mutex lock;
        std::vector<std::uint8_t> latest;
        int latestWidth = 0;
        int latestHeight = 0;
        std::uint64_t frameSerial = 0;
    };
}

@implementation GoDotCameraDelegate
- (void) captureOutput: (AVCaptureOutput*) captureOutput
 didOutputSampleBuffer: (CMSampleBufferRef) sampleBuffer
        fromConnection: (AVCaptureConnection*) connection
{
    (void) captureOutput;
    (void) connection;

    if (lock == nullptr)
        return;

    const std::scoped_lock hold (*lock);

    if (owner != nullptr)
        owner->arrived (sampleBuffer);
}
@end

namespace wfg::video::camera
{
    namespace
    {
        NSArray<AVCaptureDevice*>* devices()
        {
            NSArray<AVCaptureDeviceType>* types = nil;

            if (@available (macOS 14.0, *))
                types = @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeContinuityCamera ];
            else
                types = @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown ];

            auto* discovery = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes: types
                                                                                     mediaType: AVMediaTypeVideo
                                                                                      position: AVCaptureDevicePositionUnspecified];
            return discovery.devices;
        }

        std::string text (NSString* value)
        {
            return value != nil ? std::string ([value UTF8String]) : std::string();
        }

        /*  Each format of the device by its word - its size at its fastest
            rate - and the format and duration that open it. */
        struct Offer
        {
            AVCaptureDeviceFormat* format = nil;
            CMTime duration {};
        };

        std::map<std::string, Offer> offersOf (AVCaptureDevice* device, std::vector<std::string>& order)
        {
            std::map<std::string, Offer> offers;

            for (AVCaptureDeviceFormat* format in device.formats)
            {
                const auto size = CMVideoFormatDescriptionGetDimensions (format.formatDescription);

                for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
                {
                    const auto duration = range.minFrameDuration;

                    if (duration.value <= 0 || duration.timescale <= 0)
                        continue;

                    modes::Format words;
                    words.width = size.width;
                    words.height = size.height;
                    words.frameDuration = duration.value;
                    words.timeScale = duration.timescale;
                    const auto word = modes::wordFor (words);

                    if (word.empty() || offers.count (word) != 0)
                        continue;

                    offers[word] = { format, duration };
                    order.push_back (word);
                }
            }

            return offers;
        }

        class AvfCameras final : public System
        {
        public:
            Found find() override
            {
                Found found;

                @autoreleasepool
                {
                    for (AVCaptureDevice* device in devices())
                    {
                        Camera camera;
                        camera.id = idPrefix + text (device.uniqueID);
                        camera.name = text (device.localizedName);

                        const std::scoped_lock hold (lock);

                        if (const auto known = formatsKnown.find (camera.id); known != formatsKnown.end())
                            camera.formats = known->second;

                        found.cameras.push_back (std::move (camera));
                    }
                }

                return found;
            }

            std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) override
            {
                @autoreleasepool
                {
                    //  ASKED ONCE: until the user answers, said, and tried again.
                    const auto status = [AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeVideo];

                    if (status == AVAuthorizationStatusNotDetermined)
                    {
                        [AVCaptureDevice requestAccessForMediaType: AVMediaTypeVideo completionHandler: ^(BOOL) {}];
                        why = "waiting for permission to use the camera";
                        return nullptr;
                    }

                    if (status != AVAuthorizationStatusAuthorized)
                    {
                        why = "macOS does not let Go.dot use the camera: allow it in System Settings, Privacy & Security, Camera";
                        return nullptr;
                    }

                    AVCaptureDevice* chosen = nil;

                    for (AVCaptureDevice* device in devices())
                        if (chosen == nil && idPrefix + text (device.uniqueID) == id)
                            chosen = device;

                    if (chosen == nil)
                    {
                        why = "the camera is not connected";
                        return nullptr;
                    }

                    std::vector<std::string> order;
                    const auto offers = offersOf (chosen, order);

                    {
                        const std::scoped_lock hold (lock);
                        formatsKnown[id] = order;
                    }

                    const auto word = format == modes::automatic || format.empty() ? chooseAutomatic (order) : format;
                    const auto picked = offers.find (word);

                    if (picked == offers.end())
                    {
                        why = word.empty() ? std::string ("the camera offers no format Go.dot reads") : "the camera does not offer " + word;
                        return nullptr;
                    }

                    auto source = std::make_unique<AvfSource> (word, order);

                    if (! source->begin (chosen, picked->second.format, picked->second.duration, why))
                        return nullptr;

                    return source;
                }
            }

        private:
            std::mutex lock;
            std::map<std::string, std::vector<std::string>> formatsKnown;
        };
    }

    std::unique_ptr<System> makeSystemCameras()
    {
        return std::make_unique<AvfCameras>();
    }
}
