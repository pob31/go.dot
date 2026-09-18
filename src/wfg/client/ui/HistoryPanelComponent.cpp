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

#include <wfg/client/ui/HistoryPanelComponent.h>

#include <wfg/client/model/Scrub.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <string>
#include <utility>

namespace wfg::client::ui
{
    HistoryPanelComponent::HistoryPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        heading.setText ("Load to time", juce::dontSendNotification);
        addAndMakeVisible (heading);
        addAndMakeVisible (aimLabel);
        addAndMakeVisible (answer);

        /*  THE NUMBER, TYPED. Enter commits; the box is never overwritten
            while it holds the focus, so the reading cannot fight a hand. A
            word that is not a number - "before", or nothing - is the aim
            before the cue fired. */
        offsetBox.setSelectAllWhenFocused (true);
        offsetBox.setJustification (juce::Justification::centredRight);
        offsetBox.onReturnKey = [this] { commitOffset(); };
        offsetBox.onFocusLost = [this] { commitOffset(); };
        addAndMakeVisible (offsetBox);

        beforeButton.onClick = [this]
        {
            if (actions.aim && ! reading.aimCue.empty())
                actions.aim (reading.aimCue, -1.0);
        };
        addAndMakeVisible (beforeButton);

        loadButton.onClick = [this]
        {
            if (actions.load)
                actions.load();
        };
        addAndMakeVisible (loadButton);

        viewport.setViewedComponent (&stack, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        setWantsKeyboardFocus (false);
        applyTheme (theme);
    }

    int HistoryPanelComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void HistoryPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        heading.setFont (Look::font (theme, 13.0f));
        heading.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        aimLabel.setFont (Look::font (theme, 13.0f));
        aimLabel.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
        answer.setFont (Look::font (theme, 12.0f));
        answer.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        answer.setMinimumHorizontalScale (1.0f);
        answer.setJustificationType (juce::Justification::topLeft);

        offsetBox.setFont (Look::font (theme, 13.0f));
        offsetBox.setColour (juce::TextEditor::backgroundColourId, Look::colour (theme, "panel-in"));
        offsetBox.setColour (juce::TextEditor::textColourId, Look::colour (theme, "ink"));
        offsetBox.setColour (juce::TextEditor::outlineColourId, Look::colour (theme, "rule"));
        offsetBox.setColour (juce::TextEditor::focusedOutlineColourId, Look::colour (theme, "picked"));

        resized();
        repaint();
    }

    void HistoryPanelComponent::show (const model::LoadToTimeReading& readingToShow)
    {
        reading = readingToShow;

        aimLabel.setText (reading.aimed
                            ? "aim: " + juce::String (reading.nameOf (reading.aimCue))
                            : juce::String ("pick a cue to aim at"),
                          juce::dontSendNotification);

        if (! offsetBox.hasKeyboardFocus (true))
            offsetBox.setText (reading.aimed && reading.aimOffset >= 0.0
                                 ? juce::String (osc::formatDouble (reading.aimOffset))
                                 : juce::String ("before"),
                               juce::dontSendNotification);

        /*  THE ANSWER IN WORDS: how it was read, what would be sounding, where
            the pointer lands, and what the engine could not know. */
        juce::String words;

        if (! reading.aimed)
            words = "Point at a cue in the list, or click a step.";
        else if (! reading.ok)
            words = "The aim names nothing this list holds.";
        else
        {
            words = reading.how == "history" ? "Read from what happened."
                                             : "Read from the list's order: this cue was not fired.";

            auto sounding = 0;

            for (const auto& line : reading.runs)
                if (line.when == "sounding")
                {
                    words += "\n" + juce::String (reading.nameOf (line.cue)) + "  "
                               + juce::String (model::clockText (line.offset)) + " in";
                    ++sounding;
                }

            for (const auto& line : reading.runs)
                if (line.when == "due")
                    words += "\n" + juce::String (reading.nameOf (line.cue)) + "  in "
                               + juce::String (model::clockText (line.startsIn));

            if (sounding == 0)
                words += "\nnothing would be sounding";

            if (! reading.standby.empty())
                words += "\npointer lands on " + juce::String (reading.nameOf (reading.standby));

            for (const auto& why : reading.confused)
                words += "\ncould not know: " + juce::String (why);
        }

        answer.setText (words, juce::dontSendNotification);
        loadButton.setEnabled (reading.aimed && reading.ok);
        beforeButton.setEnabled (reading.aimed);

        /*  The stack is redrawn when the steps, the aim or the instant moved,
            and left alone otherwise: it is a few rows, but it is a few rows
            twenty-five times a second. */
        std::string key = reading.aimCue + " " + std::to_string (reading.instant) + " ";

        for (const auto& step : reading.steps)
            key += std::to_string (step.tick) + step.cue;

        if (key != drawnKey)
        {
            drawnKey = key;
            layOutStack();
            stack.repaint();
        }

        resized();
    }

    int HistoryPanelComponent::stackHeight() const noexcept
    {
        return rowHeight() * static_cast<int> (reading.steps.size() + 1);
    }

    void HistoryPanelComponent::layOutStack()
    {
        stack.setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()),
                       juce::jmax (1, stackHeight()));
    }

    void HistoryPanelComponent::resized()
    {
        const auto row = rowHeight();
        auto area = getLocalBounds().reduced (row / 3, 0);

        heading.setBounds (area.removeFromTop (row + row / 3).withTrimmedTop (row / 3));
        aimLabel.setBounds (area.removeFromTop (row));

        auto controls = area.removeFromTop (row);
        loadButton.setBounds (controls.removeFromRight (row * 2).reduced (2));
        beforeButton.setBounds (controls.removeFromRight (row * 2).reduced (2));
        offsetBox.setBounds (controls.removeFromRight (row * 3).reduced (2));

        /*  As many lines as the answer has, up to a third of the panel. */
        const auto lines = juce::jmax (1, answer.getText().length() == 0 ? 1
                                          : juce::StringArray::fromLines (answer.getText()).size());
        const auto answerHeight = juce::jmin (area.getHeight() / 3,
                                              juce::roundToInt (14.0 * theme.type) * lines + 6);
        answer.setBounds (area.removeFromTop (answerHeight));
        area.removeFromTop (row / 3);

        viewport.setBounds (area);
        layOutStack();
    }

    void HistoryPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-inspect"));
    }

    void HistoryPanelComponent::Stack::paint (juce::Graphics& g)
    {
        owner.paintStack (g, getWidth());
    }

    void HistoryPanelComponent::Stack::mouseUp (const juce::MouseEvent& event)
    {
        owner.stackClicked (event.y);
    }

    void HistoryPanelComponent::paintStack (juce::Graphics& g, int width)
    {
        const auto row = rowHeight();
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto picked = Look::colour (theme, "picked");

        g.fillAll (Look::colour (theme, "panel-inspect"));

        /*  THE POINTER'S PLACE AMONG THE STEPS: the instant is a wall tick,
            and the steps are on the same clock, newest first - so the line
            goes above the first step at or before the instant. "Now" - no
            answer yet, or an aim on a cue with no step - sits at the top. */
        auto pointerBefore = 0;

        if (reading.instant >= 0)
        {
            pointerBefore = static_cast<int> (reading.steps.size());

            for (std::size_t n = 0; n < reading.steps.size(); ++n)
                if (reading.steps[n].tick <= reading.instant)
                {
                    pointerBefore = static_cast<int> (n);
                    break;
                }
        }

        auto y = 0;
        auto drawnPointer = false;

        const auto paintPointer = [&]
        {
            g.setColour (picked);
            g.fillRect (0, y + row / 2 - 1, width, 2);
            g.setFont (Look::font (theme, 12.0f));

            /*  The words on their own ground, so the line does not strike
                them through. */
            const auto words = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + "aim  "
                                 + juce::String (model::offsetText (reading.aimOffset));
            const auto box = juce::Rectangle<int> (unit / 2, y, width - unit, row)
                                 .withWidth (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), words) + unit);
            g.setColour (Look::colour (theme, "panel-inspect"));
            g.fillRect (box);
            g.setColour (picked);
            g.drawText (words, box, juce::Justification::centred, false);
            y += row;
            drawnPointer = true;
        };

        if (reading.steps.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText ("no steps taken yet", juce::Rectangle<int> (unit, 0, width, row),
                        juce::Justification::centredLeft, false);
            return;
        }

        for (std::size_t n = 0; n < reading.steps.size(); ++n)
        {
            if (! drawnPointer && static_cast<int> (n) == pointerBefore && reading.aimed)
                paintPointer();

            const auto& step = reading.steps[n];
            const auto own = step.cue == reading.aimCue
                          && std::none_of (reading.steps.begin(), reading.steps.begin() + static_cast<long> (n),
                                           [&step] (const model::HistoryStep& earlier)
                                           { return earlier.cue == step.cue; });
            const auto undone = reading.instant >= 0 && step.tick > reading.instant;

            if (own)
            {
                g.setColour (picked.withAlpha (0.16f));
                g.fillRect (0, y, width, row);
            }

            const auto ink = Look::colour (theme, undone ? "ink-off" : "ink");
            auto line = juce::Rectangle<int> (unit / 2, y, width - unit, row);

            g.setColour (Look::colour (theme, undone ? "ink-off" : "ink-faint"));
            g.setFont (Look::font (theme, 11.0f));
            g.drawText (juce::String (model::agoText (step.tick, reading.tick)),
                        line.removeFromLeft (unit * 8), juce::Justification::centredLeft, false);

            g.setColour (Look::colour (theme, undone ? "ink-off" : "ink-faint"));
            g.drawText (juce::String (model::originWord (step.origin)),
                        line.removeFromRight (unit * 6), juce::Justification::centredRight, false);

            g.setColour (ink);
            g.setFont (Look::font (theme, 13.0f));
            g.drawText (juce::String (reading.nameOf (step.cue)), line,
                        juce::Justification::centredLeft, true);

            g.setColour (Look::colour (theme, "rule").withAlpha (0.4f));
            g.fillRect (0, y + row - 1, width, 1);
            y += row;
        }

        if (! drawnPointer && reading.aimed)
            paintPointer();
    }

    void HistoryPanelComponent::stackClicked (int y)
    {
        /*  A CLICK ON A STEP AIMS BEFORE IT, as the page's chips do: a step
            is a load-to-time target, `<cue> -1`. The pointer's own row, and
            the space under the stack, take nothing. */
        const auto row = rowHeight();
        auto at = 0;
        auto pointerBefore = 0;

        if (reading.instant >= 0)
        {
            pointerBefore = static_cast<int> (reading.steps.size());

            for (std::size_t n = 0; n < reading.steps.size(); ++n)
                if (reading.steps[n].tick <= reading.instant)
                {
                    pointerBefore = static_cast<int> (n);
                    break;
                }
        }

        auto drawnPointer = false;

        for (std::size_t n = 0; n < reading.steps.size(); ++n)
        {
            if (! drawnPointer && static_cast<int> (n) == pointerBefore && reading.aimed)
            {
                if (y >= at && y < at + row)
                    return;

                at += row;
                drawnPointer = true;
            }

            if (y >= at && y < at + row)
            {
                if (actions.aim)
                    actions.aim (reading.steps[n].cue, -1.0);

                return;
            }

            at += row;
        }
    }

    void HistoryPanelComponent::commitOffset()
    {
        if (! actions.aim || reading.aimCue.empty())
            return;

        const auto typed = offsetBox.getText().trim().toStdString();
        const auto number = osc::parseDouble (typed);
        const auto offset = number.has_value() && *number >= 0.0 ? *number : -1.0;

        if (reading.aimed && std::abs (offset - reading.aimOffset) < 1.0e-9)
            return;

        actions.aim (reading.aimCue, offset);
    }
}
