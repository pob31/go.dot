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

/*
    THIS COMPUTER'S POWER (namespace draft §108): what the system is told and
    what it says. The Mac declares and reads; everywhere else is unhindered
    and declares nothing, and both are checked for saying so truthfully.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/app/MachinePower.h>

#include <juce_core/juce_core.h>

#include <string>

using namespace wfg;

TEST_CASE ("power: the process declares itself latency-critical where the system takes it, and reads what holds the computer back")
{
    const app::LatencyCritical critical { "Go.dot tests" };
    const auto power = app::readMachinePower();
    const std::string thermal = app::wordOf (power.thermal);

    CHECK ((thermal == "nominal" || thermal == "fair" || thermal == "serious" || thermal == "critical"));

   #if JUCE_MAC
    CHECK (critical.declared());
   #else
    CHECK_FALSE (critical.declared());
    CHECK_FALSE (power.lowPowerMode);
    CHECK (thermal == "nominal");
   #endif

    //  The four words, each its own, and nominal for anything out of range.
    CHECK (std::string (app::wordOf (app::Thermal::nominal)) == "nominal");
    CHECK (std::string (app::wordOf (app::Thermal::fair)) == "fair");
    CHECK (std::string (app::wordOf (app::Thermal::serious)) == "serious");
    CHECK (std::string (app::wordOf (app::Thermal::critical)) == "critical");
    CHECK (std::string (app::wordOf (static_cast<app::Thermal> (9))) == "nominal");

    //  Declared twice over is two activities, each ended on its own.
    {
        const app::LatencyCritical again { "Go.dot tests, again" };
        CHECK (again.declared() == critical.declared());
    }

    CHECK (app::readMachinePower() == power);
}
