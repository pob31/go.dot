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

#include <wfg/engine/video/render/Card.h>

#include <juce_core/juce_core.h>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <objbase.h>
#endif

namespace wfg::video::card
{
    System& system()
    {
        //  Made once, from whichever thread asks first; a function-local
        //  static is made once whatever the threads (C++11).
        static const std::unique_ptr<System> made =
            juce::SystemStats::getEnvironmentVariable ("WFG_DECKLINK_FAKE", {}) == "1" ? makeFakeSystem()
                                                                                       : makeDeckLinkSystem();
        return *made;
    }

    ThreadScope::ThreadScope()
    {
       #if defined (_WIN32)
        //  The multithreaded apartment: a card's callbacks come on the
        //  driver's own threads. A thread already in a single-threaded one
        //  (JUCE's message thread) is refused, and is not where cards are
        //  opened.
        entered = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
       #endif
    }

    ThreadScope::~ThreadScope()
    {
       #if defined (_WIN32)
        if (entered)
            CoUninitialize();
       #endif
    }

    const char* keyingWord (Keying keying) noexcept
    {
        switch (keying)
        {
            case Keying::external: return "external";
            case Keying::internal: return "internal";
            case Keying::off:      break;
        }

        return "off";
    }

    Keying keyingFrom (const std::string& word) noexcept
    {
        if (word == "external")  return Keying::external;
        if (word == "internal")  return Keying::internal;
        return Keying::off;
    }

    const char* referenceWord (Reference reference) noexcept
    {
        switch (reference)
        {
            case Reference::locked:   return "locked";
            case Reference::unlocked: return "unlocked";
            case Reference::none:     break;
        }

        return "none";
    }

    std::string describe (const Found& found)
    {
        if (! found.problem.empty())
            return "problem\t" + found.problem;

        std::string out;

        for (const auto& card : found.cards)
        {
            std::string can;
            const auto add = [&can] (bool has, const std::string& word)
            {
                if (has)
                    can += (can.empty() ? "" : " ") + word;
            };

            add (card.output, "output");
            add (card.input, "input");
            add (card.internalKey, "internal");
            add (card.externalKey, "external");
            add (card.tenBit, "10bit");
            add (card.audioChannels > 0, "sound" + std::to_string (card.audioChannels));

            std::string sends, takes;

            for (const auto& mode : card.modes)
            {
                if (mode.output)
                    sends += (sends.empty() ? "" : " ") + mode.word;

                if (mode.input)
                    takes += (takes.empty() ? "" : " ") + mode.word;
            }

            out += (out.empty() ? "" : "\n") + card.id + "\t" + card.name + "\t" + can + "\t" + sends + "\t" + takes;
        }

        return out;
    }

    int rowBytesFor (PixelFormat format, int width) noexcept
    {
        switch (format)
        {
            case PixelFormat::bgra8: return width * 4;
            case PixelFormat::yuv8:  return width * 2;
            case PixelFormat::yuv10: return ((width + 47) / 48) * 128;
        }

        return width * 4;
    }
}
