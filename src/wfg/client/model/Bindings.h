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
    THE SHOW'S BINDINGS, AS THE WINDOW READS THEM (namespace draft §74, BD.11):
    every binding's rows off the tree, in the show's order, and the words a row
    says them in - what moves it, how, what it moves, and when it is live. Std
    only, so a test reads them with no window.
*/

#include <wfg/client/model/MidiPorts.h>

#include <string>
#include <vector>

namespace wfg::tree
{
    class TreeSnapshot;
}

namespace wfg::client::model
{
    struct BindingRow
    {
        std::string id;
        std::string name;
        std::string cue;                    ///< empty: live while the show is open
        bool enabled = true;

        std::string source = "midi";
        std::string port;
        int channel = 0;
        std::string type = "controlChange";
        int number = 0;
        std::string address;
        int argument = 0;

        std::string control = "absolute";
        std::string target;
        bool pickup = true;
        bool feedback = false;

        bool operator== (const BindingRow&) const = default;
    };

    /** Every binding of the show, in its order. */
    std::vector<BindingRow> readBindings (const tree::TreeSnapshot& snapshot);

    /*  WHAT MOVES IT: "Desk · ch 2 · CC 7", "any port · note 60", "OSC
        /desk/fader/1 · argument 2" - a port by the show's name for it - and
        "nothing yet" when it names no control. */
    std::string movedByWords (const BindingRow& row, const std::vector<PortRow>& ports);

    /** How its control moves, as one word for the row: Absolute, Relative, Rate. */
    std::string howWords (const std::string& control);

    /** The next way round, for a click on that word. */
    std::string nextControl (const std::string& control);

    /*  WHAT IT MOVES: a cue's row as the cue's number and name and the row
        ("12 Door · level"); on a binding of a cue, a row of its own by name
        ("its level"); a device's node as its address; "nothing yet" when it
        names nothing. */
    std::string movesWords (const BindingRow& row, const tree::TreeSnapshot& snapshot);

    /** When it is live: "all show", or "while 12 Door runs". */
    std::string liveWords (const BindingRow& row, const tree::TreeSnapshot& snapshot);

    /*  THE ADDRESS IT WRITES, as the engine composes it: its target when that
        is an address, its own cue's row when it names one. Empty otherwise. */
    std::string targetAddressOf (const BindingRow& row);

    /** The bindings that write this address. */
    std::vector<BindingRow> bindingsOn (const std::vector<BindingRow>& rows, const std::string& address);

    /** The binding listening for Learn (`/godot/engine/bindingLearn`), or empty. */
    std::string bindingLearning (const tree::TreeSnapshot& snapshot);
}
