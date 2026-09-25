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
    THE TEST GAIN'S WHOLE STATE, as bytes (the author's decision of 2026-09-25:
    a plugin's whole state kept per cue, not only its parameters).

    What a real plugin keeps and a host cannot see - an impulse response, a
    sample - the test gain has one of: PAD, twelve decibels down, a switch
    that is NOT a parameter. So a CI runner with no plugin can still prove the
    whole path: the editing helper captures it, the cue keeps the file, and
    the voice child loads it before the cue plays and is heard a quarter as
    loud. `loadDelayMs` is for tests only: the voice child sleeps that long
    while loading, which is how a slow plugin is made on purpose.

    Plain text, a word a line, so a test can write one by hand. Std only: the
    voice child's test gain has no JUCE at all.
*/

#include <cstdlib>
#include <locale>
#include <sstream>
#include <string>

namespace wfg::plugin
{
    struct TestGainState
    {
        float gain = 0.5f;
        bool die = false;
        bool pad = false;
        int loadDelayMs = 0;

        /** What Pad does to the signal: twelve decibels, a quarter. */
        float padFactor() const noexcept { return pad ? 0.25f : 1.0f; }

        std::string toText() const
        {
            std::ostringstream out;
            out.imbue (std::locale::classic());
            out << "gain=" << gain << "\ndie=" << (die ? 1 : 0) << "\npad=" << (pad ? 1 : 0) << "\n";

            if (loadDelayMs > 0)
                out << "loadDelayMs=" << loadDelayMs << "\n";

            return out.str();
        }

        static TestGainState fromText (const std::string& text)
        {
            TestGainState state;
            std::istringstream in (text);
            in.imbue (std::locale::classic());
            std::string line;

            while (std::getline (in, line))
            {
                const auto equals = line.find ('=');

                if (equals == std::string::npos)
                    continue;

                const auto key = line.substr (0, equals);
                std::istringstream value (line.substr (equals + 1));
                value.imbue (std::locale::classic());

                if (key == "gain")              value >> state.gain;
                else if (key == "die")          { int v = 0; value >> v; state.die = v != 0; }
                else if (key == "pad")          { int v = 0; value >> v; state.pad = v != 0; }
                else if (key == "loadDelayMs")  value >> state.loadDelayMs;
            }

            return state;
        }
    };
}
