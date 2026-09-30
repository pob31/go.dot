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
    ANOTHER GO.DOT'S WINDOW, BROUGHT TO THE FRONT: what a double-click on a
    show that is already open does instead of opening it twice (OpenShows.h).

    By process, because that is all one process knows of another's window.
    Windows finds the process's largest visible top-level window and puts it
    in front; macOS activates the application; Linux answers false, and the
    caller says the show is already open instead.

    STD ONLY, so that <windows.h> and AppKit stay in the .cpp and the .mm.
*/

namespace wfg::app
{
    /*  This process, as the system numbers it. */
    long currentProcessId() noexcept;

    /*  True when the window came forward, or at least the system was asked
        and did not refuse. */
    bool raiseProcessWindows (long processId);
}
