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
    THE PAGES OF SHOW SETTINGS, BY NAME (namespace draft §39.14, §64): what a
    problem names when it says where it is put right, and what the window shows
    when it is asked for one. The window's own table (`pageWords`, in
    ShowSettingsWindow.cpp) puts them in sections and in order; the names live
    here so that the model - std only - can say which page fixes what, and a
    page renamed in one place is renamed in both.
*/

#include <wfg/client/model/Pair.h>
#include <wfg/client/model/Timecode.h>

namespace wfg::client::model::settingsPages
{
    /*  NOT A PAGE: where a problem of the computer itself is said to belong
        (namespace draft §108) - Low Power Mode, a Mac slowed for heat - which
        no page of Show settings puts right. The main window's warning opens
        the system's own settings instead. */
    inline constexpr const char* thisComputer = "This computer";

    //  Show.
    inline constexpr const char* playback = "Playback";
    inline constexpr const char* templates = "Templates";
    inline constexpr const char* redundancy = pairWords::tab;               // AGP

    //  Audio. "Audio interface" is the author's (2026-10-15).
    inline constexpr const char* audioInterface = "Audio interface";
    inline constexpr const char* outputs = "Outputs";
    inline constexpr const char* outputPatch = "Output patch";
    inline constexpr const char* inputs = "Inputs";
    inline constexpr const char* inputPatch = "Input patch";
    inline constexpr const char* rack = "Rack";
    inline constexpr const char* plugins = "Plugins";

    //  Video: one page.
    inline constexpr const char* video = "Video";

    //  Controllers.
    inline constexpr const char* network = "Network";
    inline constexpr const char* midi = "MIDI";
    inline constexpr const char* surfaces = "Surfaces";
    inline constexpr const char* bindings = "Bindings";                     // §74
    inline constexpr const char* timecode = timecodeWords::tab;
    inline constexpr const char* serial = "Serial";

    inline constexpr const char* gettingStarted = "Getting started";
}
