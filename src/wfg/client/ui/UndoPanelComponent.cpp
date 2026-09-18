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

#include <wfg/client/ui/UndoPanelComponent.h>

#include <wfg/client/ui/Look.h>

#include <string>
#include <utility>

namespace wfg::client::ui
{
    UndoPanelComponent::UndoPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        heading.setText ("Undo history", juce::dontSendNotification);
        addAndMakeVisible (heading);
        addAndMakeVisible (summary);

        okButton.onClick = [this] { if (actions.ok) actions.ok(); };
        cancelButton.onClick = [this] { if (actions.cancel) actions.cancel(); };
        addAndMakeVisible (okButton);
        addAndMakeVisible (cancelButton);

        viewport.setViewedComponent (&stackView, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        setWantsKeyboardFocus (false);
        applyTheme (theme);
    }

    int UndoPanelComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void UndoPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        heading.setFont (Look::font (theme, 13.0f));
        heading.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        summary.setFont (Look::font (theme, 12.0f));
        summary.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        summary.setJustificationType (juce::Justification::topLeft);
        summary.setMinimumHorizontalScale (1.0f);

        resized();
        repaint();
    }

    void UndoPanelComponent::show (const model::UndoReading& readingToShow, const model::Diff& diff,
                                   int openedAtToShow)
    {
        reading = readingToShow;
        changes = diff;
        openedAt = openedAtToShow;
        stack = model::standings (reading);

        /*  WHAT STANDING HERE CHANGED, in words: counts, and the names of
            what is gone, since a cue that is not in the list cannot be
            marked in it. */
        juce::String words;

        if (changes.empty())
            words = reading.position() == openedAt ? "Standing where the panel opened."
                                                   : "Nothing the list shows differs.";
        else
        {
            juce::StringArray parts;

            if (! changes.changed.empty())
                parts.add (juce::String (static_cast<int> (changes.changed.size()))
                             + (changes.changed.size() == 1 ? " cue changed" : " cues changed"));

            if (! changes.added.empty())
                parts.add (juce::String (static_cast<int> (changes.added.size()))
                             + (changes.added.size() == 1 ? " cue added" : " cues added"));

            if (! changes.removed.empty())
                parts.add (juce::String (static_cast<int> (changes.removed.size()))
                             + (changes.removed.size() == 1 ? " cue gone" : " cues gone"));

            words = parts.joinIntoString (", ") + " against where the panel opened.";

            for (const auto& name : changes.removed)
                words += "\ngone: " + juce::String (name);
        }

        summary.setText (words, juce::dontSendNotification);

        std::string key = std::to_string (reading.position()) + "/" + std::to_string (openedAt)
                        + "/" + std::to_string (pointing) + " ";

        for (const auto& name : reading.undo)
            key += name + " ";

        key += "| ";

        for (const auto& name : reading.redo)
            key += name + " ";

        if (key != drawnKey)
        {
            drawnKey = key;
            layOutStack();
            stackView.repaint();
        }

        resized();
    }

    int UndoPanelComponent::stackHeight() const noexcept
    {
        return rowHeight() * static_cast<int> (stack.size() + 1);
    }

    void UndoPanelComponent::layOutStack()
    {
        stackView.setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()),
                           juce::jmax (1, stackHeight()));
    }

    void UndoPanelComponent::resized()
    {
        const auto row = rowHeight();
        auto area = getLocalBounds().reduced (row / 3, 0);

        heading.setBounds (area.removeFromTop (row + row / 3).withTrimmedTop (row / 3));

        auto buttons = area.removeFromBottom (row + row / 3).withTrimmedBottom (row / 3);
        okButton.setBounds (buttons.removeFromRight (row * 2).reduced (2));
        cancelButton.setBounds (buttons.removeFromRight (row * 2 + row / 2).reduced (2));

        const auto lines = juce::jmax (1, juce::StringArray::fromLines (summary.getText()).size());
        summary.setBounds (area.removeFromTop (juce::jmin (area.getHeight() / 3,
                                                           juce::roundToInt (14.0 * theme.type) * lines + 6)));
        area.removeFromTop (row / 3);

        viewport.setBounds (area);
        layOutStack();
    }

    void UndoPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-inspect"));
    }

    void UndoPanelComponent::Stack::paint (juce::Graphics& g)
    {
        owner.paintStack (g, getWidth());
    }

    void UndoPanelComponent::Stack::mouseDown (const juce::MouseEvent& event)
    {
        owner.pointedAt (event.y, false);
    }

    void UndoPanelComponent::Stack::mouseDrag (const juce::MouseEvent& event)
    {
        owner.pointedAt (event.y, false);
    }

    void UndoPanelComponent::Stack::mouseUp (const juce::MouseEvent& event)
    {
        owner.pointedAt (event.y, true);
    }

    void UndoPanelComponent::paintStack (juce::Graphics& g, int width)
    {
        const auto row = rowHeight();
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto picked = Look::colour (theme, "picked");
        const auto position = pointing >= 0 ? pointing : reading.position();

        g.fillAll (Look::colour (theme, "panel-inspect"));

        auto y = 0;
        auto drawnPointer = false;

        /*  THE POINTER: a line above the first row that is applied at the
            position, with the word beside it. While the hand drags, it is
            where the hand is, not where the show is. */
        const auto paintPointer = [&]
        {
            g.setColour (picked);
            g.fillRect (0, y + row / 2 - 1, width, 2);
            g.setFont (Look::font (theme, 12.0f));

            const auto words = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 "))
                                 + (position == openedAt ? "here when opened" : "standing here");
            const auto box = juce::Rectangle<int> (unit / 2, y, width - unit, row)
                                 .withWidth (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), words) + unit);
            g.setColour (Look::colour (theme, "panel-inspect"));
            g.fillRect (box);
            g.setColour (picked);
            g.drawText (words, box, juce::Justification::centred, false);
            y += row;
            drawnPointer = true;
        };

        for (const auto& standing : stack)
        {
            if (! drawnPointer && standing.index <= position)
                paintPointer();

            const auto applied = standing.index <= position;
            const auto ink = Look::colour (theme, applied ? "ink" : "ink-off");
            auto line = juce::Rectangle<int> (unit / 2, y, width - unit, row);

            if (standing.index == openedAt && ! standing.opening)
            {
                g.setColour (Look::colour (theme, "standby").withAlpha (0.12f));
                g.fillRect (0, y, width, row);
            }

            g.setColour (Look::colour (theme, applied ? "ink-faint" : "ink-off"));
            g.setFont (Look::font (theme, 11.0f));
            g.drawText (standing.opening ? juce::String() : juce::String (standing.index),
                        line.removeFromLeft (unit * 3), juce::Justification::centredLeft, false);

            g.setColour (ink);
            g.setFont (standing.opening ? Look::font (theme, 12.0f).italicised()
                                        : Look::font (theme, 13.0f));
            g.drawText (juce::String (standing.name), line, juce::Justification::centredLeft, true);

            g.setColour (Look::colour (theme, "rule").withAlpha (0.4f));
            g.fillRect (0, y + row - 1, width, 1);
            y += row;
        }

        if (! drawnPointer)
            paintPointer();
    }

    void UndoPanelComponent::pointedAt (int y, bool settled)
    {
        /*  WHICH STANDING THE HAND IS OVER, counting the pointer's own row as
            it is drawn at the moment: above the first applied row. The row
            under the hand means "stand after this". */
        const auto row = rowHeight();
        const auto position = pointing >= 0 ? pointing : reading.position();
        auto at = 0;
        auto drawnPointer = false;
        auto target = -1;

        for (const auto& standing : stack)
        {
            if (! drawnPointer && standing.index <= position)
            {
                if (y >= at && y < at + row)
                    target = position;

                at += row;
                drawnPointer = true;
            }

            if (y >= at && y < at + row)
                target = standing.index;

            at += row;
        }

        if (target < 0)
            target = y < 0 ? (stack.empty() ? 0 : stack.front().index) : 0;

        if (settled)
        {
            pointing = -1;

            if (target != reading.position() && actions.moveTo)
                actions.moveTo (target);

            stackView.repaint();
            return;
        }

        if (target != pointing)
        {
            pointing = target;
            stackView.repaint();
        }
    }
}
