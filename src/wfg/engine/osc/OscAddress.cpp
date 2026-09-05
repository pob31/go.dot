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

#include <wfg/engine/osc/OscAddress.h>

namespace wfg::osc
{
    namespace
    {
        constexpr std::string_view wildcards = "*?[]{}";

        /*  Reserved by the spec in an address part. `/` is the separator and is
            handled by the walk rather than by this; the rest are the wildcards
            plus `#`, which starts a bundle, and `,`, which starts a type-tag
            string. */
        constexpr std::string_view reservedInAPart = "#,*?[]{}";

        bool isPrintableAscii (char c) noexcept
        {
            const auto byte = static_cast<unsigned char> (c);

            /*  0x20 is a space, which the spec reserves; 0x7F is DEL. So the
                legal range is the printable characters between them. */
            return byte > 0x20 && byte < 0x7F;
        }

        bool check (std::string_view address, bool allowWildcards)
        {
            if (address.empty() || address.front() != '/')
                return false;

            if (address.size() == 1)
                return true;             // "/" is the root, and names it

            if (address.back() == '/')
                return false;            // a trailing separator names an empty part

            std::size_t partLength = 0;

            for (std::size_t i = 1; i < address.size(); ++i)
            {
                const auto c = address[i];

                if (c == '/')
                {
                    if (partLength == 0)
                        return false;    // "//", an empty part

                    partLength = 0;
                    continue;
                }

                if (! isPrintableAscii (c))
                    return false;

                const auto reserved = reservedInAPart.find (c) != std::string_view::npos;
                const auto wild = wildcards.find (c) != std::string_view::npos;

                if (reserved && ! (allowWildcards && wild))
                    return false;

                ++partLength;
            }

            return partLength > 0;
        }
    }

    //==============================================================================
    bool isValidAddress (std::string_view address)
    {
        return check (address, false);
    }

    bool isValidPattern (std::string_view address)
    {
        return check (address, true);
    }

    bool containsWildcard (std::string_view address)
    {
        return address.find_first_of (wildcards) != std::string_view::npos;
    }

    std::vector<std::string> partsOf (std::string_view address)
    {
        std::vector<std::string> parts;
        std::size_t i = 0;

        while (i < address.size())
        {
            while (i < address.size() && address[i] == '/')
                ++i;

            const auto start = i;

            while (i < address.size() && address[i] != '/')
                ++i;

            if (i > start)
                parts.emplace_back (address.substr (start, i - start));
        }

        return parts;
    }
}
