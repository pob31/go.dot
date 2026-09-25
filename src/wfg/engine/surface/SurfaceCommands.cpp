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

#include <wfg/engine/surface/SurfaceCommands.h>

#include <string>

namespace wfg::surface
{
    void registerSurfaceCommands (CommandRegistry& registry, doc::ShowDocument& document,
                                  SurfaceTable& table)
    {
        registry.add ({ "surface.aim",
                        "Chooses the media cue a surface's rotaries edit on its EQ and Send pages,"
                        " or none with an empty argument. One cue for every surface.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &table] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            /*  EMPTY IS NONE, and applied: letting go of the
                                aim is a thing a hand does - a second press of
                                a lit SELECT - not a mistake. */
                            if (id.empty())
                            {
                                table.setAim ({});
                                return Outcome::ok (args);
                            }

                            const auto cue = document.findById (id);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (! cue.hasType ("Media"))
                                return Outcome::rejected (reason::badValue);

                            table.setAim (id);
                            return Outcome::ok (args);
                        } });
    }
}
