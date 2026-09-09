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

#include <wfg/engine/cue/ListState.h>

#include <wfg/engine/osc/OscValue.h>

namespace wfg::cue
{
    std::string spellAim (const ListAim& aim)
    {
        if (! aim.isSet())
            return {};

        /*  THROUGH THE PROJECT'S OWN FORMATTER, which is the whole of the
            locale rule: a `std::to_string` here would put a comma in the offset
            under `fr_FR` and a client would read the cue and the seconds as
            three fields instead of two. */
        return aim.cue + " " + osc::formatDouble (aim.offset);
    }

    ListAim readAim (const std::string& text)
    {
        const auto space = text.find (' ');

        if (space == std::string::npos || space == 0)
            return {};

        ListAim aim;
        aim.cue = text.substr (0, space);

        /*  A cue with no number after it is not half an aim, it is not an aim:
            "before this cue" is -1 and has to be written down, because the
            difference between it and nought seconds is the difference between
            standby on a cue and the cue sounding. */
        const auto offset = osc::parseDouble (text.substr (space + 1));

        if (! offset.has_value())
            return {};

        aim.offset = *offset;
        return aim;
    }
}
