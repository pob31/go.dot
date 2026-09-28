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

#include <wfg/client/ui/LiveBarComponent.h>

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    LiveBarComponent::LiveBarComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        keep.setButtonText ("Keep in the show");
        keep.setTooltip ("Writes the changes ridden live into the show - one step to undo");
        keep.setWantsKeyboardFocus (false);
        keep.onClick = [this]
        {
            if (actions.keep)
                actions.keep();
        };

        drop.setButtonText ("Discard");
        drop.setTooltip ("Lets the changes go: every cue sounds as it is saved");
        drop.setWantsKeyboardFocus (false);
        drop.onClick = [this]
        {
            if (actions.drop)
                actions.drop();
        };

        setWantsKeyboardFocus (false);
    }

    void LiveBarComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        resized();
        repaint();
    }

    bool LiveBarComponent::setLive (int count, bool locked)
    {
        if (count == shownCount && locked == shownLocked)
            return count > 0;

        shownCount = count;
        shownLocked = locked;

        const auto changes = juce::String (count) + (count == 1 ? " change" : " changes");

        /*  LOCKED, IT ONLY SAYS: nothing can be kept while the show refuses
            edits, so there is nothing to press yet. */
        said = locked ? changes + " riding live - heard, not saved; keep or discard them once the show is unlocked"
                      : changes + " ridden live while the show was locked - not saved";

        if (locked)
        {
            removeChildComponent (&keep);
            removeChildComponent (&drop);
        }
        else
        {
            addAndMakeVisible (keep);
            addAndMakeVisible (drop);
        }

        resized();
        repaint();
        return count > 0;
    }

    int LiveBarComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    int LiveBarComponent::preferredHeight() const noexcept
    {
        return rowHeight() + rowHeight() / 3;
    }

    void LiveBarComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-in"));

        const auto row = rowHeight();
        auto area = getLocalBounds().reduced (row / 2, 0);

        if (keep.getParentComponent() != nullptr)
            area.removeFromRight (row * 9);

        g.setColour (Look::colour (theme, "ink"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawFittedText (said, area, juce::Justification::centredLeft, 1);

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }

    void LiveBarComponent::resized()
    {
        const auto row = rowHeight();
        auto area = getLocalBounds().reduced (row / 6, row / 6);

        drop.setBounds (area.removeFromRight (row * 4).reduced (row / 4, 0));
        keep.setBounds (area.removeFromRight (row * 5).reduced (row / 4, 0));
    }
}
