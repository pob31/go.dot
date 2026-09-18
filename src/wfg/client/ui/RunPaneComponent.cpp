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

#include <wfg/client/ui/RunPaneComponent.h>

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    RunPaneComponent::RunPaneComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        list.setRowHeight (rowHeight());
        list.setWantsKeyboardFocus (false);
        list.getViewport()->setScrollBarsShown (true, false);
        addAndMakeVisible (list);

        applyTheme (theme);
    }

    int RunPaneComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void RunPaneComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        list.setRowHeight (rowHeight());
        list.setColour (juce::ListBox::backgroundColourId, Look::colour (theme, "panel"));
        list.setColour (juce::ListBox::outlineColourId, Look::colour (theme, "rule"));

        resized();
        repaint();
    }

    void RunPaneComponent::show (std::vector<model::RunRow> runs)
    {
        /*  THE WHOLE LIST OR NOTHING. A run's position moves every tick, so
            comparing row by row to find what changed would cost more than
            redrawing the dozen rows a busy pane holds - which is the opposite
            of the cue list's answer, and right for the opposite reason. What
            is still worth avoiding is `updateContent()` when the SET of runs
            has not changed, because that relays out every row. */
        const auto sameRuns = runs.size() == rows.size()
                           && std::equal (runs.begin(), runs.end(), rows.begin(),
                                          [] (const model::RunRow& a, const model::RunRow& b)
                                          { return a.id == b.id; });

        rows = std::move (runs);

        if (sameRuns)
            list.repaint();
        else
            list.updateContent();
    }

    int RunPaneComponent::getNumRows()
    {
        return static_cast<int> (rows.size());
    }

    void RunPaneComponent::paintListBoxItem (int row, juce::Graphics& g,
                                             int width, int height, bool)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        g.fillAll (Look::colour (theme, row % 2 == 0 ? "panel" : "panel-high"));

        /*  THE STATE IS THE ROW'S LEFT EDGE AS WELL AS ITS MARK, so a pane
            read at a glance from across a booth still sorts what is sounding
            from what is waiting. Shape first, colour second (§4.8). */
        const auto colourFor = [this] (const std::string& state)
        {
            if (state == "playing")  return Look::colour (theme, "live");
            if (state == "armed")    return Look::colour (theme, "standby");
            if (state == "stopping") return Look::colour (theme, "stopping");
            if (state == "failed")   return Look::colour (theme, "failed");

            return Look::colour (theme, "waiting");
        };

        const auto tint = colourFor (entry.state);

        g.setColour (tint);
        g.fillRect (0, 0, 3, height);

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);
        area.removeFromLeft (pad);

        /*  THE KILL, at the right edge where the cross is drawn. Only a click
            on the cross sends it: a run stopped by a click that landed
            anywhere on the row is a cue an operator did not mean to stop. */
        auto killCell = area.removeFromRight (unit * 2);
        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97")),
                    killCell, juce::Justification::centred, false);

        //  Where it has got to, when it has been let go.
        auto positionCell = area.removeFromRight (unit * 5);
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 12.0f));
        g.drawText (entry.position, positionCell, juce::Justification::centredRight, false);

        //  The mark, or the word for every state that is not one of the two.
        auto stateCell = area.removeFromLeft (unit * 6);
        g.setColour (tint);
        g.setFont (Look::font (theme, entry.mark().empty() ? 11.0f : 13.0f));
        g.drawText (entry.mark().empty() ? juce::String (entry.state)
                                         : juce::String (juce::CharPointer_UTF8 (entry.mark().c_str())),
                    stateCell, juce::Justification::centredLeft, true);

        area.removeFromLeft (entry.depth * unit);

        /*  THE CUE'S NAME, because a run identifier is eight characters the
            engine drew and nobody recognises. A failure says why, in place of
            the name it would otherwise repeat from the row above. */
        g.setColour (Look::colour (theme, entry.error.empty() ? "ink" : "failed"));
        g.setFont (Look::font (theme, 13.0f));

        const auto name = entry.cueName.empty() ? juce::String (entry.cueId)
                                                : juce::String (entry.cueName);

        g.drawText (entry.error.empty() ? name : name + "  " + juce::String (entry.error),
                    area, juce::Justification::centredLeft, true);

        g.setColour (Look::colour (theme, "rule").withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
    }

    void RunPaneComponent::listBoxItemClicked (int row, const juce::MouseEvent& event)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()) || ! actions.kill)
            return;

        const auto unit = juce::roundToInt (theme.type * 7.0);

        if (event.x >= getWidth() - unit * 3)
            actions.kill (rows[static_cast<std::size_t> (row)].id);
    }

    void RunPaneComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        /*  AN EMPTY PANE SAYS SO. A blank rectangle and a pane that has stopped
            answering look identical, and the difference matters most at the
            moment somebody is wondering why nothing is happening. */
        if (rows.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText ("nothing running", getLocalBounds(), juce::Justification::centred, false);
        }
    }

    void RunPaneComponent::resized()
    {
        list.setBounds (getLocalBounds());
    }
}
