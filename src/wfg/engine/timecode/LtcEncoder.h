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
    What 80 bits an LTC frame is (namespace draft §59, TC.2): the words the
    audio thread will draw as biphase-mark sound (TC.3), worked out here on
    their own so the drawing and a test's decoder can both be checked against
    the standard rather than against each other.

    THE LAYOUT is SMPTE 12M's as it is publicly documented, checked on
    2026-10-12 against two descriptions that agree with each other and with
    this table: the bit table of Wikipedia's "Linear timecode" article, and
    the `LTCFrame` structure and `ltc_frame_set_parity` in libltc's `ltc.h`
    (Robin Gareus, github.com/x42/libltc). Each field goes out least
    significant bit first, in binary-coded decimal:

        0-3    frame units           32-35  minute units
        4-7    user bits 1           36-39  user bits 5
        8-9    frame tens            40-42  minute tens
        10     drop-frame flag       43     BGF0 (BGF2 at 25)
        11     colour frame          44-47  user bits 6
        12-15  user bits 2           48-51  hour units
        16-19  second units          52-55  user bits 7
        20-23  user bits 3           56-57  hour tens
        24-26  second tens           58     BGF1
        27     polarity (BGF0 at 25) 59     BGF2 (polarity at 25)
        28-31  user bits 4           60-63  user bits 8
                                     64-79  sync word 0011 1111 1111 1101

    The user bits, the colour frame and the binary group flags are all nought:
    Go.dot carries no user data, and claims no colour lock and no wall clock.
    The drop-frame flag is set at 29.97 drop-frame alone.

    THE POLARITY-CORRECTION BIT makes the number of ones in the 80 bits even -
    libltc's words are "an even number of zeroes", the same thing in a word of
    80. In biphase-mark every bit cell begins with a transition and a one adds
    a second in its middle, so a frame holds 80 + ones transitions; an even
    count ends each frame on the level it began on, so every frame starts on
    the same edge and a scope or a reader finds the sync word the same way up
    each time. The bit is 27 at 24, 23.976 and the 30 family and 59 at 25,
    where the EBU standard swapped it with a group flag.

    PURE, and no allocation: TC.3 builds one word per frame on the audio thread.
*/

#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstdint>

namespace wfg::timecode
{
    constexpr int ltcBitsPerFrame = 80;

    /*  An LTC frame's 80 bits, bit 0 first on the line: bit `n` is
        `(bits[n / 8] >> (n % 8)) & 1`, which is also how libltc's `LTCFrame`
        lies in memory on a little-endian machine. */
    using LtcBits = std::array<std::uint8_t, 10>;

    /*  The frame a label is at a rate. The label is normalised first, so a
        name the rate lacks is sent as the next one there is (AHF), and each
        digit is kept to its field's width so no field ever spills into the
        flag beside it. */
    LtcBits ltcBits (const Label& label, Rate rate) noexcept;

    //  Bit `n` of a frame, 0-79; anything outside reads as nought.
    bool ltcBit (const LtcBits& bits, int n) noexcept;

    //  How many of the 80 bits are ones: even, in every frame `ltcBits` makes.
    int onesIn (const LtcBits& bits) noexcept;

    /*  How many level changes a frame draws in biphase-mark: one at the start
        of every cell and one more in the middle of every one - 80 + ones. */
    int transitionsIn (const LtcBits& bits) noexcept;
}
