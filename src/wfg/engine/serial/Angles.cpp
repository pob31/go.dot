// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/serial/Angles.h>

namespace wfg::serial
{
    std::vector<std::string> AngleCutter::feed (const std::string& bytes, std::size_t longest)
    {
        std::vector<std::string> frames;

        for (const char c : bytes)
        {
            if (! inFrame)
            {
                if (c == '<')
                {
                    inFrame = true;
                    inBraces = false;
                    partial.assign (1, c);
                }
                continue;
            }

            if (c == '<' && ! inBraces)
            {
                ++dropped;
                partial.assign (1, c);
                continue;
            }

            partial.push_back (c);

            if (c == '{')
            {
                inBraces = true;
            }
            else if (c == '}')
            {
                inBraces = false;
            }
            else if (c == '>' && ! inBraces)
            {
                frames.push_back (std::move (partial));
                partial.clear();
                inFrame = false;
                continue;
            }

            if (partial.size() > longest)
            {
                ++dropped;
                partial.clear();
                inFrame = false;
                inBraces = false;
            }
        }

        return frames;
    }
}
