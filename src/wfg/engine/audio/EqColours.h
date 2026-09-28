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
    THE EQ'S COLOURS, ONE PER HANDLE: the high-pass, the four bands, the
    low-pass - spatcore's own first six, red to purple along the field as the
    handles stand by default (author, 2026-09-25: "Having different colours on
    each handle like on the EQ of the spatcore library really helps").

    ONE SOURCE FOR TWO READERS. The desktop's EQ panel draws its handles in
    them (the theme's `eq-hp`, `eq-1`..`eq-4`, `eq-lp`, whose defaults are
    these), and a D700 lights each rotary of its EQ page in its band's colour
    (author, 2026-09-25: "Use the colour coding of each band as in the rest of
    the interface"). The engine links no client, so the numbers live here,
    beside the EQ, and the client's theme takes its defaults from them. A
    theme file that recolours the window recolours the window only: the
    surface keeps these.

    0xAARRGGBB, as the theme spells a colour. std only.
*/

#include <array>
#include <cstdint>

namespace wfg::audio
{
    inline constexpr std::uint32_t eqHighPassColour = 0xFFE74C3C;   // red
    inline constexpr std::uint32_t eqBand1Colour    = 0xFFE67E22;   // orange
    inline constexpr std::uint32_t eqBand2Colour    = 0xFFFFEB3B;   // yellow
    inline constexpr std::uint32_t eqBand3Colour    = 0xFF2ECC71;   // green
    inline constexpr std::uint32_t eqBand4Colour    = 0xFF3498DB;   // blue
    inline constexpr std::uint32_t eqLowPassColour  = 0xFF9B59B6;   // purple

    /** In field order: the high-pass, bands one to four, the low-pass. */
    inline constexpr std::array<std::uint32_t, 6> eqColours { eqHighPassColour, eqBand1Colour,
                                                              eqBand2Colour,    eqBand3Colour,
                                                              eqBand4Colour,    eqLowPassColour };
}
