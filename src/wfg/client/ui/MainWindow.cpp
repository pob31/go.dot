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

#include <wfg/client/ui/MainWindow.h>

#include <utility>

namespace wfg::client::ui
{
    MainWindow::MainWindow (const juce::String& title, juce::Colour background,
                            std::function<void()> onCloseRequested)
        : juce::DocumentWindow (title, background, juce::DocumentWindow::allButtons),
          closeRequested (std::move (onCloseRequested))
    {
        /*  THE FRAME IS THE THEME'S, NOT THE SYSTEM'S (author, 2026-09-18:
            "remove the white window frame for a colour themed one"). JUCE
            draws the title bar in the window's background colour, which is
            the theme's `ground`, and the look-and-feel draws its buttons - so
            the one light rectangle on a dark booth screen goes. The price is
            the system's own frame gestures, so the resize corner is drawn
            and the window keeps a minimum it can still be read at. */
        setUsingNativeTitleBar (false);
        setTitleBarHeight (28);
        setTitleBarTextCentred (false);
        setResizable (true, true);
        setResizeLimits (640, 400, 8192, 8192);
    }

    void MainWindow::closeButtonPressed()
    {
        if (closeRequested)
            closeRequested();
    }
}
