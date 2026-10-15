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
    THE REMOTES' OWN COMMAND (namespace draft §68, ANP, ANT).

    `remote.find <id> [seconds]` asks a tablet to show itself - its screen
    flashing, an alarm, a vibration - as WFS-DIY's remote is found from its
    desk: the author's request of 2026-10-15. `<id>` is what the tablet said in
    its HELLO, read back at `/godot/network/remotes`; thirty seconds when no
    time is given, and never past five minutes.

    THIS MACHINE'S ALONE (AGC): the tablet's connection is to this computer, and
    a backup told the same would ask the same tablet twice. Never the show's and
    no step of undo; a named command all the same (§4.11), logged, so a replay
    reads it - and, with no server, does nothing with it.

    Registered wherever the other commands are: `serve` with its server, and
    every verb that replays or lists a log with none.
*/

namespace wfg
{
    class CommandRegistry;
}

namespace wfg::oscquery
{
    class OscQueryServer;

    void registerRemoteCommands (CommandRegistry& registry, OscQueryServer* server);
}
