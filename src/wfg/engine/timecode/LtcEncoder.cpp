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

#include <wfg/engine/timecode/LtcEncoder.h>

#include <bit>
#include <cstddef>

namespace wfg::timecode
{
    namespace
    {
        /*  THE SYNC WORD, bits 64 to 79 in the order they go out: 0011 1111
            1111 1101. Laid least significant bit first it fills byte 8 with
            0xFC and byte 9 with 0xBF - libltc's 0xBFFC, read as one
            little-endian word. Twelve ones in a row appear nowhere else in a
            frame, which is how a reader finds it; the 00 before them and the
            01 after tell it which way the code is running. */
        constexpr std::uint8_t syncLow = 0xFC;
        constexpr std::uint8_t syncHigh = 0xBF;

        constexpr int dropFrameFlag = 10;

        /*  Where the polarity correction sits: bit 59 at 25, where the EBU
            standard swapped it with a binary group flag, and bit 27 at every
            other rate - the 24 and 30 families, 23.976 and 29.97 among them. */
        int polarityBitOf (Rate rate) noexcept
        {
            return rate == Rate::fps25 ? 59 : 27;
        }

        /*  `width` bits of `value` from bit `first`, least significant first.
            Only the field's own bits are ever touched, whatever the value. */
        void put (LtcBits& bits, int first, int width, int value) noexcept
        {
            for (int b = 0; b < width; ++b)
            {
                if (((value >> b) & 1) == 0)
                    continue;

                const auto n = first + b;
                auto& byte = bits[static_cast<std::size_t> (n / 8)];
                byte = static_cast<std::uint8_t> (byte | (1u << (n % 8)));
            }
        }
    }

    LtcBits ltcBits (const Label& label, Rate rate) noexcept
    {
        /*  NORMALISED FIRST, so what goes out is a label the rate has (AHF):
            the audio side hands `labelOf`'s labels, which already are, but a
            caller that hands anything else still sends a real frame. */
        const auto named = normalise (label, rate);

        LtcBits bits {};

        put (bits, 0, 4, named.frames % 10);
        put (bits, 8, 2, named.frames / 10);
        put (bits, 16, 4, named.seconds % 10);
        put (bits, 24, 3, named.seconds / 10);
        put (bits, 32, 4, named.minutes % 10);
        put (bits, 40, 3, named.minutes / 10);
        put (bits, 48, 4, named.hours % 10);
        put (bits, 56, 2, named.hours / 10);

        if (dropsFrames (rate))
            put (bits, dropFrameFlag, 1, 1);

        bits[8] = syncLow;
        bits[9] = syncHigh;

        /*  THE POLARITY BIT LAST, over everything else - sync word included,
            as libltc's `ltc_frame_set_parity` counts it - so the ones come out
            even and every frame begins on the same edge. */
        if (onesIn (bits) % 2 != 0)
            put (bits, polarityBitOf (rate), 1, 1);

        return bits;
    }

    bool ltcBit (const LtcBits& bits, int n) noexcept
    {
        if (n < 0 || n >= ltcBitsPerFrame)
            return false;

        return ((bits[static_cast<std::size_t> (n / 8)] >> (n % 8)) & 1) != 0;
    }

    int onesIn (const LtcBits& bits) noexcept
    {
        int ones = 0;

        for (const auto byte : bits)
            ones += std::popcount (byte);

        return ones;
    }

    int transitionsIn (const LtcBits& bits) noexcept
    {
        return ltcBitsPerFrame + onesIn (bits);
    }
}
