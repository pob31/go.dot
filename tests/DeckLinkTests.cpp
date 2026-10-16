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

/*  DECKLINK CARDS (namespace draft §71). No card is on any machine that runs
    this suite, CI's or the author's: what reaches a real card is checked by the
    testers' probe (DL.10). These cases hold what can be held without one. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/video/render/DeckLinkApi.h>

#include <cstring>

TEST_CASE ("decklink: the SDK's headers are 16.0's and reach the library's entry point (DL.0)")
{
    //  The version the headers were carried at (scripts/vendor-decklink.py),
    //  which is the oldest Desktop Video Go.dot asks for (APA).
    CHECK (BLACKMAGIC_DECKLINK_API_VERSION == 0x10000000);
    CHECK (std::strcmp (BLACKMAGIC_DECKLINK_API_VERSION_STRING, "16.0") == 0);

    //  The way to the library links on this system: MIDL's identifiers on
    //  Windows, the dispatch file's loader elsewhere. Nothing is called - with
    //  no Desktop Video here, which the next stages say in words.
   #if defined (_WIN32)
    CHECK (CLSID_CDeckLinkIterator.Data1 != 0u);
    CHECK (IID_IDeckLinkOutput.Data1 != 0u);
   #else
    IDeckLinkIterator* (* volatile create)() = CreateDeckLinkIteratorInstance;
    CHECK (create != nullptr);
   #endif
}
