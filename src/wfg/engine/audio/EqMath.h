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
    The EQ's arithmetic: second-order sections from a band's numbers, and the
    magnitude a section has at a frequency.

    ONE FUNCTION FOR THE SOUND AND THE PICTURE. CueEq builds its filters from
    the coefficient functions below, and the desktop client's EQ panel draws
    its curve from responseDb over the same settings - so the curve a designer
    drags is, by construction, the response the voice plays, and a test in
    CueEqTests pins the two together by measuring a sine through the filters
    and asking this header what it should have read. Header-only and std-only
    so the client can include it without meeting JUCE: the client boundary
    keeps JUCE out of model/, and the maths does not need it.

    THE FORMULAS are Robert Bristow-Johnson's audio EQ cookbook, the ones every
    console uses: a peaking band, a low and a high shelf whose Q sets how steep
    the corner is, and second-order high-pass and low-pass filters at
    Butterworth Q, twelve decibels an octave. Coefficients are normalised so
    that a0 is one.

    DOUBLES THROUGHOUT. A biquad at 20 Hz on a 96 kHz stream has poles within
    a thousandth of the unit circle; single precision there is audible as
    noise at the bottom of a fade, and the cost of doubles in six sections is
    nothing beside the file being read.
*/

#include <wfg/engine/audio/EqSettings.h>

#include <algorithm>
#include <cmath>

namespace wfg::audio::eqmath
{
    /** A second-order section, normalised (a0 = 1). */
    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
    };

    constexpr double pi = 3.14159265358979323846;

    /** The frequency a section may be asked for: above nought, below Nyquist
        with margin, so a cookbook formula never sees an angle it divides by. */
    inline double clampFrequency (double frequency, double sampleRate) noexcept
    {
        const auto top = std::max (100.0, sampleRate * 0.45);
        return std::clamp (frequency, 10.0, top);
    }

    inline double clampQ (double q) noexcept
    {
        return std::clamp (q, 0.05, 20.0);
    }

    inline Coefficients identity() noexcept
    {
        return {};
    }

    namespace detail
    {
        struct Angles
        {
            double sinW = 0.0, cosW = 0.0, alpha = 0.0;
        };

        inline Angles anglesFor (double sampleRate, double frequency, double q) noexcept
        {
            const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;
            const auto w = 2.0 * pi * clampFrequency (frequency, rate) / rate;
            Angles out;
            out.sinW = std::sin (w);
            out.cosW = std::cos (w);
            out.alpha = out.sinW / (2.0 * clampQ (q));
            return out;
        }

        inline Coefficients normalised (double b0, double b1, double b2,
                                        double a0, double a1, double a2) noexcept
        {
            const auto inv = 1.0 / a0;
            Coefficients c;
            c.b0 = b0 * inv;
            c.b1 = b1 * inv;
            c.b2 = b2 * inv;
            c.a1 = a1 * inv;
            c.a2 = a2 * inv;
            return c;
        }
    }

    /** A peaking band: gain in dB at the centre, unity far from it. */
    inline Coefficients peak (double sampleRate, double frequency, double q, double gainDb) noexcept
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const auto w = detail::anglesFor (sampleRate, frequency, q);

        return detail::normalised (1.0 + w.alpha * a, -2.0 * w.cosW, 1.0 - w.alpha * a,
                                   1.0 + w.alpha / a, -2.0 * w.cosW, 1.0 - w.alpha / a);
    }

    /** A low shelf: gain in dB below the corner, unity above; Q is the slope. */
    inline Coefficients lowShelf (double sampleRate, double frequency, double q, double gainDb) noexcept
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const auto w = detail::anglesFor (sampleRate, frequency, q);
        const auto cosW = w.cosW;
        const auto twoRootA = 2.0 * std::sqrt (a) * w.alpha;

        return detail::normalised (a * ((a + 1.0) - (a - 1.0) * cosW + twoRootA),
                                   2.0 * a * ((a - 1.0) - (a + 1.0) * cosW),
                                   a * ((a + 1.0) - (a - 1.0) * cosW - twoRootA),
                                   (a + 1.0) + (a - 1.0) * cosW + twoRootA,
                                   -2.0 * ((a - 1.0) + (a + 1.0) * cosW),
                                   (a + 1.0) + (a - 1.0) * cosW - twoRootA);
    }

    /** A high shelf: gain in dB above the corner, unity below; Q is the slope. */
    inline Coefficients highShelf (double sampleRate, double frequency, double q, double gainDb) noexcept
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const auto w = detail::anglesFor (sampleRate, frequency, q);
        const auto cosW = w.cosW;
        const auto twoRootA = 2.0 * std::sqrt (a) * w.alpha;

        return detail::normalised (a * ((a + 1.0) + (a - 1.0) * cosW + twoRootA),
                                   -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW),
                                   a * ((a + 1.0) + (a - 1.0) * cosW - twoRootA),
                                   (a + 1.0) - (a - 1.0) * cosW + twoRootA,
                                   2.0 * ((a - 1.0) - (a + 1.0) * cosW),
                                   (a + 1.0) - (a - 1.0) * cosW - twoRootA);
    }

    /** Butterworth's Q for a second-order section: maximally flat. */
    constexpr double butterworthQ = 0.70710678118654752440;

    /** A second-order high-pass, -3 dB at the frequency, 12 dB an octave below. */
    inline Coefficients highPass (double sampleRate, double frequency) noexcept
    {
        const auto w = detail::anglesFor (sampleRate, frequency, butterworthQ);
        const auto cosW = w.cosW;
        const auto alpha = w.alpha;

        return detail::normalised ((1.0 + cosW) * 0.5, -(1.0 + cosW), (1.0 + cosW) * 0.5,
                                   1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
    }

    /** A second-order low-pass, -3 dB at the frequency, 12 dB an octave above. */
    inline Coefficients lowPass (double sampleRate, double frequency) noexcept
    {
        const auto w = detail::anglesFor (sampleRate, frequency, butterworthQ);
        const auto cosW = w.cosW;
        const auto alpha = w.alpha;

        return detail::normalised ((1.0 - cosW) * 0.5, 1.0 - cosW, (1.0 - cosW) * 0.5,
                                   1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
    }

    /** The section a band's numbers describe. */
    inline Coefficients forBand (const EqSettings::Band& band, double sampleRate) noexcept
    {
        switch (band.shape)
        {
            case EqSettings::Shape::lowShelf:  return lowShelf (sampleRate, band.freq, band.q, band.gain);
            case EqSettings::Shape::highShelf: return highShelf (sampleRate, band.freq, band.q, band.gain);
            case EqSettings::Shape::peak:      return peak (sampleRate, band.freq, band.q, band.gain);
        }

        return identity();
    }

    /** |H| of one section at a frequency, in decibels. */
    inline double magnitudeDb (const Coefficients& c, double frequency, double sampleRate) noexcept
    {
        const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        const auto w = 2.0 * pi * std::clamp (frequency, 0.0, rate * 0.5) / rate;
        const auto cosW = std::cos (w);
        const auto cos2W = std::cos (2.0 * w);

        const auto numerator = c.b0 * c.b0 + c.b1 * c.b1 + c.b2 * c.b2
                             + 2.0 * (c.b0 * c.b1 + c.b1 * c.b2) * cosW
                             + 2.0 * c.b0 * c.b2 * cos2W;

        const auto denominator = 1.0 + c.a1 * c.a1 + c.a2 * c.a2
                               + 2.0 * (c.a1 + c.a1 * c.a2) * cosW
                               + 2.0 * c.a2 * cos2W;

        if (denominator <= 0.0 || numerator <= 0.0)
            return -200.0;

        return 10.0 * std::log10 (numerator / denominator);
    }

    /** The whole EQ's response at a frequency, in decibels: the sum of every
        section that is in. Nought for an identity EQ, by construction. */
    inline double responseDb (const EqSettings& settings, double frequency, double sampleRate) noexcept
    {
        if (! settings.on)
            return 0.0;

        auto total = 0.0;

        if (settings.hpf)
            total += magnitudeDb (highPass (sampleRate, settings.hpfFreq), frequency, sampleRate);

        if (settings.lpf)
            total += magnitudeDb (lowPass (sampleRate, settings.lpfFreq), frequency, sampleRate);

        for (const auto& band : settings.band)
            if (EqSettings::bandIsActive (band))
                total += magnitudeDb (forBand (band, sampleRate), frequency, sampleRate);

        return total;
    }
}
