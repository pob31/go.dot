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
    THE PICTURES' OWN COMMANDS (Phase 8a, namespace draft 35).

    `videoOutput.identify <output> <on>` puts an output's test pattern - a
    white frame and a cross - over whatever it shows, or takes it away: how
    somebody at the wall finds which projector is which. Tonight's, never the
    show's (PRD §4.10), so it is no edit of the document and is taken under
    the show lock; a named command all the same (§4.11), logged, so a replay
    reads it - and, with no renderer, does nothing with it.

    Registered wherever the other commands are - `serve` with its host, and
    every verb that replays or lists a log with none.
*/

namespace wfg { class CommandRegistry; }

namespace wfg::video
{
    class VideoHost;

    void registerVideoCommands (CommandRegistry& registry, VideoHost* host);
}
