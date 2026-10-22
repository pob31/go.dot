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

#include <wfg/engine/app/MachinePower.h>

namespace wfg::app
{
    const char* wordOf (Thermal thermal) noexcept
    {
        switch (thermal)
        {
            case Thermal::fair:     return "fair";
            case Thermal::serious:  return "serious";
            case Thermal::critical: return "critical";
            case Thermal::nominal:  break;
        }

        return "nominal";
    }
}

//  Windows and Linux: nothing to declare, nothing to read. The Mac's is MachinePower_mac.mm.
#if ! defined (__APPLE__)

namespace wfg::app
{
    LatencyCritical::LatencyCritical (const std::string&) {}
    LatencyCritical::~LatencyCritical() = default;

    MachinePower readMachinePower() { return {}; }
}

#endif
