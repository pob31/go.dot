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

//  The Mac's half of WindowApplication.h: finishing the launch.

#include <wfg/engine/app/WindowApplication.h>

#import <AppKit/AppKit.h>

namespace wfg::app
{
    std::string WindowApplication::describeSystemDelegate()
    {
        @autoreleasepool
        {
            id delegate = NSApp != nil ? [NSApp delegate] : nil;
            return delegate != nil ? std::string ([NSStringFromClass ([delegate class]) UTF8String])
                                   : std::string ("none");
        }
    }

    void WindowApplication::finishLaunching()
    {
        @autoreleasepool
        {
            if ([[NSRunningApplication currentApplication] isFinishedLaunching])
                return;

            /*  RUN UNTIL LAUNCHED, THEN STOP - the way GLFW finishes a launch
                before it has windows to show. The files a double-click started
                Go.dot for arrive between "will" and "did finish launching"
                (application:openFiles:, before applicationDidFinishLaunching:),
                so by the time this observer runs they have been handed over.
                -stop: takes effect after the event being handled, so an empty
                one is posted to be that event. */
            id observer = [[NSNotificationCenter defaultCenter]
                             addObserverForName: NSApplicationDidFinishLaunchingNotification
                                          object: NSApp
                                           queue: nil
                                      usingBlock: ^(NSNotification*)
                                      {
                                          [NSApp stop: nil];

                                          NSEvent* wake = [NSEvent otherEventWithType: NSEventTypeApplicationDefined
                                                                             location: NSZeroPoint
                                                                        modifierFlags: 0
                                                                            timestamp: 0
                                                                         windowNumber: 0
                                                                              context: nil
                                                                              subtype: 0
                                                                                data1: 0
                                                                                data2: 0];
                                          [NSApp postEvent: wake atStart: YES];
                                      }];

            [NSApp run];

            [[NSNotificationCenter defaultCenter] removeObserver: observer];
        }
    }
}
