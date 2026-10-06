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

#include <wfg/engine/video/VideoCommands.h>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/video/VideoHost.h>

#include <string>
#include <vector>

namespace wfg::video
{
    void registerVideoCommands (CommandRegistry& registry, VideoHost* host)
    {
        registry.add ({ "videoOutput.identify",
                        "Puts a video output's test pattern - a white frame and a cross - over what it shows,"
                        " or takes it away: to find which projector is which. Tonight's, never saved.",
                        { { "output", 's', false }, { "on", 'T', false } },
                        false,
                        [host] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (host != nullptr)
                                host->identify (args[0].getString(), args[1].isBool() && args[1].getBool());

                            return Outcome::ok (args);
                        } });
    }
}
