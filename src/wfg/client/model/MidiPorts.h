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
    The MIDI ports this show declares, and what this machine has to plug them
    into.

    TWO DIFFERENT KINDS OF FACT, and keeping them apart is the whole design. A
    PORT is the show's: it is called "Lights" or "The desk", a cue names it by
    identifier, and it travels in the file. A DEVICE is this building's: it is
    a cable in a socket, and the same show at the next venue finds different
    ones. The port says which device it wants; the engine goes and looks.

    WHICH IS WHY MOVING AN INTERFACE IS ONE EDIT. A cue and a trigger name the
    port, never the cable, so a cable that moves to another USB socket changes
    one row here and leaves every cue in the show alone (author, 2026-09-22).

    AND WHY A PORT REMEMBERS TWO THINGS ABOUT ITS DEVICE. The identifier the
    operating system gave it is the only thing that tells two identical
    interfaces apart, and the first thing to break when a cable moves; the name
    is the other way round. Both are kept and the engine tries the identifier
    first. The window shows the NAME, because that is what a person reads and
    what the menu offers; the identifier is the machine's business and never
    appears on screen.

    std only, and a pure reading of the snapshot the window already holds.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct PortRow
    {
        std::string id;

        /** What the show calls it: "Lights". Never a device name. */
        std::string name;

        /** The devices it asks for, by the name `wfg midi` lists. */
        std::string outputDevice;
        std::string inputDevice;

        /** Whether it listens, and whether it is sent to. */
        bool rx = true;
        bool tx = true;

        /** Whether anything is behind it tonight. */
        bool bound = false;

        /*  Why not, in the engine's own sentence, and empty when it is. Never
            rewritten here: a client inventing its own words for a refusal is a
            second place for them to be wrong. */
        std::string problem;

        /** What to show in a list or a menu; never blank. */
        std::string label() const;

        /** The one line a row shows about its state, in words and not colour. */
        std::string stateWord() const;
    };

    /** Every declared port, in identifier order. */
    std::vector<PortRow> readPorts (const tree::TreeSnapshot&);

    /*  What this machine has, for the two menus. Newline-separated in the
        tree, because a device name contains spaces and nothing else would
        split it back. */
    std::vector<std::string> readMidiInputs (const tree::TreeSnapshot&);
    std::vector<std::string> readMidiOutputs (const tree::TreeSnapshot&);

    /*  The menu a MIDI cue's `port` row offers: the identifier it writes,
        paired with the name somebody reads. "(none)" first, because a cue with
        no port is a cue somebody has not finished writing and has to be
        sayable. */
    std::vector<std::pair<std::string, std::string>> portChoices (const std::vector<PortRow>&);
}
