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
    PHASE 0 SCAFFOLD — replaced wholesale in Phase 1.

    This header exists to prove one structural property of the build, and it
    proves it by what it does NOT contain: there is no #include here, and no
    JUCE or Tracktion type appears in the declared surface.

    That absence is load-bearing. It is what lets Phase 5's UI client and
    Phase 9's plugin host / out-of-process plugin scanner link wfg::engine
    without each of them dragging in — and recompiling — the whole JUCE and
    Tracktion header set. The moment a juce::String or a tracktion::TimePosition
    appears in a public signature here, every consumer of this library inherits
    31 vendor translation units' worth of headers, and the boundary is gone.

    Phase 1 will widen this surface. Keep the rule: vendor types stay on the
    implementation side of it.
*/

namespace wfg
{
    /** Runs the Go.dot console front end. Returns the process exit code. */
    int runConsole (int argc, char** argv);
}
