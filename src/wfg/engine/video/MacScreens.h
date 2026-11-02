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

/*  The Mac's half of the display list, between Displays.cpp, which asks, and
    Displays_mac.mm, which answers: each screen's localised name and display
    identifier, with its frame turned into JUCE's top-left coordinates so it
    can be matched to JUCE's list. One declaration for the two files, where
    each used to spell the struct out for itself. */

#include <string>
#include <vector>

namespace wfg::video
{
    struct MacScreen
    {
        std::string name;
        std::string id;
        int x = 0, y = 0, width = 0, height = 0;
    };

    std::vector<MacScreen> macScreens();
}
