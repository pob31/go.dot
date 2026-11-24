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

#pragma once

/*
    NO WINDOW OF GO.DOT'S GOES TO A SPACE OF ITS OWN (namespace draft §150,
    the author, 2026-11-24). macOS's own full screen moves a window to a new
    Space: the cue list slides away from the desk with the GO on it, and a
    projector left on a Space its display no longer shows is not seen. So a
    window covers its screen the other way - borderless over the display's
    whole frame, on the Space it is on.

    STD ONLY, so that AppKit stays in the .mm. Nothing happens elsewhere:
    Windows and Linux have no Spaces to send a window to.
*/

namespace wfg::app
{
    /*  FULL SCREEN STAYS ON THE SPACE IT IS ON, for every window of this
        process, the way WFS-DIY does it (its WindowUtils, a5b754fa): AppKit's
        toggleFullScreen: is answered by Go.dot, so the maximise button, a
        title-bar double-click, Ctrl+Cmd+F and ResizableWindow::setFullScreen
        all make a JUCE window borderless over its display instead, with the
        menu bar and Dock hiding - each coming back when the pointer reaches
        its edge (the author's pick, 2026-11-24) - while it covers a display
        that has them. The same gestures bring the old frame back; so do
        closing, minimising, and its display going away. Windows that are not
        JUCE's (an open panel, an alert) keep AppKit's own. Once, before the
        first window opens; again does nothing. */
    void installCoverTheScreen();

    /*  A PROJECTOR'S WINDOW: on every Space of its display, left in place by
        Mission Control, out of ⌘` cycling, and with no full screen of its own
        - it belongs to its display, not to a desktop. `nativeView` is JUCE's
        getWindowHandle(), an NSView on the Mac. Said again as often as
        wanted: it is only set when it differs. */
    void keepOnEverySpace (void* nativeView);
}
