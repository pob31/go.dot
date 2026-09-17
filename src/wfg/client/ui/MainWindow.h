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
    The one window. A DocumentWindow whose close button asks somebody else.

    Closing it is not a window matter: in process, the window going away means
    the engine going away, and whether that is allowed depends on what the
    engine says about the show (locked, or not). So the button reports and
    Client.cpp decides - and nothing here deletes anything.
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace wfg::client::ui
{
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& title, juce::Colour background,
                    std::function<void()> onCloseRequested);

        void closeButtonPressed() override;

    private:
        std::function<void()> closeRequested;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };
}
