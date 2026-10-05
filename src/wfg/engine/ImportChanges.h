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
    AN ABLETON LIVE IMPORT, AS THE WINDOW ASKS FOR ONE, in plain values
    (namespace draft §29).

    The window's File > Import Ableton Live set... reads a set's scenes to
    offer them as a list with a tick box each (QS), then hands back the sets,
    the scenes ticked and where the show goes, and is told what was written.
    STD ONLY, as Console.h is, so the client holds these without naming the
    importer's types (scripts/check-client-boundary.py). The work is
    import/AlsImport.h's.
*/

#include <functional>
#include <string>
#include <vector>

namespace wfg
{
    /*  ONE SCENE AS THE LIST OFFERS IT: its number as Live shows it, its name,
        the first line of its annotation, and whether it is ticked to start -
        named and doing something. */
    struct ImportScene
    {
        int index = 0;
        std::string name;
        std::string firstLine;
        bool doesSomething = false;
        bool ticked = false;
    };

    struct ImportScenes
    {
        std::string error;                  ///< why the set could not be read; empty when it was
        std::string creator;                ///< "Ableton Live 11.2.6"
        std::vector<ImportScene> scenes;
    };

    struct ImportRequest
    {
        std::vector<std::string> sets;      ///< one set is a show; several are a tour (QF)
        std::string templateSet;            ///< with several: the template, empty for the newest
        std::vector<int> scenes;            ///< ticked, by index in the template's set
        std::string into;                   ///< the show folder to make
    };

    struct ImportResult
    {
        bool ok = false;
        std::string said;                   ///< a sentence for the foot
        std::string show;                   ///< the folder written, to open
        std::string report;                 ///< the report beside it, to open
        int performances = 0;
    };
}
