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
    WHAT A SURFACE'S ROTARIES ARE AIMED AT, as a named command.

    `surface.aim <cue>` says which cue the rotaries edit when a surface shows
    its EQ or its Send page (author, 2026-09-25). Two hands send it: a SELECT
    on a sample strip (the bridge, origin `surface:<id>`), and a click on a
    running cue's name in the window. It is a command rather than a value a
    client pokes because it is an action a gesture reaches (PRD §4.11), and so
    it is logged and a replay reproduces it.

    WHAT IT IS NOT: the client's pick, which stays the window's own and never
    reaches the engine, and the list GO acts on, which `list.focus` chooses.
    Picking a row in the cue list does not re-aim the rotaries - the author's
    decision, so a click while programming never moves what a hand on the desk
    is turning.

    A cue that is not media is refused, because only a media cue has an EQ and
    sends; an empty argument clears it. Nothing is stored in the show: the aim
    is what the hands are on tonight (PRD §4.10), held in the SurfaceTable and
    published at /godot/surface/aim.

    `surface.dial <address>` (2026-09-26) is its twin for the master dial: the
    number last clicked or touched in the window, published at
    /godot/surface/dial, freed by an empty argument.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/surface/SurfaceTable.h>

namespace wfg::surface
{
    /*  Adds surface.aim, bound to the document (to ask whether a cue is media)
        and to a table the caller owns, which must outlive the registry. */
    void registerSurfaceCommands (CommandRegistry& registry, doc::ShowDocument& document,
                                  SurfaceTable& table);
}
