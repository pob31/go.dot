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
    THE NAMES GO.DOT SENDS PICTURES UNDER (namespace draft §44, YA, YE; §47,
    AAK), in one place: the engine sends under them and the window says them,
    and two copies of a default are two answers the day one changes.

    When the show names nothing: an output sends as "Go.dot - " and its name,
    an insert as "Go.dot - insert " and its name - so a renamed insert's send
    moves with it. A NEW insert (§47, AAK) is given both its names when it is
    made, written into the show and never moved after: "Go.dot - Spout insert
    1 (send)" and, expected back from the other program, "... (return)" - the
    other program connects once and keeps its connection whatever the insert
    is called later (the author, 2026-10-09: TouchDesigner or After Effects
    "would lose the connection").
*/

#include <string>

namespace wfg::video
{
    inline std::string outputSendName (const std::string& sendName, const std::string& name, const std::string& id)
    {
        return ! sendName.empty() ? sendName : "Go.dot - " + (name.empty() ? id : name);
    }

    inline std::string insertSendName (const std::string& sendName, const std::string& name, const std::string& id)
    {
        return ! sendName.empty() ? sendName : "Go.dot - insert " + (name.empty() ? id : name);
    }

    inline std::string newInsertSendName (const std::string& insertName)
    {
        return "Go.dot - " + insertName + " (send)";
    }

    inline std::string newInsertReturnName (const std::string& insertName)
    {
        return "Go.dot - " + insertName + " (return)";
    }
}
