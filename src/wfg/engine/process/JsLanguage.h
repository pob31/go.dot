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
    A SCRIPT CUE'S JAVASCRIPT (namespace draft §66, AMM, AMR, AMS).

    JUCE's JavascriptEngine - QuickJS inside - one per script, on the script's
    own thread (ScriptInstance.h). What a script can call is `godot`, written
    in JavaScript as a prelude run before the script (`preludeSource`): it
    keeps the script's callbacks and timers, and the instance hands it each
    tick's inputs in one call and takes what it sent through one native door.
    So the API's shape is the prelude's, and Lua's prelude has the same one.

    THE SANDBOX (AMS) is QuickJS's own standard objects and nothing more: no
    `std` or `os` module, no module loader, no file and no network. A script
    reaches the world only through `godot`.
*/

#include <wfg/engine/process/ScriptInstance.h>

#include <memory>
#include <string_view>

namespace wfg::process
{
    /*  The language's word, as the cue's `language` row stores it. */
    inline constexpr std::string_view javascriptLanguage = "javascript";

    /*  A JavaScript interpreter writing to `output`. */
    std::unique_ptr<ScriptLanguage> makeJavascript (ScriptOutput& output);

    /*  The prelude, for the help card's test to read the API from. */
    std::string_view javascriptPrelude();

    /*  A problem as a script's editor shows it: the language's message, then
        "(line N)" when the language said a line - read off its first frame, as
        QuickJS writes a stack. */
    std::string javascriptProblem (std::string_view messageAndStack);
}
