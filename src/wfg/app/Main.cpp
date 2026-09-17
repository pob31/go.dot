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
    The entry point, and deliberately nothing else.

    The interesting thing about this file is the include list: two headers,
    ours, and no JUCE. That is the proof that the src/ library boundary is
    real. If this file ever needs <juce_core/juce_core.h> to compile,
    something has leaked a vendor type into wfg/engine/Console.h or
    wfg/client/Client.h and the boundary described there has quietly stopped
    existing.

    The second header is the compiled client (namespace draft §14.16), handed
    to the console as a factory rather than linked by it: the client library
    links the engine, so the engine cannot link the client back. This is the
    one place that knows both, which is what an entry point is for.

    Plain main(), not JUCE's START_JUCE_APPLICATION: we build with
    JUCE_MODULES_ONLY=ON and a plain add_executable, so JUCE_STANDALONE_APPLICATION
    is never defined and the macro would expand to nothing useful. The JUCE
    message thread is started where it is actually needed, inside
    wfg::runConsole, by a scoped initialiser.
*/

#include <wfg/client/Client.h>
#include <wfg/engine/Console.h>

int main (int argc, char** argv)
{
    return wfg::runConsole (argc, argv, wfg::client::factory());
}
