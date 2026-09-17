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

    juce::Font Look::font (const model::Theme& theme, float height)
    {
        return juce::Font (juce::FontOptions{}.withHeight (height * static_cast<float> (theme.type)));
    }

    void Look::apply (const model::Theme& theme)
    {
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
