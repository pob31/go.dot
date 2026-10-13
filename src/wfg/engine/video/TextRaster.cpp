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

#include <wfg/engine/video/TextRaster.h>

#include <wfg/engine/video/Blur.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wfg::video
{
    namespace
    {
        juce::Colour colourOf (std::uint32_t rgb) noexcept
        {
            return juce::Colour (0xff000000u | (rgb & 0xffffffu));
        }

        juce::Justification justificationOf (TextAlign align) noexcept
        {
            switch (align)
            {
                case TextAlign::left:   return juce::Justification::left;
                case TextAlign::right:  return juce::Justification::right;
                case TextAlign::centre: break;
            }

            return juce::Justification::horizontallyCentred;
        }

        /*  THE WORDS SET IN A BOX `width` WIDE, each line aligned in it, the
            first line's top at nought. */
        juce::GlyphArrangement setIn (const juce::Font& font, const juce::String& words, float width,
                                      juce::Justification justification, float leading)
        {
            juce::GlyphArrangement glyphs;
            glyphs.addJustifiedText (font, words, 0.0f, font.getAscent(), width, justification, leading);
            return glyphs;
        }
    }

    TextRaster rasteriseText (const TextLook& look, std::uint32_t fill, int canvasWidth, int canvasHeight,
                              double scale)
    {
        TextRaster out;

        const auto words = juce::String::fromUTF8 (look.words.c_str()).trimEnd();

        if (words.isEmpty() || canvasWidth <= 0 || canvasHeight <= 0)
            return out;

        //  EVERYTHING IN THE DRAWN PICTURE'S PIXELS: the canvas's, times the scale.
        const auto times = std::clamp (scale, 1.0, 4.0);
        const auto frameWidth = canvasWidth * times;
        const auto frameHeight = canvasHeight * times;

        const auto height = static_cast<float> (std::max (1.0, look.fontSize / 100.0 * frameHeight));
        const auto style = (look.bold ? juce::Font::bold : 0) | (look.italic ? juce::Font::italic : 0);
        const juce::Font font (look.font.empty() ? juce::FontOptions (height, style)
                                                 : juce::FontOptions (juce::String::fromUTF8 (look.font.c_str()), height, style));
        const auto leading = static_cast<float> ((std::max (50.0, look.lineSpacing) / 100.0 - 1.0) * height);
        const auto justification = justificationOf (look.align);

        /*  THE BOX: `wrap` % of the frame's width, centred - or, nothing
            wrapping, as wide as the longest line, laid out first to find it. */
        const auto wraps = look.wrap > 0.0;
        auto boxWidth = static_cast<float> (std::clamp (look.wrap, 0.0, 100.0) / 100.0 * frameWidth);

        if (! wraps)
        {
            //  A pixel over, so the longest line is never wrapped by a rounding.
            const auto loose = setIn (font, words, 1.0e6f, juce::Justification::left, leading);
            boxWidth = std::max (1.0f, loose.getBoundingBox (0, -1, true).getWidth() + 1.0f);
        }

        auto glyphs = setIn (font, words, std::max (1.0f, boxWidth), justification, leading);

        if (glyphs.getNumGlyphs() == 0)
            return out;

        juce::Path letters;
        glyphs.createPath (letters);

        //  WHERE THE BLOCK GOES: centred across; at the top, the middle or the bottom.
        const auto block = glyphs.getBoundingBox (0, -1, true);
        const auto margin = wraps ? (frameWidth - boxWidth) / 2.0 : 0.05 * frameWidth;
        const auto left = (frameWidth - boxWidth) / 2.0;
        double top = 0.0;

        switch (look.alignV)
        {
            case TextAlignV::top:    top = margin; break;
            case TextAlignV::bottom: top = frameHeight - margin - block.getHeight(); break;
            case TextAlignV::middle: top = (frameHeight - block.getHeight()) / 2.0; break;
        }

        const auto shift = juce::AffineTransform::translation (static_cast<float> (left),
                                                               static_cast<float> (top) - block.getY());
        letters.applyTransform (shift);

        /*  THE OUTLINE beneath the letters: a stroke twice its width about
            their edge, half of it under the fill - and its blur's reach. */
        const auto outlineWidth = static_cast<float> (std::max (0.0, look.outline) * times);
        const auto blurRadius = std::max (0.0, look.outlineBlur) * times;
        juce::Path outline;

        if (outlineWidth > 0.0f)
            juce::PathStrokeType (2.0f * outlineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
                .createStrokedPath (outline, letters);

        const auto ink = outlineWidth > 0.0f ? outline.getBounds() : letters.getBounds();
        const auto room = static_cast<float> (std::ceil (blurRadius)) + 2.0f;
        auto area = ink.expanded (room).getSmallestIntegerContainer();

        //  NEVER MORE THAN THE MOST: the far side of a huge title is cut, not the machine.
        area.setWidth (std::min (area.getWidth(), textRasterMost));
        area.setHeight (std::min (area.getHeight(), textRasterMost));

        if (area.isEmpty())
            return out;

        juce::Image image (juce::Image::ARGB, area.getWidth(), area.getHeight(), true, juce::SoftwareImageType());
        const auto toImage = juce::AffineTransform::translation (static_cast<float> (-area.getX()),
                                                                 static_cast<float> (-area.getY()));

        if (outlineWidth > 0.0f)
        {
            /*  ITS SHAPE AS ONE PLANE, blurred, then laid in its colour. */
            juce::Image shape (juce::Image::SingleChannel, area.getWidth(), area.getHeight(), true, juce::SoftwareImageType());

            {
                juce::Graphics g (shape);
                g.setColour (juce::Colours::white);
                g.fillPath (outline, toImage);
            }

            const auto w = area.getWidth();
            const auto h = area.getHeight();
            std::vector<float> plane (static_cast<std::size_t> (w) * static_cast<std::size_t> (h));

            {
                const juce::Image::BitmapData bits (shape, juce::Image::BitmapData::readOnly);

                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        plane[static_cast<std::size_t> (y) * static_cast<std::size_t> (w) + static_cast<std::size_t> (x)]
                            = static_cast<float> (*bits.getPixelPointer (x, y)) / 255.0f;
            }

            blurPlane (plane, w, h, blurRadius);

            const auto colour = colourOf (look.outlineColour);
            juce::Image::BitmapData bits (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const auto alpha = std::clamp (plane[static_cast<std::size_t> (y) * static_cast<std::size_t> (w)
                                                         + static_cast<std::size_t> (x)], 0.0f, 1.0f);
                    bits.setPixelColour (x, y, colour.withAlpha (alpha));
                }
        }

        {
            juce::Graphics g (image);
            g.setColour (colourOf (fill));
            g.fillPath (letters, toImage);
        }

        out.image = image;
        out.x0 = area.getX() / frameWidth;
        out.y0 = area.getY() / frameHeight;
        out.x1 = area.getRight() / frameWidth;
        out.y1 = area.getBottom() / frameHeight;
        return out;
    }

    std::vector<std::string> fontsNotInstalled (const std::vector<std::string>& families)
    {
        const auto installed = juce::Font::findAllTypefaceNames();
        std::vector<std::string> missing;

        for (const auto& family : families)
            if (! family.empty() && ! installed.contains (juce::String::fromUTF8 (family.c_str()), true)
                  && std::find (missing.begin(), missing.end(), family) == missing.end())
                missing.push_back (family);

        return missing;
    }
}
