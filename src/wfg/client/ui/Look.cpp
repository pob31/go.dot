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

#include <wfg/client/ui/Look.h>

#include <wfg/client/ui/Icons.h>

namespace wfg::client::ui
{
    Look::Look (const model::Theme& theme)
    {
        apply (theme);
    }

    juce::Colour Look::colour (const model::Theme& theme, const char* name)
    {
        return juce::Colour (static_cast<juce::uint32> (theme.colour (name)));
    }

    const juce::Identifier& Look::glyphButton()
    {
        static const juce::Identifier id { "wfgGlyphButton" };
        return id;
    }

    const juce::Identifier& Look::fontScale()
    {
        static const juce::Identifier id { "wfgFontScale" };
        return id;
    }

    const juce::Identifier& Look::caption()
    {
        static const juce::Identifier id { "wfgCaption" };
        return id;
    }

    const juce::Identifier& Look::icon()
    {
        static const juce::Identifier id { "wfgIcon" };
        return id;
    }

    const juce::Identifier& Look::iconColour()
    {
        static const juce::Identifier id { "wfgIconColour" };
        return id;
    }

    const juce::Identifier& Look::iconOnly()
    {
        static const juce::Identifier id { "wfgIconOnly" };
        return id;
    }

    namespace
    {
        /*  AN ICON BUTTON'S PARTS, measured once so the drawing and the width
            a row asks for cannot disagree: the picture's side, the gap after
            it, the word without its list mark, and the mark's own width and gap. */
        struct IconButtonParts
        {
            juce::String word;
            bool opensList = false;
            float side = 0.0f, gap = 0.0f, markWidth = 0.0f, markGap = 0.0f, wordWidth = 0.0f;
        };

        IconButtonParts partsOf (const juce::TextButton& button, const juce::Font& font, float type)
        {
            IconButtonParts parts;

            const auto marker = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"));
            parts.opensList = button.getButtonText().endsWith (marker);
            parts.word = parts.opensList ? button.getButtonText().dropLastCharacters (marker.length()).trimEnd()
                                         : button.getButtonText();

            parts.side = juce::jmin (static_cast<float> (button.getHeight()) * 0.56f, 15.0f * type);
            parts.gap = parts.side * 0.35f;
            parts.markWidth = parts.opensList ? parts.side * 0.5f : 0.0f;
            parts.markGap = parts.opensList ? parts.gap : 0.0f;
            parts.wordWidth = static_cast<float> (juce::GlyphArrangement::getStringWidthInt (font, parts.word));

            return parts;
        }
    }

    int Look::iconButtonWidth (juce::TextButton& button)
    {
        const auto parts = partsOf (button, getTextButtonFont (button, button.getHeight()), type);

        return juce::roundToInt (parts.side + parts.gap + parts.wordWidth + parts.markGap + parts.markWidth + 8.0f);
    }

    juce::Font Look::getTextButtonFont (juce::TextButton& button, int buttonHeight)
    {
        auto font = LookAndFeel_V4::getTextButtonFont (button, buttonHeight);

        if (const auto scale = static_cast<float> (double (button.getProperties()[fontScale()]));
            scale > 0.0f)
            font = font.withHeight (font.getHeight() * scale);

        return font;
    }

    void Look::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
        if (const auto line = button.getProperties()[caption()].toString(); line.isNotEmpty())
        {
            /*  THE TEXT ABOVE, THE CAPTION UNDER IT, centred as a pair: the
                button still reads as its word first. */
            const auto big = getTextButtonFont (button, button.getHeight());
            const auto small = LookAndFeel_V4::getTextButtonFont (button, button.getHeight())
                                   .withHeight (12.0f * type);

            const auto bigHeight = juce::roundToInt (big.getHeight());
            const auto smallHeight = juce::roundToInt (small.getHeight());

            auto area = button.getLocalBounds().reduced (2);
            area = area.withSizeKeepingCentre (area.getWidth(),
                                               juce::jmin (area.getHeight(), bigHeight + smallHeight));

            g.setColour (button.findColour (juce::TextButton::textColourOffId)
                            .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));

            g.setFont (big);
            g.drawText (button.getButtonText(), area.removeFromTop (bigHeight),
                        juce::Justification::centred, false);

            g.setFont (small);
            g.drawFittedText (line, area, juce::Justification::centred, 1, 1.0f);
            return;
        }

        if (button.getProperties().contains (icon()))
        {
            /*  THE PICTURE, THEN THE WORDS, centred as a pair so the button
                still reads as one thing. The picture keeps its colour when the
                button is disabled, halved like the words.

                A TRAILING ▾ IS THE LIST'S MARK and is drawn apart from the
                words (author, 2026-09-30: "the drop down arrow is hidden when
                the full width of the button is not shown ... Can the icon and
                arrow be visible at all times"). When everything fits, the three
                stand together in the middle; when it does not, the picture
                holds the left edge, the mark the right, and only the WORD gives
                way between them. A row of these drops every word at once
                (`iconOnly`) rather than leave half-words; the picture says the
                kind and the mark says a list opens, and the tooltip keeps the
                word. */
            const auto picture = static_cast<model::Icon> (static_cast<int> (button.getProperties()[icon()]));
            const auto tint = juce::Colour (static_cast<juce::uint32> (static_cast<juce::int64> (button.getProperties()[iconColour()])));
            const auto font = getTextButtonFont (button, button.getHeight());
            const auto parts = partsOf (button, font, type);
            const auto showWord = ! static_cast<bool> (button.getProperties()[iconOnly()]);

            const auto opensList = parts.opensList;
            const auto& text = parts.word;
            const auto side = parts.side;
            const auto gap = showWord ? parts.gap : 0.0f;
            const auto markWidth = parts.markWidth;
            const auto markGap = parts.markGap;
            const auto textWidth = showWord ? parts.wordWidth : 0.0f;
            const auto alpha = button.isEnabled() ? 1.0f : 0.5f;
            const auto ink = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                        : juce::TextButton::textColourOffId)
                               .withMultipliedAlpha (alpha);

            auto area = button.getLocalBounds().toFloat()
                            .reduced (juce::jmin (4.0f, static_cast<float> (button.getWidth()) * 0.08f), 0.0f);
            area = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), side + gap + textWidth + markGap + markWidth),
                                               area.getHeight());

            icons::draw (g, picture, area.removeFromLeft (side), tint.withMultipliedAlpha (alpha));

            if (opensList)
            {
                /*  A small triangle pointing down, drawn rather than typed so
                    it is the same size whatever the word beside it does. */
                const auto mark = area.removeFromRight (markWidth);
                const auto c = mark.getCentre();
                const auto half = markWidth * 0.5f;

                juce::Path triangle;
                triangle.addTriangle (c.x - half, c.y - half * 0.55f, c.x + half, c.y - half * 0.55f,
                                      c.x, c.y + half * 0.65f);

                g.setColour (ink);
                g.fillPath (triangle);
                area.removeFromRight (markGap);
            }

            area.removeFromLeft (gap);

            //  The word only when a letter or two of it can be read; an ellipsis alone says nothing.
            if (showWord && area.getWidth() >= font.getHeight())
            {
                g.setColour (ink);
                g.setFont (font);
                g.drawText (text, area, juce::Justification::centredLeft, true);
            }

            return;
        }

        if (! button.getProperties().contains (glyphButton()))
        {
            LookAndFeel_V4::drawButtonText (g, button, shouldDrawButtonAsHighlighted,
                                            shouldDrawButtonAsDown);
            return;
        }

        /*  THE FIRST CHARACTER IS THE SHAPE and everything after it is the
            word: the twist, then "details". Two fonts, one colour, centred as a
            pair so the button still reads as one thing. */
        const auto text = button.getButtonText();
        const auto glyph = text.substring (0, 1);
        const auto word = text.substring (1).trimStart();

        const auto large = juce::Font (juce::FontOptions{}.withHeight (22.0f * type));
        const auto small = LookAndFeel_V4::getTextButtonFont (button, button.getHeight());

        const auto gap = juce::roundToInt (4.0f * type);
        const auto glyphWidth = juce::GlyphArrangement::getStringWidthInt (large, glyph);
        const auto wordWidth = juce::GlyphArrangement::getStringWidthInt (small, word);

        auto area = button.getLocalBounds();
        area = area.withSizeKeepingCentre (glyphWidth + gap + wordWidth, area.getHeight());

        g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                : juce::TextButton::textColourOffId)
                        .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));

        g.setFont (large);
        g.drawText (glyph, area.removeFromLeft (glyphWidth), juce::Justification::centred, false);

        area.removeFromLeft (gap);

        g.setFont (small);
        g.drawText (word, area, juce::Justification::centredLeft, false);
    }

    juce::Font Look::font (const model::Theme& theme, float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (height * static_cast<float> (theme.type)));
    }

    void Look::apply (const model::Theme& theme)
    {
        type = static_cast<float> (theme.type);

        const auto ink = colour (theme, "ink");
        const auto ground = colour (theme, "ground");
        const auto panel = colour (theme, "panel");
        const auto high = colour (theme, "panel-high");
        const auto rule = colour (theme, "rule");
        const auto standby = colour (theme, "standby");

        setColour (juce::ResizableWindow::backgroundColourId, ground);
        setColour (juce::DocumentWindow::textColourId, ink);

        /*  THE TITLE BAR AND ITS BUTTONS are filled from the scheme's widget
            background, read live when they paint - nothing else in
            LookAndFeel_V4 reads that entry after construction, so this moves
            the bar and nothing more. */
        getCurrentColourScheme().setUIColour (ColourScheme::widgetBackground, colour (theme, "title-bar"));

        setColour (juce::Label::textColourId, ink);
        setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        /*  GO is the one button, and the page draws it in the standby colour
            when pressed: the same yellow that marks the cue it will fire. */
        setColour (juce::TextButton::buttonColourId, high);
        setColour (juce::TextButton::buttonOnColourId, standby);
        setColour (juce::TextButton::textColourOffId, ink);
        setColour (juce::TextButton::textColourOnId, ground);
        setColour (juce::ComboBox::outlineColourId, rule);   // LookAndFeel_V4 draws button outlines with it

        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, ink);
        setColour (juce::AlertWindow::outlineColourId, rule);

        /*  THE LISTS A BUTTON OPENS (2026-09-27: the new-cue lists, and the
            settings window's menus, which drew in JUCE's own grey until now)
            wear the show's colours: a list is a panel, its headings the dim
            ink the cue list's derived lines use, the line under the pointer
            the raised panel a picked row is drawn on. */
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::textColourId, ink);
        setColour (juce::PopupMenu::headerTextColourId, colour (theme, "ink-dim"));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, high);
        setColour (juce::PopupMenu::highlightedTextColourId, ink);
    }
}
