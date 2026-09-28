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
    A media cue's EQ, as a value.

    Twenty-three numbers and flags: whether the EQ is in at all, a high-pass
    and a low-pass each with a flag and a frequency, and four bands each with a
    switch, a shape, a frequency, a gain and a width. The document holds them
    as twenty-three rows on the media cue (namespace draft §17.2; the band
    switches since 2026-09-25); this is the same twenty-three
    as one struct, which is what crosses from the tick thread to the audio
    side - in an ArmRequest at arm, and through CueEq::set while a cue sounds.

    NAMES NO JUCE OR TRACKTION TYPE, deliberately. The desktop client's EQ
    panel reads the same twenty-three off the tree and draws the response through
    EqMath.h, which takes this struct; the client boundary forbids JUCE in
    model/ and the DSP is happier without it anyway.

    FLAT BY CONSTRUCTION. The defaults here are the CSV rows' defaults, and a
    default-constructed EqSettings is identity: every gain nought, both filters
    off. PRD §4.6 - every parameter has a defined resting state - is the
    default, and eq.reset writes exactly this.

    EQUALITY IS BITWISE on the floats. The strict Linux build treats -Wfloat-equal
    as an error, and it is right to: a comparison meant to answer "did anything
    change since the last push" wants the bits, not a tolerance.
*/

#include <bit>
#include <cstdint>

namespace wfg::audio
{
    struct EqSettings
    {
        /** What a band is. The document lets band one be a peak or a low shelf
            and band four a peak or a high shelf; the DSP takes any shape on any
            band and the rows keep the designer honest. */
        enum class Shape : int
        {
            peak = 0,
            lowShelf = 1,
            highShelf = 2
        };

        struct Band
        {
            Shape shape = Shape::peak;
            float freq = 1000.0f;
            float gain = 0.0f;
            float q = 0.7f;

            /** Whether the band is in the signal (media/eqB<n>On). Off keeps
                every number, so switching it back on brings back the same
                band - the click of a rotary on a page. Last, so a braced
                shape-frequency-gain-width reads as a band that is on. */
            bool on = true;
        };

        static constexpr int numBands = 4;

        bool on = true;
        bool hpf = false;
        bool lpf = false;
        float hpfFreq = 80.0f;
        float lpfFreq = 12000.0f;

        Band band[numBands] = { { Shape::peak, 100.0f, 0.0f, 0.7f },
                                { Shape::peak, 500.0f, 0.0f, 0.7f },
                                { Shape::peak, 2000.0f, 0.0f, 0.7f },
                                { Shape::peak, 8000.0f, 0.0f, 0.7f } };

        /** The resting state: every gain nought, both filters off, on. */
        static EqSettings flat() noexcept { return {}; }

        /** Whether a band contributes anything: a band switched off, or with a
            gain of exactly nought, is out of the signal, whatever its
            frequency and width say. */
        static bool bandIsActive (const Band& b) noexcept
        {
            return b.on
                && std::bit_cast<std::uint32_t> (b.gain) != std::bit_cast<std::uint32_t> (0.0f)
                && std::bit_cast<std::uint32_t> (b.gain) != std::bit_cast<std::uint32_t> (-0.0f);
        }

        /** Whether this EQ would change a signal at all. */
        bool isIdentity() const noexcept
        {
            if (! on)
                return true;

            if (hpf || lpf)
                return false;

            for (const auto& b : band)
                if (bandIsActive (b))
                    return false;

            return true;
        }

        /** Field-wise equality, bitwise on the floats. */
        bool sameAs (const EqSettings& other) const noexcept
        {
            if (on != other.on || hpf != other.hpf || lpf != other.lpf)
                return false;

            if (! sameFloat (hpfFreq, other.hpfFreq) || ! sameFloat (lpfFreq, other.lpfFreq))
                return false;

            for (int i = 0; i < numBands; ++i)
                if (! sameBand (band[i], other.band[i]))
                    return false;

            return true;
        }

        static bool sameFloat (float a, float b) noexcept
        {
            return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b);
        }

        static bool sameBand (const Band& a, const Band& b) noexcept
        {
            return a.shape == b.shape
                && a.on == b.on
                && sameFloat (a.freq, b.freq)
                && sameFloat (a.gain, b.gain)
                && sameFloat (a.q, b.q);
        }
    };
}
