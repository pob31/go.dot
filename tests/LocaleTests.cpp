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
    PHASE 0 SCAFFOLD — the locale obligation, stood up before there is anything
    to serialise.

    devplan:337 requires every serialisation test to run under fr_FR as well as
    C. Phase 0 serialises nothing, so what these two cases establish is the
    HARNESS and the hazard, so that Phase 1's document tests inherit a working
    one instead of inventing it.

    The hazard, concretely: under a French locale the C library's "%f" family
    writes "0,5". A show document that stores a fade time as "0,5" is a file
    that reopens wrong — or does not reopen — on an English machine, and PRD
    §3.20 exists because that is a premiere-night failure and not an
    inconvenience. JUCE's own number formatting is locale-independent by
    design; case 2 pins that promise so we notice if it ever stops being true.

    INTERPRETATION FLAG, for the author: devplan:337 is read here as an
    IN-PROCESS std::setlocale, not as an LC_ALL environment variable on the
    test process. In-process is the only reading that behaves identically on
    glibc, the macOS libc and the Windows UCRT, but it IS a reading, and it is
    worth confirming before Phase 1 writes assertions on top of it.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <juce_core/juce_core.h>

#include <clocale>
#include <cstdio>
#include <string>


//==============================================================================
TEST_CASE ("locale: the requested locale actually took effect")
{
    /*  THE ANTI-NO-OP GUARD, and the reason this case is written the way it is.

        A locale test that merely runs twice proves nothing: if setlocale
        silently did nothing, both runs would be a "C" run and both would pass.
        So this case asserts the DIFFERENCE — that the C library really is
        formatting with a comma under fr_FR and a full stop under C. If the
        locale switch is a no-op, the fr_FR run fails here, loudly.

        The locale NAME is never spelled out below. WFG_LOCALE_FR carries it
        because the Windows UCRT wants "fr-FR" and glibc wants "fr_FR.UTF-8";
        hard-coding either one here would make this file pass on one platform
        and be meaningless on another. (Both spellings verified working on
        their own platform, 2026-09-04.)
    */
    const std::string applied { wfgtest::appliedLocaleName() };
    const std::string french  { WFG_LOCALE_FR };

    INFO ("locale in effect: " << applied);

    char formatted[32] = {};
    std::snprintf (formatted, sizeof (formatted), "%.1f", 0.5);

    const std::string decimalPoint { std::localeconv()->decimal_point };

    if (applied == french)
    {
        CHECK (decimalPoint == ",");
        CHECK (std::string (formatted) == "0,5");
    }
    else if (applied == "C")
    {
        CHECK (decimalPoint == ".");
        CHECK (std::string (formatted) == "0.5");
    }
    else
    {
        // Someone ran the binary by hand with a third locale. Do not guess what
        // its decimal separator should be; the next case still applies to it.
        MESSAGE ("no decimal-separator expectation for locale \"" << applied << "\" - skipping that half");
    }
}

//==============================================================================
TEST_CASE ("locale: JUCE number formatting is locale-independent")
{
    /*  This is the property Phase 1's document layer will depend on, and the
        single most useful thing this file asserts: juce::String's number
        conversion must produce "0.5" under EVERY locale, because that is what
        goes into the show file.

        It runs under both locales for the obvious reason — under "C" it is a
        tautology, and under fr_FR it is the actual test. Keeping the tautology
        is deliberate: if it ever fails under "C", something far stranger has
        happened than a locale bug.
    */
    // std::string, not the bare const char* - doctest stringifies a raw
    // char pointer as an ADDRESS, which turns a useful failure message into
    // "locale in effect: 00007FF72DA9E500". Found the hard way, 2026-09-04.
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CHECK (juce::String (0.5).toStdString() == "0.5");
    CHECK (juce::String (-0.25).toStdString() == "-0.25");

    // The round trip matters as much as the formatting: a reader that honours
    // the locale would parse "0.5" as 0 on a French machine.
    CHECK (juce::String ("0.5").getDoubleValue() == doctest::Approx (0.5));
}
