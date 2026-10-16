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
    THE DECKLINK SDK, ONE INCLUDE FOR EVERY SYSTEM (namespace draft §71, APA).

    Go.dot carries the SDK's three include folders in ThirdParty/decklink -
    their agreement lifts its restrictions for them, and their notice asks only
    to be kept (PRD §6.3) - and never the library, which comes with the user's
    Desktop Video. Each system reaches it its own way:

    - Windows: COM. The header MIDL made from DeckLinkAPI.idl when the files
      were copied in, and its identifiers, compiled into wfg_thirdparty; an
      iterator is CoCreateInstance(CLSID_CDeckLinkIterator) on a thread that has
      called CoInitializeEx - never JUCE's message thread, a single-threaded
      apartment of JUCE's.
    - macOS and Linux: the SDK's dispatch file, compiled into wfg_thirdparty,
      loads DeckLinkAPI.framework or libDeckLinkAPI.so the first time
      CreateDeckLinkIteratorInstance is called, and returns null when Desktop
      Video is not installed.

    Only render/Card_decklink.cpp and the tests include this: everything else
    talks to render/Card.h.
*/

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <objbase.h>
 #include <oleauto.h>
 #include <DeckLinkAPI_h.h>
#else
 #include <DeckLinkAPI.h>
#endif

#include <DeckLinkAPIVersion.h>
