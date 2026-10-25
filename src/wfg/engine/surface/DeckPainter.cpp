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

#include <wfg/engine/surface/DeckPainter.h>

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>

namespace wfg::surface::painter
{
    namespace
    {
        using Glyph = KeyPicture::Glyph;

        juce::Colour colourOf (DeckColour colour)
        {
            return juce::Colour (colour.red, colour.green, colour.blue);
        }

        /*  White words on a dark ground, black on a light one (Rec. 709 luma). */
        juce::Colour wordsOn (juce::Colour ground)
        {
            const auto luma = 0.2126f * ground.getFloatRed() + 0.7152f * ground.getFloatGreen()
                            + 0.0722f * ground.getFloatBlue();
            return luma < 0.55f ? juce::Colours::white : juce::Colours::black;
        }

        juce::Font fontOf (float height, bool bold)
        {
            return juce::Font (juce::FontOptions (height).withStyle (bold ? "Bold" : "Regular"));
        }

        juce::Image canvas (int width, int height)
        {
            return juce::Image (juce::Image::RGB, std::max (1, width), std::max (1, height), true,
                                juce::SoftwareImageType());
        }

        DeckPixels pixelsOf (const juce::Image& image)
        {
            DeckPixels out;
            out.width = image.getWidth();
            out.height = image.getHeight();
            out.rgb.resize (static_cast<std::size_t> (out.width * out.height * 3));

            const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);

            for (int y = 0; y < out.height; ++y)
            {
                for (int x = 0; x < out.width; ++x)
                {
                    const auto colour = data.getPixelColour (x, y);
                    const auto at = static_cast<std::size_t> ((y * out.width + x) * 3);
                    out.rgb[at] = colour.getRed();
                    out.rgb[at + 1] = colour.getGreen();
                    out.rgb[at + 2] = colour.getBlue();
                }
            }

            return out;
        }

        juce::Image imageOf (const DeckPixels& picture)
        {
            auto image = canvas (picture.width, picture.height);

            if (picture.rgb.size() < static_cast<std::size_t> (picture.width * picture.height * 3))
                return image;

            juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < picture.height; ++y)
            {
                for (int x = 0; x < picture.width; ++x)
                {
                    const auto at = static_cast<std::size_t> ((y * picture.width + x) * 3);
                    data.setPixelColour (x, y, juce::Colour (picture.rgb[at], picture.rgb[at + 1], picture.rgb[at + 2]));
                }
            }

            return image;
        }

        /*  THE SHOW'S KEYS WEAR THEIR OWN COLOURS, always with their word. */
        juce::Colour groundOf (const KeyPicture& picture)
        {
            switch (picture.glyph)
            {
                case Glyph::go:     return juce::Colour (0xff1e8c3a);
                case Glyph::esc:    return juce::Colour (0xffb3261e);
                case Glyph::doh:    return juce::Colour (0xffc77c0e);
                case Glyph::pause:  return juce::Colour (0xff2f6fb0);
                case Glyph::none:
                case Glyph::standbyUp:
                case Glyph::standbyDown:
                case Glyph::pageNext:
                case Glyph::aimPrevious:
                case Glyph::aimNext:
                case Glyph::controlsPrevious:
                case Glyph::controlsNext:
                case Glyph::pageSend:
                case Glyph::pageEq:
                case Glyph::pageFx:
                case Glyph::pageLoop:
                    break;
            }

            //  A cue's own colour, darkened so the words read; none, a near black.
            const auto own = colourOf (picture.background);

            if (picture.background == DeckColour {})
                return juce::Colour (0xff1a1a1a);

            return own.withMultipliedBrightness (0.6f);
        }

        /*  A TRIANGLE POINTING, for the arrows: up, down, left or right. */
        void arrow (juce::Graphics& g, juce::Rectangle<float> box, int towards)
        {
            juce::Path shape;
            const auto c = box.getCentre();
            const auto r = std::min (box.getWidth(), box.getHeight()) * 0.5f;

            switch (towards)
            {
                case 0:  shape.addTriangle (c.x, c.y - r, c.x + r, c.y + r * 0.6f, c.x - r, c.y + r * 0.6f); break;
                case 1:  shape.addTriangle (c.x, c.y + r, c.x + r, c.y - r * 0.6f, c.x - r, c.y - r * 0.6f); break;
                case 2:  shape.addTriangle (c.x - r, c.y, c.x + r * 0.6f, c.y - r, c.x + r * 0.6f, c.y + r); break;
                default: shape.addTriangle (c.x + r, c.y, c.x - r * 0.6f, c.y - r, c.x - r * 0.6f, c.y + r); break;
            }

            g.fillPath (shape);
        }

        int arrowOf (Glyph glyph)
        {
            switch (glyph)
            {
                case Glyph::standbyUp:          return 0;
                case Glyph::standbyDown:        return 1;
                case Glyph::aimPrevious:
                case Glyph::controlsPrevious:   return 2;
                case Glyph::aimNext:
                case Glyph::controlsNext:
                case Glyph::pageNext:           return 3;
                case Glyph::none:
                case Glyph::go:
                case Glyph::esc:
                case Glyph::doh:
                case Glyph::pause:
                case Glyph::pageSend:
                case Glyph::pageEq:
                case Glyph::pageFx:
                case Glyph::pageLoop:
                    break;
            }

            return -1;
        }
    }

    //==========================================================================
    DeckPixels key (const KeyPicture& picture, int pixels)
    {
        const auto size = std::max (16, pixels);
        auto image = canvas (size, size);
        juce::Graphics g (image);

        const auto s = static_cast<float> (size);
        auto ground = groundOf (picture);

        if (picture.dim)
            ground = ground.withMultipliedBrightness (0.45f);

        g.fillAll (ground);

        auto words = wordsOn (ground);

        if (picture.dim)
            words = words.withAlpha (0.55f);

        const auto inset = s * 0.07f;
        auto area = juce::Rectangle<float> (0.0f, 0.0f, s, s).reduced (inset);

        //  How far a going pad has got: a bar along the bottom edge.
        if (picture.progress >= 0)
        {
            const auto bar = juce::Rectangle<float> (0.0f, s - s * 0.07f, s, s * 0.07f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (bar);
            g.setColour (juce::Colours::white);
            g.fillRect (bar.withWidth (s * static_cast<float> (std::clamp (picture.progress, 0, 1000)) / 1000.0f));
        }

        g.setColour (words);

        if (! picture.top.empty())
        {
            g.setFont (fontOf (s * 0.17f, true));
            g.drawText (juce::String::fromUTF8 (picture.top.c_str()), area.removeFromTop (s * 0.19f),
                        juce::Justification::topLeft, true);
        }

        const auto bottomBox = picture.bottom.empty() ? juce::Rectangle<float> {}
                                                      : area.removeFromBottom (s * 0.18f);

        //  The glyph over the word, for the show's keys and the page keys.
        if (const auto towards = arrowOf (picture.glyph); towards >= 0)
        {
            auto glyphBox = area.removeFromTop (area.getHeight() * (picture.label.empty() ? 1.0f : 0.55f));
            arrow (g, glyphBox.reduced (glyphBox.getHeight() * 0.18f), towards);
        }

        if (! picture.label.empty())
        {
            const auto bold = picture.glyph != Glyph::none;
            const auto height = bold && arrowOf (picture.glyph) < 0 ? s * 0.28f : s * 0.18f;
            g.setFont (fontOf (height, bold));
            g.drawFittedText (juce::String::fromUTF8 (picture.label.c_str()), area.toNearestInt(),
                              juce::Justification::centred, 3, 0.8f);
        }

        if (! picture.bottom.empty())
        {
            g.setFont (fontOf (s * 0.14f, false));
            g.drawFittedText (juce::String::fromUTF8 (picture.bottom.c_str()), bottomBox.toNearestInt(),
                              juce::Justification::centredBottom, 1, 0.7f);
        }

        //  Lit: a white frame - and the bottom word says why.
        if (picture.lit)
        {
            g.setColour (juce::Colours::white);
            g.drawRect (juce::Rectangle<float> (0.0f, 0.0f, s, s), s * 0.05f);
        }

        return pixelsOf (image);
    }

    DeckPixels zone (const ZonePicture& picture, int width, int height)
    {
        auto image = canvas (width, height);
        juce::Graphics g (image);

        const auto w = static_cast<float> (image.getWidth());
        const auto h = static_cast<float> (image.getHeight());
        const auto alpha = picture.off ? 0.45f : 1.0f;

        g.fillAll (juce::Colour (0xff101010));

        //  A thin line between zones, so four read as four.
        g.setColour (juce::Colour (0xff3a3a3a));
        g.drawVerticalLine (image.getWidth() - 1, 0.0f, h);

        auto area = juce::Rectangle<float> (0.0f, 0.0f, w, h).reduced (w * 0.04f, h * 0.06f);
        const auto colour = picture.colour == DeckColour {} ? juce::Colours::white : colourOf (picture.colour);

        if (! picture.title.empty())
        {
            g.setColour (juce::Colours::lightgrey);
            g.setFont (fontOf (h * 0.15f, false));
            g.drawText (juce::String::fromUTF8 (picture.title.c_str()), area.removeFromTop (h * 0.17f),
                        juce::Justification::topLeft, true);
        }
        else
        {
            area.removeFromTop (h * 0.17f);
        }

        g.setColour (colour.withAlpha (alpha));
        g.setFont (fontOf (h * 0.2f, true));
        g.drawText (juce::String::fromUTF8 (picture.label.c_str()), area.removeFromTop (h * 0.24f),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.setFont (fontOf (h * 0.26f, false));
        g.drawFittedText (juce::String::fromUTF8 (picture.value.c_str()), area.removeFromTop (h * 0.3f).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.7f);

        if (picture.fill >= 0.0)
        {
            const auto bar = juce::Rectangle<float> (area.getX(), h - h * 0.14f, area.getWidth(), h * 0.08f);
            const auto fill = static_cast<float> (std::clamp (picture.fill, 0.0, 1.0));

            g.setColour (juce::Colour (0xff333333));
            g.fillRect (bar);
            g.setColour (colour.withAlpha (alpha));

            if (picture.fromCentre)
            {
                const auto middle = bar.getCentreX();
                const auto to = bar.getX() + bar.getWidth() * fill;
                g.fillRect (juce::Rectangle<float> (std::min (middle, to), bar.getY(),
                                                    std::max (1.0f, std::abs (to - middle)), bar.getHeight()));
            }
            else
            {
                g.fillRect (bar.withWidth (bar.getWidth() * fill));
            }
        }

        return pixelsOf (image);
    }

    DeckPixels screen (const ScreenPicture& picture, int width, int height)
    {
        auto image = canvas (width, height);
        juce::Graphics g (image);

        const auto w = static_cast<float> (image.getWidth());
        const auto h = static_cast<float> (image.getHeight());

        g.fillAll (juce::Colours::black);
        auto area = juce::Rectangle<float> (0.0f, 0.0f, w, h).reduced (w * 0.03f, h * 0.08f);

        g.setColour (juce::Colours::white);
        g.setFont (fontOf (h * 0.38f, true));
        g.drawFittedText (juce::String::fromUTF8 (picture.first.c_str()),
                          area.removeFromTop (area.getHeight() * 0.55f).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.7f);

        g.setColour (juce::Colours::lightgrey);
        g.setFont (fontOf (h * 0.3f, false));
        g.drawFittedText (juce::String::fromUTF8 (picture.second.c_str()), area.toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.7f);

        return pixelsOf (image);
    }

    //==========================================================================
    DeckBytes jpeg (const DeckPixels& picture)
    {
        juce::MemoryOutputStream out;
        juce::JPEGImageFormat format;
        format.setQuality (0.9f);

        if (! format.writeImageToStream (imageOf (picture), out))
            return {};

        const auto* const data = static_cast<const std::uint8_t*> (out.getData());
        return DeckBytes (data, data + out.getDataSize());
    }

    DeckBytes keyBytes (const DeckSpec& spec, const DeckPixels& picture)
    {
        const auto turned = deckOriented (spec, picture);

        switch (spec.image)
        {
            case DeckImage::bmp:  return deckBmp (turned);
            case DeckImage::jpeg: return jpeg (turned);
            case DeckImage::none: break;
        }

        return {};
    }

    DeckBytes stripBytes (const DeckSpec& spec, const DeckPixels& picture)
    {
        //  The Neo's screen is upside down; the Stream Deck+'s strip is not.
        if (spec.model == DeckModel::neo)
            return jpeg (deckTransformed (picture, 2, false, false));

        return jpeg (picture);
    }

    bool writePng (const DeckPixels& picture, const std::string& path)
    {
        const juce::File file (juce::String::fromUTF8 (path.c_str()));
        file.deleteFile();

        juce::FileOutputStream out (file);

        if (! out.openedOk())
            return false;

        juce::PNGImageFormat format;
        return format.writeImageToStream (imageOf (picture), out);
    }
}
