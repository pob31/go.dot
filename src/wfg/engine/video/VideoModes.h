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
    A CARD'S FORMAT AS A WORD (namespace draft §71.3). A DeckLink card offers
    formats it knows by codes of its own; the show stores what someone chose as
    a word made from the format itself, so it reads the same on every card and
    on a machine with none:

    - the usual widescreen sizes by their height: 1280x720 `720`, 1920x1080
      `1080`, 3840x2160 `2160`, 7680x4320 `4320`;
    - cinema's wider ones (2048, 4096 and 8192 wide) as `dci` and the height;
    - anything else as its width `x` its height;

    then `p` for progressive or `i` for interlaced, then the rate - pictures a
    second for progressive, fields a second for interlaced, as broadcast names
    them - whole where it is whole, else to two places: 1080p50, 1080p59.94,
    1080i50, 2160p23.98, dci1080p24, 1920x1200p60. A new card output starts at
    1080p50 (AOY).

    Plain functions, no SDK and no JUCE: the engine checks a word with them,
    the renderer matches a card's formats against them.
*/

#include <cstdint>
#include <optional>
#include <string>

namespace wfg::video::modes
{
    /*  A format: its size, its frame's duration in `timeScale` units, whether
        it is interlaced. A frame of 1080p59.94 lasts 1001 of 60000; one of
        1080i50, two fields, lasts 1 of 25. */
    struct Format
    {
        int width = 0;
        int height = 0;
        std::int64_t frameDuration = 0;
        std::int64_t timeScale = 0;
        bool interlaced = false;

        /*  Pictures a second: 25 for 1080i50, whose fields are 50. */
        double frameRate() const noexcept;
    };

    /*  The word for `format`; empty when it has no size or no rate. */
    std::string wordFor (const Format& format);

    /*  The format a word names, its rate as the exact fraction broadcast uses
        (59.94 is 60000/1001); nothing when the word is not one. */
    std::optional<Format> parse (const std::string& word);

    /*  The format a new card output starts at (AOY). */
    inline constexpr const char* defaultWord = "1080p50";

    /*  A card input's format found by the card. */
    inline constexpr const char* automatic = "auto";
}
