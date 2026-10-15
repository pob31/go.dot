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

#include <wfg/engine/command/RescanCommands.h>
#include <wfg/engine/command/CommandRegistry.h>

#include <utility>
#include <vector>

namespace wfg
{
    void registerRescanCommands (CommandRegistry& registry, std::function<void()> midi, std::function<void()> serial)
    {
        registry.add (machineLocal ({ "midi.rescan",
                        "Lists this machine's MIDI devices again (namespace draft 64): a declared port with no"
                        " device is tried again, so one plugged in since the show opened is bound now, and a bound"
                        " port whose device has gone is let go of and says so. ports/scans counts each look."
                        " Taken and ignored where nothing listens.",
                        {},
                        false,
                        [look = std::move (midi)] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (look)
                                look();

                            return Outcome::ok (args);
                        } }));

        registry.add (machineLocal ({ "serial.rescan",
                        "Lists this machine's serial ports again at once (namespace draft 64), rather than at the"
                        " next five-second look. engine/serialScans counts each look."
                        " Taken and ignored where nothing listens.",
                        {},
                        false,
                        [look = std::move (serial)] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (look)
                                look();

                            return Outcome::ok (args);
                        } }));
    }
}
