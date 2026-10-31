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
    The system's message queue, emptied, for a window test that needs a window
    really shown (namespace draft §125: a touch is given to a component only
    once the system says the window is under it). Its own file, because
    windows.h's macros - min, max, small - break the test files that would
    otherwise include it.
*/

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <thread>

namespace wfg::test
{
    void pumpWindowMessages (int milliseconds)
    {
        MSG message {};

        while (PeekMessage (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessage (&message);
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (milliseconds));
    }
}
