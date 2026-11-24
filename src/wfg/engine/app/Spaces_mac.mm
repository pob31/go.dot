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
    THE MAC'S HALF OF Spaces.h (§150), carried over from WFS-DIY's
    Source/gui/WindowUtils.mm (a5b754fa, the same author's, GPL-3), so the two
    programs cover a screen the same way. What differs: the menu bar and Dock
    AUTO-hide here (the author's pick for Go.dot, 2026-11-24), a JUCE window is
    known by its class and not looked up among JUCE's components, so no JUCE
    is needed in this file, and minimising lets go of the cover.

    WHY A HOOK ON NSWindow: JUCE marks every resizable window with a maximise
    button FullScreenPrimary, and its own window class answers
    toggleFullScreen: by calling NSWindow's through super. Replacing NSWindow's
    implementation therefore catches the maximise button, the system's
    shortcut and ResizableWindow::setFullScreen for every JUCE window, without
    patching JUCE.

    Written to build with or without ARC.
*/

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <wfg/engine/app/Spaces.h>

#include <algorithm>
#include <map>
#include <vector>

namespace wfg::app
{
    namespace
    {
        IMP originalToggleFullScreen = nullptr;
        int menuBarHiders = 0;

        struct Covering
        {
            NSRect frame {};                    // where the window was
            NSWindowStyleMask styleMask = 0;    // and what it was
            bool hidesMenuBar = false;          // counted in menuBarHiders
        };

        std::map<void*, Covering>& coveringWindows()
        {
            static std::map<void*, Covering> windows;
            return windows;
        }

        void* keyFor (NSWindow* window)  { return (__bridge void*) window; }

        //  JUCE's window class is registered as "JUCEWindow_" and a random suffix.
        bool isJuceWindow (NSWindow* window)
        {
            return window != nil && [NSStringFromClass ([window class]) hasPrefix: @"JUCEWindow_"];
        }

        bool screenHasMenuBar (NSScreen* screen)
        {
            return screen != nil && ([NSScreen screensHaveSeparateSpaces] || screen == [[NSScreen screens] firstObject]);
        }

        /*  The presentation options are the application's, not a display's: on
            while any covering window is on a display with a menu bar. The
            system applies them only while Go.dot is in front. */
        void changeMenuBarHiders (int delta)
        {
            const auto before = menuBarHiders;
            menuBarHiders = std::max (0, menuBarHiders + delta);

            if ((before > 0) != (menuBarHiders > 0))
                [NSApp setPresentationOptions: menuBarHiders > 0 ? (NSApplicationPresentationAutoHideMenuBar
                                                                      | NSApplicationPresentationAutoHideDock)
                                                                 : NSApplicationPresentationDefault];
        }

        void cover (NSWindow* window)
        {
            NSScreen* screen = [window screen] != nil ? [window screen] : [NSScreen mainScreen];

            if (screen == nil)
                return;

            Covering covering;
            covering.frame = [window frame];
            covering.styleMask = [window styleMask];
            covering.hidesMenuBar = screenHasMenuBar (screen);

            coveringWindows()[keyFor (window)] = covering;

            if (covering.hidesMenuBar)
                changeMenuBarHiders (+1);

            [window setStyleMask: NSWindowStyleMaskBorderless];
            [window setFrame: [screen frame] display: YES];
            [window makeKeyAndOrderFront: nil];
        }

        //  `putBack` false when the window is closing: only the bookkeeping goes.
        void uncover (NSWindow* window, bool putBack)
        {
            auto& windows = coveringWindows();
            const auto it = windows.find (keyFor (window));

            if (it == windows.end())
                return;

            const auto covering = it->second;
            windows.erase (it);

            if (covering.hidesMenuBar)
                changeMenuBarHiders (-1);

            if (! putBack)
                return;

            [window setStyleMask: covering.styleMask];

            //  AppKit draws no title after a borderless -> titled switch until it changes.
            NSString* title = [NSString stringWithString: [window title]];
            [window setTitle: @" "];
            [window setTitle: title];

            [window setFrame: covering.frame display: YES];
        }

        bool offersFullScreen (NSWindow* window)
        {
            return ([window collectionBehavior] & NSWindowCollectionBehaviorFullScreenPrimary) != 0;
        }

        //  False when the window is not one this answers for.
        bool toggleCover (NSWindow* window)
        {
            if (coveringWindows().count (keyFor (window)) != 0)
            {
                uncover (window, true);
                return true;
            }

            if (([window styleMask] & NSWindowStyleMaskFullScreen) != 0)
                return false;       // already in AppKit's full screen: let AppKit bring it back

            if (! isJuceWindow (window))
                return false;

            cover (window);
            return true;
        }

        void replacementToggleFullScreen (id self, SEL command, id sender)
        {
            if (toggleCover ((NSWindow*) self))
                return;

            if (originalToggleFullScreen != nullptr)
                ((void (*) (id, SEL, id)) originalToggleFullScreen) (self, command, sender);
        }

        //  A window whose display moved, changed size or went away.
        void refitCoveringWindows()
        {
            std::vector<void*> keys;

            for (const auto& entry : coveringWindows())
                keys.push_back (entry.first);

            for (auto* key : keys)
            {
                auto* window = (__bridge NSWindow*) key;
                NSScreen* screen = [window screen];

                if (screen == nil)
                {
                    uncover (window, true);
                    continue;
                }

                auto& covering = coveringWindows()[key];
                const auto hides = screenHasMenuBar (screen);

                if (hides != covering.hidesMenuBar)
                {
                    covering.hidesMenuBar = hides;
                    changeMenuBarHiders (hides ? +1 : -1);
                }

                [window setFrame: [screen frame] display: YES];
            }
        }
    }

    void installCoverTheScreen()
    {
        if (originalToggleFullScreen != nullptr)
            return;

        Method method = class_getInstanceMethod ([NSWindow class], @selector (toggleFullScreen:));

        if (method == nullptr)
            return;

        originalToggleFullScreen = method_setImplementation (method, (IMP) replacementToggleFullScreen);

        /*  Ctrl+Cmd+F: AppKit only offers it through a menu named View, which
            Go.dot's menu bar may not have, so it is listened for here. */
        [NSEvent addLocalMonitorForEventsMatchingMask: NSEventMaskKeyDown
                                              handler: ^NSEvent* (NSEvent* event)
        {
            const auto mods = [event modifierFlags] & (NSEventModifierFlagShift | NSEventModifierFlagControl
                                                       | NSEventModifierFlagOption | NSEventModifierFlagCommand);

            if (mods == (NSEventModifierFlagControl | NSEventModifierFlagCommand)
                  && [[event charactersIgnoringModifiers] caseInsensitiveCompare: @"f"] == NSOrderedSame)
            {
                NSWindow* window = [event window] != nil ? [event window] : [NSApp keyWindow];

                if (window != nil
                      && (offersFullScreen (window) || coveringWindows().count (keyFor (window)) != 0)
                      && toggleCover (window))
                    return nil;
            }

            return event;
        }];

        auto* centre = [NSNotificationCenter defaultCenter];

        [centre addObserverForName: NSWindowWillCloseNotification
                            object: nil
                             queue: nil
                        usingBlock: ^(NSNotification* note) { uncover ((NSWindow*) [note object], false); }];

        //  A window put in the Dock leaves no menu bar hidden behind it.
        [centre addObserverForName: NSWindowDidMiniaturizeNotification
                            object: nil
                             queue: nil
                        usingBlock: ^(NSNotification* note) { uncover ((NSWindow*) [note object], true); }];

        [centre addObserverForName: NSApplicationDidChangeScreenParametersNotification
                            object: nil
                             queue: nil
                        usingBlock: ^(NSNotification*) { refitCoveringWindows(); }];
    }

    void keepOnEverySpace (void* nativeView)
    {
        auto* view = (__bridge NSView*) nativeView;
        NSWindow* window = view != nil ? [view window] : nil;

        if (window == nil)
            return;

        /*  JUCE's own is FullScreenAuxiliary, tied to the one Space the window
            opened on; and JUCE says its own again when a window leaves full
            screen (resetWindowPresentation), hence asked each second. */
        const NSWindowCollectionBehavior behaviour = NSWindowCollectionBehaviorFullScreenNone
                                                   | NSWindowCollectionBehaviorCanJoinAllSpaces
                                                   | NSWindowCollectionBehaviorStationary
                                                   | NSWindowCollectionBehaviorIgnoresCycle;

        if ([window collectionBehavior] != behaviour)
            [window setCollectionBehavior: behaviour];
    }
}
