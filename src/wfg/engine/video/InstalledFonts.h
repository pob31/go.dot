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
    THE FILES BEHIND THE FONTS INSTALLED ON THIS MACHINE (namespace draft §62,
    ALA, ALI): what `fonts.carry` copies into a show, and what a QLab text
    cue's PostScript name - "HelveticaNeue-Bold" - turns out to be here.

    JUCE names installed families but never says where their files are, so each
    system is asked in its own words: DirectWrite on Windows
    (InstalledFonts_windows.cpp), CoreText on macOS (InstalledFonts_mac.cpp),
    fontconfig on Linux (InstalledFonts_linux.cpp). They only find files; what
    a file holds - its family, its style, a collection's faces - is read by
    Go.dot itself (FontFile.h, AKY), the same way everywhere.
*/

#include <optional>
#include <string>
#include <vector>

namespace wfg::video::installed
{
    /*  Every file of an installed family - each of its styles - once each, as
        the system lists them. Empty when the family is not installed, or when
        its faces are made up by the system from another family's file. */
    std::vector<std::string> filesOfFamily (const std::string& family);

    /** The files of the installed face whose PostScript name is `postScript`; empty when there is none. */
    std::vector<std::string> filesOfPostScript (const std::string& postScript);

    /** An installed face found by its PostScript name, as its own file names it. */
    struct Face
    {
        std::string family;
        std::string style;
        bool bold = false;
        bool italic = false;
        std::string file;
        int index = 0;              ///< within a collection
    };

    std::optional<Face> byPostScript (const std::string& postScript);
}
