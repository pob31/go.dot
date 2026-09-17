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
    The compiled client, seen from the outside: one function.

    `wfg serve <bundle> --window` opens a window over the engine it runs inside
    (namespace draft §14.16, question E settled 2026-09-17). The console does
    not link this library - the arrow points the other way - so it takes the
    window as a factory it is handed by main(), and a build without one answers
    `--window` with a sentence. This header is the whole of that hand-off, and
    it is vendor-free for the reason Console.h is: Main.cpp includes it, and
    Main.cpp's include list is the proof that the library boundary is real.

    Everything JUCE lives on the implementation side, in ui/. Everything a test
    can hold lives in model/, which names no JUCE type at all - the boundary
    gate (scripts/check-client-boundary.py) reads the source and says so.
*/

#include <wfg/engine/Console.h>

namespace wfg::client
{
    /** The window, as the console wants it handed over. */
    ClientFactory factory();
}
