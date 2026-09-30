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

//  The Mac's half of FolderIcon.h.

#include <wfg/engine/app/FolderIcon.h>

#import <AppKit/AppKit.h>

#include <cstddef>

namespace wfg::app
{
    //  The generated FolderIconData.cpp (src/CMakeLists.txt): Go.dot-folder.icns.
    extern const unsigned char folderIconBytes[];
    extern const std::size_t folderIconSize;

    bool isMarkedShowFolder (const juce::File& folder)
    {
        //  The Finder keeps a folder's own icon in a hidden file called "Icon\r".
        return folder.getChildFile ("Icon\r").existsAsFile();
    }

    void markShowFolder (const juce::File& folder)
    {
        if (! folder.isDirectory() || isMarkedShowFolder (folder))
            return;

        NSString* path = [NSString stringWithUTF8String: folder.getFullPathName().toRawUTF8()];

        /*  ON THE MAIN THREAD, which is the message thread: AppKit's objects
            are the main thread's unless said otherwise, and a save lands on
            the writer's. The block keeps `path` alive: copying a block retains
            the objects it captures, with or without ARC. */
        dispatch_async (dispatch_get_main_queue(), ^
        {
            @autoreleasepool
            {
                NSData* bytes = [NSData dataWithBytesNoCopy: const_cast<unsigned char*> (folderIconBytes)
                                                     length: folderIconSize
                                               freeWhenDone: NO];
                NSImage* icon = [[NSImage alloc] initWithData: bytes];

                if (icon != nil)
                    [[NSWorkspace sharedWorkspace] setIcon: icon forFile: path options: 0];

               #if ! __has_feature (objc_arc)
                [icon release];
               #endif
            }
        });
    }
}
