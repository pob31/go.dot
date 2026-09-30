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

//  The Mac's half of Raise.h; Raise.cpp is everyone else's.

#include <wfg/engine/app/Raise.h>

#import <AppKit/AppKit.h>
#include <unistd.h>

namespace wfg::app
{
    long currentProcessId() noexcept
    {
        return static_cast<long> (getpid());
    }

    bool raiseProcessWindows (long processId)
    {
        @autoreleasepool
        {
            NSRunningApplication* application =
                [NSRunningApplication runningApplicationWithProcessIdentifier: static_cast<pid_t> (processId)];

            if (application == nil)
                return false;

            return [application activateWithOptions: NSApplicationActivateAllWindows] == YES;
        }
    }
}
