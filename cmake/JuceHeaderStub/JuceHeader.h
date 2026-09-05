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
    A stub `JuceHeader.h`, on the include path of the juce_simpleweb module and
    of nothing else.

    juce_simpleweb's .cpp files were authored for the Projucer, which GENERATES
    a global JuceHeader.h naming every module in the project and puts it on the
    include path. A CMake build has no such file - JUCE 8's supported style is to
    include module headers directly (JUCE docs/CMake API.md:721-723), and Go.dot
    does exactly that everywhere else. So the module's four .cpp files would fail
    on their first line with "JuceHeader.h: No such file or directory".

    The two includes below are the whole of what those .cpp files actually reach
    for through it. They are deliberately NOT the full module list a Projucer
    header would carry: this file exists to satisfy an include, not to become a
    second, competing definition of Go.dot's compile environment. That lives in
    wfg::deps, in one place, and adding a module here rather than there is how it
    would quietly acquire two.

    It is reached ONLY by the juce_simpleweb module target, because
    WfgThirdParty.cmake puts this directory on that target's interface and
    nothing else's. Our own translation units never see it, and a `#include
    <JuceHeader.h>` anywhere in src/ or tests/ is still the error it should be.

    (spatcore ships an equivalent stub for the same reason. It is transcribed
    rather than included, for the reason the whole recipe is: see
    WfgThirdParty.cmake section 3b.)
*/

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
