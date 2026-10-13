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
    A TEXT CUE'S WORDS AND THEIR STYLE (namespace draft 61, AKJ, AKK), as they
    cross to the renderer: the words already as the canvas shows them, and how
    they are set - a font, a size, a fill (the cue's paint, which crosses as a
    layer's paint does), an outline and its blur.

    THE FONT: a family by name, or the face file of a family the show carries
    (namespace draft §62), which wins over an installed font of that name.

    THE UNITS: the size is a % of the canvas's height, as offsets are a % of it
    (VQ); the outline's width and its blur are canvas pixels, as a mask's
    feather is. The box the words wrap in is a % of the canvas's width, centred;
    nought never wraps. The words are laid out as on a slide, the canvas the
    slide's frame, which a layer's geometry then moves as a fill's.

    std only: the engine writes it, the renderer reads it, a test builds one.
*/

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wfg::video
{
    enum class TextAlign : std::uint32_t { left = 0, centre = 1, right = 2 };
    enum class TextAlignV : std::uint32_t { top = 0, middle = 1, bottom = 2 };

    inline TextAlign textAlignFrom (std::string_view word) noexcept
    {
        if (word == "left")  return TextAlign::left;
        if (word == "right") return TextAlign::right;
        return TextAlign::centre;
    }

    inline TextAlignV textAlignVFrom (std::string_view word) noexcept
    {
        if (word == "top")    return TextAlignV::top;
        if (word == "bottom") return TextAlignV::bottom;
        return TextAlignV::middle;
    }

    /*  The words a layer carries, at most: a slide's worth (61, AKM). */
    constexpr std::size_t wordsBytes = 4096;

    struct TextLook
    {
        std::string words;              ///< as shown, lines split by a line break
        std::string font;               ///< an installed family; empty is the default sans-serif
        double fontSize = 8.0;          ///< % of the canvas's height
        bool bold = false;
        bool italic = false;
        TextAlign align = TextAlign::centre;
        TextAlignV alignV = TextAlignV::middle;
        double lineSpacing = 100.0;     ///< %
        double wrap = 90.0;             ///< % of the canvas's width; nought never wraps
        std::uint32_t outlineColour = 0x000000;
        double outline = 0.0;           ///< canvas px, its width outside the letters
        double outlineBlur = 0.0;       ///< canvas px, how far it is blurred

        /*  THE FACE FILE THAT DRAWS IT, when the show carries the family
            (namespace draft §62, AKZ): its absolute path and its place in a
            collection, and what Go.dot draws itself that the face lacks. Empty,
            the renderer asks the system for `font` by name, as before. */
        std::string fontFile;
        int fontFace = 0;
        bool embolden = false;
        bool slant = false;
    };

    /*  Two looks the same, compared by hand: a defaulted `==` on a double is
        what GCC's -Wfloat-equal refuses. */
    inline bool sameTextLook (const TextLook& a, const TextLook& b) noexcept
    {
        const auto alike = [] (double x, double y) { return std::abs (x - y) < 1.0e-9; };

        return a.words == b.words && a.font == b.font && a.bold == b.bold && a.italic == b.italic
            && a.align == b.align && a.alignV == b.alignV && a.outlineColour == b.outlineColour
            && a.fontFile == b.fontFile && a.fontFace == b.fontFace && a.embolden == b.embolden && a.slant == b.slant
            && alike (a.fontSize, b.fontSize) && alike (a.lineSpacing, b.lineSpacing) && alike (a.wrap, b.wrap)
            && alike (a.outline, b.outline) && alike (a.outlineBlur, b.outlineBlur);
    }

    /*  TEXT CUT TO `bytes` AT A CHARACTER'S BOUNDARY (61, AKM): never half a
        letter, which would show as a stray mark or nothing at all. */
    inline std::string cutAtCharacter (std::string text, std::size_t bytes)
    {
        if (text.size() <= bytes)
            return text;

        auto end = bytes;

        //  Back over continuation bytes (10xxxxxx) to the start of the character cut.
        while (end > 0 && (static_cast<unsigned char> (text[end]) & 0xC0u) == 0x80u)
            --end;

        text.resize (end);
        return text;
    }
}
