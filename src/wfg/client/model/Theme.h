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
    The look, as tokens - the same ones clients/console/styles.css declares.

    A compiled client cannot be refreshed: every look costs a build and a show
    restart, and the author designs by looking. So the look is not compiled in.
    It starts from the page's palette, transcribed here as defaults so the
    window opens with no file at all, and `wfg serve --window --theme=<file>`
    lays a JSON file of the same tokens over it - read at start and again on
    F5, which is the key the author already presses to see a change.

    A token the file leaves out keeps its default. A token the file misspells
    is refused BY NAME, and a file that does not parse changes nothing at all:
    a look that is half-applied is worse than one that is not, because the
    half that landed hides the sentence about the half that did not.

    Colours are 0xAARRGGBB integers here and become juce::Colour in ui/, so
    this file names no JUCE type and the model library stays testable with no
    window. A colour nobody declared answers magenta - visibly wrong on any
    ground - and never black, because black on the ground colour is invisible
    and an invisible mistake is the one that ships.
*/

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    struct Theme
    {
        /** Every font size is multiplied by this. The page's `--type`. */
        double type = 1.25;

        /** How often the window reads the engine, in passes per second. */
        double refreshHz = 25.0;

        /** A row's height in pixels before `type` scales it. The page's `--row`. */
        double row = 26.0;

        /** The palette, by the page's names, complete from construction. */
        std::map<std::string, std::uint32_t> colours;

        Theme();

        /** The names a file may set, in the order styles.css declares them. */
        static const std::vector<std::string>& colourNames();

        /** By name; 0xFFFF00FF for a name nobody declared. */
        std::uint32_t colour (std::string_view name) const;

        /*  Lays a JSON object of tokens over this theme. Empty when it landed;
            otherwise one sentence saying what was refused and where, and the
            theme is exactly as it was. */
        std::string apply (std::string_view jsonText);

        bool operator== (const Theme& other) const noexcept;
        bool operator!= (const Theme& other) const noexcept { return ! (*this == other); }
    };
}
