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

#include <wfg/engine/serial/Slip.h>

namespace wfg::serial::slip
{
    std::string encode (const std::vector<std::uint8_t>& packet)
    {
        std::string out;
        out.reserve (packet.size() + 8);
        out.push_back (static_cast<char> (end));

        for (const auto byte : packet)
        {
            if (byte == end)
            {
                out.push_back (static_cast<char> (esc));
                out.push_back (static_cast<char> (escapedEnd));
            }
            else if (byte == esc)
            {
                out.push_back (static_cast<char> (esc));
                out.push_back (static_cast<char> (escapedEsc));
            }
            else
            {
                out.push_back (static_cast<char> (byte));
            }
        }

        out.push_back (static_cast<char> (end));
        return out;
    }

    std::vector<std::vector<std::uint8_t>> Decoder::feed (const std::string& bytes)
    {
        std::vector<std::vector<std::uint8_t>> packets;

        for (const auto c : bytes)
        {
            const auto byte = static_cast<std::uint8_t> (c);

            if (byte == end)
            {
                if (! spoiled && ! escaping && ! frame.empty())
                    packets.push_back (std::move (frame));
                else if (spoiled || escaping)
                    ++lost;

                frame.clear();
                escaping = false;
                spoiled = false;
                continue;
            }

            if (spoiled)
                continue;

            if (escaping)
            {
                escaping = false;
                if (byte == escapedEnd)
                    frame.push_back (end);
                else if (byte == escapedEsc)
                    frame.push_back (esc);
                else
                    spoiled = true;
            }
            else if (byte == esc)
            {
                escaping = true;
                continue;
            }
            else
            {
                frame.push_back (byte);
            }

            if (frame.size() > largestFrame)
            {
                spoiled = true;
                frame.clear();
            }
        }

        return packets;
    }
}
