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
    USB touch screens on the Mac (namespace draft §125, BFD, BFG; MT.8).

    macOS treats a touch screen as a mouse: one pointer, never two fingers.
    Windows hands each finger to the window under it, and Linux will under
    JUCE 9; on the Mac Go.dot reads the panel itself, through spatcore's
    MacHidTouchManager - WFS-DIY's reader - which switches the panel out of
    mouse emulation, hands each finger to JUCE as a touch source of its own
    (patches/JUCE-local lets JUCE make one), drives the cursor for a finger
    outside Go.dot's windows, and puts the panel back in mouse mode on quit.

    WHAT THIS ADDS TO IT, and no more (BFG):
      - one display, no setup: the panel covers it;
      - two or more, and a panel nobody has placed: a circle on every display,
        each in its own place, touched once on each panel - WFS-DIY's
        identify, offered once per panel and session;
      - a sentence at the foot when macOS withholds the touches (Input
        Monitoring) or JUCE drops them (a build without patches/JUCE-local).
    A window to set panels up by hand is not built (MT.9).

    THE SELF-TEST, `WFG_TEST_TOUCH_INJECT=1` (or `=exit`, which closes the
    window after): a desk of two sampler strips in a window of its own, two
    fingers injected on its two pads as the panel reader would inject them,
    and a line on stderr - "touch self-test: passed", or what went wrong.
    It opens a window on the Mac's screen, so it runs only when asked.

    `makeTouchPanels` answers nothing on any other system.
*/

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace wfg::client::ui
{
    class TouchPanels
    {
    public:
        virtual ~TouchPanels() = default;
    };

    /*  The Mac's panel reader, started: built after the main window is shown
        and destroyed before it goes (so fingers lift while the windows still
        exist, and the panels go back to mouse mode). `say` puts a sentence at
        the foot; `close` closes the window, for the self-test's `=exit`. */
    std::unique_ptr<TouchPanels> makeTouchPanels (const model::Theme& theme,
                                                  std::function<void (const juce::String&)> say,
                                                  std::function<void()> close);
}
