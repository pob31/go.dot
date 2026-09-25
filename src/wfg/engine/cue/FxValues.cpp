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

#include <wfg/engine/cue/FxValues.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace wfg::cue
{
    std::map<int, double> parseFxValues (const std::string& text)
    {
        std::map<int, double> out;
        std::istringstream in (text);
        std::string pair;

        while (in >> pair)
        {
            const auto colon = pair.find (':');

            if (colon == std::string::npos || colon == 0)
                continue;

            /*  The index is digits and nothing else: atoi reads "x" as nought,
                which would hand a stray word to parameter nought. */
            const auto digits = pair.substr (0, colon);
            const auto numeric = ! digits.empty()
                                   && std::all_of (digits.begin(), digits.end(),
                                                   [] (char c) { return c >= '0' && c <= '9'; });

            if (! numeric || digits.size() > 6)
                continue;

            const auto index = std::atoi (digits.c_str());
            const auto value = osc::parseDouble (pair.substr (colon + 1));

            if (value.has_value() && std::isfinite (*value))
                out[index] = std::clamp (*value, 0.0, 1.0);
        }

        return out;
    }

    std::string formatFxValues (const std::map<int, double>& values)
    {
        std::string out;

        for (const auto& [index, value] : values)
        {
            if (! out.empty())
                out += ' ';

            out += std::to_string (index) + ":" + osc::formatDouble (value);
        }

        return out;
    }
}
