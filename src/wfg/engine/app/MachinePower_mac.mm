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

//  The Mac's half of MachinePower.h; MachinePower.cpp is everyone else's.

#include <wfg/engine/app/MachinePower.h>

#import <Foundation/Foundation.h>

namespace wfg::app
{
    LatencyCritical::LatencyCritical (const std::string& reason)
    {
        @autoreleasepool
        {
            /*  USER-INITIATED CARRIES IDLE-SYSTEM-SLEEP-DISABLED: a show that
                is waiting for its next GO is not a computer that is idle. The
                display may still sleep; the show does not need it. */
            id<NSObject> activity = [[NSProcessInfo processInfo]
                                        beginActivityWithOptions: NSActivityLatencyCritical | NSActivityUserInitiated
                                                          reason: [NSString stringWithUTF8String: reason.c_str()]];

           #if __has_feature(objc_arc)
            token = (__bridge_retained void*) activity;
           #else
            token = (void*) [activity retain];
           #endif
        }
    }

    LatencyCritical::~LatencyCritical()
    {
        if (token == nullptr)
            return;

        @autoreleasepool
        {
           #if __has_feature(objc_arc)
            id<NSObject> activity = (__bridge_transfer id<NSObject>) token;
            [[NSProcessInfo processInfo] endActivity: activity];
           #else
            id<NSObject> activity = (id<NSObject>) token;
            [[NSProcessInfo processInfo] endActivity: activity];
            [activity release];
           #endif
        }

        token = nullptr;
    }

    MachinePower readMachinePower()
    {
        MachinePower power;

        @autoreleasepool
        {
            NSProcessInfo* process = [NSProcessInfo processInfo];
            power.lowPowerMode = process.lowPowerModeEnabled;

            switch (process.thermalState)
            {
                case NSProcessInfoThermalStateFair:     power.thermal = Thermal::fair; break;
                case NSProcessInfoThermalStateSerious:  power.thermal = Thermal::serious; break;
                case NSProcessInfoThermalStateCritical: power.thermal = Thermal::critical; break;
                case NSProcessInfoThermalStateNominal:  break;
            }
        }

        return power;
    }
}
