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

    void Look::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
    {
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
    }
}
