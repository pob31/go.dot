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
    A SCRIPT CUE'S LUA (namespace draft §66, AMM, AMR, AMS, AMX).

    Lua 5.4, built by Go.dot (cmake/WfgThirdParty.cmake, wfg_lua), its C API
    used directly: one state per script, on the script's own thread
    (ScriptInstance.h). What a script can call is `godot`, written in Lua as a
    prelude run before the script - JavaScript's prelude in Lua's words, the
    same functions under the same names - over a native door of three
    functions it keeps to itself.

    THE SANDBOX (AMS): the base library without dofile, loadfile and binary
    chunks, and string (without dump), table, math, utf8 and coroutine. Never
    io, os, package or debug - they are not even linked. A script's memory is
    capped.

    A STUCK SCRIPT IS INTERRUPTED (AMQ) by a hook every thousand instructions
    that raises an error nothing in the script can catch: pcall, xpcall and
    coroutine.resume pass it on.
*/

#include <wfg/engine/process/ScriptInstance.h>

#include <memory>
#include <string>
#include <string_view>

namespace wfg::process
{
    /*  The language's word, as the cue's `language` row stores it. */
    inline constexpr std::string_view luaLanguage = "lua";

    /*  A Lua state writing to `output`. */
    std::unique_ptr<ScriptLanguage> makeLua (ScriptOutput& output);

    /*  The prelude, for the help card's test to read the API from. */
    std::string_view luaPrelude();

    /*  A problem as a script's editor shows it: Lua's "script:7: message" as
        "message (line 7)". */
    std::string luaProblem (std::string_view message);
}
