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
    THE ONE SYMBOL THE WALKERS TAKE FROM ImportCommon.cpp, which the fuzz
    build cannot link whole (it reaches the document model and
    juce_audio_formats). A copy of `wfg::import::number`, for the fuzz targets
    only: if the original changes, change this with it. The walkers use it to
    round a number before writing it, so it is on the path the fuzzer walks.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <string>

namespace wfg::import
{
    std::string number (double value, int decimals)
    {
        const auto scale = std::pow (10.0, decimals);
        return osc::formatDouble (std::round (value * scale) / scale);
    }
}
