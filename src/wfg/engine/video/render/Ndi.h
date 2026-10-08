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
    THE NDI RUNTIME, WHERE THE USER INSTALLED IT (namespace draft §44, N.2).

    NDI's library is not open source and is never shipped with Go.dot - the
    author accepted the separate install on 2026-10-08. Go.dot carries only
    NDI's headers, which NDI publishes under the MIT licence for open-source
    projects (ThirdParty/ndi), and loads the runtime the NDI Tools or the NDI
    runtime installer put on the machine: where NDI_RUNTIME_DIR_V6 says (the
    installers set it on Windows), then the places each system's installers
    use, then the system's own search.

    Loaded once a process, on first use, and kept: NDI's library may not be
    unloaded while anything it made is alive, and unloading gains nothing.
*/

#include <string>

struct NDIlib_v6_3;

namespace wfg::video::ndi
{
    struct Runtime
    {
        const NDIlib_v6_3* lib = nullptr;   ///< null when none was found or it would not start
        std::string path;                   ///< the library loaded
        std::string version;                ///< what it says it is
        std::string problem;                ///< why not, when not
    };

    /*  The runtime, found, loaded and initialised the first time - from any
        thread; the same answer every time after. */
    const Runtime& runtime();

    /*  Where to get it, for a sentence on screen. */
    const char* downloadUrl() noexcept;
}
