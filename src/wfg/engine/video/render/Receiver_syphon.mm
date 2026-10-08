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
    A SYPHON RECEIVER (namespace draft §44, YB): Syphon's Metal client on the
    renderer's own device, the server found in Syphon's directory by the name
    `videoInputs/available` lists - the application's name, " - ", and the
    server's, or the application's alone. Each new frame is a Metal texture on
    the IOSurface the server shares, given to sokol as it is.

    Built by CI, owed to the author's Mac mini. Written to build with or
    without ARC.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Syphon/SyphonMetalClient.h>
#import <Syphon/SyphonServerDirectory.h>

#include <wfg/engine/video/render/Receiver.h>

#include <chrono>
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
        double secondsNow() noexcept
        {
            return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        /*  A SERVER'S NAME as Go.dot lists it. */
        std::string nameOf (NSDictionary* description)
        {
            NSString* app = [description objectForKey: SyphonServerDescriptionAppNameKey];
            NSString* name = [description objectForKey: SyphonServerDescriptionNameKey];
            const std::string appText = app != nil ? [app UTF8String] : "";
            const std::string nameText = name != nil ? [name UTF8String] : "";

            if (nameText.empty())
                return appText;

            return appText.empty() ? nameText : appText + " - " + nameText;
        }

        class SyphonReceiver final : public Receiver
        {
        public:
            explicit SyphonReceiver (std::string senderToTake) : sender (std::move (senderToTake)) {}

            ~SyphonReceiver() override
            {
                release();
                stopClient();
            }

            void update() override
            {
                if (client == nil || ! [client isValid])
                {
                    stopClient();

                    //  Looked for again each time until it is there.
                    for (NSDictionary* description in [[SyphonServerDirectory sharedDirectory] servers])
                        if (nameOf (description) == sender)
                        {
                            client = [[SyphonMetalClient alloc] initWithServerDescription: description
                                                                                   device: (__bridge id<MTLDevice>) sg_mtl_device()
                                                                                  options: nil
                                                                          newFrameHandler: nil];
                            break;
                        }

                    if (client == nil)
                    {
                        release();
                        return;
                    }
                }

                if (! [client hasNewFrame])
                    return;

                id<MTLTexture> texture = [client newFrameImage];

                if (texture == nil)
                    return;

                release();

                sg_image_desc made {};
                made.width = static_cast<int> ([texture width]);
                made.height = static_cast<int> ([texture height]);
                made.pixel_format = SG_PIXELFORMAT_BGRA8;
                made.mtl_texture = (__bridge const void*) texture;
                image = sg_make_image (made);

                sg_view_desc view {};
                view.texture.image = image;
                textureView = sg_make_view (view);
                pictureWidth = made.width;
                pictureHeight = made.height;
                arrivals.arrived (secondsNow());

                //  sokol holds its own; this one was handed over by `new`.
                WFG_RELEASE (texture);
            }

            sg_view picture() const override     { return textureView; }
            int width() const override           { return pictureWidth; }
            int height() const override          { return pictureHeight; }
            bool connected() const override      { return client != nil && textureView.id != SG_INVALID_ID; }
            double frameRate() const override    { return arrivals.rate (secondsNow()); }

            std::string problem() const override
            {
                return client != nil ? std::string {} : "nothing is sending over Syphon as " + sender;
            }

        private:
            void stopClient()
            {
                if (client != nil)
                {
                    [client stop];
                    WFG_RELEASE (client);
                    client = nil;
                }
            }

            void release()
            {
                if (textureView.id != SG_INVALID_ID)
                    sg_destroy_view (textureView);

                if (image.id != SG_INVALID_ID)
                    sg_destroy_image (image);

                textureView = {};
                image = {};
            }

            const std::string sender;
            SyphonMetalClient* client = nil;
            sg_image image {};
            sg_view textureView {};
            int pictureWidth = 0;
            int pictureHeight = 0;
            ArrivalRate arrivals;
        };
    }

    std::unique_ptr<Receiver> makeSyphonReceiver (const std::string& sender, std::string& why)
    {
        if (sg_mtl_device() == nullptr)
        {
            why = "Syphon has no Metal device to receive on";
            return nullptr;
        }

        return std::make_unique<SyphonReceiver> (sender);
    }

    std::string discoverSyphon()
    {
        std::string out;

        @autoreleasepool
        {
            for (NSDictionary* description in [[SyphonServerDirectory sharedDirectory] servers])
            {
                const auto name = nameOf (description);

                if (! name.empty())
                    out += "syphon\t" + name + "\n";
            }
        }

        return out;
    }
}
