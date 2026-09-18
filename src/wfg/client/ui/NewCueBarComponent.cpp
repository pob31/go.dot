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

#include <wfg/client/ui/NewCueBarComponent.h>

#include <wfg/client/model/NewCue.h>
#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    NewCueBarComponent::NewCueBarComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        /*  ONE BUTTON PER KIND THE MODEL NAMES, so a kind the engine grows is
            one word in one list and the row follows. The label is "+ kind":
            the plus is what says "new" without a heading, and the kind is
            the word the list's own column shows for it. */
        for (const auto& kind : model::cueKinds())
        {
            auto button = std::make_unique<juce::TextButton> ("+ " + juce::String (kind));
            button->setWantsKeyboardFocus (false);
            button->onClick = [this, kind]
            {
                if (actions.create)
                    actions.create (kind);
            };

            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
        }

        setWantsKeyboardFocus (false);
        setDestination ("at the end of the list");
    }

    void NewCueBarComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        resized();
        repaint();
    }

    void NewCueBarComponent::setDestination (const juce::String& sentence)
    {
        if (sentence == destination)
            return;

        destination = sentence;

        for (auto& button : buttons)
            button->setTooltip ("a new " + button->getButtonText().fromFirstOccurrenceOf ("+ ", false, false)
                                  + " cue, " + destination);
    }

    int NewCueBarComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    int NewCueBarComponent::preferredHeight() const noexcept
    {
        return rowHeight() + rowHeight() / 3;
    }

    void NewCueBarComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));
        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }

    void NewCueBarComponent::resized()
    {
        const auto row = rowHeight();
        const auto pad = row / 6;

        auto area = getLocalBounds().reduced (pad, pad);

        /*  EQUAL WIDTHS, LEFT TO RIGHT, so a button is where it was last time
            whatever the window's width: a hand that has learnt where "+ fade"
            is finds it there. Capped so seven buttons do not stretch across a
            wide window into targets nobody can miss by a mile. */
        const auto count = static_cast<int> (buttons.size());
        const auto each = juce::jmin (row * 4, juce::jmax (row, area.getWidth() / juce::jmax (1, count)));

        for (auto& button : buttons)
            button->setBounds (area.removeFromLeft (each).reduced (1, 0));
    }
}
