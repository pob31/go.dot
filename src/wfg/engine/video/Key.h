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
    A CUE'S KEY (namespace draft 61, AKA, AKB, AKH): what makes a picture, a
    movie or a capture see-through besides its own alpha - worked out once
    here, in arithmetic the GPU's shader repeats line for line, as Grade.h's is.

    On the colour as it arrives, straight (not premultiplied), 0..1, before the
    grade (AKG), each point is given a MEASURE m:
      - LUMA and MATTE: its brightness, Rec. 709's weights;
      - COLOUR: how far its colour lies from the key colour's in the colour
        plane - Cb and Cr, Rec. 709's - each taken RELATIVE TO ITS OWN
        BRIGHTNESS (never less than a tenth), so a shadow across the screen,
        the same green half as bright, is still the screen; counted so that
        the key colour is nought and grey is one.
    Then a RAMP: k = clamp ((m - level) / softness + 1/2, 0, 1), a step at
    `level` when the softness is nought; turned round by `invert`. The point's
    alpha is multiplied by k. At level 50 % and softness 100 % a luma key is
    exactly "brightness is opacity".

    A COLOUR KEY TAKES ITS SPILL OFF what it keeps: the part of each point's
    colour that lies along the key colour's own direction in the plane, scaled
    by `spill`, its brightness kept - so it works for any key colour, not only
    green and blue. A MATTE shows the cue's paint, cut by the measure: the
    picture is only its shape.

    `none` changes nothing, and is what a cue starts with. Pure, no allocation.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wfg::video
{
    enum class KeyMode : std::uint32_t { none = 0, luma = 1, matte = 2, colour = 3 };

    inline KeyMode keyModeFrom (std::string_view word) noexcept
    {
        if (word == "luma")   return KeyMode::luma;
        if (word == "matte")  return KeyMode::matte;
        if (word == "colour") return KeyMode::colour;
        return KeyMode::none;
    }

    struct Key
    {
        KeyMode mode = KeyMode::none;
        std::uint32_t colour = 0x00ff00;    ///< the colour a colour key clears, 0xRRGGBB
        double level = 50.0;                ///< %, where the cut falls
        double softness = 100.0;            ///< %, how gradual it is
        double spill = 50.0;                ///< %, a colour key's spill taken off
        bool invert = false;

        bool isNone() const noexcept { return mode == KeyMode::none; }
    };

    /*  Two keys the same, compared by hand: a defaulted `==` on a double is
        what GCC's -Wfloat-equal refuses. */
    inline bool sameKey (const Key& a, const Key& b) noexcept
    {
        const auto alike = [] (double x, double y) { return std::abs (x - y) < 1.0e-9; };

        return a.mode == b.mode && a.colour == b.colour && a.invert == b.invert
            && alike (a.level, b.level) && alike (a.softness, b.softness) && alike (a.spill, b.spill);
    }

    namespace keying
    {
        constexpr double lumaRed = 0.2126;
        constexpr double lumaGreen = 0.7152;
        constexpr double lumaBlue = 0.0722;

        /*  Rec. 709's colour-difference scales: Cb = (B - Y) / 1.8556,
            Cr = (R - Y) / 1.5748. */
        constexpr double cbScale = 1.8556;
        constexpr double crScale = 1.5748;

        /*  The least brightness a colour is taken relative to: below it a
            near-black point's colour is mostly noise. */
        constexpr double darkest = 0.1;

        inline double lumaOf (double r, double g, double b) noexcept
        {
            return lumaRed * r + lumaGreen * g + lumaBlue * b;
        }

        inline double channel (std::uint32_t rgb, int shift) noexcept
        {
            return static_cast<double> ((rgb >> shift) & 0xffu) / 255.0;
        }
    }

    /*  ONE POINT KEYED: its straight colour 0..1 and its alpha 0..1, changed in
        place. `paint` is the cue's colour, which a matte shows. */
    inline void applyKey (const Key& key, std::uint32_t paint, double& red, double& green, double& blue,
                          double& alpha) noexcept
    {
        using namespace keying;

        if (key.isNone())
            return;

        const auto y = lumaOf (red, green, blue);
        auto measure = y;

        if (key.mode == KeyMode::colour)
        {
            const auto kr = channel (key.colour, 16), kg = channel (key.colour, 8), kb = channel (key.colour, 0);
            const auto ky = lumaOf (kr, kg, kb);
            const auto keyCb = (kb - ky) / cbScale, keyCr = (kr - ky) / crScale;
            auto cb = (blue - y) / cbScale, cr = (red - y) / crScale;

            //  Each colour relative to its own brightness: a shadow is the same colour.
            const auto keyScale = 1.0 / std::max (ky, darkest), scale = 1.0 / std::max (y, darkest);
            const auto norm = std::max (std::hypot (keyCb, keyCr) * keyScale, 1.0e-3);
            measure = std::hypot (cb * scale - keyCb * keyScale, cr * scale - keyCr * keyScale) / norm;

            //  The spill: what lies along the key's direction, taken off by `spill`.
            const auto length = std::max (std::hypot (keyCb, keyCr), 1.0e-6);
            const auto dirCb = keyCb / length, dirCr = keyCr / length;
            const auto along = std::max (0.0, cb * dirCb + cr * dirCr) * std::clamp (key.spill / 100.0, 0.0, 1.0);
            cb -= dirCb * along;
            cr -= dirCr * along;

            const auto r = y + crScale * cr;
            const auto b = y + cbScale * cb;
            const auto g = (y - lumaRed * r - lumaBlue * b) / lumaGreen;
            red = std::clamp (r, 0.0, 1.0);
            green = std::clamp (g, 0.0, 1.0);
            blue = std::clamp (b, 0.0, 1.0);
        }

        const auto level = key.level / 100.0;
        const auto softness = key.softness / 100.0;
        auto k = softness > 0.0 ? std::clamp ((measure - level) / softness + 0.5, 0.0, 1.0)
                                : (measure >= level ? 1.0 : 0.0);

        if (key.invert)
            k = 1.0 - k;

        if (key.mode == KeyMode::matte)
        {
            red = channel (paint, 16);
            green = channel (paint, 8);
            blue = channel (paint, 0);
        }

        alpha *= k;
    }
}
