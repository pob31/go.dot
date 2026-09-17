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

#include <wfg/client/ui/TransportComponent.h>

#include <wfg/client/ui/Look.h>

#include <string>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        juce::String text (const std::string& s) { return juce::String (s); }
    }

    TransportComponent::TransportComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        for (auto* label : { &showLabel, &tickLabel, &clockLabel, &rateLabel, &listLabel,
                             &standbyLabel, &statusLabel, &errorLabel, &noticeLabel })
        {
            label->setJustificationType (juce::Justification::centredLeft);
            label->setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (label);
        }

        tickLabel.setJustificationType (juce::Justification::centredRight);
        clockLabel.setJustificationType (juce::Justification::centredRight);
        rateLabel.setJustificationType (juce::Justification::centredRight);

        /*  The button never takes the keyboard: Space is this component's,
            so that focus resting on the button does not turn Space into a
            second route to the same click. */
        goButton.setWantsKeyboardFocus (false);
        goButton.onClick = [this] { if (actions.go) actions.go(); };
        addAndMakeVisible (goButton);

        setWantsKeyboardFocus (true);
        applyTheme (theme);
    }

    int TransportComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void TransportComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        const auto ink = Look::colour (theme, "ink");
        const auto dim = Look::colour (theme, "ink-dim");
        const auto faint = Look::colour (theme, "ink-faint");

        showLabel.setFont (Look::font (theme, 16.0f));
        showLabel.setColour (juce::Label::textColourId, ink);

        for (auto* label : { &tickLabel, &clockLabel, &rateLabel })
        {
            label->setFont (Look::font (theme, 13.0f));
            label->setColour (juce::Label::textColourId, dim);
        }

        listLabel.setFont (Look::font (theme, 13.0f));
        listLabel.setColour (juce::Label::textColourId, faint);

        standbyLabel.setFont (Look::font (theme, 22.0f));
        standbyLabel.setColour (juce::Label::textColourId, Look::colour (theme, "standby"));

        statusLabel.setFont (Look::font (theme, 13.0f));
        statusLabel.setColour (juce::Label::textColourId, dim);

        errorLabel.setFont (Look::font (theme, 13.0f));
        errorLabel.setColour (juce::Label::textColourId, Look::colour (theme, "failed"));

        noticeLabel.setFont (Look::font (theme, 13.0f));
        noticeLabel.setColour (juce::Label::textColourId, Look::colour (theme, "waiting"));

        resized();
        repaint();
    }

    void TransportComponent::show (const model::TransportReading& reading)
    {
        if (shownOnce && reading == last)
            return;

        showLabel.setText (text (reading.show) + (reading.dirty ? "   ● unsaved" : ""),
                           juce::dontSendNotification);
        tickLabel.setText ("tick " + text (reading.tick), juce::dontSendNotification);
        clockLabel.setText (text (reading.clock), juce::dontSendNotification);
        rateLabel.setText (text (reading.rate), juce::dontSendNotification);

        listLabel.setText (reading.listId.empty() ? juce::String ("no list")
                                                  : "standby in " + text (reading.listName),
                           juce::dontSendNotification);
        standbyLabel.setText (text (reading.standbyLine()), juce::dontSendNotification);

        statusLabel.setText ("audio " + text (reading.status)
                               + (reading.locked ? "   · locked" : ""),
                             juce::dontSendNotification);
        errorLabel.setText (reading.lastError.empty() ? juce::String()
                                                      : "error: " + text (reading.lastError),
                            juce::dontSendNotification);

        last = reading;
        shownOnce = true;
    }

    void TransportComponent::setNotice (const juce::String& notice)
    {
        noticeLabel.setText (notice, juce::dontSendNotification);
    }

    void TransportComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "ground"));

        const auto row = rowHeight();
        const auto pad = row / 2;

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (pad, pad + row, getWidth() - 2 * pad, 1);
        g.fillRect (pad, pad + row + row / 2 + 2 * row, getWidth() - 2 * pad, 1);
    }

    void TransportComponent::resized()
    {
        const auto row = rowHeight();
        const auto pad = row / 2;

        auto area = getLocalBounds().reduced (pad);

        auto top = area.removeFromTop (row);
        rateLabel.setBounds (top.removeFromRight (row * 5));
        clockLabel.setBounds (top.removeFromRight (row * 3));
        tickLabel.setBounds (top.removeFromRight (row * 5));
        showLabel.setBounds (top);

        area.removeFromTop (row / 2);

        auto middle = area.removeFromTop (row * 2);
        goButton.setBounds (middle.removeFromRight (row * 3).reduced (2));
        middle.removeFromRight (pad);
        listLabel.setBounds (middle.removeFromTop (row * 3 / 4));
        standbyLabel.setBounds (middle);

        area.removeFromTop (row / 2);

        auto bottom = area.removeFromTop (row);
        statusLabel.setBounds (bottom.removeFromLeft (row * 7));
        errorLabel.setBounds (bottom);

        noticeLabel.setBounds (area.removeFromTop (row));
    }

    bool TransportComponent::keyPressed (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress (juce::KeyPress::spaceKey))
        {
            if (actions.go) actions.go();
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::F5Key))
        {
            if (actions.reloadTheme) actions.reloadTheme();
            return true;
        }

        return false;
    }
}
