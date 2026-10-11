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
    WHAT THE OTHER MACHINE NEVER SEES (namespace draft §58, RD.3, decision AGC).

    A main and a backup play one show on two computers, and some of what a
    session does is about the computer and not the show: choosing this
    machine's audio interface, scanning its plugins, saving a copy of the show
    to one of its folders, writing which display an output opens on. Two kinds
    of record are like that, and this is the one place that says which:

      - a command marked `Command::machineLocal` - whatever it is handed;
      - a `node.set` on a row the parameter table marks `machine`, or a
        `node.setMany` whose every address is one. A set that mixes machine
        rows with the show's is the show's: it is an edit somebody made to
        the show, and the link (RD.5) sends it without its machine pairs -
        `doc::Schema::isMachineAddress` is how it finds them.

    TWO CALLERS, ONE ANSWER. The transaction hooks `serve` and `wfg replay`
    install ask it before every applied command, and open no undo step for a
    record it names (`ShowDocument::beginMachineEdit`); the link will ask it of
    every record before sending one down. They must agree: a step on the main
    for something the backup is never sent would be a step a mirrored undo
    takes back on one machine and not the other.

    DECIDED FROM THE RECORD ALONE - its command and its arguments - and never
    from the document or anything else a machine holds, so the main, the
    backup and a replay of either give the same answer to the same record.
*/

#include <wfg/engine/osc/OscValue.h>

#include <vector>

namespace wfg
{
    struct Command;
}

namespace wfg::pair
{
    /** Whether a record is this machine's alone: never mirrored, never a step
        of undo. `args` as applied (or as coerced, which for these is the
        same: an address is a string either way). */
    bool isMachineLocal (const Command& command, const std::vector<osc::Value>& args);
}
