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

#include <wfg/client/model/Icons.h>
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
            one word in one list and the row follows. The label is the kind,
            the word the list's own column shows for it, after its picture;
            what says "new" is the one word at the head of the row (author,
            2026-09-30: "Maybe the + of the add buttons can go and we place a
            New or Add label at the beginning of the row") - "Add" rather than
            "New", which the File menu already says of a whole show. */
        for (const auto& kind : model::cueKinds())
        {
            /*  A BUTTON THAT OPENS A LIST SAYS SO with the small triangle a
                list under a button carries everywhere else (2026-09-27): the
                click that used to make a cue now asks which one. */
            const auto opens = model::opensList (kind);
            auto label = juce::String (model::kindWord (kind));

            if (opens)
                label << " " << juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"));

            auto button = std::make_unique<juce::TextButton> (label);
            button->setWantsKeyboardFocus (false);

            /*  AND THE KIND'S PICTURE BEFORE ITS WORD, in its accent - the one
                its rows wear in the list below (2026-09-30). A group's is the
                plain container, since which kind of group is chosen from its
                list. The colour is set in `applyTheme`, which F5 calls. */
            button->getProperties().set (Look::icon(), static_cast<int> (model::iconFor (kind)));
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
        applyTheme (theme);
    }

    void NewCueBarComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (std::size_t at = 0; at < buttons.size() && at < kinds.size(); ++at)
            buttons[at]->getProperties().set (Look::iconColour(),
                                              static_cast<juce::int64> (Look::colour (theme, model::accentFor (kinds[at]).c_str()).getARGB()));

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
        if (kind == "video")      return "a new video cue, " + destination + ": choose its canvas";

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

    juce::Rectangle<int> NewCueBarComponent::labelArea() const
    {
        const auto row = rowHeight();
        const auto font = Look::font (theme, 13.0f);
        const auto width = juce::GlyphArrangement::getStringWidthInt (font, "Add") + row / 2;

        return getLocalBounds().reduced (row / 6).withWidth (width).withTrimmedLeft (row / 4);
    }

    void NewCueBarComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));
        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        //  The one word the row's buttons share, at its head.
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawText ("Add", labelArea(), juce::Justification::centredLeft, false);
    }

    void NewCueBarComponent::resized()
    {
        const auto row = rowHeight();
        const auto pad = row / 6;

        auto area = getLocalBounds().reduced (pad, pad);
        area.removeFromLeft (labelArea().getRight() - area.getX());

        /*  EQUAL WIDTHS, LEFT TO RIGHT, so a button is where it was last time
            whatever the window's width: a hand that has learnt where "+ fade"
            is finds it there. Capped so seven buttons do not stretch across a
            wide window into targets nobody can miss by a mile. The row's
            word stands before the first. */
        const auto count = static_cast<int> (buttons.size());
        const auto each = juce::jmin (row * 5, juce::jmax (row, area.getWidth() / juce::jmax (1, count)));

        /*  A QUARTER-ROW OF AIR EACH SIDE (author, 2026-09-18: "more padding
            between the add buttons"): seven targets in a row read as seven,
            not as one bar with words on it. */
        /*  Less air on a narrow window, where the room is better spent on each
            button's picture and list mark (2026-09-30). */
        for (auto& button : buttons)
            button->setBounds (area.removeFromLeft (each).reduced (juce::jmin (row / 4, each / 12), 0));

        /*  EVERY WORD OR NONE (2026-09-30): when one button cannot carry its
            word whole, the row is pictures and list marks throughout - the
            inspector's panel bar's rule, and for its reason: a row reading
            half one way and half the other, or in half-words, reads worse
            than pictures the list below has already taught. */
        if (auto* look = dynamic_cast<Look*> (&getLookAndFeel()))
        {
            auto wordsFit = true;

            for (auto& button : buttons)
                wordsFit = wordsFit && look->iconButtonWidth (*button) <= button->getWidth();

            for (auto& button : buttons)
                if (static_cast<bool> (button->getProperties()[Look::iconOnly()]) == wordsFit)
                {
                    button->getProperties().set (Look::iconOnly(), ! wordsFit);
                    button->repaint();
                }
        }
    }
}
