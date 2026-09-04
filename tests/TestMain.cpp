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
    PHASE 0 SCAFFOLD — the doctest entry point and the locale switch.

    This is the ONE translation unit that defines DOCTEST_CONFIG_IMPLEMENT, and
    it must stay the only one. The include below is Tracktion's WRAPPER around
    doctest, never doctest.h itself: the wrapper brackets doctest in choc's
    warning-suppression pragmas — which is what lets it compile under
    juce_recommended_warning_flags, and under -Werror in the `strict` preset —
    and it #undefs DOCTEST_CONFIG_IMPLEMENT on the way out, so the other test
    TUs can include the same wrapper without redefining the runner.

    It resolves with no include wiring of its own because wfg::deps puts
    ThirdParty/tracktion_engine/modules on the SYSTEM include path.
*/

#define DOCTEST_CONFIG_IMPLEMENT
#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <clocale>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace wfgtest
{
    namespace
    {
        std::string appliedLocale { "C" };
    }

    /*  Defined here, re-declared (not #included) in LocaleTests.cpp.

        Phase 0 has no tests/ header and should not grow one for a single
        function: three source files and one shared symbol is not a library.
        If a fourth test TU ever needs this, that is the moment to add
        tests/TestSupport.h — not before.
    */
    const char* appliedLocaleName()
    {
        return appliedLocale.c_str();
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    /*  --wfg-locale=<name> is OURS, not doctest's. doctest would treat an
        unrecognised argument as a test-name filter, so it is stripped from the
        vector before applyCommandLine ever sees it. Everything else is passed
        straight through, which keeps `wfg_tests --test-case=...` and the rest
        of doctest's command line working for a human debugging a failure.

        The name is deliberately NOT hard-coded on either side: the Windows UCRT
        wants "fr-FR" and glibc wants "fr_FR.UTF-8", and cmake/WfgOptions.cmake's
        WFG_LOCALE_FR is the single place that difference is allowed to exist.
    */
    static constexpr const char* localeFlag = "--wfg-locale=";
    const auto localeFlagLength = std::strlen (localeFlag);

    std::vector<const char*> forwarded;
    forwarded.reserve (static_cast<std::size_t> (argc));

    const char* requestedLocale = nullptr;

    for (int i = 0; i < argc; ++i)
    {
        if (i > 0 && std::strncmp (argv[i], localeFlag, localeFlagLength) == 0)
            requestedLocale = argv[i] + localeFlagLength;
        else
            forwarded.push_back (argv[i]);
    }

    if (requestedLocale != nullptr)
    {
        /*  HARD FAILURE, not a skip, and this is the point of the whole file.

            If the locale is not installed on this machine, the locale-dependent
            assertions in LocaleTests.cpp would all silently run under "C" and
            report green — a suite that proves nothing while looking like it
            proved something. That is exactly the premiere-night bug PRD §3.20
            exists to prevent, so a missing locale must redden the build.

            On the Linux runner the prerequisite is `locale-gen fr_FR.UTF-8`;
            ci.yml runs it, and this exit code is what catches it if it stops
            running it.
        */
        if (std::setlocale (LC_ALL, requestedLocale) == nullptr)
        {
            std::cerr << "FATAL: locale \"" << requestedLocale
                      << "\" is not available on this system" << std::endl;
            return 2;
        }

        wfgtest::appliedLocale = requestedLocale;
    }

    doctest::Context context;
    context.applyCommandLine (static_cast<int> (forwarded.size()), forwarded.data());

    // context.run() returns 0 only if every assertion passed, and returns
    // doctest's own code for the query options (--help, --list-test-cases).
    // There is nothing to add on top of it, and adding something would give
    // the suite's verdict a second place to live.
    return context.run();
}
