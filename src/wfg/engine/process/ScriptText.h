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
    WORDS A SCRIPT IS WRITTEN IN, as Go.dot writes them (namespace draft §66,
    AMV, AMW): what a new script cue opens on, and the lines that fire a cue or
    hear and send to a serial port, in the script's own language - what "Copy
    for a script" puts on the clipboard and a cue dropped on the editor types.
    Each is said in JavaScript, and in Lua for a script written in it; any
    other language word reads as JavaScript, the default.
*/

#include <string>
#include <string_view>
#include <vector>

namespace wfg::process
{
    /*  What a new script cue's source is: comments that say how a script
        reaches the world, and one line to start from, commented out. */
    std::string starterScript (std::string_view language);

    /*  The line that fires the cue `cueId`. */
    std::string scriptLineToFireCue (std::string_view language, std::string_view cueId);

    /*  The lines that hear what the serial port `portId` says, and send it one. */
    std::string scriptLinesToHearSerial (std::string_view language, std::string_view portId);
    std::string scriptLineToSendSerial (std::string_view language, std::string_view portId);

    /*  Whether a language word is Lua's; anything else is JavaScript. */
    bool isLua (std::string_view language) noexcept;

    /*  GO.DOT'S READY-MADE SCRIPTS FOR A LANGUAGE (AMW): its `js` or `lua`
        folder, beside the binary or in the bundle's Resources on macOS, as the
        `pd` folder is found; empty where there is none. */
    std::string readyMadeFolder (std::string_view language);

    /*  What is in such a folder, the help card first and the rest by name:
        file names, with the language's own ending. */
    std::vector<std::string> readyMadeFiles (const std::string& folder, std::string_view language);

    /*  The help card's file name: godot-help.js or godot-help.lua. */
    std::string helpCardName (std::string_view language);
}
