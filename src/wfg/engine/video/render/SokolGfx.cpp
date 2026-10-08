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

/*
    SOKOL_GFX, BUILT ONCE (namespace draft §44, XZ): compiled into
    wfg_thirdparty under the vendor warning policy, as JUCE's and Tracktion's
    sources are. The backend - Direct3D 11, Metal or OpenGL - is the one
    cmake/WfgThirdParty.cmake names for every translation unit.

    A check that fails does not stop the renderer (SOKOL_VALIDATE_NON_FATAL):
    the call it guarded is skipped and the log says why, which render/Gpu.cpp
    keeps for the renderer's problem readout. A picture not drawn is a lesser
    fault than a renderer gone.
*/

#define SOKOL_IMPL
#define SOKOL_VALIDATE_NON_FATAL

#include <sokol/sokol_gfx.h>
