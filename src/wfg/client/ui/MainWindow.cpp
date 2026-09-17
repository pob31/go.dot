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
        setUsingNativeTitleBar (true);
        setResizable (true, false);
    }

    void MainWindow::closeButtonPressed()
    {
        if (closeRequested)
            closeRequested();
    }
}
