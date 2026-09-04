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
    Shared declarations for the test suite.

    Phase 0 had three test translation units and one shared symbol between them,
    and said in TestMain.cpp that the fourth was the moment to add this header.
    Phase 1 is that moment.

    Keep it small: declarations and tiny helpers only. A test helper with real
    behaviour in it needs its own test, and a suite that tests its own scaffolding
    is a suite that has stopped testing the product.
*/

#include <string>

namespace wfgtest
{
    /*  The locale the runner actually applied, as spelled on the command line.
        Defined in TestMain.cpp. The NAME is never hard-coded in a test: the
        Windows UCRT wants "fr-FR" and glibc wants "fr_FR.UTF-8", so
        WFG_LOCALE_FR (cmake/WfgOptions.cmake) is the single place that
        difference is allowed to exist. */
    const char* appliedLocaleName();

    /** True when the suite is running under the French locale rather than C. */
    inline bool runningUnderFrenchLocale()
    {
        return std::string (appliedLocaleName()) == std::string (WFG_LOCALE_FR);
    }
}
