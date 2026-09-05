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
    The one spatcore header Phase 1 compiles.

    spatcore is consumed at SOURCE level - there is no add_subdirectory and no
    CMake target, because spatcore's own targets call juce_add_modules() again
    and would compile JUCE a second time in this build tree. So the submodule
    contributes exactly one thing to Phase 1: an include path. Nothing else in
    the build would notice if that path broke, which is why this file exists.

    A TRANSLATION UNIT OF ITS OWN, and that is a hard requirement rather than
    tidiness. RtThreadPriority.h includes <windows.h>. asio - which arrives in
    SimpleWebToolchainTests.cpp through juce_simpleweb - includes <winsock2.h>.
    windows.h pulls in winsock.h, which redefines what winsock2.h declares, and
    the result is a wall of redefinition errors on Windows alone. The two
    headers are kept in separate files, and this comment is the reason anyone
    will find when they try to merge them.

    WHAT IS DELIBERATELY NOT ASSERTED is whether the elevation succeeded. It is
    allowed to fail: an unprivileged CI container cannot get SCHED_FIFO, and a
    test that REQUIREd success would be red on the runner and green on a
    workstation for reasons that have nothing to do with any code. Phase 2 is
    where a failed elevation becomes something an operator has to be told about
    (PRD 4.2); Phase 1 only needs to know the header compiles, links, and
    returns.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <spatcore/rt/RtThreadPriority.h>

//==============================================================================
TEST_CASE ("spatcore: RtThreadPriority.h compiles, links and answers")
{
    /*  One audio block at 48 kHz / 128 frames is 2.67 ms, and a tenth of that
        is a fair guess at the computation budget. The numbers only reach the
        macOS time-constraint policy; Windows and Linux ignore them. */
    const auto elevated = spatcore::rt::setCurrentThreadAudioPriority (2.67, 0.27);

    INFO ("setCurrentThreadAudioPriority returned " << (elevated ? "true" : "false"));
    CHECK ((elevated == true || elevated == false));

    /*  The other half of the header, and the one with a contract worth
        checking: physicalCoreCount() documents "Always >= 1", including on the
        fallback path where the OS topology query is unavailable. Phase 2 sizes
        its worker pools off this, and a zero would be a division by it. */
    const auto cores = spatcore::rt::physicalCoreCount();

    INFO ("physicalCoreCount() returned " << cores);
    CHECK (cores >= 1);
}
