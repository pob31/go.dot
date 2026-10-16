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
    WHOSE A VIDEO DEVICE IS, FROM ITS IDENTIFIER (namespace draft §72, APV).

    A card's or a camera's identifier carries its source before anything else:
    `dl-` and `dlt-` a DeckLink card's (Desktop Video's persistent and
    topological identifiers), `aja-` an AJA card's, `cam:` a camera's, `fake-`
    the test card's. Plain functions, read by the engine, the renderer and the
    window alike.
*/

#include <string>
#include <string_view>

namespace wfg::video
{
    /*  The maker as a person reads it - DeckLink, AJA, camera - or empty for
        the test card and anything else. */
    inline std::string makerOf (std::string_view id)
    {
        if (id.rfind ("dl-", 0) == 0 || id.rfind ("dlt-", 0) == 0)  return "DeckLink";
        if (id.rfind ("aja-", 0) == 0)                               return "AJA";
        if (id.rfind ("cam:", 0) == 0)                               return "camera";
        return {};
    }

    /*  A device's name with its maker in front where the name does not already
        say it: "AJA Kona 5", "DeckLink Duo 2 (1)", "Logitech C920 - camera". */
    inline std::string nameWithMaker (std::string_view id, const std::string& name)
    {
        const auto maker = makerOf (id);

        if (maker.empty() || name.rfind (maker, 0) == 0)
            return name;

        if (maker == "camera")
            return name + " - camera";

        return maker + " " + name;
    }
}
