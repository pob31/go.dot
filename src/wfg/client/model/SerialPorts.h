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
    THE SHOW'S SERIAL PORTS AS THE SERIAL TAB READS THEM (namespace draft §51,
    ACR; PC.10): each port the show declares - its name, where it is on this
    machine, its speed, rx and tx - and how it is tonight, in words; and the
    ports this machine has, for the path menu.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct SerialRow
    {
        std::string id;
        std::string name;
        std::string path;
        int baud = 115200;
        std::string framing = "lines";
        bool rx = true;
        bool tx = true;

        std::string state = "closed";     // closed, opening, open, retrying
        std::string problem;
        std::string lastLine;

        /*  How it is, in words for the row: "open", "opening", "trying again"
            with the reason, "no port chosen", or "closed". */
        std::string stateWords() const;

        bool operator== (const SerialRow&) const = default;
    };

    std::vector<SerialRow> readSerialPorts (const tree::TreeSnapshot& snapshot);

    struct SystemSerialPort
    {
        std::string path;
        std::string about;
    };

    /*  What this machine has now, from `/godot/engine/serialPorts`. */
    std::vector<SystemSerialPort> readSystemSerialPorts (const tree::TreeSnapshot& snapshot);

    /*  The speeds a port opens at, in the order the menu offers them. */
    const std::vector<int>& serialSpeeds();
}
