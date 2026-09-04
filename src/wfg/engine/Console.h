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
    The `wfg` command line.

    Replaces the Phase 0 Boot.h, whose job was to prove the toolchain; this one
    drives the engine. The surface grows one verb per subphase - `serve`,
    `validate`, `canon` and `schema` arrive with the document and the OSCQuery
    server - and every verb that writes anything a human or a diff will read
    accepts --wfg-locale, so the same binary can be run twice under two locales
    and the results compared (the cross-cutting rule in the development plan).

    Vendor-free, like Engine.h, and for the same reason.
*/

namespace wfg
{
    /** Runs the Go.dot console front end. Returns the process exit code. */
    int runConsole (int argc, char** argv);
}
