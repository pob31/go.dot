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
#include <wfg/client/model/NewCueMenus.h>
#include <wfg/client/ui/Look.h>

#include <cstddef>
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
            /*  A BUTTON THAT OPENS A LIST SAYS SO with the small triangle a
                list under a button carries everywhere else (2026-09-27): the
                click that used to make a cue now asks which one. */
            const auto opens = model::opensList (kind);
            auto label = "+ " + juce::String (kind);

            if (opens)
                label << " " << juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"));

            auto button = std::make_unique<juce::TextButton> (label);
            button->setWantsKeyboardFocus (false);
            button->onClick = [this, kind, opens, pressed = button.get()]
            {
                if (opens && actions.choose)
                    actions.choose (kind, *pressed);
                else if (actions.create)
                    actions.create (kind);
            };

            addAndMakeVisible (*button);
            buttons.push_back (std::move (button));
            kinds.push_back (kind);
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

        for (std::size_t at = 0; at < buttons.size(); ++at)
            buttons[at]->setTooltip (tooltipFor (kinds[at]));
    }

    juce::String NewCueBarComponent::tooltipFor (const std::string& kind) const
    {
        /*  The list's own first line says the rest - where it lands, what it
            is aimed at - so the tooltip says what the list is for. */
        if (kind == "group")      return "a new group, " + destination + ": choose its kind";
        if (kind == "transport")  return "a cue that stops, advances or starts another: choose which";
        if (kind == "midi")       return "a new MIDI cue, " + destination + ": choose what it sends";
        if (kind == "mic")        return "a new mic cue, " + destination + ": choose its input";

        return "a new " + juce::String (kind) + " cue, " + destination;
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

        /*  A QUARTER-ROW OF AIR EACH SIDE (author, 2026-09-18: "more padding
            between the add buttons"): seven targets in a row read as seven,
            not as one bar with words on it. */
        for (auto& button : buttons)
            button->setBounds (area.removeFromLeft (each).reduced (row / 4, 0));
    }
}
