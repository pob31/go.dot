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
    THE DISPLAYS THIS MACHINE HAS, BY THE NAMES THE SYSTEM GIVES THEM (Phase 8a,
    namespace draft 35.2, decision VL).

    JUCE's display list says where each display is and how large, and nothing
    about which one it is. A show names an output's display the way it names a
    MIDI port's device: by the name somebody reads - EPSON PJ, DELL U2720Q -
    which travels to the next venue, and beside it the identifier this machine
    gave it, tried first. So the system is asked: on Windows the monitor's own
    name and its device path, through the display-configuration API; on macOS
    the screen's localised name and its display identifier; elsewhere, for now,
    a name made of its number and size.

    Message thread, with JUCE's GUI up - the renderer's, which is the program
    that opens them.
*/

#include <string>
#include <vector>

namespace wfg::video
{
    struct DisplayInfo
    {
        std::string name;       ///< what the system calls it, or "Display 2"
        std::string id;         ///< what this machine calls it, stable while it stays plugged in
        int x = 0, y = 0, width = 0, height = 0;    ///< JUCE's logical bounds
        float refreshHz = 60.0f;
        bool isMain = false;
    };

    std::vector<DisplayInfo> listDisplays();

    /*  WHICH DISPLAY AN OUTPUT MEANS: its identifier first, then its name - the
        port's rule (§15). -1 with a reason when there is none, or when the name
        fits two and no identifier settles it: ambiguity is said, never
        guessed. */
    int findDisplay (const std::vector<DisplayInfo>& displays, const std::string& name,
                     const std::string& id, std::string& why);
}
