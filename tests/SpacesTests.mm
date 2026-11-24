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
    NO WINDOW GOES TO A SPACE OF ITS OWN (namespace draft §150), checked on a
    real Mac desktop: real windows, made as the cue list and a projector are
    made, asked for full screen the ways the operator can ask, and measured.

    WINDOWS ON A SCREEN, so only when asked: WFG_MAC_WINDOW_TESTS=1. The CI's
    hosted runner leaves it unset and these say so and pass. Run over SSH on
    a Mac someone is logged in to: AppKit draws there, and nothing here needs
    a key pressed (an SSH session has no Accessibility to press one).
*/

#import <AppKit/AppKit.h>
#include <dlfcn.h>

#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <wfg/engine/app/Spaces.h>

#include <cmath>
#include <cstdlib>
#include <string>

namespace
{
    bool asked()
    {
        return std::getenv ("WFG_MAC_WINDOW_TESTS") != nullptr;
    }

    //  AppKit's and JUCE's queues both, for a while.
    void pump (double seconds)
    {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow: seconds];

        while ([until timeIntervalSinceNow] > 0)
        {
            @autoreleasepool
            {
                NSEvent* event = [NSApp nextEventMatchingMask: NSEventMaskAny
                                                    untilDate: [NSDate dateWithTimeIntervalSinceNow: 0.01]
                                                       inMode: NSDefaultRunLoopMode
                                                      dequeue: YES];
                if (event != nil)
                    [NSApp sendEvent: event];
            }
        }
    }

    void beAnApplication()
    {
        [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps: YES];
        pump (0.5);
    }

    NSWindow* windowOf (juce::Component& component)
    {
        auto* view = (__bridge NSView*) component.getWindowHandle();
        return view != nil ? [view window] : nil;
    }

    /*  How many full-screen Spaces the window server has, every display's:
        the private call the WFS-DIY session found works over SSH (type 4 is a
        full-screen Space). -1 when it cannot be asked. */
    int fullScreenSpaces()
    {
        using Connection = int (*)();
        using Copy = CFArrayRef (*) (int);

        static auto* connection = (Connection) dlsym (RTLD_DEFAULT, "CGSMainConnectionID");
        static auto* copy = (Copy) dlsym (RTLD_DEFAULT, "CGSCopyManagedDisplaySpaces");

        if (connection == nullptr || copy == nullptr)
            return -1;

        CFArrayRef displays = copy (connection());

        if (displays == nullptr)
            return -1;

        int count = 0;

        for (NSDictionary* display in (__bridge NSArray*) displays)
            for (NSDictionary* space in display[@"Spaces"])
                if ([space[@"type"] intValue] == 4)
                    ++count;

        CFRelease (displays);
        return count;
    }

    //  The cue list as Client.cpp makes it (MainWindow.cpp): its own title bar, every button, resizable.
    struct CueListLike final : juce::DocumentWindow
    {
        CueListLike() : juce::DocumentWindow ("Go.dot §150 test", juce::Colours::darkgrey, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (28);
            setResizable (true, true);
            setResizeLimits (640, 400, 8192, 8192);
            centreWithSize (800, 600);
            setVisible (true);
        }

        void closeButtonPressed() override {}
    };

    bool same (NSRect a, NSRect b)
    {
        return std::abs (a.origin.x - b.origin.x) < 1.0 && std::abs (a.origin.y - b.origin.y) < 1.0
            && std::abs (a.size.width - b.size.width) < 1.0 && std::abs (a.size.height - b.size.height) < 1.0;
    }
}

TEST_CASE ("spaces: the cue list's full screen covers its display on the Space it is on, and comes back (§150)")
{
    if (! asked())
    {
        MESSAGE ("windows on a screen: set WFG_MAC_WINDOW_TESTS=1 to run");
        return;
    }

    wfg::app::installCoverTheScreen();
    beAnApplication();

    const auto spacesBefore = fullScreenSpaces();
    CueListLike window;
    pump (1.0);

    NSWindow* ns = windowOf (window);
    REQUIRE (ns != nil);

    const NSRect before = [ns frame];
    const NSRect screen = [[ns screen] frame];
    const auto styleBefore = [ns styleMask];

    /*  What the app's options read before: a Mac set to hide its menu bar by
        itself reports that here, whatever Go.dot asks (the Mac mini, 2026-11-24). */
    const auto optionsBefore = [NSApp presentationOptions];

    //  The maximise button and a title-bar double-click: DocumentWindow::maximiseButtonPressed.
    window.setFullScreen (! window.isFullScreen());
    pump (1.5);

    CHECK (same ([ns frame], screen));
    CHECK (([ns styleMask] & NSWindowStyleMaskFullScreen) == 0);
    CHECK (([ns styleMask] & (NSWindowStyleMaskTitled | NSWindowStyleMaskResizable)) == 0);
    CHECK (([ns styleMask] & NSWindowStyleMaskMiniaturizable) != 0);
    CHECK (([NSApp presentationOptions] & NSApplicationPresentationAutoHideMenuBar) != 0);
    CHECK (([NSApp presentationOptions] & NSApplicationPresentationAutoHideDock) != 0);
    CHECK (fullScreenSpaces() == spacesBefore);
    MESSAGE ("covering: " << std::string ([NSStringFromRect ([ns frame]) UTF8String]) << " on "
               << std::string ([NSStringFromRect (screen) UTF8String])
               << ", full-screen Spaces " << fullScreenSpaces() << " (before " << spacesBefore << ")");

    //  Pressed again: the same call, since JUCE's isFullScreen() says no.
    window.setFullScreen (! window.isFullScreen());
    pump (1.5);

    CHECK (same ([ns frame], before));
    CHECK ([ns styleMask] == styleBefore);
    CHECK ([NSApp presentationOptions] == optionsBefore);

    //  Ctrl+Cmd+F and the window menu: AppKit's toggleFullScreen: on the window itself.
    [ns toggleFullScreen: nil];
    pump (1.5);
    CHECK (same ([ns frame], screen));
    CHECK (fullScreenSpaces() == spacesBefore);

    [ns toggleFullScreen: nil];
    pump (1.5);
    CHECK (same ([ns frame], before));

    /*  Minimised while covering - AppKit's miniaturize: (Cmd+M, the Dock) and
        JUCE's setMinimised (its own minimise button): really in the Dock, the
        bars back, and back from it as the window it was. */
    for (int how = 0; how < 2; ++how)
    {
        CAPTURE (how);
        [ns toggleFullScreen: nil];
        pump (1.0);
        REQUIRE (same ([ns frame], screen));

        if (how == 0)
            [ns miniaturize: nil];
        else
            window.setMinimised (true);

        pump (1.5);
        CHECK ([ns isMiniaturized]);
        CHECK ([NSApp presentationOptions] == optionsBefore);

        [ns deminiaturize: nil];
        pump (1.5);
        CHECK (! [ns isMiniaturized]);
        CHECK (same ([ns frame], before));
        CHECK ([ns styleMask] == styleBefore);
    }
}

TEST_CASE ("spaces: a projector's window is on every Space of its display, and has no full screen of its own (§150)")
{
    if (! asked())
    {
        MESSAGE ("windows on a screen: set WFG_MAC_WINDOW_TESTS=1 to run");
        return;
    }

    beAnApplication();

    //  As VideoRenderChild.cpp's ProjectorWindow is made, but small: not over the operator's screen.
    juce::Component projector;
    projector.setOpaque (true);
    projector.setBounds (100, 100, 320, 180);
    projector.addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses);
    projector.setAlwaysOnTop (true);
    projector.setVisible (true);
    wfg::app::keepOnEverySpace (projector.getWindowHandle());
    pump (0.5);

    NSWindow* ns = windowOf (projector);
    REQUIRE (ns != nil);

    const auto behaviour = [ns collectionBehavior];
    CHECK ((behaviour & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0);
    CHECK ((behaviour & NSWindowCollectionBehaviorStationary) != 0);
    CHECK ((behaviour & NSWindowCollectionBehaviorIgnoresCycle) != 0);
    CHECK ((behaviour & NSWindowCollectionBehaviorFullScreenNone) != 0);
    CHECK ((behaviour & NSWindowCollectionBehaviorFullScreenPrimary) == 0);

    //  Above the menu bar and the Dock by its level alone.
    CHECK ([ns level] > NSMainMenuWindowLevel);
    CHECK ([ns level] > CGWindowLevelForKey (kCGDockWindowLevelKey));

    //  Said again each second: unchanged, and not set anew.
    wfg::app::keepOnEverySpace (projector.getWindowHandle());
    CHECK ([ns collectionBehavior] == behaviour);
}
