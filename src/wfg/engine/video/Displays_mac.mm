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

/*  THE MAC'S HALF OF Displays.cpp (Phase 8a, decision VL): each screen's
    localised name - "DELL U2720Q", "Built-in Retina Display" - and its display
    identifier, with its frame turned from AppKit's bottom-left coordinates to
    JUCE's top-left ones, so Displays.cpp can match the two lists by where each
    screen is. The identifier is the display's vendor, model and serial: what
    stays the same while the same projector is plugged in, wherever. */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <string>
#include <vector>

namespace wfg::video
{
    struct MacScreen
    {
        std::string name;
        std::string id;
        int x = 0, y = 0, width = 0, height = 0;
    };

    std::vector<MacScreen> macScreens()
    {
        std::vector<MacScreen> out;

        @autoreleasepool
        {
            NSArray<NSScreen*>* screens = [NSScreen screens];

            if (screens.count == 0)
                return out;

            /*  AppKit counts y up from the bottom of the FIRST screen, JUCE
                down from its top. */
            const auto mainHeight = screens[0].frame.size.height;

            for (NSScreen* screen in screens)
            {
                MacScreen entry;
                const auto frame = screen.frame;

                entry.x = static_cast<int> (std::lround (frame.origin.x));
                entry.y = static_cast<int> (std::lround (mainHeight - (frame.origin.y + frame.size.height)));
                entry.width = static_cast<int> (std::lround (frame.size.width));
                entry.height = static_cast<int> (std::lround (frame.size.height));

                if (NSString* name = screen.localizedName; name != nil)
                    entry.name = std::string ([name UTF8String]);

                if (NSNumber* number = screen.deviceDescription[@"NSScreenNumber"]; number != nil)
                {
                    const auto display = static_cast<CGDirectDisplayID> ([number unsignedIntValue]);
                    entry.id = "CG:" + std::to_string (CGDisplayVendorNumber (display)) + ":"
                                 + std::to_string (CGDisplayModelNumber (display)) + ":"
                                 + std::to_string (CGDisplaySerialNumber (display));
                }

                out.push_back (std::move (entry));
            }
        }

        return out;
    }
}
